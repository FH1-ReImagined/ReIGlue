// FH1 (patch 0650): the native renderer's model lists in shadow mode, on the command processor's
// side (FH1 docs/native-renderer.md, "M4, third step"; the pure part: fh1_native_list.h).
//
// The game (FH1 native_lists=shadow) converts each model list when it first runs, sends the
// template once (kOpListDefine) and binds the list's physical address to it (kOpListBind), in
// the stream before the RunCommandBuffer call's own words; kOpListDrop when the model unloads.
// These ops are handled here on the decoder, as they are read. When ExecuteIndirectBuffer runs
// a registered address, the buffer is decoded as always, and beside it:
// - the lookup's checks: the indirect buffer's length is the registered one, the command buffer
//   still names this buffer, and the list's static words still have the template's key (read
//   again on every run, tiled and untiled alike);
// - the template's slots are read from the live list when its state word's generation changed
//   (as a replay would), and also on every run, to count a slot written without a fixup;
// - each draw: the state the template says it starts with (the register file at the buffer's
//   start, the list's writes up to the draw overlaid, the tile block applied for the live bin
//   select) against the register file the packets produced, over what the draw's shaders and
//   fixed function read (0x2000-0x23FF, the ALU, fetch, bool and loop constants), the shaders,
//   the bin mask, whether the predicate lets it run;
// - after the buffer: the whole register file but D3D's marks, the bin mask and the shaders;
// - before each draw and after the last: the registers the packets wrote (the command
//   processor's write paths report them while the run is open) against the ones the template
//   writes, whatever their values, so a write the template leaves out or misplaces is seen
//   also when it only repeats a value the register file already has.
// Anything a run is not compared for is counted with its reason. A summary every 600 frames and
// at exit.
#include <rex/graphics/command_processor.h>

#include <rex/graphics/fh1_host_packet.h>
#include <rex/graphics/register_file.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#if REX_PLATFORM_WIN32
#include <Windows.h>
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/command_processor.h>
#endif
#endif
#include "fh1_native_list.h"
#include "fh1_command_cursor.h"
#include "fh1_native_replay_plan.h"
#include "fh1_replay_benchmark.h"

namespace rex::graphics {

namespace fh = fh1_host;
namespace fl = fh1_list;

namespace {

constexpr uint32_t kGroups = 22;  // pass groups 0-20, then "not known"
uint32_t GroupIndex(uint32_t group) {
  return group < 21 ? group : 21;
}
constexpr uint32_t kRefusals = 32;
// The template words the plugin keeps at most (64 MB; a session of the benchmark keeps 1.6 M,
// the most measured 2.2 M). Templates are never freed in this step (docs, "M4, third step").
constexpr uint64_t kTemplateWordsCap = uint64_t(16) << 20;

struct GroupCounts {
  uint64_t runs = 0, compared = 0, agree = 0, draws = 0, draws_agree = 0, draws_skipped = 0;
  uint64_t skips[size_t(fl::Skip::kCount)] = {};
  uint64_t untiled_runs = 0;
};

// A guest virtual address the game could have given (the plugin reads guest memory at it).
bool Plausible(uint32_t address) {
  return address >= 0x00100000u && address < 0xF0000000u;
}

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

#if REX_PLATFORM_WIN32
struct RetainedReplay {
  uint64_t allocation_stamp = 0;
  std::shared_ptr<const fl::ReplayPlan> plan;
};
#endif

}  // namespace

struct CommandProcessor::Fh1Lists {
  fl::Registry registry;
  fl::ShadowRun run;
  fl::Shaders shaders;
  std::vector<uint32_t> host_words, fresh_pool, pool;
  // The run being compared.
  fl::Binding* binding = nullptr;
  uint32_t group = 21, phys = 0;
  bool untiled = false, run_agree = true;
  uint32_t run_fields = 0;

  GroupCounts groups[kGroups];
  uint64_t refusals[kRefusals] = {};
  uint64_t other_buffers = 0;
  // Lookup misses at an address a list was bound at before (dropped since): not counted with
  // the other buffers, which were never a list.
  uint64_t dropped_address_misses = 0;
  // Template words kept are capped (kTemplateWordsCap): a define past it is acknowledged and
  // counted, not kept (its lists' runs: no-template).
  uint64_t define_capped = 0;
  // Static key checks: [untiled][differs].
  uint64_t key_checks[2][2] = {};
  uint64_t generation_changes = 0, in_progress = 0, stale_pools = 0;
  uint64_t defines = 0, define_repeats = 0, define_invalid = 0, define_unreadable = 0,
           define_hash = 0, template_words = 0;
  uint64_t binds = 0, bind_same = 0, bind_replaced = 0, bind_no_template = 0, drops = 0,
           drop_unknown = 0, malformed = 0;
  uint64_t stale_binds = 0;
#if REX_PLATFORM_WIN32
  bool audit_binding = false;
  bool audit_capture = false;
  bool audit_upload = false;
  bool audit_bind = false;
#if REX_HAS_VULKAN
  vulkan::VulkanCommandProcessor* index_audit_cp = nullptr;
  bool timed_replay = false;
  bool timed_pixel_parity = false;
  bool timed_texture_parity = false;
  bool retained_pixel_only = false;
  bool refused_replay_control = false;
  uint32_t pixel_parity_sampling_passes = 0;
  std::shared_ptr<const fl::ReplayPlan> timed_plan;
#endif
  uint32_t audit_attempts = 0;
  std::array<uint64_t, 16> audited_lifetimes{};
  std::array<RetainedReplay, 16> retained_replays{};
  std::array<uint8_t, 16> capture_visits{};
  uint32_t capture_visit_cap = 64;
  bool repeat_capture = false;
  bool sustained_replay = false;
  bool sustained_checked_lifetime = false;
  std::array<uint32_t, 16> sustained_phys{};
  uint64_t sustained_capture_ns = 0;
  uint64_t sustained_captured = 0, sustained_reused = 0;
  uint64_t sustained_lists = 0, sustained_failed = 0, sustained_operations = 0;
  uint64_t sustained_first_ns = 0, sustained_last_ns = 0;
  bool reuse_snapshot = false;
  std::optional<uint64_t> repeat_capture_frame;
  uint32_t repeat_captures_this_frame = 0;

