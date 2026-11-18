#pragma once

#include "fh1_native_list.h"

#include <rex/graphics/fh1_host_packet.h>

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

  struct Operation {
    Kind kind;
    uint32_t header;
    uint32_t first_word;
    uint32_t count;
    uint32_t input = UINT32_MAX;
  };

  struct MemorySnapshot {
    uint32_t address;
    // Indirect operands in guest byte order.
    std::vector<uint32_t> guest_words;
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
    // Keep both the input image and the operation table bounded. Large lists
    // continue through the ordinary decoder.
    constexpr size_t kMaxWords = 1u << 16;
    if ((snapshot.generation_before & 1) ||
        snapshot.generation_before != snapshot.generation_after ||
        snapshot.words.empty() || snapshot.words.size() > kMaxWords ||
        !Validate(registered.words.data(), registered.words.size()) ||
        TemplateKey(registered.words.data()) != snapshot.key ||
        registered.words[kTListWords] != snapshot.words.size()) {
      return std::nullopt;
    }
    ReplayPlan plan(std::move(snapshot.words));
    size_t input_count = 0;
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
        if ((address & 3) || input_count == snapshot.inputs.size() ||
            dwords > kMaxWords - input_words ||
            dwords * sizeof(uint32_t) > 0x20000000u - physical) {
          return std::nullopt;
        }
        const auto& input = snapshot.inputs[input_count];
        if (input.address != address || input.guest_words.size() != dwords)
          return std::nullopt;
        op.input = uint32_t(input_count++);
        input_words += dwords;
      }
      plan.operations_.push_back(op);
      i += count;
    }
    if (plan.operations_.empty() || input_count != snapshot.inputs.size()) return std::nullopt;
    plan.inputs_ = std::move(snapshot.inputs);
    // Use the same byte order as a stock PM4 reader. This conversion runs
    // once at preparation, rather than once per recorded packet.
    for (auto& word : plan.words_) word = __builtin_bswap32(word);
    return plan;
  }

  std::span<const Operation> operations() const { return operations_; }
  std::span<const uint32_t> guest_words() const { return words_; }
  std::span<const uint32_t> input_words(const Operation& op) const {
    return op.input == UINT32_MAX ? std::span<const uint32_t>{} : inputs_[op.input].guest_words;
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
  explicit ReplayPlan(std::vector<uint32_t> words) : words_(std::move(words)) {}

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
};

// Holds a prepared replay until the next indirect-buffer admission. The
// snapshot provider must pin or copy the producer storage before returning.
// This class only owns copied bytes and never dereferences guest addresses.
class PendingReplay {
 public:
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
