// FH1 pass ledger (patch 0670): what each pass of the game's frame costs on the threads that
// limit it, and which render state changes from draw to draw. A measurement tool for FH1's native
// renderer roadmap (R0), off unless fh1_pass_cost_csv names a file; FH1 docs/rendering.md "Pass
// ledger" says how to run it and read it.
//
// The game marks its command stream at its D3D flush points (host packets, fh1_host_packet.h
// kOpPassClass and kOpPassEnd): which draw class the commands after a mark belong to (D3D draws,
// the track's lists, the procedural models', a car's, ...), and where a segment of the frame
// ends (after each resolve, and before the swap) with its label (the game's presentation pass
// and its occurrence in the frame, the shadow mask, or "so many segments after" the last named
// one). With each mark it sends what the render thread did for the class or segment that ends.
//
// The command processor reads the marks in stream order on each of its threads:
// - the decoder ("GPU Commands", or the whole command processor when it is not split) adds its
//   thread's CPU time since the previous mark to the class and segment in progress, and counts
//   the draws it decoded (and those a predicate skipped);
// - the recorder ("GPU Recorder") adds its time inside the decoder's batches (its waits for the
//   next batch are left out, and counted apart), and times each IssueDraw (draws and resolves);
// - at the swap both hand their frame to a window, written to the CSV every fh1_pass_cost_every
//   frames.
// With fh1_pass_delta_stats (or FORZA_VK_DELTA_STATS=1) the decoder also compares, at each draw,
// the register groups (rex/graphics/register_groups.h) with their values at the previous draw,
// and the recorder compares what its stages chose (shaders, pipeline, render pass, texture and
// constant descriptor sets, samplers, viewport and scissor, system constants) with the previous
// draw's: the change-mask counter. Nothing is skipped; the comparisons cost time, so a run that
// times passes keeps them off.
//
// The deletion switch (patch 0680, test only, FH1 docs/rendering.md "Pass deletion"):
// FORZA_TEST_SKIP_PASS=<kinds>[,copies] makes the recorder return at the top of IssueDraw for the
// draws of those passes (SkipSpec names them by label; the game's class marks carry each draw's
// pass id so far, since a segment's label is only known at its end); without `copies` a skipped
// draw still binds its render targets (the EDRAM ownership transfers into and out of the pass's
// surfaces stay) and the pass's resolves run; with `copies` neither. SkipTracker counts the draws
// whose pass id was not their segment's final label. Read when the ledger starts; unset, nothing
// of it exists.
//
// This header is the part without the runtime (fh1_pass_ledger.cpp has the command processor's):
// cells and rows, label names, the time accrual at marks and the CSV lines, for
// tests/fh1_pass_ledger_test.cpp.
#pragma once

#include <rex/graphics/fh1_host_packet.h>
#include <rex/graphics/register_groups.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rex::graphics::fh1_pass_ledger {

namespace fh = fh1_host;
namespace rg = register_groups;

// A draw class with its layer (model lists: the pass's layer 0-2; 0 for the others).
constexpr uint32_t kLayers = 4;
constexpr uint32_t kSlots = fh::kPassClassCount * kLayers;
constexpr uint32_t SlotOf(uint32_t class_word) {
  const uint32_t cls = class_word & 0xF;
  const uint32_t layer = (class_word >> 4) & (kLayers - 1);
  return (cls < fh::kPassClassCount ? cls : uint32_t(fh::kPassClassNone)) +
         fh::kPassClassCount * layer;
}
constexpr uint32_t SlotClass(uint32_t slot) {
  return slot % fh::kPassClassCount;
}
constexpr uint32_t SlotLayer(uint32_t slot) {
  return slot / fh::kPassClassCount;
}

inline const char* ClassName(uint32_t cls) {
  constexpr const char* kNames[fh::kPassClassCount] = {"d3d",        "up",  "clear",      "track",
                                                       "procedural", "car", "other_list", "none"};
  return cls < fh::kPassClassCount ? kNames[cls] : "?";
}

