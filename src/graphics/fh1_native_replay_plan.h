#pragma once

#include "fh1_native_list.h"

#include <rex/graphics/fh1_host_packet.h>

#include <array>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

namespace rex::graphics::fh1_list {

// A plan is compiled entirely from a host-owned copy. Registration and this
// packet check alone do not grant permission to read live guest memory.
class ReplayPlan {
 public:
  enum class Kind { kRegisterRange, kRepeatedRegister, kType3 };
  enum class AddressSpace { kVirtual, kPhysical };

  struct Operation {
    Kind kind;
    uint32_t header;
    uint32_t first_word;
    uint32_t count;
    uint32_t input = UINT32_MAX;
    uint32_t index_input = UINT32_MAX;
  };

  struct MemorySnapshot {
    uint32_t address;
    // Indirect operands in guest byte order.
    std::vector<uint32_t> guest_words;
  };

  struct IndexSnapshot {
    uint32_t address;
    uint32_t width;
    uint32_t endian;
    std::vector<std::byte> bytes;
  };

  struct Snapshot {
    // The provider pins the binding and each referenced range through the
    // copies and samples the generations around the complete snapshot.
    uint32_t generation_before;
    uint32_t generation_after;
    Key key;
    std::vector<uint32_t> words;
    // One entry for each IM_LOAD or LOAD_ALU_CONSTANT, in packet order.
    std::vector<MemorySnapshot> inputs;
  };

  static std::optional<ReplayPlan> Compile(const Template& registered, Snapshot snapshot) {
    if ((snapshot.generation_before & 1) ||
        snapshot.generation_before != snapshot.generation_after ||
        snapshot.words.empty() || snapshot.words.size() > kMaxWords ||
        !Validate(registered.words.data(), registered.words.size()) ||
        TemplateKey(registered.words.data()) != snapshot.key ||
        registered.words[kTListWords] != snapshot.words.size()) {
      return std::nullopt;
    }
    size_t input_count = 0;
    auto plan = Build(std::move(snapshot.words), [&](uint32_t address, uint32_t dwords)
                         -> std::optional<MemorySnapshot> {
      if (input_count == snapshot.inputs.size()) return std::nullopt;
      auto& input = snapshot.inputs[input_count++];
      if (input.address != address || input.guest_words.size() != dwords) return std::nullopt;
      return std::move(input);
    });
    if (input_count != snapshot.inputs.size()) return std::nullopt;
    return plan;
  }

  // Called with a producer lease that excludes writes and frees for the
  // binding and checks ownership of every requested range. Generations
  // detect fixups and publication changes, but do not confer that lease.
  template <typename Lease>
  static std::optional<ReplayPlan> Capture(const Template& registered, const Binding& binding,
                                           Lease& lease) {
    if (!binding.producer_lifetime || !binding.incarnation || !binding.state_word || !binding.cb ||
        (binding.cb & 3) || (binding.state_word & 3) || (binding.patch & 3) ||
        binding.patch_words > kMaxWords || (binding.patch_words && !binding.patch) ||
        !binding.words || binding.words > kMaxWords || (binding.phys & 3) ||
        binding.phys >= kPhysicalBytes ||
        binding.words * sizeof(uint32_t) > kPhysicalBytes - binding.phys ||
        !Validate(registered.words.data(), registered.words.size()) ||
        registered.words[kTListWords] != binding.words || !lease.Matches(binding))
      return std::nullopt;

    const auto generation = lease.ReadGeneration(binding.state_word);
    if (!generation || (*generation & 1)) return std::nullopt;
    const auto copy = [&](AddressSpace space, uint32_t address, auto& data) {
      auto bytes = std::as_writable_bytes(std::span(data));
      return bytes.empty() || (bytes.size() - 1 <= UINT32_MAX - address &&
                               lease.Copy(space, address, bytes));
    };
    std::array<uint8_t, kHeaderBytes> header;
    std::array<uint8_t, 16> link;
    if (!copy(AddressSpace::kVirtual, binding.cb, header)) return std::nullopt;
    const uint32_t chain = Word(header.data(), kCommandChain / 4);
    if (!chain || (chain & 3) || Word(header.data(), kCommandPatch / 4) != binding.patch ||
        Word(header.data(), kHeaderPatchWords / 4) != binding.patch_words ||
        !copy(AddressSpace::kVirtual, chain, link) ||
        Word(link.data(), 0) || Word(link.data(), 1) != 1 ||
        (Word(link.data(), 2) & 0xffffffu) != binding.words ||
        (Word(link.data(), 3) & 0x1fffffffu) != binding.phys) return std::nullopt;

    std::vector<uint32_t> patch(binding.patch_words), words(binding.words);
    if (!copy(AddressSpace::kVirtual, binding.patch, patch) ||
        !copy(AddressSpace::kPhysical, binding.phys, words)) return std::nullopt;
    const uint32_t* t = registered.words.data();
    if (LiveKey(t, header.data(), FilesFormFlags(Word(header.data(), kHeaderFlags / 4)),
                reinterpret_cast<const uint8_t*>(patch.data()), binding.patch_words,
                reinterpret_cast<const uint8_t*>(words.data()), binding.words) != TemplateKey(t))
      return std::nullopt;

    for (auto& word : words) word = __builtin_bswap32(word);
    auto plan = Build(std::move(words), [&](uint32_t address, uint32_t dwords)
                          -> std::optional<MemorySnapshot> {
      MemorySnapshot input{address, std::vector<uint32_t>(dwords)};
      if (!copy(AddressSpace::kPhysical, address & 0x1fffffffu, input.guest_words))
        return std::nullopt;
      return input;
    });
    if (!plan || !plan->CaptureIndices(lease) ||
        lease.ReadGeneration(binding.state_word) != generation) return std::nullopt;
    plan->publication_ = Publication{binding.producer_lifetime, binding.incarnation, binding.phys,
                                     binding.words, *generation};
    plan->header_ = header;
    plan->link_ = link;
    plan->chain_ = chain;
    plan->patch_ = std::move(patch);
    return plan;
  }