  void RetireSustainedBinding(uint32_t phys, uint64_t lifetime = 0) {
    if (!sustained_replay) return;
    for (size_t i = 0; i < audited_lifetimes.size(); ++i) {
      if (audited_lifetimes[i] && sustained_phys[i] == phys &&
          (!lifetime || audited_lifetimes[i] == lifetime)) {
        audited_lifetimes[i] = 0;
        retained_replays[i] = {};
      }
    }
  }
#endif
  uint64_t draw_fields[fl::kFieldCount] = {}, end_fields[fl::kFieldCount] = {};
  uint64_t ns_begin = 0, ns_draw = 0, ns_end = 0, ns_packets = 0;
  uint32_t last_ack = 0;
  uint32_t detailed = 0;
  uint64_t frames = 0;  // swaps decoded since the first registry op
  uint64_t perturb_every = 0, perturb_count = 0, perturbed = 0;
  // FH1_NATIVE_LISTS_PERTURB_TILE=N: in every Nth compared run of a tiled list, the first tile
  // block's mask in the template view is the second's (the selected tile's window is in the
  // register file already, so only the writes can tell); tile_effective counts the runs whose
  // select no block of the view matches any more. FH1_NATIVE_LISTS_PERTURB_SLOT=N: every Nth
  // template defined has a one-word slot moved one dword on (the key differs from the first
  // check: key-at-bind).
  uint64_t perturb_tile_every = 0, perturb_tile_count = 0, tile_perturbed = 0, tile_effective = 0;
  uint64_t perturb_slot_every = 0, slot_defines = 0, slot_moved = 0;
};

namespace {

const uint8_t* Physical(void* context, uint32_t address) {
  return static_cast<const uint8_t*>(
      static_cast<memory::Memory*>(context)->TranslatePhysical(address));
}

#if REX_PLATFORM_WIN32
struct LiveCaptureProbe {
  memory::Memory& memory;
  const fl::Binding& binding;
  const fl::Template& registered;
  const char* status = "binding-busy";
  uint32_t generation = 0, address = 0;
  size_t input_bytes = 0, range_bytes = 0, copied = 0;
  size_t index_bytes = 0, index_draws = 0;
  fl::ReplayPlan::AddressSpace space = fl::ReplayPlan::AddressSpace::kVirtual;
  bool refused_range = false;
  std::shared_ptr<const fl::ReplayPlan> plan;
  RetainedReplay* retained = nullptr;
  size_t compared_bytes = 0;
  bool retained_hit = false;
};

struct LiveCaptureLease {
  LiveCaptureProbe& probe;
  const memory::Memory::ReplayPhysicalCapture& capture;