// What the recorder's stages chose, compared with the previous draw's.
enum Stage : uint32_t {
  kStageShaders,      // the vertex and pixel shaders
  kStagePipeline,     // the host pipeline
  kStageRenderPass,   // the render pass and framebuffer (render targets)
  kStageTextureSets,  // the descriptor sets of the textures (vertex and pixel)
  kStageSamplers,     // the samplers chosen
  kStageConstantSet,  // the descriptor set of the constant buffers
  kStageDynamic,      // viewport and scissor
  kStageSystemConstants,
  kStageCount,
};
inline const char* StageName(uint32_t stage) {
  constexpr const char* kNames[kStageCount] = {"shaders",      "pipeline",        "render_pass",
                                               "texture_sets", "samplers",        "constant_set",
                                               "dynamic",      "system_constants"};
  return stage < kStageCount ? kNames[stage] : "?";
}

// One draw class of one segment label, summed over a window.
struct Cell {
  // The render thread (the game's numbers from the marks).
  uint64_t rdr_flush_ns = 0;     // in the D3D flush points
  uint64_t rdr_flush_calls = 0;  // flush point calls
  uint64_t submitted = 0;        // draws the game made (a list's from its packets)
  uint64_t runs = 0;             // command lists run (RunCommandBuffer)
  // The decoder (unsplit: the whole command processor thread).
  uint64_t dec_ns = 0;       // thread CPU time
  uint64_t dec_draws = 0;    // draw packets decoded and run
  uint64_t dec_skipped = 0;  // draw packets a predicate skipped (other tiles)
  // The recorder (split only for rec_ns; unsplit its work is in dec_ns).
  uint64_t rec_ns = 0;        // busy inside batches
  uint64_t rec_draws = 0;     // IssueDraw calls that draw
  uint64_t rec_issue_ns = 0;  // their time
  uint64_t rec_copies = 0;    // IssueDraw calls that resolve (EDRAM copy mode)
  uint64_t rec_copy_ns = 0;
  uint64_t rec_empty = 0;  // draws that recorded nothing (nothing to draw, pipeline not ready)
  // The deletion switch (0680): draws and resolves it skipped, and the time of what a skipped
  // draw still does without `copies` (its render targets bound).
  uint64_t rec_skipped = 0, rec_skipped_copies = 0, rec_skip_ns = 0;
  // The change-mask counter.
  uint64_t delta_draws = 0;
  uint64_t unchanged[rg::kGroupCount] = {};
  uint64_t stage_draws = 0;
  uint64_t stage_same[kStageCount] = {};

  void Add(const Cell& o) {
    rdr_flush_ns += o.rdr_flush_ns;
    rdr_flush_calls += o.rdr_flush_calls;
    submitted += o.submitted;
    runs += o.runs;
    dec_ns += o.dec_ns;
    dec_draws += o.dec_draws;
    dec_skipped += o.dec_skipped;
    rec_ns += o.rec_ns;
    rec_draws += o.rec_draws;
    rec_issue_ns += o.rec_issue_ns;
    rec_copies += o.rec_copies;
    rec_copy_ns += o.rec_copy_ns;
    rec_empty += o.rec_empty;
    rec_skipped += o.rec_skipped;
    rec_skipped_copies += o.rec_skipped_copies;
    rec_skip_ns += o.rec_skip_ns;
    delta_draws += o.delta_draws;
    for (uint32_t g = 0; g < rg::kGroupCount; ++g)
      unchanged[g] += o.unchanged[g];
    stage_draws += o.stage_draws;
    for (uint32_t s = 0; s < kStageCount; ++s)
      stage_same[s] += o.stage_same[s];
  }
  bool Empty() const {
    return !rdr_flush_ns && !rdr_flush_calls && !submitted && !runs && !dec_ns && !dec_draws &&
           !dec_skipped && !rec_ns && !rec_draws && !rec_issue_ns && !rec_copies && !rec_copy_ns &&
           !rec_empty && !rec_skipped && !rec_skipped_copies && !rec_skip_ns && !delta_draws &&
           !stage_draws;
  }
};