  // RunCommandBuffer (TU4 82446F10–82446F7C) owns the command buffer's
  // last-run stamp and performs its side effects before the decoder sees this
  // list. Neither the replay plan nor the live key consumes that stamp.
  static constexpr size_t kLastRunStampOffset = 8;

  // Guard precisely the same spans Capture owned. The caller still
  // holds the original producer lease and the physical capture transaction.
  template <typename Lease>
  bool Revalidate(const Binding& binding, Lease& lease) const {
    if (!MatchesPublication(binding) || !lease.Matches(binding) ||
        lease.ReadGeneration(binding.state_word) != captured_generation()) return false;

    const auto match = [&](AddressSpace space, uint32_t address, const auto& data) {
      auto bytes = std::as_bytes(std::span(data));
      return bytes.empty() || lease.MatchBytes(space, address, bytes);
    };
    if (!lease.MatchCommandHeader(binding.cb, std::as_bytes(std::span(header_)),
                                  kLastRunStampOffset) ||
        !match(AddressSpace::kVirtual, chain_, link_) ||
        !match(AddressSpace::kVirtual, binding.patch, patch_) ||
        !match(AddressSpace::kPhysical, binding.phys, words_)) return false;
    for (const auto& input : inputs_) {
      if (!match(AddressSpace::kPhysical, input.address & 0x1fffffffu, input.guest_words))
        return false;
    }
    for (const auto& input : indices_) {
      if (!match(AddressSpace::kPhysical, input.address, input.bytes)) return false;
    }
    return lease.ReadGeneration(binding.state_word) == captured_generation();
  }

  // Registry::Bind changes incarnation whenever a wire identity field changes.
  // Plain Compile has no title lease and cannot mint an owned publication.
  bool MatchesPublication(const Binding& binding) const {
    return publication_ && publication_->lifetime == binding.producer_lifetime &&
           publication_->incarnation == binding.incarnation && publication_->phys == binding.phys &&
           publication_->words == binding.words;
  }
  bool IsCurrentList(Registry& registry, uint32_t phys, uint32_t words) const {
    if (!publication_ || publication_->phys != phys || publication_->words != words) return false;
    Skip skip;
    const auto* binding = registry.Lookup(phys, words, &skip);
    return binding && MatchesPublication(*binding) && binding->pool_valid &&
           publication_->generation == binding->pool_generation;
  }
  uint64_t producer_lifetime() const { return publication_ ? publication_->lifetime : 0; }
  std::optional<uint32_t> captured_generation() const {
    if (!publication_) return std::nullopt;
    return publication_->generation;
  }
  std::span<const Operation> operations() const { return operations_; }
  std::span<const uint32_t> guest_words() const { return words_; }
  std::span<const uint32_t> input_words(const Operation& op) const {
    return op.input == UINT32_MAX ? std::span<const uint32_t>{} : inputs_[op.input].guest_words;
  }
  const IndexSnapshot* index_input(const Operation& op) const {
    return op.index_input == UINT32_MAX ? nullptr : &indices_[op.index_input];
  }
  auto body(const Operation& op) const {
    return guest_words().subspan(op.first_word, op.count) |
           std::views::transform([](uint32_t word) { return __builtin_bswap32(word); });
  }

