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
//   (as a replay would), and also on every run, to count a slot written without a fixup; the
//   generation is read again after the buffer, and a run a fixup overlapped gets no verdict;
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

#include <rex/assert.h>

#include <rex/graphics/fh1_host_packet.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "fh1_native_list.h"

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

// 0660 (verify mode). Why a run at a registered address is not verified (it is decoded as
// always).
enum VerifySkip : uint32_t {
  kVerifyLength,           // registered with another length
  kVerifyRefused,          // the game's converter refused the list
  kVerifyNoTemplate,       // bound to a template the plugin does not have
  kVerifyStereo,           // stereo is on (the packets are logged for the second eye)
  kVerifyNestedList,       // an indirect buffer inside a verified run
  kVerifyRebound,          // before its template's program: the command buffer names another
  kVerifyKey,              // before its template's program: the key is not the template's
  kVerifyNoProgram,        // the template's program could not be compiled
  kVerifyFixupInProgress,  // the generation is odd
  kVerifyNestedFixup,      // a fixup of the list ran inside another (kListStateNested)
  kVerifyFixupDuringRun,   // the generation moved during the run: no verdict, no time
  kVerifySkipCount,
};
constexpr const char* kVerifySkipNames[kVerifySkipCount] = {
    "length", "refused",    "no-template",       "stereo",       "nested-list",     "rebound",
    "key",    "no-program", "fixup-in-progress", "nested-fixup", "fixup-during-run"};
// What a verified run's replay left that differs from what the packets left.
enum VerifyField : uint32_t {
  kVerifyRecordWords,  // split: the record words, byte for byte
  kVerifyRegisters,    // the registers the program writes
  kVerifyCounters,     // split: the registers handed in and recorded (record_registers_in/out)
  kVerifyStores,       // split: the scratch write-backs noted for WAIT_REG_MEM
  kVerifyBinMask,      // the bin mask
  kVerifyShaders,      // the IM_LOAD words, and unsplit the shaders loaded
  kVerifyFull,         // the sampled compare of the whole register file from 0x2000
  kVerifyDraws,        // unsplit, sampled: the program's registers at each draw, and whether it ran
  kVerifyFieldCount,
};
constexpr const char* kVerifyFieldNames[kVerifyFieldCount] = {
    "record words", "registers", "record counters",     "noted stores",
    "bin mask",     "shaders",   "whole register file", "draws"};
// The runs timed, by which went first: [0] the packets, [1] the replay.
struct VerifyTiming {
  uint64_t runs = 0, ops = 0, dwords = 0, draws = 0, ns_stock = 0, ns_replay = 0;
};
// The capture's copies (Fh1ListPublishing) are left out of the packets' time; the runs in which
// the packets published a batch (a lock, a notify, at times a wait for the recorder: work a
// replay that records would do the same way, but this one does not) are also timed apart.

// The command buffer still names this buffer (its first segment: +116 the chain, then the
// count, the words and the physical address): its header, or null.
const uint8_t* NamedHeader(memory::Memory* memory, const fl::Binding* b, uint32_t count) {
  if (!Plausible(b->cb) || !Plausible(b->patch) || b->patch_words > (1u << 16))
    return nullptr;
  const auto* header = memory->TranslateVirtual<const uint8_t*>(b->cb);
  const uint32_t chain = fl::Word(header, 116 / 4);
  const auto* link = Plausible(chain) ? memory->TranslateVirtual<const uint8_t*>(chain) : nullptr;
  if (!link || fl::Word(link, 1) != 1 || (fl::Word(link, 2) & 0xFFFFFF) != count ||
      (fl::Word(link, 3) & 0x1FFFFFFFu) != b->phys) {
    return nullptr;
  }
  return header;
}

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
  // The list's state word and its generation at the run's start (read again at its end).
  const uint8_t* state = nullptr;
  uint32_t generation = 0;
  // The run's draws, added to its group at its end unless the verdict is dropped.
  uint64_t run_draws = 0, run_draws_agree = 0, run_draws_skipped = 0;
  uint64_t run_draw_fields[fl::kFieldCount] = {};

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

  // 0660: what the runs of registered lists get (kOpListMode); kOff: refused, decoded as always.
  enum class Mode : uint8_t { kShadow, kVerify, kOff } mode = Mode::kShadow;
  // Verify mode.
  struct Verify {
    uint64_t runs = 0, agree = 0, full_checks = 0;
    uint64_t fields[kVerifyFieldCount] = {};
    uint64_t skips[kVerifySkipCount] = {};
    uint64_t handovers[size_t(fl::Handover::kCount)] = {};
    uint64_t compiles[size_t(fl::CompileFail::kCount)] = {};
    uint64_t other_buffers = 0, dropped_address_misses = 0;
    VerifyTiming timing[2], quiet[2];
    // Lookup and checks (what a replay needs before it starts), compiles, snapshot and compare.
    uint64_t ns_begin = 0, ns_compile = 0, ns_check = 0;
    double clock_ns = 0;  // one NowNs() pair, back to back
    uint64_t full_every = 64;
    // Self-checks: every Nth verified run, (header) one op's header in the program the replay
    // walks is another (the replay must hand over at that op), (value) the first range the
    // replay writes has its first value changed (the run must disagree), (tile) a tiled list's
    // first tile block gives tile 0 the second tile's mask (counted when that changes whether
    // its window is written).
    uint64_t perturb_header_every = 0, perturb_value_every = 0, perturb_tile_every = 0;
    uint64_t header_perturbed = 0, header_caught = 0, value_perturbed = 0, value_caught = 0,
             tile_perturbed = 0, tile_effective = 0, tile_caught = 0;
    uint64_t compared = 0;  // verified runs compared, but the self-checks' and the dropped
    // Unsplit, every Nth run (and every self-check's) also compares the state at each draw: the
    // packets then run with the draw seam open, so these runs are not timed.
    uint64_t draw_check_every = 8, draw_check_count = 0, draw_checked = 0;
    uint32_t detailed = 0;
  } v;
  std::unique_ptr<RegisterFile> scratch_regs;
  std::unique_ptr<RecordBatch> scratch_batch;
  // The packets' record words of the run being verified (kept at each publish), from
  // capture_from of the batch being recorded.
  std::vector<uint32_t> captured, scratch_draws, full_start, full_expected;
  size_t capture_from = 0;
  uint64_t capture_ns = 0;
  // Unsplit draw checks: per draw whether it ran, then the program's registers.
  std::vector<uint32_t> stock_draw_states, replay_draw_states;
  const std::vector<uint32_t>* draw_ranges = nullptr;
  // Unsplit draw checks also time each executed draw packet of the packets' run: from its seam to
  // the next seam after its own three register writes (the draw's issue, plus the next packet's
  // decode up to its first write; an IM_LOAD in between is in it too).
  uint64_t draw_span_start = 0, draw_span_ns = 0, draw_spans = 0;
  uint32_t draw_span_skip = 0;
  void CloseDrawSpan() {
    if (!draw_span_start)
      return;
    if (draw_span_skip) {
      --draw_span_skip;
      return;
    }
    draw_span_ns += NowNs() - draw_span_start;
    ++draw_spans;
    draw_span_start = 0;
  }
  bool verifying = false;
  fl::ReplayProgram perturbed_program;
};