// One segment label: its cells, and what belongs to the segment as a whole.
struct Row {
  uint32_t label = 0;
  uint64_t segments = 0;      // segment ends seen (a tiled segment's replays count each)
  uint64_t rdr_cpu_ns = 0;    // the render thread's CPU time (once per segment)
  uint64_t other_device = 0;  // flush calls on another D3D device
  // The label's place in the frame: the sum over frames of its first segment's position among
  // the frame's labels, and the frames it was in (the report orders labels by the mean).
  uint64_t order_sum = 0, order_frames = 0;
  // The render targets of the label's first draw: RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO.
  uint32_t rt[3] = {};
  bool rt_known = false;
  Cell cells[kSlots];

  void Add(const Row& o) {
    segments += o.segments;
    rdr_cpu_ns += o.rdr_cpu_ns;
    other_device += o.other_device;
    order_sum += o.order_sum;
    order_frames += o.order_frames;
    if (!rt_known && o.rt_known) {
      for (int i = 0; i < 3; ++i)
        rt[i] = o.rt[i];
      rt_known = true;
    }
    for (uint32_t s = 0; s < kSlots; ++s)
      cells[s].Add(o.cells[s]);
  }
};

// The rows of a frame or a window, in the order their labels first came.
struct Rows {
  std::vector<Row> rows;
  Row& At(uint32_t label) {
    for (Row& r : rows) {
      if (r.label == label)
        return r;
    }
    rows.emplace_back();
    rows.back().label = label;
    return rows.back();
  }
  void Add(const Rows& o) {
    for (const Row& r : o.rows)
      At(r.label).Add(r);
  }
  void Clear() { rows.clear(); }
};

// What the thread clocks say over a window, for the check against /proc.
struct Totals {
  uint64_t frames = 0;
  uint64_t frame_draws = 0;    // fh1_frame_cost's draws (IssueDraw calls), 0 where not kept
  uint64_t dec_thread_ns = 0;  // the decoder thread's CPU time, swap to swap
  uint64_t rec_thread_ns = 0;  // the recorder thread's (split)
  uint64_t rec_idle_ns = 0;    // the recorder between batches (spinning or asleep)
  uint64_t dec_idle_ns = 0;    // the decoder's CPU outside the ring (waiting for the game)
  uint64_t marks = 0;          // marks read on the decoder
  uint64_t stale_marks = 0;  // marks seen again (a tiled segment's replay): game numbers kept once
  // The deletion switch (0680): draws (and resolves, with `copies`) skipped although their
  // segment's final label is not one it names, and kept although it is.
  uint64_t skip_wrong_skipped = 0, skip_wrong_kept = 0;
  void Add(const Totals& o) {
    frames += o.frames;
    frame_draws += o.frame_draws;
    dec_thread_ns += o.dec_thread_ns;
    rec_thread_ns += o.rec_thread_ns;
    rec_idle_ns += o.rec_idle_ns;
    dec_idle_ns += o.dec_idle_ns;
    marks += o.marks;
    stale_marks += o.stale_marks;
    skip_wrong_skipped += o.skip_wrong_skipped;
    skip_wrong_kept += o.skip_wrong_kept;
  }
};