  // A false result means dispatch may already have produced effects. The
  // caller decides on stock fallback before starting this operation.
  template <typename Recorder>
  bool Dispatch(Recorder&& record) const {
    for (const auto& op : operations_) {
      if (!record(op, body(op))) return false;
    }
    return true;
  }

 private:
  static constexpr size_t kMaxWords = 1u << 16;
  static constexpr uint32_t kPhysicalBytes = 0x20000000u;
  static constexpr uint32_t kCommandChain = 116, kCommandPatch = 144, kHeaderPatchWords = 124;

  explicit ReplayPlan(std::vector<uint32_t> words) : words_(std::move(words)) {}

  template <typename Lease>
  bool CaptureIndices(Lease& lease) {
    size_t remaining = kMaxWords * sizeof(uint32_t);
    for (auto& op : operations_) {
      if (op.kind != Kind::kType3 || ((op.header >> 8) & 0x7f) != 0x22) continue;
      // Use the same VGT_DRAW_INITIATOR and VGT_DMA_SIZE interpretation as
      // PrimitiveProcessor, including the DMA-count clamp and alignment.
      const uint32_t initiator = __builtin_bswap32(words_[op.first_word + 1]);
      const uint32_t width = (initiator & (1u << 11)) ? 4 : 2;
      const uint32_t address = __builtin_bswap32(words_[op.first_word + 2]) & ~(width - 1);
      const uint32_t dma_size = __builtin_bswap32(words_[op.first_word + 3]);
      const uint32_t count = std::min(initiator >> 16, dma_size & 0xffffffu);
      const uint32_t bytes = count * width;
      if (!bytes) continue;
      if (address >= kPhysicalBytes || bytes > kPhysicalBytes - address || bytes > remaining)
        return false;
      IndexSnapshot input{address, width, dma_size >> 30, std::vector<std::byte>(bytes)};
      if (!lease.Copy(AddressSpace::kPhysical, address, input.bytes)) return false;
      op.index_input = uint32_t(indices_.size());
      indices_.push_back(std::move(input));
      remaining -= bytes;
    }
    return true;
  }

  template <typename ReadInput>
  static std::optional<ReplayPlan> Build(std::vector<uint32_t> words, ReadInput&& read_input) {
    ReplayPlan plan(std::move(words));
    size_t input_words = 0;
    for (uint32_t i = 0; i < plan.words_.size();) {
      const uint32_t header = plan.words_[i++];
      const uint32_t packet_type = header >> 30;
      if (packet_type == 2) continue;
      if (!header || (packet_type != 0 && packet_type != 3)) return std::nullopt;
      const uint32_t count = ((header >> 16) & 0x3fff) + 1;
      if (count > plan.words_.size() - i) return std::nullopt;

      Kind kind = Kind::kType3;
      if (packet_type == 0) {
        const uint32_t start = header & 0x7fff;
        const bool repeat = (header & 0x8000) != 0;
        if (start + (repeat ? 1 : count) > kRegisterLimit) return std::nullopt;
        kind = repeat ? Kind::kRepeatedRegister : Kind::kRegisterRange;
      } else if (!SupportedType3(header, std::span(plan.words_).subspan(i, count))) {
        return std::nullopt;
      }
      Operation op{kind, header, i, count};
      const uint32_t opcode = (header >> 8) & 0x7f;
      if (kind == Kind::kType3 && (opcode == 0x27 || opcode == 0x2f)) {
        const uint32_t address = plan.words_[i] & (opcode == 0x27 ? ~3u : 0x3fffffffu);
        const uint32_t dwords = opcode == 0x27 ? plan.words_[i + 1] : kLiteralWords;
        const uint32_t physical = address & 0x1fffffffu;
        if ((address & 3) || dwords > kMaxWords - input_words ||
            dwords * sizeof(uint32_t) > kPhysicalBytes - physical) {
          return std::nullopt;
        }
        auto input = read_input(address, dwords);
        if (!input) return std::nullopt;
        op.input = uint32_t(plan.inputs_.size());
        plan.inputs_.push_back(std::move(*input));
        input_words += dwords;
      }
      plan.operations_.push_back(op);
      i += count;
    }
    if (plan.operations_.empty()) return std::nullopt;
    // Use the same byte order as a stock PM4 reader. This conversion runs
    // once at preparation, rather than once per recorded packet.
    for (auto& word : plan.words_) word = __builtin_bswap32(word);
    return plan;
  }

