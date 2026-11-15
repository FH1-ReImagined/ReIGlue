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
      "other buffers {}{}",
      when, frames, all.runs, all.compared, all.untiled_runs, all.agree,
      pct(all.agree, all.compared), all.draws, all.draws_skipped, all.draws_agree,
      pct(all.draws_agree, all.draws), skips, refused.empty() ? " none" : refused,
      s.key_checks[0][0], s.key_checks[0][1], s.key_checks[1][0], s.key_checks[1][1],
      s.generation_changes, s.in_progress, s.stale_pools, s.other_buffers,
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
      "words; defines {}, repeats {}, invalid {}, unreadable {}, hash {}), bound {} (binds {}, "
      "same {}, replaced {}, to an unknown template {}), drops {} ({} unknown), malformed {}; "
      "compare {:.2f} us a run (begin {:.2f}, draws {:.2f}, end {:.2f}), {:.3f} ms a frame, "
      "packets {:.3f} ms in all",
      draw_fields.empty() ? " none" : draw_fields, end_fields.empty() ? " none" : end_fields,
      s.registry.templates(), s.template_words, s.defines, s.define_repeats, s.define_invalid,
      s.define_unreadable, s.define_hash, s.registry.bindings(), s.binds, s.bind_same,
      s.bind_replaced, s.bind_no_template, s.drops, s.drop_unknown, s.malformed,
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
      if (s.registry.FindTemplate(id)) {
        ++s.define_repeats;
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
      if (Plausible(ack) && sequence > s.last_ack) {
        s.last_ack = sequence;
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual<uint32_t*>(ack), sequence);
      }
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
  const uint64_t t0 = NowNs();
  fl::Skip skip;
  fl::Binding* b = s.registry.Lookup(ptr & 0x1FFFFFFFu, count, &skip);
  if (!b) {
    if (skip == fl::Skip::kLength) {
      ++s.groups[21].runs;
      ++s.groups[21].skips[size_t(skip)];
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
  const uint32_t pool_words = t[fl::kTPoolWords];
  s.fresh_pool.resize(pool_words);
  fl::Resolve(t, pm4, s.fresh_pool.data());
  const auto* state =
      Plausible(b->state_word) ? memory_->TranslateVirtual<const uint8_t*>(b->state_word) : nullptr;
  const uint32_t generation = state ? fl::Word(state, 0) : 0;
  if (generation & 1) {
    ++s.in_progress;
    b->pool_valid = false;
    s.pool = s.fresh_pool;
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
  s.ns_begin += NowNs() - t0;
  return true;
}

void CommandProcessor::Fh1ListDraw(uint32_t packet_offset, bool executed) {
  auto& s = *fh1_lists_;
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

void CommandProcessor::Fh1ListEnd() {
  auto& s = *fh1_lists_;
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
  if (++s.frames % 600 == 0)
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