  bool Matches(const fl::Binding& binding) const { return &binding == &probe.binding; }
  std::optional<uint32_t> ReadGeneration(uint32_t address) const {
    if (address != probe.binding.state_word) return std::nullopt;
    auto* word = probe.memory.TranslateVirtual<uint32_t*>(address);
    const uint32_t generation =
        __builtin_bswap32(std::atomic_ref<uint32_t>(*word).load(std::memory_order_acquire));
    return generation == probe.generation ? std::optional{generation} : std::nullopt;
  }
  bool MatchCommandHeader(uint32_t address, std::span<const std::byte> bytes,
                          size_t ignored_word) {
    probe.compared_bytes += bytes.size();
    return capture.MatchesPhysicalAliasExceptWord(
        address, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
        probe.retained->allocation_stamp, ignored_word);
  }
  bool MatchBytes(fl::ReplayPlan::AddressSpace space, uint32_t address,
                  std::span<const std::byte> bytes) {
    const auto* expected = reinterpret_cast<const uint8_t*>(bytes.data());
    const uint64_t stamp = probe.retained->allocation_stamp;
    probe.compared_bytes += bytes.size();
    return space == fl::ReplayPlan::AddressSpace::kVirtual
               ? capture.MatchesPhysicalAlias(address, expected, bytes.size(), stamp)
               : capture.Matches(address, expected, bytes.size(), stamp);
  }
  bool Copy(fl::ReplayPlan::AddressSpace space, uint32_t address, std::span<std::byte> bytes) {
    probe.space = space;
    probe.address = address;
    probe.range_bytes = bytes.size();
    auto* out = reinterpret_cast<uint8_t*>(bytes.data());
    const bool copied = space == fl::ReplayPlan::AddressSpace::kVirtual
                            ? capture.CopyPhysicalAlias(address, out, bytes.size())
                            : capture.Copy(address, out, bytes.size());
    probe.refused_range = !copied;
    probe.copied += copied;
    return copied;
  }
};

bool ProbeLiveCapture(void* context, uint32_t generation) noexcept {
  auto& probe = *static_cast<LiveCaptureProbe*>(context);
  probe.generation = generation;
  probe.status = "runtime-busy";
  try {
    return probe.memory.WithReplayPhysicalCapture(
        [](void* context, const memory::Memory::ReplayPhysicalCapture& capture) {
          auto& probe = *static_cast<LiveCaptureProbe*>(context);
          LiveCaptureLease lease{probe, capture};
          if (probe.retained && probe.retained->plan &&
              probe.retained->plan->Revalidate(probe.binding, lease)) {
            probe.plan = probe.retained->plan;
            probe.retained_hit = true;
            probe.status = "reused";
            return true;
          }
          if (probe.retained) *probe.retained = {};
          auto plan = fl::ReplayPlan::Capture(probe.registered, probe.binding, lease);
          if (!plan) {
            probe.status = probe.refused_range ? "range-refused" : "plan-refused";
            return false;
          }
          probe.plan = std::make_shared<const fl::ReplayPlan>(std::move(*plan));
          if (probe.retained) {
            *probe.retained = {capture.AllocationStamp(), probe.plan};
          }
          for (const auto& op : probe.plan->operations()) {
            probe.input_bytes += probe.plan->input_words(op).size_bytes();
            if (const auto* index = probe.plan->index_input(op)) {
              ++probe.index_draws;
              probe.index_bytes += index->bytes.size();
            }
          }
          probe.status = "captured";
          return true;
        },
        &probe);
  } catch (...) {
    probe.status = "capture-error";
    return false;
  }
}
#endif

void LogLists(const CommandProcessor::Fh1Lists& s, const char* when, uint64_t frames) {
#if REX_PLATFORM_WIN32
  if (s.sustained_replay) {
    REXGPU_INFO("M4 sustained replay: frames={} attempts={} captured={} reused={} "
                "lists={} failed={} ops={} guard_ns={} active_ns={}",
                frames, s.audit_attempts, s.sustained_captured, s.sustained_reused,
                s.sustained_lists, s.sustained_failed, s.sustained_operations,
                s.sustained_capture_ns, s.sustained_last_ns - s.sustained_first_ns);
  }
#endif
  auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
  GroupCounts all;
  for (const GroupCounts& g : s.groups) {
    all.runs += g.runs;
    all.compared += g.compared;
    all.agree += g.agree;
    all.draws += g.draws;
    all.draws_agree += g.draws_agree;
    all.draws_skipped += g.draws_skipped;
    all.untiled_runs += g.untiled_runs;
    for (size_t k = 0; k < size_t(fl::Skip::kCount); ++k)
      all.skips[k] += g.skips[k];
  }
  std::string skips;
  for (size_t k = 1; k < size_t(fl::Skip::kCount); ++k)
    skips += fmt::format(" {} {};", fl::SkipName(fl::Skip(k)), all.skips[k]);
  std::string refused;
  for (uint32_t r = 0; r < kRefusals; ++r)
    if (s.refusals[r])
      refused += fmt::format(" {}:{}", r, s.refusals[r]);
  const double per_run = all.compared ? 1e-3 / double(all.compared) : 0.0;
  REXGPU_INFO(
      "FH1 native lists ({}, {} frames): runs at registered lists {}: compared {} ({} untiled), "
      "agree {} ({:.4f}%); draws {} ({} predicated off), agree {} ({:.4f}%); not compared:{} "
      "refusal codes:{}; key checks tiled {} same {} differ, untiled {} same {} differ; pools: "
      "{} generation changes, {} runs during a fixup, {} stale (a slot changed without a fixup); "
      "other buffers {}, misses at a dropped list's address {}{}",
      when, frames, all.runs, all.compared, all.untiled_runs, all.agree,
      pct(all.agree, all.compared), all.draws, all.draws_skipped, all.draws_agree,
      pct(all.draws_agree, all.draws), skips, refused.empty() ? " none" : refused,
      s.key_checks[0][0], s.key_checks[0][1], s.key_checks[1][0], s.key_checks[1][1],
      s.generation_changes, s.in_progress, s.stale_pools, s.other_buffers, s.dropped_address_misses,
      (s.perturb_every
           ? fmt::format("; {} runs perturbed on purpose (FH1_NATIVE_LISTS_PERTURB)", s.perturbed)
           : std::string()) +
          (s.perturb_tile_every
               ? fmt::format("; {} runs with a tile mask perturbed, {} of them for their select "
                             "(FH1_NATIVE_LISTS_PERTURB_TILE)",
                             s.tile_perturbed, s.tile_effective)
               : std::string()) +
          (s.perturb_slot_every
               ? fmt::format("; {} templates with a slot moved (FH1_NATIVE_LISTS_PERTURB_SLOT)",
                             s.slot_moved)
               : std::string()));
  std::string draw_fields, end_fields;
  for (uint32_t f = 0; f < fl::kFieldCount; ++f) {
    if (s.draw_fields[f])
      draw_fields += fmt::format(" {} {};", fl::kFieldNames[f], s.draw_fields[f]);
    if (s.end_fields[f])
      end_fields += fmt::format(" {} {};", fl::kFieldNames[f], s.end_fields[f]);
  }
  REXGPU_INFO(
      "FH1 native lists: draws differing in:{} | list ends differing in:{} | templates {} ({} "
      "words; defines {}, repeats {}, invalid {}, unreadable {}, hash {}, past the cap {}), "
      "bound {} (binds {}, "
      "same {}, replaced {}, to an unknown template {}), drops {} ({} unknown), malformed {}; "
      "compare {:.2f} us a run (begin {:.2f}, draws {:.2f}, end {:.2f}), {:.3f} ms a frame, "
      "packets {:.3f} ms in all",
      draw_fields.empty() ? " none" : draw_fields, end_fields.empty() ? " none" : end_fields,
      s.registry.templates(), s.template_words, s.defines, s.define_repeats, s.define_invalid,
      s.define_unreadable, s.define_hash, s.define_capped, s.registry.bindings(), s.binds,
      s.bind_same, s.bind_replaced, s.bind_no_template, s.drops, s.drop_unknown, s.malformed,
      double(s.ns_begin + s.ns_draw + s.ns_end) * per_run, double(s.ns_begin) * per_run,
      double(s.ns_draw) * per_run, double(s.ns_end) * per_run,
      frames ? double(s.ns_begin + s.ns_draw + s.ns_end) * 1e-6 / double(frames) : 0.0,
      double(s.ns_packets) * 1e-6);
  std::string groups;
  for (uint32_t g = 0; g < kGroups; ++g) {
    const GroupCounts& c = s.groups[g];
    if (!c.runs)
      continue;
    groups += fmt::format(" {}: runs {} compared {} agree {} draws {}/{}",
                          g == 21 ? "?" : fmt::format("{}", g), c.runs, c.compared, c.agree,
                          c.draws_agree, c.draws);
    for (size_t k = 1; k < size_t(fl::Skip::kCount); ++k)
      if (c.skips[k])
        groups += fmt::format(" {} {}", fl::SkipName(fl::Skip(k)), c.skips[k]);
    groups += ";";
  }
  REXGPU_INFO("FH1 native lists by pass group:{}", groups.empty() ? " none" : groups);
  if (s.stale_binds) REXGPU_INFO("FH1 native lists: {} stale binds ignored", s.stale_binds);
}

}  // namespace

void CommandProcessor::Fh1ListPacket(const uint32_t* words, uint32_t count) {
  if (!fh1_lists_) {
    fh1_lists_ = new Fh1Lists;
#if REX_PLATFORM_WIN32
    if (const char* e = std::getenv("FH1_M4_BINDING_AUDIT")) {
#if REX_HAS_VULKAN
      fh1_lists_->timed_replay = std::strcmp(e, "timed-replay") == 0;
      const char* reuse = std::getenv("FH1_M4_REUSE_SNAPSHOT");
      fh1_lists_->repeat_capture = fh1_lists_->timed_replay && reuse &&
                                  (std::strcmp(reuse, "0") == 0 || std::strcmp(reuse, "1") == 0);
      fh1_lists_->reuse_snapshot = fh1_lists_->repeat_capture && reuse[0] == '1';
      fh1_lists_->sustained_replay = fl::SustainedReplayRequested();
      const char* parity = std::getenv("FH1_M4_REPLAY_PIXEL_PARITY");
      fh1_lists_->timed_texture_parity =
          fh1_lists_->timed_replay && parity && std::strcmp(parity, "texture") == 0;
      fh1_lists_->timed_pixel_parity =
          fh1_lists_->timed_texture_parity ||
          (fh1_lists_->timed_replay && parity && std::strcmp(parity, "1") == 0);
      // Let the pixel audit see several bindings rather than spending the
      // whole sample on one repeating attachment configuration.
      if (fh1_lists_->timed_pixel_parity) fh1_lists_->capture_visit_cap = 3;
      const char* retained = std::getenv("FH1_M4_PIXEL_RETAINED_ONLY");
      fh1_lists_->retained_pixel_only = fh1_lists_->timed_texture_parity &&
                                        fh1_lists_->reuse_snapshot && retained &&
                                        std::strcmp(retained, "1") == 0;
#endif
      fh1_lists_->audit_bind =
          std::strcmp(e, "bind") == 0 || std::strcmp(e, "bind-vertex") == 0 ||
          std::strcmp(e, "bind-texture") == 0 || std::strcmp(e, "timed-replay") == 0;
      fh1_lists_->audit_upload = fh1_lists_->audit_bind || std::strcmp(e, "upload") == 0;
      fh1_lists_->audit_capture = fh1_lists_->audit_upload || std::strcmp(e, "capture") == 0;
      fh1_lists_->audit_binding = fh1_lists_->audit_capture || std::strcmp(e, "1") == 0;
    }
#endif
    if (const char* e = std::getenv("FH1_NATIVE_LISTS_PERTURB"))
      fh1_lists_->perturb_every = std::strtoull(e, nullptr, 10);
    if (const char* e = std::getenv("FH1_NATIVE_LISTS_PERTURB_TILE"))
      fh1_lists_->perturb_tile_every = std::strtoull(e, nullptr, 10);
    if (const char* e = std::getenv("FH1_NATIVE_LISTS_PERTURB_SLOT"))
      fh1_lists_->perturb_slot_every = std::strtoull(e, nullptr, 10);
    REXGPU_INFO(
        "FH1 native lists: the game registers its model lists; each run of one is compared "
        "with its template (shadow mode: every buffer is decoded as before)");
  }
  auto& s = *fh1_lists_;
  const uint64_t t0 = NowNs();
  switch (words[1]) {
    case fh::kOpListDefine: {
      if (count < fh::kListDefineWords) {
        ++s.malformed;
        break;
      }
      const uint32_t id = words[2], address = words[3], n = words[4], sequence = words[5],
                     hash = words[6], ack = words[7];
      if (!Plausible(ack) || ack % fl::kDefineAckAlignment != 0) {
        ++s.malformed;
        break;
      }
      // A replay may name storage the game has already reused, even if the first decode
      // rejected the template or hit the cap and never registered its id.
      if (!fl::IsNewDefineSequence(sequence, s.last_ack)) {
        ++s.define_repeats;
        break;
      }
      if (s.registry.FindTemplate(id)) {
        ++s.define_repeats;
      } else if (s.template_words + n > kTemplateWordsCap) {
        ++s.define_capped;
      } else {
        const auto* blob = n && n <= (8u << 20) && Plausible(address) && Plausible(address + n * 4)
                               ? memory_->TranslateVirtual<const uint8_t*>(address)
                               : nullptr;
        if (!blob) {
          ++s.define_unreadable;
        } else {
          s.host_words.resize(n);
          fl::Hasher h;
          for (uint32_t i = 0; i < n; ++i) {
            s.host_words[i] = fl::Word(blob, i);
            h.Add(s.host_words[i]);
          }
          const bool hash_ok = h.Finish().w[0] == hash;
          if (hash_ok && s.perturb_slot_every && ++s.slot_defines % s.perturb_slot_every == 0 &&
              fl::Validate(s.host_words.data(), n)) {
            uint32_t* t = s.host_words.data();
            uint32_t* slot = t + t[fl::kTSlots];
            for (uint32_t i = 0; i < t[fl::kTSlotCount]; ++i, slot += fl::kSlotWords) {
              const bool room =
                  i + 1 == t[fl::kTSlotCount] || slot[fl::kSlotWords + 1] > slot[1] + 1;
              if (slot[3] == 1 && room && slot[1] + 1 < t[fl::kTListWords]) {
                ++slot[1];
                if (fl::Validate(t, n)) {
                  ++s.slot_moved;
                } else {
                  --slot[1];
                }
                break;
              }
            }
          }
          if (!hash_ok) {
            ++s.define_hash;
          } else if (s.registry.Define(id, s.host_words.data(), n) == fl::DefineResult::kNew) {
            ++s.defines;
            s.template_words += n;
          } else {
            ++s.define_invalid;
          }
        }
      }
      fl::PublishDefineAck(*memory_->TranslateVirtual<uint32_t*>(ack), sequence);
      s.last_ack = sequence;
      break;
    }
    case fh::kOpListBind: {
      if (count < fh::kListBindWords) {
        ++s.malformed;
        break;
      }
      fl::Binding b;
      b.phys = words[2] & 0x1FFFFFFFu;
      b.words = words[3];
      b.template_id = words[4];
      b.refusal = words[5];
      b.state_word = words[6];
      b.cb = words[7];
      b.patch = words[8];
      b.patch_words = words[9];
      b.group = words[10];
      b.flags = words[11];
      if (count == fh::kListBindLifetimeWords)
        b.producer_lifetime = uint64_t(words[13]) << 32 | words[12];
      if (b.template_id != fh::kListRefused && !s.registry.FindTemplate(b.template_id))
        ++s.bind_no_template;
      switch (s.registry.Bind(b)) {
        case fl::BindResult::kNew:
          ++s.binds;
          break;
        case fl::BindResult::kSame:
          ++s.bind_same;
          break;
        case fl::BindResult::kReplaced:
          ++s.bind_replaced;
#if REX_PLATFORM_WIN32
          s.RetireSustainedBinding(b.phys);
#endif
          break;
        case fl::BindResult::kStale:
          ++s.stale_binds;
          break;
      }
      break;
    }
    case fh::kOpListDrop: {
      if (count < fh::kListDropWords) {
        ++s.malformed;
        break;
      }
      const uint64_t lifetime =
          count == fh::kListDropLifetimeWords ? uint64_t(words[5]) << 32 | words[4] : 0;
      if (count == fh::kListDropLifetimeWords && !lifetime) {
        ++s.malformed;
        break;
      }
      const uint32_t phys = words[2] & 0x1FFFFFFFu;
      if (s.registry.Drop(phys, lifetime)) {
        ++s.drops;
#if REX_PLATFORM_WIN32
        s.RetireSustainedBinding(phys, lifetime);
#endif
      } else {
        ++s.drop_unknown;  // already dropped (a replayed packet) or never bound
      }
      break;
    }
    default:
      ++s.malformed;
      break;
  }
  s.ns_packets += NowNs() - t0;
}

bool CommandProcessor::Fh1ListBegin(uint32_t ptr, uint32_t count, memory::RingBuffer* reader) {
  auto& s = *fh1_lists_;
#if REX_PLATFORM_WIN32 && REX_HAS_VULKAN
  std::shared_ptr<const fl::ReplayPlan> bind_plan;
  const bool timed_replay = s.timed_replay;
  bool measuring = !timed_replay;
  if (timed_replay && (s.sustained_replay || s.audit_attempts < 64)) {
    using Measuring = bool (*)() noexcept;
    static const auto race_measuring = reinterpret_cast<Measuring>(
        GetProcAddress(GetModuleHandleW(nullptr), "ForzaM4BenchmarkMeasuringV1"));
    measuring = race_measuring && race_measuring();
    if (measuring && s.timed_texture_parity) {
      const RegisterFile& regs = record_split_ ? *decode_register_file_ : *register_file_;
      measuring = regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kColorDepth &&
                  regs.Get<reg::RB_COLOR_MASK>().value != 0;
    }
  }
#else
  constexpr bool measuring = true;
#endif
  const uint64_t t0 = NowNs();
  fl::Skip skip;
  fl::Binding* b = s.registry.Lookup(ptr & 0x1FFFFFFFu, count, &skip);
  if (!b) {
    if (skip == fl::Skip::kLength) {
      ++s.groups[21].runs;
      ++s.groups[21].skips[size_t(skip)];
    } else if (s.registry.WasBound(ptr & 0x1FFFFFFFu)) {
      ++s.dropped_address_misses;
    } else {
      ++s.other_buffers;
    }
    return false;
  }
  GroupCounts& g = s.groups[GroupIndex(b->group)];
  ++g.runs;
  const auto no = [&](fl::Skip why) {
    ++g.skips[size_t(why)];
    return false;
  };
  if (fh1_list_reader_)
    return no(fl::Skip::kNested);
  if (b->template_id == fh::kListRefused) {
    ++s.refusals[std::min<uint32_t>(b->refusal, kRefusals - 1)];
    return no(fl::Skip::kRefused);
  }
  const fl::Template* tmpl = s.registry.FindTemplate(b->template_id);
  if (!tmpl)
    return no(fl::Skip::kNoTemplate);
#if REX_PLATFORM_WIN32
  if (s.repeat_capture && s.repeat_capture_frame != s.frames) {
    s.repeat_capture_frame = s.frames;
    s.repeat_captures_this_frame = 0;
  }
  if (s.audit_binding && measuring && (s.sustained_replay || s.audit_attempts < 64) &&
      b->producer_lifetime && (!s.repeat_capture || s.repeat_captures_this_frame < 3)) {
    auto& tested = s.audited_lifetimes;
    const auto previous = std::ranges::find(tested, b->producer_lifetime);
    const auto slot = previous == tested.end() ? std::ranges::find(tested, 0) : previous;
    if (slot != tested.end() &&
        (previous == tested.end() ||
         (s.repeat_capture && (s.sustained_replay ||
                               s.capture_visits[slot - tested.begin()] < s.capture_visit_cap)))) {
      if (s.repeat_capture) ++s.repeat_captures_this_frame;
      using Visit = bool (*)(void*, uint32_t) noexcept;
      using WithBinding = bool (*)(const uint32_t*, uint32_t, Visit, void*) noexcept;
      static const auto with_binding = reinterpret_cast<WithBinding>(
          GetProcAddress(GetModuleHandleW(nullptr), "ForzaM4WithBindingV1"));
      ++s.audit_attempts;
      if (with_binding) {
        uint32_t query[fh::kListBindLifetimeWords] = {
            fh::kMagic, fh::kOpListBind, b->phys, b->words, b->template_id, b->refusal,
            b->state_word, b->cb, b->patch, b->patch_words, b->group, b->flags,
            uint32_t(b->producer_lifetime), uint32_t(b->producer_lifetime >> 32)};
        uint32_t generation = 0;
        const auto capture = [](void* out, uint32_t value) noexcept {
          *static_cast<uint32_t*>(out) = value;
          return true;
        };
        if (with_binding(query, fh::kListBindLifetimeWords, capture, &generation)) {
          *slot = b->producer_lifetime;
          const size_t slot_index = slot - tested.begin();
          if (s.sustained_replay) {
            s.sustained_phys[slot_index] = b->phys;
          } else {
            ++s.capture_visits[slot_index];
          }
          if (s.audit_capture) {
            LiveCaptureProbe probe{*memory_, *b, *tmpl};
            if (s.reuse_snapshot) probe.retained = &s.retained_replays[slot_index];
            const uint64_t started = NowNs();
            const bool captured =
                with_binding(query, fh::kListBindLifetimeWords, ProbeLiveCapture, &probe);
            const uint64_t duration = NowNs() - started;
            if (s.sustained_replay) {
              s.sustained_capture_ns += duration;
              s.sustained_reused += captured && probe.retained_hit;
              s.sustained_captured += captured && !probe.retained_hit;
            } else {
              REXGPU_INFO(
                  "M4 capture audit: phys={:08X} lifetime={} status={} ranges={} inputs={} "
                  "index_draws={} index_bytes={} last_space={} last_range={:08X} bytes={} "
                  "compared_bytes={} duration_ns={}",
                  b->phys, b->producer_lifetime, probe.status, probe.copied, probe.input_bytes,
                  probe.index_draws, probe.index_bytes,
                  probe.space == fl::ReplayPlan::AddressSpace::kVirtual ? "virtual" : "physical",
                  probe.address, probe.range_bytes, probe.compared_bytes, duration);
            }
#if REX_HAS_VULKAN
            // A fresh plan can populate the cache without consuming a
            // retained-only pixel comparison on that first execution.
            if (captured && s.audit_upload && (!s.retained_pixel_only || probe.retained_hit)) {
              if (auto* vk_cp = dynamic_cast<vulkan::VulkanCommandProcessor*>(this)) {
                if (s.audit_bind) {
                  bind_plan = std::move(probe.plan);
                } else {
                  RecordCall([vk_cp, plan = std::move(probe.plan)] {
                    vk_cp->AuditFh1OwnedIndices(*plan, false);
                  });
                }
              }
            }
#else
            (void)captured;
#endif
          }
          if (!s.sustained_replay || !std::exchange(s.sustained_checked_lifetime, true)) {
            query[13] ^= 0x80000000u;
            const bool wrong = with_binding(query, fh::kListBindLifetimeWords, capture, &generation);
            REXGPU_INFO("M4 binding audit: phys={:08X} lifetime={} generation={} wrong_lifetime={}",
                        b->phys, b->producer_lifetime, generation, wrong);
          }
        }
      }
    }
  }
#endif
  const uint32_t* t = tmpl->words.data();
  constexpr auto generation_alignment = std::atomic_ref<uint32_t>::required_alignment;
  auto* state = Plausible(b->state_word)
                    ? memory_->TranslateVirtual<uint32_t*>(b->state_word)
                    : nullptr;
  if (state && reinterpret_cast<uintptr_t>(state) % generation_alignment)
    return no(fl::Skip::kFixupInProgress);
  const auto load_generation = [state]() {
    return state ? __builtin_bswap32(std::atomic_ref<uint32_t>(*state).load(
                       std::memory_order_acquire))
                 : 0u;
  };
  const auto generation_changed = [&]() {
    ++s.in_progress;
    b->pool_valid = false;
    return no(fl::Skip::kFixupInProgress);
  };
  const uint32_t generation = load_generation();
  if (generation & 1) return generation_changed();

  // The command buffer still names this buffer (its first segment: +116 the chain, then the
  // count, the words and the physical address).
  if (!Plausible(b->cb) || !Plausible(b->patch) || b->patch_words > (1u << 16))
    return no(fl::Skip::kRebound);
  const auto* header = memory_->TranslateVirtual<const uint8_t*>(b->cb);
  const uint32_t chain = fl::Word(header, 116 / 4);
  const auto* link = Plausible(chain) ? memory_->TranslateVirtual<const uint8_t*>(chain) : nullptr;
  if (!link || fl::Word(link, 1) != 1 || (fl::Word(link, 2) & 0xFFFFFF) != count ||
      (fl::Word(link, 3) & 0x1FFFFFFFu) != b->phys) {
    return no(fl::Skip::kRebound);
  }
  const auto* pm4 = memory_->TranslatePhysical<const uint8_t*>(b->phys);
  const auto* patch = memory_->TranslateVirtual<const uint8_t*>(b->patch);
  const bool untiled = fl::Untiled(t, pm4);
  const fl::Key key =
      fl::LiveKey(t, header, fl::FilesFormFlags(fl::Word(header, fl::kHeaderFlags / 4)), patch,
                  b->patch_words, pm4, count);
  const bool differs = key != fl::TemplateKey(t);
  ++s.key_checks[untiled][differs];
  const fl::Skip key_skip = fl::KeyCheck(b, differs);
  if (key_skip != fl::Skip::kNone) {
    if (s.detailed < 32) {
      ++s.detailed;
      REXGPU_INFO(
          "FH1 native lists: list {:08X} (group {}, {} dwords{}) has another key {}, decoded "
          "as before",
          b->phys, b->group, count, untiled ? ", untiled" : "",
          key_skip == fl::Skip::kKeyAtBind
              ? "than its template at its first check since the bind: the template is wrong"
              : "now: its static words changed after a run that had the template's");
    }
    return no(key_skip);
  }
  // The pool: read again when the generation changed, as a replay would; read every run too, to
  // find a slot that changed without a fixup.
  // As a seqlock's reader (the game's fixup hook is the writer): the generation, the slots, the
  // generation again; odd, or changed in between, is a fixup under way.
  const uint32_t pool_words = t[fl::kTPoolWords];
  s.fresh_pool.resize(pool_words);
  fl::Resolve(t, pm4, s.fresh_pool.data());
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint32_t generation_after = load_generation();
  if (generation_after != generation) {
    // The rejected snapshot cannot establish agreement with the packet decoder.
    return generation_changed();
  } else if (b->pool_valid && b->pool_generation == generation) {
    if (b->pool != s.fresh_pool)
      ++s.stale_pools;
    s.pool = b->pool;
  } else {
    if (b->pool_valid)
      ++s.generation_changes;
    b->pool = s.fresh_pool;
    b->pool_valid = true;
    b->pool_generation = generation;
    s.pool = s.fresh_pool;
  }
  // FH1_NATIVE_LISTS_PERTURB=N (a check of the check): every Nth compared run whose first draw
  // has a register written before it starts from a pool with that value changed, so the run has
  // to disagree.
  if (s.perturb_every && ++s.perturb_count % s.perturb_every == 0 && t[fl::kTDrawCount]) {
    const uint32_t* r = fl::DrawRecord(t, 0);
    const uint32_t* e = t + t[fl::kTDeltas] + r[fl::kDDelta] * fl::kDeltaWords;
    for (uint32_t i = 0; i < r[fl::kDDeltaCount]; ++i, e += fl::kDeltaWords) {
      const uint32_t* op = fl::Op(t, e[2]);
      if (e[0] >= 0x2000 && !fl::MarkRegister(e[0]) && (op[0] & 0xFF) == fl::kOpRegs &&
          !(untiled && (op[0] & fl::kOpTileBound))) {
        s.pool[op[3] + e[3]] ^= 0x00010000u;
        ++s.perturbed;
        break;
      }
    }
  }
  if (s.perturb_tile_every && !untiled && (t[fl::kTFlags] & fl::kTemplateTiled) &&
      ++s.perturb_tile_count % s.perturb_tile_every == 0) {
    for (uint32_t k = 0; k <= t[fl::kTDrawCount]; ++k) {
      const uint32_t* r = fl::DrawRecord(t, k);
      if (r[fl::kDTile] == fl::kNone)
        continue;
      uint32_t* tile = s.pool.data() + fl::Op(t, r[fl::kDTile])[1];
      const uint32_t first = tile[0];
      tile[0] = tile[4];
      ++s.tile_perturbed;
      if ((bin_select_ & first) && !(bin_select_ & tile[4]) && !(bin_select_ & tile[8]))
        ++s.tile_effective;
      break;
    }
  }
  const RegisterFile& regs = record_split_ ? *decode_register_file_ : *register_file_;
  s.run.Begin(t, s.pool.data(), bin_select_, bin_mask_, untiled, regs.values, s.shaders, Physical,
              memory_);
  s.binding = b;
  s.group = GroupIndex(b->group);
  s.phys = b->phys;
  s.untiled = untiled;
  s.run_agree = true;
  s.run_fields = 0;
  ++g.compared;
  if (untiled)
    ++g.untiled_runs;
  fh1_list_reader_ = reader;
#if REX_PLATFORM_WIN32 && REX_HAS_VULKAN
  if (bind_plan) {
    auto* vk_cp = static_cast<vulkan::VulkanCommandProcessor*>(this);
    if (timed_replay) {
      if (bind_plan->captured_generation() == generation && s.pool == s.fresh_pool &&
          !s.perturb_every && !s.perturb_tile_every && !s.perturb_slot_every) {
        std::vector<uint32_t> sealed_pool(pool_words);
        fl::Resolve(t, reinterpret_cast<const uint8_t*>(bind_plan->guest_words().data()),
                    sealed_pool.data());
        if (sealed_pool == s.fresh_pool) {
          s.timed_plan = std::move(bind_plan);
        }
      }
    } else {
      s.index_audit_cp = vk_cp;
      RecordCall([vk_cp, plan = std::move(bind_plan)] {
        vk_cp->AuditFh1OwnedIndices(*plan, true);
      });
    }
  }
#endif
  s.ns_begin += NowNs() - t0;
  return true;
}

bool CommandProcessor::TryFh1ReplayOwnedList(bool& success) {
#if REX_PLATFORM_WIN32 && REX_HAS_VULKAN
  auto& s = *fh1_lists_;
  const auto plan = std::exchange(s.timed_plan, {});
  if (!plan || fh1_stereo_state_ != Fh1StereoState::kOff || Fh1StereoWanted() ||
      !plan->IsCurrentPublication(s.registry)) return false;
  const auto lifetime = plan->producer_lifetime();
  if (!std::exchange(s.refused_replay_control, true)) {
    REXGPU_INFO("M4 timed list fallback: lifetime={} reason=control", lifetime);
    return false;
  }
  auto* vk_cp = static_cast<vulkan::VulkanCommandProcessor*>(this);
  s.index_audit_cp = vk_cp;
  RecordCall([vk_cp, plan] { vk_cp->AuditFh1OwnedIndices(*plan, true); });
  const uint64_t started = NowNs();
  success = Fh1RecordOwnedList(*plan);
  if (s.sustained_replay) {
    if (!s.sustained_first_ns) s.sustained_first_ns = started;
    s.sustained_last_ns = NowNs();
    s.sustained_lists += success;
    s.sustained_failed += !success;
    if (success) s.sustained_operations += plan->operations().size();
  } else {
    REXGPU_INFO("M4 timed list replay: lifetime={} generation={} operations={} success={} dispatch_ns={}",
                lifetime, *plan->captured_generation(), plan->operations().size(), success,
                NowNs() - started);
  }
  return true;
#else
  (void)success;
  return false;
#endif
}

bool CommandProcessor::Fh1RecordOwnedList(const fl::ReplayPlan& plan) {
  const auto words = plan.guest_words();
  // RingBuffer's constructor takes a writable pointer. The selected PM4
  // handlers only read this backing and advance the cursor.
  auto* data = reinterpret_cast<uint8_t*>(const_cast<uint32_t*>(words.data()));
  memory::RingBuffer reader(data, words.size_bytes());
  reader.set_write_offset(words.size_bytes());
  fh1_command_cursor::Scope cursor_scope(&reader);
  auto* const stock_reader = std::exchange(fh1_list_reader_, &reader);
  for (const auto& op : plan.operations()) {
    reader.set_read_offset_within(op.first_word * sizeof(uint32_t));
    const auto input = plan.input_words(op);
    bool ok = true;
    const uint32_t opcode = (op.header >> 8) & 0x7f;
    if (op.kind != fl::ReplayPlan::Kind::kType3) {
      ok = ExecutePacketType0(&reader, op.header);
    } else if ((op.header & 1) && !(bin_select_ & bin_mask_)) {
      if (opcode == 0x22) Fh1ListDraw(op.first_word * sizeof(uint32_t), false);
      reader.AdvanceRead(op.count * sizeof(uint32_t));
    } else if (opcode == 0x27 && !input.empty()) {  // IM_LOAD
      const uint32_t addr_type = reader.ReadAndSwap<uint32_t>();
      const uint32_t start_size = reader.ReadAndSwap<uint32_t>();
      const auto shader_type = static_cast<xenos::ShaderType>(addr_type & 3);
      if (fh1_lists_) Fh1ListShader(addr_type, start_size);
      if (record_split_) {
        // The existing immediate-shader record owns its payload through the
        // recorder's last read, even after the replay plan is destroyed.
        uint32_t* record = RecordWords(3 + uint32_t(input.size()));
        record[0] = kRecordShaderImmediate;
        record[1] = uint32_t(shader_type);
        record[2] = uint32_t(input.size());
        std::memcpy(record + 3, input.data(), input.size_bytes());
      } else {
        auto* shader = LoadShader(shader_type, addr_type & ~3u, input.data(), uint32_t(input.size()));
        (shader_type == xenos::ShaderType::kVertex ? active_vertex_shader_ : active_pixel_shader_) =
            shader;
      }
    } else if (opcode == 0x2f && !input.empty()) {
      // LOAD_ALU_CONSTANT. Preparation checks the literal count and ALU type.
      reader.AdvanceRead(sizeof(uint32_t));
      const uint32_t index = reader.ReadAndSwap<uint32_t>() & 0x7ff;
      reader.AdvanceRead(sizeof(uint32_t));
      PacketWriteRegistersFromMem(fh::kAluFirstRegister + index, input.data(),
                                  uint32_t(input.size()));
    } else {
      switch (opcode) {
        case 0x10:  // NOP without a host packet.
          reader.AdvanceRead(op.count * sizeof(uint32_t));
          break;
        case 0x22:  // DRAW_INDX.
          Fh1ListDraw(op.first_word * sizeof(uint32_t), true);
          ok = ExecutePacketType3_DRAW_INDX(&reader, op.header, op.count);
          break;
        case 0x2d:  // The sealed tile-window register bundle.
          ok = ExecutePacketType3_SET_CONSTANT(&reader, op.header, op.count);
          break;
        case 0x60:
          bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | reader.ReadAndSwap<uint32_t>();
          break;
        case 0x61:
          bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) |
                      (uint64_t(reader.ReadAndSwap<uint32_t>()) << 32);
          break;
        default:
          ok = false;
          break;
      }
    }
    // Dispatch has started and may have changed emulated state. The caller
    // reports an execution failure instead of restarting on the stock path.
    const size_t end = (op.first_word + op.count) * sizeof(uint32_t);
    const size_t next = end == words.size_bytes() ? 0 : end;
    if (!ok || reader.read_offset() != next) {
      fh1_list_reader_ = stock_reader;
      return false;
    }
  }
  fh1_list_reader_ = stock_reader;
  return true;
}