// A label's name: "p4 #0" (the scene's first segment), "p6 #2" (the third shadow map), "mask
// #1", "start +1", "p4 #0 +3" (the third segment after the scene's), "tail".
inline std::string LabelName(uint32_t label) {
  const uint32_t kind = label >> 24, pass = (label >> 16) & 0xFF, occurrence = (label >> 8) & 0xFF,
                 after = label & 0xFF;
  char text[64];
  switch (kind) {
    case fh::kPassKindStart:
      std::snprintf(text, sizeof(text), "start");
      break;
    case fh::kPassKindPresentation:
      std::snprintf(text, sizeof(text), "p%u #%u", pass, occurrence);
      break;
    case fh::kPassKindShadowMask:
      std::snprintf(text, sizeof(text), "mask #%u", occurrence);
      break;
    case fh::kPassKindCars:
      std::snprintf(text, sizeof(text), "c%u #%u", pass, occurrence);
      break;
    case fh::kPassKindTail:
      return "tail";
    case fh::kPassKindSwap:
      return "swap";
    case 0xFF:
      return "unlabelled";
    default:
      std::snprintf(text, sizeof(text), "label %08X", label);
      return text;
  }
  std::string name = text;
  if (after) {
    std::snprintf(text, sizeof(text), " +%u", after);
    name += text;
  }
  return name;
}

// Sequence numbers of the game's marks: a mark read again (the scene's commands run once per
// EDRAM tile) carries the game's numbers once.
class Sequence {
 public:
  bool New(uint32_t seq) {
    if (started_ && int32_t(seq - last_) <= 0)
      return false;
    started_ = true;
    last_ = seq;
    return true;
  }

 private:
  bool started_ = false;
  uint32_t last_ = 0;
};

// A thread's time between marks: Switch at a mark (the time since the previous point goes to
// the slot that was current), Pause and Resume around the time not to count (the recorder's waits
// between batches; Pause adds the time up to it). Times are a clock's ns (thread CPU, or a steady
// clock where the thread does not wait in between).
class Accrual {
 public:
  void Start(uint64_t now, uint32_t slot, bool running) {
    last_ = now;
    slot_ = slot;
    running_ = running;
  }
  void Switch(uint64_t now, uint32_t slot, uint64_t* ns_by_slot) {
    Add(now, ns_by_slot);
    slot_ = slot;
  }
  void Mark(uint64_t now, uint64_t* ns_by_slot) { Add(now, ns_by_slot); }
  // A new slot without a reading (the time up to here went to the old one already).
  void SetSlot(uint32_t slot) { slot_ = slot; }
  // Returns the time since the previous Pause (the wait it ends).
  uint64_t Resume(uint64_t now) {
    const uint64_t waited = running_ || now < last_ ? 0 : now - last_;
    running_ = true;
    last_ = now;
    return waited;
  }
  void Pause(uint64_t now, uint64_t* ns_by_slot) {
    Add(now, ns_by_slot);
    running_ = false;
  }
  uint32_t slot() const { return slot_; }
  bool running() const { return running_; }

 private:
  void Add(uint64_t now, uint64_t* ns_by_slot) {
    if (!running_)
      return;  // a wait: Resume counts it from the Pause
    if (now > last_)
      ns_by_slot[slot_] += now - last_;
    last_ = now;
  }
  uint64_t last_ = 0;
  uint32_t slot_ = fh::kPassClassNone;
  bool running_ = true;
};

// The CSV: a row per label and draw class with anything in it, a "segment" row per label (the
// numbers of the segment as a whole) and a "frame" row per window (the thread totals).
inline std::string CsvHeader() {
  std::string h =
      "window,epoch,frames,split,label,name,class,layer,segments,order,rdr_cpu_ms,other_device,"
      "rdr_flush_ms,rdr_flush_calls,submitted,runs,dec_ms,dec_draws,dec_skipped,rec_ms,"
      "rec_draws,rec_issue_ms,rec_copies,rec_copy_ms,rec_empty,rt_surface,rt_color,rt_depth,"
      "frame_draws,dec_thread_ms,rec_thread_ms,rec_idle_ms,dec_idle_ms,marks,stale_marks,"
      "delta_draws";
  for (uint32_t g = 0; g < rg::kGroupCount; ++g) {
    if (g == rg::kSystem || g == rg::kFetchConstants)
      continue;  // never counted
    h += ",same_";
    h += rg::GroupName(g);
  }
  h += ",stage_draws";
  for (uint32_t s = 0; s < kStageCount; ++s) {
    h += ",stage_same_";
    h += StageName(s);
  }
  h += ",rec_skipped,rec_skipped_copies,rec_skip_ms,skip_wrong_skipped,skip_wrong_kept";
  return h + "\n";
}