  static bool SupportedType3(uint32_t header, std::span<const uint32_t> body) {
    if (header & 0x80fe) return false;
    switch ((header >> 8) & 0x7f) {
      case kPm4Nop:
        // Host packets carried in NOPs have their own state and side effects.
        return !(header & 1) && body.front() != fh1_host::kMagic;
      case 0x22:  // Draw using DMA indices with no visibility query.
        return body.size() == 4 && body[0] == 0 && ((body[1] >> 6) & 3) == 0;
      case 0x27:  // IM_LOAD, one shader from its original memory source.
        return !(header & 1) && body.size() == 2 && (body[0] & 3) <= 1 &&
               body[1] && !(body[1] >> 16);
      case 0x2d:  // The three tile-window registers.
        return header == 0xc0032d01 && body.front() == 0x00040080;
      case 0x2f:  // One 16-word ALU literal block.
        return !(header & 1) && body.size() == 3 && !(body[1] >> 11) &&
               body[2] == kLiteralWords && body[1] <= 0x800 - kLiteralWords;
      case 0x60:  // Bin mask low.
      case 0x61:  // Bin mask high.
        return !(header & 1) && body.size() == 1;
      default:
        return false;
    }
  }

  std::vector<uint32_t> words_;
  std::vector<Operation> operations_;
  std::vector<MemorySnapshot> inputs_;
  std::vector<IndexSnapshot> indices_;
  struct Publication {
    uint64_t lifetime, incarnation;
    uint32_t phys, words, generation;
  };
  std::optional<Publication> publication_;
  std::array<uint8_t, kHeaderBytes> header_{};
  std::array<uint8_t, 16> link_{};
  uint32_t chain_ = 0;
  std::vector<uint32_t> patch_;
};

// Holds a prepared replay until the next indirect-buffer admission. The
// snapshot provider must pin or copy the producer storage before returning.
// This class only owns copied bytes and never dereferences guest addresses.
class PendingReplay {
 public:
  template <typename Lease>
  bool PrepareFromLease(Registry& registry, uint32_t phys, uint32_t words, Lease& lease) {
    plan_.reset();
    Skip skip;
    const Binding* binding = registry.Lookup(phys, words, &skip);
    if (!binding) return false;
    const Template* registered = registry.FindTemplate(binding->template_id);
    if (!registered) return false;
    plan_ = ReplayPlan::Capture(*registered, *binding, lease);
    if (!plan_) return false;
    incarnation_ = binding->incarnation;
    generation_ = *plan_->captured_generation();
    return true;
  }

  bool Prepare(Registry& registry, uint32_t phys, uint32_t words, uint64_t captured_incarnation,
               ReplayPlan::Snapshot snapshot) {
    plan_.reset();
    Skip skip;
    const Binding* binding = registry.Lookup(phys, words, &skip);
    if (!captured_incarnation || !binding || binding->incarnation != captured_incarnation)
      return false;
    const Template* registered = registry.FindTemplate(binding->template_id);
    if (!registered) return false;
    const uint32_t generation = snapshot.generation_before;
    plan_ = ReplayPlan::Compile(*registered, std::move(snapshot));
    if (!plan_) return false;
    incarnation_ = binding->incarnation;
    generation_ = generation;
    return true;
  }

  std::optional<ReplayPlan> Take(Registry& registry, uint32_t phys, uint32_t words,
                                 std::optional<uint32_t> current_generation) {
    auto plan = std::exchange(plan_, std::nullopt);
    if (!plan || current_generation != generation_) return std::nullopt;
    Skip skip;
    const Binding* binding = registry.Lookup(phys, words, &skip);
    if (!binding || binding->incarnation != incarnation_) return std::nullopt;
    return plan;
  }

 private:
  uint64_t incarnation_ = 0;
  uint32_t generation_ = 0;
  std::optional<ReplayPlan> plan_;
};

}  // namespace rex::graphics::fh1_list