namespace {

const uint8_t* Physical(void* context, uint32_t address) {
  return static_cast<const uint8_t*>(
      static_cast<memory::Memory*>(context)->TranslatePhysical(address));
}

void LogLists(const CommandProcessor::Fh1Lists& s, const char* when, uint64_t frames) {
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
}

}  // namespace

void CommandProcessor::Fh1ListPacket(const uint32_t* words, uint32_t count) {
  if (!fh1_lists_) {
    fh1_lists_ = new Fh1Lists;
    if (const char* e = std::getenv("FH1_NATIVE_LISTS_PERTURB"))
      fh1_lists_->perturb_every = std::strtoull(e, nullptr, 10);
    if (const char* e = std::getenv("FH1_NATIVE_LISTS_PERTURB_TILE"))
      fh1_lists_->perturb_tile_every = std::strtoull(e, nullptr, 10);
    if (const char* e = std::getenv("FH1_NATIVE_LISTS_PERTURB_SLOT"))
      fh1_lists_->perturb_slot_every = std::strtoull(e, nullptr, 10);
    REXGPU_INFO(
        "FH1 native lists: the game registers its model lists; each run of one is compared "
        "with its template (shadow mode: every buffer is decoded as before), unless the game "
        "asks for verify mode");
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
          break;
      }
      break;
    }
    case fh::kOpListMode: {
      if (count < fh::kListModeWords) {
        ++s.malformed;
        break;
      }
      if (words[2] != fh::kListModeVerify || s.mode == Fh1Lists::Mode::kVerify)
        break;
      // 0660: verify mode only on x86-64 (the game's fences are compiler barriers there), with
      // pages of 4 KB, and with the Vulkan backend (the one with the decode/record split; its
      // writers are the ones the replay calls).
      const char* refused = nullptr;
#if !REX_ARCH_AMD64
      refused = "not an x86-64 host";
#endif
      if (!refused && memory::page_size() > 4096)
        refused = "host pages above 4 KB";
      if (!refused && !SupportsRecordThread())
        refused = "a GPU backend without the decode/record split";
#if (defined(REX_FH1_LIST_TRACE) && REX_FH1_LIST_TRACE) || \
    (defined(REX_FH1_PM4_TRACE) && REX_FH1_PM4_TRACE)
      if (!refused)
        refused = "a trace build (its timers would hold the packets and the replay together)";
#endif
      if (refused) {
        s.mode = Fh1Lists::Mode::kOff;
        REXGPU_WARN("FH1 native lists: verify mode refused ({}): lists are decoded as always",
                    refused);
        break;
      }
      s.mode = Fh1Lists::Mode::kVerify;
      if (const char* e = std::getenv("FH1_NATIVE_LISTS_VERIFY_HEADER"))
        s.v.perturb_header_every = std::strtoull(e, nullptr, 10);
      if (const char* e = std::getenv("FH1_NATIVE_LISTS_VERIFY_VALUE"))
        s.v.perturb_value_every = std::strtoull(e, nullptr, 10);
      if (const char* e = std::getenv("FH1_NATIVE_LISTS_VERIFY_TILE"))
        s.v.perturb_tile_every = std::strtoull(e, nullptr, 10);
      if (const char* e = std::getenv("FH1_NATIVE_LISTS_VERIFY_DRAWS_EVERY"))
        s.v.draw_check_every = std::strtoull(e, nullptr, 10);
      if (const char* e = std::getenv("FH1_NATIVE_LISTS_VERIFY_FULL_EVERY"))
        s.v.full_every = std::strtoull(e, nullptr, 10);
      uint64_t best = ~uint64_t(0);
      for (int i = 0; i < 1000; ++i) {
        const uint64_t a = NowNs();
        best = std::min(best, NowNs() - a);
      }
      s.v.clock_ns = double(best);
      REXGPU_INFO(
          "FH1 native lists: verify mode (each run of a registered list is decoded as always and "
          "its template's replay runs beside it into scratch state, compared and timed; the "
          "whole register file every {} runs)",
          s.v.full_every);
      break;
    }
    case fh::kOpListDrop:
      if (count < fh::kListDropWords) {
        ++s.malformed;
        break;
      }
      if (s.registry.Drop(words[2] & 0x1FFFFFFFu))
        ++s.drops;
      else
        ++s.drop_unknown;  // already dropped (a replayed packet) or never bound
      break;
    default:
      ++s.malformed;
      break;
  }
  s.ns_packets += NowNs() - t0;
}