inline std::string CsvLines(uint64_t window, double epoch, bool split, const Rows& rows,
                            const Totals& totals) {
  std::string out;
  char head[96];
  std::snprintf(head, sizeof(head), "%" PRIu64 ",%.3f,%" PRIu64 ",%d,", window, epoch,
                totals.frames, split ? 1 : 0);
  const auto ms = [](uint64_t ns) { return double(ns) * 1e-6; };
  const auto line = [&](const Row* row, const char* cls, uint32_t layer, const Cell& c,
                        bool segment, bool frame) {
    char text[1024];
    std::string name = row ? LabelName(row->label) : std::string("frame");
    for (char& ch : name) {
      if (ch == ',')
        ch = ' ';
    }
    int n = std::snprintf(
        text, sizeof(text),
        "%08X,%s,%s,%u,%" PRIu64 ",%.2f,%.4f,%" PRIu64 ",%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64
        ",%.4f,%" PRIu64 ",%" PRIu64 ",%.4f,%" PRIu64 ",%.4f,%" PRIu64 ",%.4f,%" PRIu64
        ",%08X,%08X,%08X,",
        row ? row->label : 0u, name.c_str(), cls, layer, segment ? row->segments : 0,
        segment && row->order_frames ? double(row->order_sum) / double(row->order_frames) : 0.0,
        segment ? ms(row->rdr_cpu_ns) : 0.0, segment ? row->other_device : 0, ms(c.rdr_flush_ns),
        c.rdr_flush_calls, c.submitted, c.runs, ms(c.dec_ns), c.dec_draws, c.dec_skipped,
        ms(c.rec_ns), c.rec_draws, ms(c.rec_issue_ns), c.rec_copies, ms(c.rec_copy_ns), c.rec_empty,
        segment && row->rt_known ? row->rt[0] : 0u, segment && row->rt_known ? row->rt[1] : 0u,
        segment && row->rt_known ? row->rt[2] : 0u);
    out.append(head);
    out.append(text, size_t(n > 0 ? n : 0));
    n = std::snprintf(text, sizeof(text),
                      "%" PRIu64 ",%.4f,%.4f,%.4f,%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64,
                      frame ? totals.frame_draws : 0, frame ? ms(totals.dec_thread_ns) : 0.0,
                      frame ? ms(totals.rec_thread_ns) : 0.0, frame ? ms(totals.rec_idle_ns) : 0.0,
                      frame ? ms(totals.dec_idle_ns) : 0.0, frame ? totals.marks : 0,
                      frame ? totals.stale_marks : 0, c.delta_draws);
    out.append(text, size_t(n > 0 ? n : 0));
    for (uint32_t g = 0; g < rg::kGroupCount; ++g) {
      if (g == rg::kSystem || g == rg::kFetchConstants)
        continue;
      n = std::snprintf(text, sizeof(text), ",%" PRIu64, c.unchanged[g]);
      out.append(text, size_t(n > 0 ? n : 0));
    }
    n = std::snprintf(text, sizeof(text), ",%" PRIu64, c.stage_draws);
    out.append(text, size_t(n > 0 ? n : 0));
    for (uint32_t s = 0; s < kStageCount; ++s) {
      n = std::snprintf(text, sizeof(text), ",%" PRIu64, c.stage_same[s]);
      out.append(text, size_t(n > 0 ? n : 0));
    }
    n = std::snprintf(text, sizeof(text), ",%" PRIu64 ",%" PRIu64 ",%.4f,%" PRIu64 ",%" PRIu64,
                      c.rec_skipped, c.rec_skipped_copies, ms(c.rec_skip_ns),
                      frame ? totals.skip_wrong_skipped : 0, frame ? totals.skip_wrong_kept : 0);
    out.append(text, size_t(n > 0 ? n : 0));
    out.push_back('\n');
  };
  const Cell none;
  for (const Row& row : rows.rows) {
    line(&row, "segment", 0, none, true, false);
    for (uint32_t s = 0; s < kSlots; ++s) {
      if (!row.cells[s].Empty())
        line(&row, ClassName(SlotClass(s)), SlotLayer(s), row.cells[s], false, false);
    }
  }
  line(nullptr, "frame", 0, none, false, true);
  return out;
}