void CommandProcessor::Fh1ListDraw(uint32_t packet_offset, bool executed) {
  auto& s = *fh1_lists_;
#if REX_PLATFORM_WIN32 && REX_HAS_VULKAN
  if (auto* vk_cp = s.index_audit_cp) {
    RecordCall([vk_cp, offset = executed ? packet_offset : UINT32_MAX] {
      vk_cp->SelectFh1IndexAuditDraw(offset);
    });
  }
#endif
  const uint64_t t0 = NowNs();
  const RegisterFile& regs = record_split_ ? *decode_register_file_ : *register_file_;
  const uint32_t k = s.run.next_draw();
  const fl::Outcome o =
      s.run.Draw(packet_offset / 4 - 1, executed, regs.values, bin_mask_, s.shaders);
  GroupCounts& g = s.groups[s.group];
  ++g.draws;
  if (!executed)
    ++g.draws_skipped;
  if (o.agree) {
    ++g.draws_agree;
  } else {
    s.run_agree = false;
    for (uint32_t f = 0; f < fl::kFieldCount; ++f)
      if (o.fields >> f & 1)
        ++s.draw_fields[f];
    if (s.detailed < 32) {
      ++s.detailed;
      std::string fields;
      for (uint32_t f = 0; f < fl::kFieldCount; ++f)
        if (o.fields >> f & 1)
          fields += fmt::format(" {};", fl::kFieldNames[f]);
      REXGPU_INFO(
          "FH1 native lists: list {:08X} (group {}{}) draw {} of {} ({}, select {:X} mask {:X}) "
          "disagrees:{} first register {:04X}: template {:08X}, packets {:08X}",
          s.phys, s.group, s.untiled ? ", untiled" : "", k, s.run.draw_count(),
          executed ? "runs" : "predicated off", bin_select_, bin_mask_, fields,
          o.reg == fl::kNone ? 0 : o.reg, o.expected, o.live);
    }
  }
  s.ns_draw += NowNs() - t0;
}