bool CommandProcessor::Fh1ListBegin(uint32_t ptr, uint32_t count, memory::RingBuffer* reader) {
  auto& s = *fh1_lists_;
  if (s.mode != Fh1Lists::Mode::kShadow)
    return false;  // 0660: verify mode (Fh1ListVerify), or refused
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
  const uint32_t* t = tmpl->words.data();
  const uint8_t* header = NamedHeader(memory_, b, count);
  if (!header)
    return no(fl::Skip::kRebound);
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
  const auto* state =
      Plausible(b->state_word) ? memory_->TranslateVirtual<const uint8_t*>(b->state_word) : nullptr;
  const uint32_t generation = state ? fl::Word(state, 0) : 0;
  std::atomic_thread_fence(std::memory_order_acquire);
  fl::Resolve(t, pm4, s.fresh_pool.data());
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint32_t generation_after = state ? fl::Word(state, 0) : 0;
  if (!fl::StableGeneration(generation, generation_after)) {
    ++s.in_progress;
    b->pool_valid = false;
    // The rejected snapshot cannot establish agreement with the packet decoder.
    return no(fl::Skip::kFixupInProgress);
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
  s.state = state;
  s.generation = generation;
  s.run_draws = s.run_draws_agree = s.run_draws_skipped = 0;
  std::fill(std::begin(s.run_draw_fields), std::end(s.run_draw_fields), 0);
  fh1_list_reader_ = reader;
  s.ns_begin += NowNs() - t0;
  return true;
}

void CommandProcessor::Fh1ListDraw(uint32_t packet_offset, bool executed) {
  auto& s = *fh1_lists_;
  if (s.mode == Fh1Lists::Mode::kVerify) {
    // 0660: an unsplit draw check (the replay's Snap, for the packets).
    s.draw_span_skip = 0;
    s.CloseDrawSpan();
    if (executed) {
      s.draw_span_skip = 3;
      s.draw_span_start = NowNs();
    }
    s.stock_draw_states.push_back(executed);
    const uint32_t* values = register_file_->values;
    for (size_t i = 0; i < s.draw_ranges->size(); i += 2) {
      s.stock_draw_states.insert(s.stock_draw_states.end(), values + (*s.draw_ranges)[i],
                                 values + (*s.draw_ranges)[i + 1]);
    }
    return;
  }
  const uint64_t t0 = NowNs();
  const RegisterFile& regs = record_split_ ? *decode_register_file_ : *register_file_;
  const uint32_t k = s.run.next_draw();
  const fl::Outcome o =
      s.run.Draw(packet_offset / 4 - 1, executed, regs.values, bin_mask_, s.shaders);
  ++s.run_draws;
  if (!executed)
    ++s.run_draws_skipped;
  if (o.agree) {
    ++s.run_draws_agree;
  } else {
    s.run_agree = false;
    for (uint32_t f = 0; f < fl::kFieldCount; ++f)
      if (o.fields >> f & 1)
        ++s.run_draw_fields[f];
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
  if (fh1_lists_->mode == Fh1Lists::Mode::kShadow)  // 0660: not a verify draw check's
    fh1_lists_->run.LiveWrites(first, count);
  else
    fh1_lists_->CloseDrawSpan();
}

// The write paths' cold halves, with a run open: PacketWriteRegister's unsplit path tail-calls
// the first (so that path needs no stack frame), Fh1WritePlainRange's call sites call the second
// (so Fh1WritePlainRangeFromRing stays main's).
void CommandProcessor::Fh1ListWriteRegister(uint32_t index, uint32_t value) {
  if (fh1_lists_->mode == Fh1Lists::Mode::kShadow)
    fh1_lists_->run.LiveWrites(index, 1);
  else
    fh1_lists_->CloseDrawSpan();
  WriteRegister(index, value);
}

bool CommandProcessor::Fh1ListWritePlainRange(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t count) {
  if (!Fh1WritePlainRangeFromRing(ring, base, count)) {
    return false;
  }
  if (fh1_lists_->mode == Fh1Lists::Mode::kShadow)
    fh1_lists_->run.LiveWrites(base, count);
  else
    fh1_lists_->CloseDrawSpan();
  return true;
}

void CommandProcessor::Fh1ListEnd() {
  auto& s = *fh1_lists_;
  const uint64_t t0 = NowNs();
  fh1_list_reader_ = nullptr;
  const RegisterFile& regs = record_split_ ? *decode_register_file_ : *register_file_;
  const fl::Outcome o = s.run.End(regs.values, bin_mask_, s.shaders);
  GroupCounts& g = s.groups[s.group];
  // The generation again, after everything the buffer's packets read (the seqlock's second
  // load, for the whole run): a fixup that started or ended while the list was decoded may have
  // rewritten words the packets read, so agreement and disagreement alike mean nothing.
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint32_t generation_end = s.state ? fl::Word(s.state, 0) : 0;
  if (!fl::StableGeneration(s.generation, generation_end)) {
    // The pool needs no invalidating: its generation is the run's start's, so the next run
    // reads the slots again.
    ++g.skips[size_t(fl::Skip::kFixupDuringRun)];
    if (s.detailed < 32 && !(o.agree && s.run_agree)) {
      ++s.detailed;
      REXGPU_INFO(
          "FH1 native lists: list {:08X} (group {}): a fixup ran during the run (generation "
          "{} -> {}), its verdict dropped",
          s.phys, s.group, s.generation, generation_end);
    }
    s.binding = nullptr;
    s.ns_end += NowNs() - t0;
    return;
  }
  ++g.compared;
  if (s.untiled)
    ++g.untiled_runs;
  g.draws += s.run_draws;
  g.draws_agree += s.run_draws_agree;
  g.draws_skipped += s.run_draws_skipped;
  for (uint32_t f = 0; f < fl::kFieldCount; ++f)
    s.draw_fields[f] += s.run_draw_fields[f];
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

// ---- 0660: verify mode ------------------------------------------------------------------------
//
// The replay's packet handlers: what the command processor's handlers do with a model list's
// packets (ExecutePacketType0, the tile block's SET_CONSTANT, LOAD_ALU_CONSTANT, IM_LOAD,
// ExecutePacketType3Draw), with the same writers, so its register writes and record words are
// the packets' by construction. In verify mode the writers see scratch state: split, the decoder's
// register file and the batch being recorded are swapped for scratch ones; unsplit, the register
// file is (the backend's writers store into it and mark their caches as the packets do). Two
// things a verified replay does not do, so it has no effect outside the scratch state: unsplit, a
// draw is not issued (its record is kept), and a write of D3D's marks (SCRATCH_REG6/7) is not
// stored to guest memory.
struct CommandProcessor::Fh1ReplayMachine {
  CommandProcessor& cp;
  uint64_t bin_select, bin_mask;
  bool split;
  std::vector<uint32_t>* draw_words;  // unsplit: the draws' records
  uint64_t draws = 0, skipped = 0;
  // Unsplit draw checks: the state at each draw packet, before it writes its registers.
  std::vector<uint32_t>* draw_states = nullptr;
  const std::vector<uint32_t>* ranges = nullptr;
  void Snap(bool executed) {
    draw_states->push_back(executed);
    const uint32_t* values = cp.register_file_->values;
    for (size_t i = 0; i < ranges->size(); i += 2)
      draw_states->insert(draw_states->end(), values + (*ranges)[i], values + (*ranges)[i + 1]);
  }
  // Self-checks (FH1_NATIVE_LISTS_VERIFY_VALUE / _TILE).
  bool perturb_value = false, value_applied = false, perturb_tile = false, tile_effective = false;

  bool AllPlain(uint32_t first, uint32_t count) const {
    const uint64_t* plain = cp.fh1_plain_registers_;
    for (uint32_t i = first; i < first + count; ++i) {
      if (!((plain[i >> 6] >> (i & 63)) & 1))
        return false;
    }
    return true;
  }
  void Range(uint32_t first, const uint8_t* guest, uint32_t count, bool last) {
    const uint32_t* words = reinterpret_cast<const uint32_t*>(guest);
    uint32_t changed[64];
    if (perturb_value && count <= 64) [[unlikely]] {
      perturb_value = false;
      value_applied = true;
      std::memcpy(changed, words, count * sizeof(uint32_t));
      changed[0] ^= fl::ToRaw(0x00010000u);
      words = changed;
    }
    if (split) {
      // ExecutePacketType0 -> WriteRegisterRangeFromRing -> PacketWriteRangeFromRing.
      cp.PacketWriteRegistersFromMem(first, words, count);
      return;
    }
    uint32_t* values = cp.register_file_->values;
    if (first < 0x2000) {
      // D3D's marks: stored, without the scratch write-back to guest memory.
      for (uint32_t i = 0; i < count; ++i)
        values[first + i] = memory::load_and_swap<uint32_t>(words + i);
      return;
    }
    // Fh1WritePlainRange (not for the buffer's last packet), else WriteRegisterRangeFromRing.
    if (cp.fh1_plain_registers_ && !last && first + count <= RegisterFile::kRegisterCount &&
        AllPlain(first, count)) {
      for (uint32_t i = 0; i < count; ++i) {
        const_cast<volatile uint32_t&>(values[first + i]) =
            memory::load_and_swap<uint32_t>(words + i);
      }
      return;
    }
    cp.WriteRegistersFromMem(first, const_cast<uint32_t*>(words), count);
  }
  void One(uint32_t index, uint32_t value) {
    if (split) {
      cp.PacketWriteRegister(index, value);
      return;
    }
    if (index < 0x2000) {
      cp.register_file_->values[index] = value;  // a mark: no scratch write-back
      return;
    }
    cp.WriteRegister(index, value);
  }
  void Literals(uint32_t first, uint32_t address, uint32_t count) {
    // ExecutePacketType3_LOAD_ALU_CONSTANT -> WriteALURangeFromMem.
    cp.PacketWriteRegistersFromMem(first, cp.memory_->TranslatePhysical<uint32_t*>(address), count);
  }
  void Shader(uint32_t address_type, uint32_t start_size) {
    cp.Fh1ListShader(address_type, start_size);
    const auto type = static_cast<xenos::ShaderType>(address_type & 3);
    const uint32_t address = address_type & ~3u, dwords = start_size & 0xFFFF;
    if (split) {
      uint32_t* record = cp.RecordWords(4);
      record[0] = kRecordShader;
      record[1] = uint32_t(type);
      record[2] = address;
      record[3] = dwords;
      return;
    }
    auto* shader =
        cp.LoadShader(type, address, cp.memory_->TranslatePhysical<uint32_t*>(address), dwords);
    (type == xenos::ShaderType::kVertex ? cp.active_vertex_shader_ : cp.active_pixel_shader_) =
        shader;
  }
  void Draw(uint32_t initiator, uint32_t dma_base, uint32_t dma_size) {
    if (draw_states) [[unlikely]]
      Snap(true);
    One(XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);
    One(XE_GPU_REG_VGT_DMA_BASE, dma_base);
    One(XE_GPU_REG_VGT_DMA_SIZE, dma_size);
    reg::VGT_DRAW_INITIATOR vgt_draw_initiator;
    vgt_draw_initiator.value = initiator;
    reg::VGT_DMA_SIZE vgt_dma_size;
    vgt_dma_size.value = dma_size;
    const uint32_t index_size_bytes = vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16
                                          ? sizeof(uint16_t)
                                          : sizeof(uint32_t);
    DrawRecord draw;
    draw.initiator = initiator;
    draw.flags = kDrawIndexed;
    draw.index_format = uint32_t(vgt_draw_initiator.index_size);
    draw.index_endianness = uint32_t(vgt_dma_size.swap_mode);
    draw.index_count = vgt_draw_initiator.num_indices;
    draw.index_guest_base = dma_base & ~(index_size_bytes - 1);
    draw.index_length = uint32_t(vgt_dma_size.num_words * index_size_bytes);
    ++draws;
    if (split) {
      uint32_t* record = cp.RecordWords(1 + sizeof(DrawRecord) / sizeof(uint32_t));
      record[0] = kRecordDraw;
      std::memcpy(record + 1, &draw, sizeof(draw));
      cp.RecordAction();
      return;
    }
    const auto* w = reinterpret_cast<const uint32_t*>(&draw);
    draw_words->insert(draw_words->end(), w, w + sizeof(draw) / sizeof(uint32_t));
  }
  void Skip() {
    if (draw_states) [[unlikely]]
      Snap(false);
    ++skipped;
  }
  uint32_t TileMask(uint32_t tile, uint32_t live) {
    if (perturb_tile && tile == 0) [[unlikely]] {
      perturb_tile = false;
      const uint32_t other = 0xC;  // the second tile's mask (FH1 native_list.h, kTileMasks)
      tile_effective = ((bin_select & ((bin_mask & ~0xFFFFFFFFull) | live)) != 0) !=
                       ((bin_select & ((bin_mask & ~0xFFFFFFFFull) | other)) != 0);
      return other;
    }
    return live;
  }
};

bool CommandProcessor::Fh1ListPublishing() {
  auto& s = *fh1_lists_;
  if (fh1_list_capture_ == 2)
    return true;  // the replay's scratch batch is never published
  const uint64_t t0 = NowNs();
  auto& words = record_batch_->words;
  if (words.size() > s.capture_from)
    s.captured.insert(s.captured.end(), words.begin() + s.capture_from, words.end());
  s.capture_from = 0;  // the next batch starts empty
  s.capture_ns += NowNs() - t0;
  return false;
}

bool CommandProcessor::Fh1ListVerify(uint32_t ptr, uint32_t count, memory::RingBuffer* reader) {
  auto& s = *fh1_lists_;
  if (s.mode != Fh1Lists::Mode::kVerify)
    return false;
  auto& v = s.v;
  if (s.verifying) {
    ++v.skips[kVerifyNestedList];
    return false;
  }
  const uint64_t t0 = NowNs();
  fl::Skip lookup_skip;
  fl::Binding* b = s.registry.Lookup(ptr & 0x1FFFFFFFu, count, &lookup_skip);
  if (!b) {
    if (lookup_skip == fl::Skip::kLength)
      ++v.skips[kVerifyLength];
    else if (s.registry.WasBound(ptr & 0x1FFFFFFFu))
      ++v.dropped_address_misses;
    else
      ++v.other_buffers;
    return false;
  }
  if (b->template_id == fh::kListRefused) {
    ++s.refusals[std::min<uint32_t>(b->refusal, kRefusals - 1)];
    ++v.skips[kVerifyRefused];
    return false;
  }
  fl::Template* tmpl = s.registry.FindTemplate(b->template_id);
  if (!tmpl) {
    ++v.skips[kVerifyNoTemplate];
    return false;
  }
  // Refused whenever stereo is on: in its speculative and recording states every packet is
  // logged in ExecutePacket for the second eye, which a replay does not go through.
  if (fh1_stereo_state_ != Fh1StereoState::kOff || Fh1StereoWanted()) {
    ++v.skips[kVerifyStereo];
    return false;
  }
  const uint32_t* t = tmpl->words.data();
  const auto* pm4 = memory_->TranslatePhysical<const uint8_t*>(b->phys);
  if (tmpl->program_state == fl::Template::kProgramNone) {
    // Compiled from a list whose static words are the template's (shadow's lookup checks).
    const uint64_t tc = NowNs();
    const uint8_t* header = NamedHeader(memory_, b, count);
    if (!header) {
      ++v.skips[kVerifyRebound];
      return false;
    }
    const fl::Key key = fl::LiveKey(
        t, header, fl::FilesFormFlags(fl::Word(header, fl::kHeaderFlags / 4)),
        memory_->TranslateVirtual<const uint8_t*>(b->patch), b->patch_words, pm4, count);
    if (key != fl::TemplateKey(t)) {
      ++v.skips[kVerifyKey];
      return false;
    }
    const fl::CompileFail f = fl::Compile(t, pm4, count, &tmpl->program);
    ++v.compiles[size_t(f)];
    tmpl->program_state =
        f == fl::CompileFail::kNone ? fl::Template::kProgramReady : fl::Template::kProgramFailed;
    if (f != fl::CompileFail::kNone && v.detailed < 32) {
      ++v.detailed;
      REXGPU_INFO("FH1 native lists (verify): the template of list {:08X} has no program: {}",
                  b->phys, fl::CompileFailName(f));
    }
    v.ns_compile += NowNs() - tc;
  }
  if (tmpl->program_state != fl::Template::kProgramReady) {
    ++v.skips[kVerifyNoProgram];
    return false;
  }
  const auto* state =
      Plausible(b->state_word) ? memory_->TranslateVirtual<const uint8_t*>(b->state_word) : nullptr;
  const uint32_t generation = state ? fl::Word(state, 0) : 0;
  std::atomic_thread_fence(std::memory_order_acquire);
  if (generation & fh::kListStateNested) {
    ++v.skips[kVerifyNestedFixup];
    return false;
  }
  if (generation & 1) {
    ++v.skips[kVerifyFixupInProgress];
    return false;
  }
  const uint64_t t_snapshot = NowNs();
  v.ns_begin += t_snapshot - t0;

  // The state the list starts with, for the replay (whichever goes first): the registers its
  // program writes and the two its writers read (the scratch write-back's mask and address), the
  // bin mask, the IM_LOAD words, the shaders (unsplit), the record counters and the noted stores
  // (split).
  const fl::ReplayProgram* program = &tmpl->program;
  const bool split = record_split_;
  if (!s.scratch_regs) {
    s.scratch_regs = std::make_unique<RegisterFile>();
    s.scratch_batch = std::make_unique<RecordBatch>();
  }
  RegisterFile& live_regs = split ? *decode_register_file_ : *register_file_;
  uint32_t* scratch = s.scratch_regs->values;
  const std::vector<uint32_t>& ranges = program->ranges;
  for (size_t i = 0; i < ranges.size(); i += 2) {
    std::memcpy(scratch + ranges[i], live_regs.values + ranges[i],
                (ranges[i + 1] - ranges[i]) * sizeof(uint32_t));
  }
  scratch[XE_GPU_REG_SCRATCH_UMSK] = live_regs.values[XE_GPU_REG_SCRATCH_UMSK];
  scratch[XE_GPU_REG_SCRATCH_ADDR] = live_regs.values[XE_GPU_REG_SCRATCH_ADDR];
  const bool full = v.full_every && ++v.full_checks % v.full_every == 0;
  if (full) {
    s.full_start.assign(live_regs.values + 0x2000, live_regs.values + RegisterFile::kRegisterCount);
  }
  const uint64_t start_mask = bin_mask_;
  const fl::Shaders start_shaders = s.shaders;
  // Unsplit only: split, the shaders loaded are the recorder's (it sets them from the records), and
  // the decoder must not touch them.
  Shader* const start_active[2] = {split ? nullptr : active_vertex_shader_,
                                   split ? nullptr : active_pixel_shader_};
  const uint64_t start_in = record_registers_in_, start_out = record_registers_out_;
  RecordedStore start_stores[kRecordStoreSlots];
  std::copy(std::begin(record_stores_), std::end(record_stores_), start_stores);
  const uint32_t start_store_next = record_store_next_;

  // Self-checks.
  const uint64_t n = v.runs + 1;
  const bool perturb_header =
      v.perturb_header_every && n % v.perturb_header_every == 0 && !program->ops.empty();
  uint32_t perturbed_op = 0;
  if (perturb_header) {
    s.perturbed_program = *program;
    perturbed_op = uint32_t(program->ops.size() / 2);
    // A tile block is checked by its static words, not by op.header.
    while (perturbed_op + 1 < program->ops.size() &&
           program->ops[perturbed_op].kind == fl::kReplayTile) {
      ++perturbed_op;
    }
    s.perturbed_program.ops[perturbed_op].header ^= fl::ToRaw(0x00010000u);
    program = &s.perturbed_program;
    ++v.header_perturbed;
  }
  bool tiled = false;
  for (const fl::ReplayOp& op : program->ops)
    tiled |= op.kind == fl::kReplayTile;

  Fh1ReplayMachine m{*this, bin_select_, start_mask, split, &s.scratch_draws};
  m.perturb_value = v.perturb_value_every && n % v.perturb_value_every == 0;
  m.perturb_tile = tiled && v.perturb_tile_every && n % v.perturb_tile_every == 0;
  const bool tile_perturbed = m.perturb_tile;
  s.scratch_draws.clear();
  const bool draw_check =
      !split && (perturb_header || m.perturb_value || m.perturb_tile ||
                 (v.draw_check_every && ++v.draw_check_count % v.draw_check_every == 0));
  if (draw_check) {
    s.stock_draw_states.clear();
    s.replay_draw_states.clear();
    s.draw_ranges = &ranges;
    m.draw_states = &s.replay_draw_states;
    m.ranges = &ranges;
  }

  // The replay into scratch state; the real state as it is around it.
  fl::ReplayEnd end;
  uint64_t ns_replay = 0, end_in = 0, end_out = 0;
  RecordedStore end_stores[kRecordStoreSlots];
  uint32_t end_store_next = 0;
  fl::Shaders end_shaders;
  Shader* end_active[2] = {};
  const auto replay = [&]() {
    const uint64_t real_in = record_registers_in_, real_out = record_registers_out_;
    RecordedStore real_stores[kRecordStoreSlots];
    std::copy(std::begin(record_stores_), std::end(record_stores_), real_stores);
    const uint32_t real_store_next = record_store_next_;
    const fl::Shaders real_shaders = s.shaders;
    Shader* const real_active[2] = {split ? nullptr : active_vertex_shader_,
                                    split ? nullptr : active_pixel_shader_};
    record_registers_in_ = start_in;
    record_registers_out_ = start_out;
    std::copy(std::begin(start_stores), std::end(start_stores), record_stores_);
    record_store_next_ = start_store_next;
    s.shaders = start_shaders;
    RegisterFile* real_file = register_file_;
    if (split) {
      s.scratch_batch->words.clear();
      s.scratch_batch->fns.clear();
      s.scratch_batch->actions = 0;
      std::swap(decode_register_file_, s.scratch_regs);
      std::swap(record_batch_, s.scratch_batch);
      fh1_list_capture_ = 2;
    } else {
      register_file_ = s.scratch_regs.get();
      active_vertex_shader_ = start_active[0];
      active_pixel_shader_ = start_active[1];
    }
    const uint64_t tr = NowNs();
    end = fl::Replay(*program, pm4, m);
    ns_replay = NowNs() - tr;
    if (split) {
      fh1_list_capture_ = 0;
      std::swap(record_batch_, s.scratch_batch);
      std::swap(decode_register_file_, s.scratch_regs);
    } else {
      register_file_ = real_file;
      end_active[0] = active_vertex_shader_;
      end_active[1] = active_pixel_shader_;
      active_vertex_shader_ = real_active[0];
      active_pixel_shader_ = real_active[1];
    }
    end_in = record_registers_in_;
    end_out = record_registers_out_;
    std::copy(std::begin(record_stores_), std::end(record_stores_), end_stores);
    end_store_next = record_store_next_;
    end_shaders = s.shaders;
    record_registers_in_ = real_in;
    record_registers_out_ = real_out;
    std::copy(std::begin(real_stores), std::end(real_stores), record_stores_);
    record_store_next_ = real_store_next;
    s.shaders = real_shaders;
  };
  // The packets, as ExecuteIndirectBuffer decodes them, with their record words kept.
  uint64_t ns_stock = 0, published = 0;
  const auto packets = [&]() {
    if (split) {
      s.captured.clear();
      s.capture_from = record_batch_->words.size();
      s.capture_ns = 0;
      fh1_list_capture_ = 1;
    }
    const uint64_t published_before = record_published_;
    s.verifying = true;
    if (draw_check)
      fh1_list_reader_ = reader;  // the draw seam (and the write seams, which do nothing here)
    const uint64_t ts = NowNs();
    do {
      if (!ExecutePacket(reader)) {
        REXGPU_ERROR("**** INDIRECT RINGBUFFER: Failed to execute packet.");
        assert_always();
        break;
      }
    } while (reader->read_count());
    ns_stock = NowNs() - ts - s.capture_ns;
    if (draw_check) {
      s.draw_span_skip = 0;
      s.CloseDrawSpan();
    }
    fh1_list_reader_ = nullptr;
    published = record_published_ - published_before;
    s.verifying = false;
    if (split) {
      fh1_list_capture_ = 0;
      auto& words = record_batch_->words;
      if (words.size() > s.capture_from)
        s.captured.insert(s.captured.end(), words.begin() + s.capture_from, words.end());
    }
  };
  const bool replay_first = b->replay_first;
  b->replay_first = !b->replay_first;
  const uint64_t t_runs = NowNs();
  if (replay_first) {
    replay();
    packets();
  } else {
    packets();
    replay();
  }
  const uint64_t t_compare = NowNs();
  ++v.runs;
  const bool value_perturbed = m.value_applied;

  // A fixup that started or ended during the run: no verdict, no time.
  std::atomic_thread_fence(std::memory_order_acquire);
  const uint32_t generation_end = state ? fl::Word(state, 0) : 0;
  if (!fl::StableGeneration(generation, generation_end)) {
    ++v.skips[kVerifyFixupDuringRun];
    v.ns_check += (t_runs - t_snapshot) + (NowNs() - t_compare);
    return true;
  }
  const bool perturbed = perturb_header || value_perturbed || tile_perturbed;
  if (end.why != fl::Handover::kNone) {
    const bool caught =
        perturb_header && end.why == fl::Handover::kHeader && end.op == perturbed_op;
    v.header_caught += caught;
    if (!perturbed)
      ++v.handovers[size_t(end.why)];
    if (!caught && v.detailed < 32) {
      ++v.detailed;
      REXGPU_INFO("FH1 native lists (verify): list {:08X} hands over at op {} of {} (dword {}): {}",
                  b->phys, end.op, program->ops.size(), end.dword, fl::HandoverName(end.why));
    }
    v.ns_check += (t_runs - t_snapshot) + (NowNs() - t_compare);
    return true;
  }

  // The compare: what the packets left against what the replay left.
  uint32_t fields = 0, first_reg = fl::kNone, packets_value = 0, replay_value = 0;
  const RegisterFile& after = split ? *decode_register_file_ : *register_file_;
  for (size_t i = 0; i < ranges.size(); i += 2) {
    const uint32_t a = ranges[i], e = ranges[i + 1];
    if (std::memcmp(after.values + a, scratch + a, (e - a) * sizeof(uint32_t)) == 0)
      continue;
    fields |= 1u << kVerifyRegisters;
    for (uint32_t r = a; r < e && first_reg == fl::kNone; ++r) {
      if (after.values[r] != scratch[r]) {
        first_reg = r;
        packets_value = after.values[r];
        replay_value = scratch[r];
      }
    }
  }
  if (split) {
    const auto& replay_words = s.scratch_batch->words;
    if (s.captured.size() != replay_words.size() ||
        std::memcmp(s.captured.data(), replay_words.data(), s.captured.size() * sizeof(uint32_t)) !=
            0) {
      fields |= 1u << kVerifyRecordWords;
    }
    if (record_registers_in_ - start_in != end_in - start_in ||
        record_registers_out_ - start_out != end_out - start_out) {
      fields |= 1u << kVerifyCounters;
    }
    bool stores_same = record_store_next_ == end_store_next;
    for (uint32_t i = 0; i < kRecordStoreSlots; ++i)
      stores_same &= record_stores_[i].address == end_stores[i].address;
    if (!stores_same)
      fields |= 1u << kVerifyStores;
  } else if (active_vertex_shader_ != end_active[0] || active_pixel_shader_ != end_active[1]) {
    fields |= 1u << kVerifyShaders;
  }
  if (bin_mask_ != m.bin_mask)
    fields |= 1u << kVerifyBinMask;
  if (draw_check) {
    v.draw_checked += !perturbed;
    if (s.stock_draw_states != s.replay_draw_states)
      fields |= 1u << kVerifyDraws;
  }
  for (uint32_t stage = 0; stage < 2; ++stage) {
    if (s.shaders.valid[stage] != end_shaders.valid[stage] ||
        s.shaders.address[stage] != end_shaders.address[stage] ||
        s.shaders.size[stage] != end_shaders.size[stage]) {
      fields |= 1u << kVerifyShaders;
    }
  }
  if (full) {
    // Everything from 0x2000 the packets left: the start, with the program's ranges the
    // replay's (anything else a packet wrote shows here).
    s.full_expected = s.full_start;
    for (size_t i = 0; i < ranges.size(); i += 2) {
      if (ranges[i + 1] <= 0x2000)
        continue;
      const uint32_t a = std::max<uint32_t>(ranges[i], 0x2000);
      std::memcpy(s.full_expected.data() + (a - 0x2000), scratch + a,
                  (ranges[i + 1] - a) * sizeof(uint32_t));
    }
    if (std::memcmp(s.full_expected.data(), after.values + 0x2000,
                    s.full_expected.size() * sizeof(uint32_t)) != 0) {
      fields |= 1u << kVerifyFull;
    }
  }
  // The self-checks' runs have their own counters only (they are not acceptance runs).
  if (!perturbed) {
    ++v.compared;
    for (uint32_t f = 0; f < kVerifyFieldCount; ++f)
      v.fields[f] += (fields >> f) & 1;
    if (!fields)
      ++v.agree;
  }
  if (value_perturbed && fields)
    ++v.value_caught;
  v.value_perturbed += value_perturbed;
  if (tile_perturbed) {
    ++v.tile_perturbed;
    v.tile_effective += m.tile_effective;
    v.tile_caught += fields != 0;
  }
  if (fields && !value_perturbed && !tile_perturbed && v.detailed < 32) {
    ++v.detailed;
    std::string names;
    for (uint32_t f = 0; f < kVerifyFieldCount; ++f)
      if (fields >> f & 1)
        names += fmt::format(" {};", kVerifyFieldNames[f]);
    REXGPU_INFO(
        "FH1 native lists (verify): list {:08X} ({} ops, {} draws, {}) differs:{} first "
        "register {:04X}: packets {:08X}, replay {:08X}; record words {} / {}",
        b->phys, program->ops.size(), program->draws,
        replay_first ? "replay first" : "packets first", names,
        first_reg == fl::kNone ? 0 : first_reg, packets_value, replay_value, s.captured.size(),
        split ? s.scratch_batch->words.size() : 0);
  }
  // Timed: only the runs whose replay is the real program (a perturbed one is not).
  if (!perturbed && !draw_check) {
    for (VerifyTiming* timing :
         {&v.timing[replay_first], published ? nullptr : &v.quiet[replay_first]}) {
      if (!timing)
        continue;
      ++timing->runs;
      timing->ops += program->ops.size();
      timing->dwords += count;
      timing->draws += m.draws;
      timing->ns_stock += ns_stock;
      timing->ns_replay += ns_replay;
    }
  }
  v.ns_check += (t_runs - t_snapshot) + (NowNs() - t_compare);
  return true;
}

namespace {

void LogVerify(const CommandProcessor::Fh1Lists& s, const char* when, uint64_t frames) {
  const auto& v = s.v;
  auto pct = [](uint64_t a, uint64_t b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
  std::string skips, fields, handovers, compiles;
  for (uint32_t k = 0; k < kVerifySkipCount; ++k)
    if (v.skips[k])
      skips += fmt::format(" {} {};", kVerifySkipNames[k], v.skips[k]);
  for (uint32_t f = 0; f < kVerifyFieldCount; ++f)
    if (v.fields[f])
      fields += fmt::format(" {} {};", kVerifyFieldNames[f], v.fields[f]);
  for (size_t h = 1; h < size_t(fl::Handover::kCount); ++h)
    if (v.handovers[h])
      handovers += fmt::format(" {} {};", fl::HandoverName(fl::Handover(h)), v.handovers[h]);
  for (size_t c = 0; c < size_t(fl::CompileFail::kCount); ++c)
    if (v.compiles[c])
      compiles += fmt::format(" {} {};", fl::CompileFailName(fl::CompileFail(c)), v.compiles[c]);
  const uint64_t compared = v.compared;
  REXGPU_INFO(
      "FH1 native lists (verify, {}, {} frames): runs verified {}: compared {}, agree {} "
      "({:.4f}%; unsplit runs with every draw compared {}), differ in:{} | handovers:{} | not "
      "verified:{} other buffers {}, misses at a "
      "dropped list's address {} | programs:{} | self-checks: header {} perturbed {} caught, "
      "value {} perturbed {} caught, tile mask {} perturbed ({} change a window) {} caught",
      when, frames, v.runs, compared, v.agree, pct(v.agree, compared), v.draw_checked,
      fields.empty() ? " none" : fields, handovers.empty() ? " none" : handovers,
      skips.empty() ? " none;" : skips, v.other_buffers, v.dropped_address_misses,
      compiles.empty() ? " none" : compiles, v.header_perturbed, v.header_caught, v.value_perturbed,
      v.value_caught, v.tile_perturbed, v.tile_effective, v.tile_caught);
  const double per_frame = frames ? 1e-6 / double(frames) : 0.0;
  for (uint32_t first = 0; first < 2; ++first) {
    const VerifyTiming& t = v.timing[first];
    const double ops = t.ops ? double(t.ops) : 1.0;
    REXGPU_INFO(
        "FH1 native lists (verify timing, {} first): {} runs, {} ops, {} dwords, {} draws; "
        "packets {:.3f} ms ({:.2f} ns an op), replay {:.3f} ms ({:.2f} ns an op) in all; a "
        "frame: {:.1f} runs, {:.0f} ops, packets {:.4f} ms, replay {:.4f} ms, difference "
        "{:.4f} ms",
        first ? "replay" : "packets", t.runs, t.ops, t.dwords, t.draws, double(t.ns_stock) * 1e-6,
        double(t.ns_stock) / ops, double(t.ns_replay) * 1e-6, double(t.ns_replay) / ops,
        frames ? double(t.runs) / double(frames) : 0.0,
        frames ? double(t.ops) / double(frames) : 0.0, double(t.ns_stock) * per_frame,
        double(t.ns_replay) * per_frame, (double(t.ns_stock) - double(t.ns_replay)) * per_frame);
  }
  for (uint32_t first = 0; first < 2; ++first) {
    const VerifyTiming& t = v.quiet[first];
    REXGPU_INFO(
        "FH1 native lists (verify timing without a publish, {} first): {} runs, {} ops, {} "
        "dwords, {} draws; packets {:.3f} ms, replay {:.3f} ms in all",
        first ? "replay" : "packets", t.runs, t.ops, t.dwords, t.draws, double(t.ns_stock) * 1e-6,
        double(t.ns_replay) * 1e-6);
  }
  REXGPU_INFO(
      "FH1 native lists (verify): {} decoder; lookup and checks before a run {:.4f} ms a frame "
      "({:.1f} ns a run), compiles {:.1f} ms in all, snapshot and compare {:.4f} ms a frame; "
      "a clock read pair {:.1f} ns; unsplit draw checks: {} draws issued, {:.1f} ns a draw",
      s.v.timing[0].runs + s.v.timing[1].runs ? "" : "no runs timed;",
      double(v.ns_begin) * per_frame, v.runs ? double(v.ns_begin) / double(v.runs) : 0.0,
      double(v.ns_compile) * 1e-6, double(v.ns_check) * per_frame, v.clock_ns, s.draw_spans,
      s.draw_spans ? double(s.draw_span_ns) / double(s.draw_spans) : 0.0);
}

}  // namespace

void CommandProcessor::Fh1ListsSwap() {
  auto& s = *fh1_lists_;
  if (++s.frames % 600 == 0) {
    if (s.mode == Fh1Lists::Mode::kVerify)
      LogVerify(s, "so far", s.frames);
    else
      LogLists(s, "so far", s.frames);
  }
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
  if (fh1_lists_->mode == Fh1Lists::Mode::kVerify)
    LogVerify(*fh1_lists_, "at exit", fh1_lists_->frames);
  else
    LogLists(*fh1_lists_, "at exit", fh1_lists_->frames);
  delete fh1_lists_;
  fh1_lists_ = nullptr;
}

}  // namespace rex::graphics