// The decoder's CPU time of a segment (`cpu_ns`, its thread clock) split among the draw classes
// by their ticks, less the ticks of the waits counted inside them (`wait_ticks`): all of it to
// `current` if no class has ticks left. Adds to `out` (ns per slot) and clears both inputs.
inline void SplitByTicks(uint64_t cpu_ns, uint64_t* ticks, uint64_t* wait_ticks, uint32_t current,
                         uint64_t* out) {
  uint64_t total = 0;
  for (uint32_t s = 0; s < kSlots; ++s) {
    ticks[s] = ticks[s] > wait_ticks[s] ? ticks[s] - wait_ticks[s] : 0;
    wait_ticks[s] = 0;
    total += ticks[s];
  }
  uint64_t given = 0;
  uint32_t largest = current;
  if (total) {
    for (uint32_t s = 0; s < kSlots; ++s) {
      const uint64_t share = uint64_t(double(cpu_ns) * double(ticks[s]) / double(total));
      out[s] += share;
      given += share;
      if (ticks[s] > ticks[largest])
        largest = s;
    }
  }
  // The rounding (or everything, without ticks) to the largest class: the total is exact.
  out[largest] += cpu_ns - given;
  for (uint32_t s = 0; s < kSlots; ++s)
    ticks[s] = 0;
}

// The decoder's comparison of the register groups at a draw: `now` the register file, `then`
// its copy at the previous draw (updated here). Adds 1 to unchanged[group] for every group whose
// values are all the same; the system group and the fetch constants as a whole are not counted.
inline void CompareGroups(const uint32_t* now, uint32_t* then, uint64_t* unchanged) {
  uint32_t changed = 0;
  // The context registers, 64 at a time; a block that differs is looked at register by register.
  for (uint32_t block = rg::kContextFirst; block < rg::kContextEnd; block += 64) {
    bool same = true;
    for (uint32_t i = 0; i < 64; ++i)
      same &= now[block + i] == then[block + i];
    if (same)
      continue;
    for (uint32_t i = block; i < block + 64; ++i) {
      if (now[i] != then[i])
        changed |= 1u << rg::GroupOf(i);
    }
  }
  const auto range_changed = [&](uint32_t first, uint32_t end) {
    bool same = true;
    for (uint32_t i = first; i < end; ++i)
      same &= now[i] == then[i];
    return !same;
  };
  if (range_changed(0x4000, 0x4400))
    changed |= 1u << rg::kVertexConstants;
  if (range_changed(0x4400, 0x4800))
    changed |= 1u << rg::kPixelConstants;
  if (range_changed(0x4900, 0x4928))
    changed |= 1u << rg::kBoolLoopConstants;
  for (uint32_t slot = 0; slot < rg::kFetchSlots; ++slot) {
    const uint32_t at = rg::kFetchFirst + slot * rg::kFetchSlotWords;
    changed |= rg::FetchSlotChange(then + at, now + at);
  }
  for (uint32_t g = 0; g < rg::kGroupCount; ++g) {
    if (g == rg::kSystem || g == rg::kFetchConstants)
      continue;
    if (!(changed & (1u << g)))
      ++unchanged[g];
  }
  for (uint32_t i = rg::kContextFirst; i < rg::kContextEnd; ++i)
    then[i] = now[i];
  for (uint32_t i = rg::kConstantsFirst; i < rg::kConstantsEnd; ++i)
    then[i] = now[i];
}