void CommandProcessor::Fh1ListNoteWrites(uint32_t first, uint32_t count) {
  fh1_lists_->run.LiveWrites(first, count);
}

// The write paths' cold halves, with a run open: PacketWriteRegister's unsplit path tail-calls
// the first (so that path needs no stack frame), Fh1WritePlainRange's call sites call the second
// (so Fh1WritePlainRangeFromRing stays main's).
void CommandProcessor::Fh1ListWriteRegister(uint32_t index, uint32_t value) {
  fh1_lists_->run.LiveWrites(index, 1);
  WriteRegister(index, value);
}

bool CommandProcessor::Fh1ListWritePlainRange(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t count) {
  if (!Fh1WritePlainRangeFromRing(ring, base, count)) {
    return false;
  }
  fh1_lists_->run.LiveWrites(base, count);
  return true;
}

void CommandProcessor::Fh1ListEnd() {
  auto& s = *fh1_lists_;
#if REX_PLATFORM_WIN32 && REX_HAS_VULKAN
  s.timed_plan.reset();
  if (auto* vk_cp = std::exchange(s.index_audit_cp, nullptr)) {
    RecordCall([vk_cp] { vk_cp->EndFh1IndexAudit(); });
  }
#endif
  const uint64_t t0 = NowNs();
  fh1_list_reader_ = nullptr;
  const RegisterFile& regs = record_split_ ? *decode_register_file_ : *register_file_;
  const fl::Outcome o = s.run.End(regs.values, bin_mask_, s.shaders);
  GroupCounts& g = s.groups[s.group];
  if (!o.agree) {
    for (uint32_t f = 0; f < fl::kFieldCount; ++f)
      if (o.fields >> f & 1)
        ++s.end_fields[f];
    if (s.detailed < 32) {
      ++s.detailed;
      std::string fields;
      for (uint32_t f = 0; f < fl::kFieldCount; ++f)
        if (o.fields >> f & 1)
          fields += fmt::format(" {};", fl::kFieldNames[f]);
      REXGPU_INFO(
          "FH1 native lists: list {:08X} (group {}{}) end ({} of {} draws seen) disagrees:{} "
          "first register {:04X}: template {:08X}, packets {:08X}",
          s.phys, s.group, s.untiled ? ", untiled" : "", s.run.next_draw(), s.run.draw_count(),
          fields, o.reg == fl::kNone ? 0 : o.reg, o.expected, o.live);
    }
  }
  if (o.agree && s.run_agree)
    ++g.agree;
  s.binding = nullptr;
  s.ns_end += NowNs() - t0;
}