// FORZA_TEST_SKIP_PASS (0680): which segment labels the deletion switch skips. Parts separated
// by commas: a pass kind by name (the pass numbers of the FH1 track, docs/rendering.md "Pass
// ledger"), a label pattern, or `copies`:
//   shadow   p6 (the sun's shadow maps, with the frame's start before them)
//   prepass  p0 (the depth pre-pass)        cube  p1 (the reflection cube's six faces)
//   scene    p4+0-3 (the 3D scene: its tiles and the tiles' resolves)
//   tiles    p4+0-3 while its commands are read again (the second and later EDRAM tiles)
//   post     p4+4-255 (everything after the scene's tiles: post-processing, bloom, the HUD)
//   mask     every segment of the sun's shadow mask and those after it until the next pass
//   cars     every cars-only segment (c<pass>)
//   start    the frame's segments before its first named one
//   none     nothing (the baseline: the game marks its pass ids as in the other runs)
//   pN, cN   presentation (cars-only) pass N, its named segments; pN+A, pN+A-B the segments
//            A to B after them (0 is the named segment itself)
class SkipSpec {
 public:
  // False (and `bad` the first part it does not know) if a part is not one of the above.
  bool Parse(const std::string& text, std::string* bad) {
    rules_.clear();
    copies_ = none_ = false;
    size_t at = 0;
    while (at <= text.size()) {
      size_t end = text.find(',', at);
      if (end == std::string::npos)
        end = text.size();
      std::string part = text.substr(at, end - at);
      while (!part.empty() && part.front() == ' ')
        part.erase(part.begin());
      while (!part.empty() && part.back() == ' ')
        part.pop_back();
      if (!part.empty() && !AddPart(part)) {
        if (bad)
          *bad = part;
        rules_.clear();
        copies_ = none_ = false;
        return false;
      }
      at = end + 1;
    }
    return true;
  }
  // A segment label, `repeat`: its commands are being read again (a tile after the first).
  bool Matches(uint32_t label, bool repeat) const {
    const uint32_t kind = label >> 24, pass = (label >> 16) & 0xFF, after = label & 0xFF;
    for (const Rule& r : rules_) {
      if (r.kind == kind && (r.pass < 0 || uint32_t(r.pass) == pass) && after >= r.after_min &&
          after <= r.after_max && (!r.repeats_only || repeat))
        return true;
    }
    return false;
  }
  bool copies() const { return copies_; }
  bool empty() const { return rules_.empty(); }
  bool none() const { return none_; }

 private:
  struct Rule {
    uint32_t kind = 0;
    int32_t pass = -1;  // any
    uint32_t after_min = 0, after_max = 0;
    bool repeats_only = false;
  };
  bool AddPart(const std::string& part) {
    const auto add = [&](uint32_t kind, int32_t pass, uint32_t lo, uint32_t hi, bool repeats) {
      rules_.push_back(Rule{kind, pass, lo, hi, repeats});
      return true;
    };
    if (part == "none") {  // nothing skipped: the baseline run, with the pass ids marked
      none_ = true;
      return true;
    }
    if (part == "copies") {
      copies_ = true;
      return true;
    }
    if (part == "shadow")
      return add(fh::kPassKindPresentation, 6, 0, 0, false);
    if (part == "prepass")
      return add(fh::kPassKindPresentation, 0, 0, 0, false);
    if (part == "cube")
      return add(fh::kPassKindPresentation, 1, 0, 0, false);
    if (part == "scene")
      return add(fh::kPassKindPresentation, 4, 0, 3, false);
    if (part == "tiles")
      return add(fh::kPassKindPresentation, 4, 0, 3, true);
    if (part == "post")
      return add(fh::kPassKindPresentation, 4, 4, 255, false);
    if (part == "mask")
      return add(fh::kPassKindShadowMask, -1, 0, 255, false);
    if (part == "cars")
      return add(fh::kPassKindCars, -1, 0, 0, false);
    if (part == "start")
      return add(fh::kPassKindStart, -1, 0, 255, false);
    if (part.size() < 2 || (part[0] != 'p' && part[0] != 'c'))
      return false;
    size_t i = 1;
    uint32_t pass = 0, lo = 0, hi = 0;
    if (!Number(part, i, &pass) || pass > 255)
      return false;
    if (i < part.size()) {
      if (part[i++] != '+' || !Number(part, i, &lo) || lo > 255)
        return false;
      hi = lo;
      if (i < part.size() && (part[i++] != '-' || !Number(part, i, &hi) || hi > 255 || hi < lo))
        return false;
      if (i != part.size())
        return false;
    }
    return add(part[0] == 'p' ? fh::kPassKindPresentation : fh::kPassKindCars, int32_t(pass), lo,
               hi, false);
  }
  static bool Number(const std::string& s, size_t& i, uint32_t* out) {
    const size_t start = i;
    uint32_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && i - start < 4)
      v = v * 10 + uint32_t(s[i++] - '0');
    *out = v;
    return i > start;
  }
  std::vector<Rule> rules_;
  bool copies_ = false, none_ = false;
};

// The recorder's decisions for FORZA_TEST_SKIP_PASS, in stream order: a class mark gives the
// pass id the game knows so far (0: none, the game's marks carry none), a segment end its final
// label. A draw is skipped if its pass id is one the spec names; a resolve too with `copies`. At
// the segment's end the draws (and with `copies` the resolves) whose decision the final label
// contradicts are counted (the game's prediction of a label missed: the frame's structure
// changed). Until the next class mark after a segment end nothing is skipped.
class SkipTracker {
 public:
  explicit SkipTracker(const SkipSpec& spec) : spec_(spec) {}
  void ClassMark(uint32_t pass_id, bool repeat) {
    current_ = pass_id && spec_.Matches(pass_id, repeat);
  }
  // A draw (`copy`: a resolve) to record: true to skip it.
  bool Draw(bool copy) {
    if (copy && !spec_.copies())
      return false;  // not a thing the switch skips: not counted either
    if (current_) {
      ++segment_skipped_;
      ++skipped_[copy];
    } else {
      ++segment_kept_;
    }
    return current_;
  }
  // `keep`: the command processor's own segment ends (the tail before the swap, the swap's
  // work), which the game does not mark: the pass id the game sent last stays.
  void SegmentEnd(uint32_t label, bool repeat, bool keep = false) {
    if (spec_.Matches(label, repeat)) {
      wrong_kept_ += segment_kept_;
    } else {
      wrong_skipped_ += segment_skipped_;
    }
    segment_skipped_ = segment_kept_ = 0;
    if (!keep)
      current_ = false;
  }
  const SkipSpec& spec() const { return spec_; }
  // Since the previous call: the wrong decisions (into a window's totals).
  void TakeWrong(uint64_t* wrong_skipped, uint64_t* wrong_kept) {
    *wrong_skipped += wrong_skipped_;
    *wrong_kept += wrong_kept_;
    all_wrong_ += wrong_skipped_ + wrong_kept_;
    wrong_skipped_ = wrong_kept_ = 0;
  }
  uint64_t skipped_draws() const { return skipped_[0]; }
  uint64_t skipped_copies() const { return skipped_[1]; }
  uint64_t all_wrong() const { return all_wrong_; }

 private:
  SkipSpec spec_;
  bool current_ = false;
  uint64_t segment_skipped_ = 0, segment_kept_ = 0;
  uint64_t wrong_skipped_ = 0, wrong_kept_ = 0, all_wrong_ = 0;
  uint64_t skipped_[2] = {};
};

}  // namespace rex::graphics::fh1_pass_ledger