void CommandProcessor::Fh1ListsSwap() {
  auto& s = *fh1_lists_;
  ++s.frames;
#if REX_PLATFORM_WIN32 && REX_HAS_VULKAN
  if (s.timed_pixel_parity && s.audit_attempts && s.frames % 240 == 0 &&
      s.pixel_parity_sampling_passes < 7) {
    s.audit_attempts = 0;
    s.audited_lifetimes.fill(0);
    s.retained_replays.fill({});
    s.capture_visits.fill(0);
    ++s.pixel_parity_sampling_passes;
  }
#endif
  if (s.frames % 600 == 0)
    LogLists(s, "so far", s.frames);
}

void CommandProcessor::Fh1ListShader(uint32_t address_type, uint32_t start_size) {
  auto& s = *fh1_lists_;
  const uint32_t stage = address_type & 1;  // 0 vertex, 1 pixel
  s.shaders.address[stage] = address_type;
  s.shaders.size[stage] = start_size;
  s.shaders.valid[stage] = true;
}

void CommandProcessor::Fh1ListsShutdown() {
  if (!fh1_lists_)
    return;
  LogLists(*fh1_lists_, "at exit", fh1_lists_->frames);
  delete fh1_lists_;
  fh1_lists_ = nullptr;
}

}  // namespace rex::graphics
