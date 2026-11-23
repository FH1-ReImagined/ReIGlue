#pragma once
// FH1 (#551): which draw the GPU was on when the device was lost.
//
// A Vega APU on Windows loses the device after 1-3 races: one submission of the 3D scene
// (500-1000 draws) never finishes and the driver resets the GPU. The log said which
// submission, not which draw. Now every guest draw (and resolve) gets a sequence number; after
// it the GPU writes that number once it has finished it (VK_AMD_buffer_marker), or passes a
// checkpoint holding it (VK_NV_device_diagnostic_checkpoints, which also says how far the top of
// the pipe got). On a device loss the last numbers the GPU wrote are
// looked up here: the last draw finished, the one that never finished, their shaders and the
// submission's draws.
//
// FH1 (#913): the marker after a draw is written once the draw and everything recorded before
// it has finished, so "unfinished" covers the draw and the work the emulator recorded after the
// draw before it (the draw before's own trailing work, a new render pass, render target
// transfers, texture loads, barriers). With
// gpu_device_lost_setup_breadcrumbs (off by default) the entries also get details, kept apart
// from them (SetDetails): the work as counted while it was recorded, the draw's shader
// modifications and render pass, and, for a draw with such work, whether a second marker went
// before the draw command (offset 8 of the buffer, written once the work has finished): the
// report then says which of the two the GPU hung on. A draw with no work recorded between it and
// the draw before (the draw command itself is not counted) needs no marker: the one after the
// draw before already says that everything before it finished. Without the setting nothing of
// this is recorded: Record is what it was, and the details are never written.
//
// Free of the Vulkan headers so that tools/test_device_lost_trail.cpp can test it.
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rex::graphics::vulkan::fh1_device_lost_trail {

enum class Kind : uint8_t { kDraw, kResolve };

// Commands recorded since the previous draw or resolve, in the main command stream. For a draw:
// before the draw command itself; for a resolve: with its own commands. A hoist point counts as
// the hoisted uploads (the shared memory uploads moved before the render pass run there, also
// those recorded for later draws).
struct Work {
  uint16_t render_passes = 0;     // render pass or dynamic rendering begins
  uint16_t render_pass_ends = 0;  // their ends (store and resolve operations)
  uint16_t draws = 0;             // host draws (render target transfers, clears drawn, effects)
  uint16_t dispatches = 0;        // compute (texture loads, resolves, effects)
  uint16_t barriers = 0;
  uint16_t copies = 0;       // buffer and image copies, blits, fills, image clears
  uint16_t clears = 0;       // attachment clears in the render pass
  uint16_t queries = 0;      // query begins, ends and resets
  uint16_t conditional = 0;  // conditional rendering begins
  uint16_t uploads = 0;      // hoist points (the hoisted uploads)
  uint16_t upscales = 0;     // Fh1Dlss passes (DLSS, FSR 3, MetalFX)
  uint16_t test_hangs = 0;   // FORZA_TEST_GPU_HANG_AT_FRAME's wait
  bool Any() const {
    return render_passes || render_pass_ends || draws || dispatches || barriers || copies ||
           clears || queries || conditional || uploads || upscales || test_hangs;
  }
};

// What a draw was drawn with, besides its shaders (0 for a resolve), and the work before it.
struct Details {
  uint64_t vertex_shader_modification = 0;
  uint64_t pixel_shader_modification = 0;
  uint32_t render_pass_key = 0;  // VulkanRenderTargetCache::RenderPassKey::key
  Work work;
  bool setup_marked = false;  // a marker after `work`, before the draw (offset 8)
};

struct Entry {
  uint32_t seq = 0;
  Kind kind = Kind::kDraw;
  uint32_t index_in_submission = 0;  // 1-based, among the submission's draws and resolves
  uint32_t vertex_count = 0;
  uint64_t submission = 0;
  uint64_t frame = 0;
  uint64_t vertex_shader = 0;
  uint64_t pixel_shader = 0;  // 0: none (depth only)
};

struct Range {
  uint64_t submission = 0;
  uint32_t first = 0;
  uint32_t last = 0;
  uint32_t entries = 0;
};

// The last draws and resolves recorded (written by the command processor thread only; read
// once, after a device loss, when nothing is recorded any more).
class Trail {
 public:
  static constexpr uint32_t kEntries = 1u << 14;  // 4 frames of a busy race
  static constexpr uint32_t kRanges = 64;

  // A new sequence number (never 0) for work about to be recorded.
  uint32_t Record(Kind kind, uint64_t submission, uint64_t frame, uint32_t vertex_count,
                  uint64_t vertex_shader, uint64_t pixel_shader) {
    uint32_t seq = ++next_;
    if (!seq) seq = ++next_;  // after 2^32 draws (about 10 hours of racing)
    Range& r = ranges_[submission % kRanges];
    if (r.submission != submission) {
      r.submission = submission;
      r.first = seq;
      r.entries = 0;
    }
    r.last = seq;
    ++r.entries;
    Entry& e = entries_[seq % kEntries];
    e.seq = seq;
    e.kind = kind;
    e.index_in_submission = r.entries;
    e.vertex_count = vertex_count;
    e.submission = submission;
    e.frame = frame;
    e.vertex_shader = vertex_shader;
    e.pixel_shader = pixel_shader;
    return seq;
  }

  // FH1 (#913): the number the next Record gives (for the marker written before a draw is
  // recorded).
  uint32_t next() const { return next_ + 1 ? next_ + 1 : 1; }
  // FH1 (#913): the details of entry `seq` (gpu_device_lost_setup_breadcrumbs only). Kept in an
  // array of their own, so that without the setting Record stores what it stored before and
  // these pages are never touched.
  void SetDetails(uint32_t seq, const Details& details) {
    DetailSlot& slot = details_[seq % kEntries];
    slot.seq = seq;
    slot.details = details;
  }
  const Details* FindDetails(uint32_t seq) const {
    const DetailSlot& slot = details_[seq % kEntries];
    return seq && slot.seq == seq && Find(seq) ? &slot.details : nullptr;
  }

  const Entry* Find(uint32_t seq) const {
    const Entry& e = entries_[seq % kEntries];
    return seq && e.seq == seq ? &e : nullptr;
  }
  const Range* FindSubmission(uint64_t submission) const {
    const Range& r = ranges_[submission % kRanges];
    return submission && r.submission == submission ? &r : nullptr;
  }
  uint32_t last_recorded() const { return next_; }

 private:
  std::array<Entry, kEntries> entries_{};
  std::array<Range, kRanges> ranges_{};
  uint32_t next_ = 0;
  struct DetailSlot {
    uint32_t seq = 0;
    Details details;
  };
  std::array<DetailSlot, kEntries> details_{};
};

// printf into a std::string (no fmt here, for the test).
template <typename... Args>
inline std::string Format(const char* format, Args... args) {
  char buffer[256];
  const int n = std::snprintf(buffer, sizeof(buffer), format, args...);
  return n < 0 ? std::string() : std::string(buffer, size_t(n) < sizeof(buffer) ? size_t(n)
                                                                                 : sizeof(buffer) - 1);
}

inline std::string DescribeEntry(const Trail& trail, uint32_t seq) {
  const Entry* e = trail.Find(seq);
  if (!e) {
    return seq ? Format("#%" PRIu32 " (no longer in the trail)", seq) : std::string("none");
  }
  const Range* r = trail.FindSubmission(e->submission);
  const char* what = e->kind == Kind::kDraw ? "draw" : "resolve";
  std::string where =
      r ? Format("%s %" PRIu32 " of %" PRIu32 " in submission %" PRIu64, what,
                 e->index_in_submission, r->entries, e->submission)
        : Format("%s in submission %" PRIu64, what, e->submission);
  if (e->kind == Kind::kResolve) {
    return Format("#%" PRIu32 " (%s, frame %" PRIu64 ")", seq, where.c_str(), e->frame);
  }
  const std::string ps =
      e->pixel_shader ? Format("%016" PRIX64, e->pixel_shader) : std::string("none");
  return Format("#%" PRIu32 " (%s, frame %" PRIu64 ", VS %016" PRIX64 ", PS %s, %" PRIu32
                " vertices)",
                seq, where.c_str(), e->frame, e->vertex_shader, ps.c_str(), e->vertex_count);
}

// The report. started: the last number the GPU reached at the top of the pipe (0: unknown);
// finished: the last one it finished. The work after `finished` is what the GPU was stuck on
// (or the first of the work it never got to).
inline std::string Describe(const Trail& trail, const char* source, uint32_t started,
                            uint32_t finished) {
  std::string out = Format("GPU breadcrumbs (%s): last finished ", source);
  out += DescribeEntry(trail, finished);
  const uint32_t next = finished + 1 ? finished + 1 : 1;
  if (finished == trail.last_recorded()) {
    out += "; every recorded draw finished (the loss came after them, or outside guest draws)";
  } else {
    out += "; unfinished: " + DescribeEntry(trail, next);
  }
  if (started) {
    out += "; last issued " + DescribeEntry(trail, started);
    if (started >= next && trail.Find(next) && trail.Find(started)) {
      out += Format(" (%" PRIu32 " issued but not finished)", started - finished);
    }
  }
  if (const Entry* e = trail.Find(next)) {
    if (const Range* r = trail.FindSubmission(e->submission)) {
      out += Format("; submission %" PRIu64 " holds #%" PRIu32 "-#%" PRIu32, r->submission,
                    r->first, r->last);
    }
  }
  out += Format("; %" PRIu32 " recorded in all", trail.last_recorded());
  return out;
}

// FH1 (#913): a render pass key (VulkanRenderTargetCache::RenderPassKey) in words: the sample
// count, the depth format and each color target's format, as the guest names them.
inline std::string DescribeRenderPass(uint32_t key) {
  static const char* const kSamples[] = {"1x", "2x", "4x", "?x"};
  static const char* const kColor[16] = {"8_8_8_8",
                                         "8_8_8_8_GAMMA",
                                         "2_10_10_10",
                                         "2_10_10_10_FLOAT",
                                         "16_16",
                                         "16_16_16_16",
                                         "16_16_FLOAT",
                                         "16_16_16_16_FLOAT",
                                         "8",
                                         "9",
                                         "2_10_10_10_AS_10_10_10_10",
                                         "11",
                                         "2_10_10_10_FLOAT_AS_16_16_16_16",
                                         "13",
                                         "32_FLOAT",
                                         "32_32_FLOAT"};
  const uint32_t used = (key >> 2) & 0x1F;
  std::string out = Format("render pass 0x%08" PRIX32 " (%s", key, kSamples[key & 3]);
  if (used & 1)
    out += (key >> 7) & 1 ? ", depth D24FS8" : ", depth D24S8";
  for (uint32_t i = 0; i < 4; ++i) {
    if (used & (2u << i)) {
      out += Format(", color %" PRIu32 " %s", i, kColor[(key >> (8 + 4 * i)) & 0xF]);
    }
  }
  if (!used)
    out += ", no attachments";
  return out + ")";
}

inline std::string DescribeWork(const Work& w) {
  if (!w.Any())
    return "nothing";
  std::string out;
  const auto part = [&out](uint32_t n, const char* one, const char* many) {
    if (!n)
      return;
    out += Format("%s%" PRIu32 " %s", out.empty() ? "" : ", ", n, n == 1 ? one : many);
  };
  part(w.render_passes, "render pass", "render passes");
  part(w.render_pass_ends, "render pass end", "render pass ends");
  part(w.draws, "draw", "draws");
  part(w.dispatches, "dispatch", "dispatches");
  part(w.barriers, "barrier", "barriers");
  part(w.copies, "copy", "copies");
  part(w.clears, "attachment clear", "attachment clears");
  part(w.queries, "query command", "query commands");
  part(w.conditional, "conditional rendering begin", "conditional rendering begins");
  part(w.uploads, "hoisted upload point", "hoisted upload points");
  part(w.upscales, "upscaling pass", "upscaling passes");
  part(w.test_hangs, "test hang", "test hangs");
  return out;
}

// The second line of the report: what the unfinished draw was drawn with, the work recorded
// between it and the last finished entry (before its draw command), and whether the GPU finished
// that work. setup_finished: the last number of a draw whose work before it the GPU finished
// (offset 8; 0: none). setup: gpu_device_lost_setup_breadcrumbs was on (the details were
// recorded); without it the line only names the setting. buffer_markers: `finished` and
// `setup_finished` come from VK_AMD_buffer_marker; VK_NV_device_diagnostic_checkpoints say
// where the checkpoints got, not exactly which work finished, so they get no verdict.
//
// Only the deferred command buffer's work is counted. Between two submissions other work runs
// on the same queue (the presenter's, other submissions), so when the last finished entry is in
// another submission (or gone), a verdict that rests on nothing having been recorded in between
// is not given.
inline std::string DescribeDetails(const Trail& trail, uint32_t finished, uint32_t setup_finished,
                                   bool setup, bool buffer_markers) {
  if (finished == trail.last_recorded())
    return std::string();
  const uint32_t next = finished + 1 ? finished + 1 : 1;
  const Entry* e = trail.Find(next);
  if (!e)
    return std::string();
  const char* const what = e->kind == Kind::kDraw ? "draw" : "resolve";
  const std::string since =
      finished ? Format("#%" PRIu32, finished) : std::string("the start of the trail");
  std::string out = Format("GPU breadcrumbs, unfinished #%" PRIu32 " (%s): ", next, what);
  if (!setup) {
    // (Appended, not formatted: Format's buffer is 256 characters.)
    return out + "whether the GPU hung in it or in the work recorded since " + since +
           " (a render pass, render target transfers, texture loads, barriers) is logged with "
           "gpu_device_lost_setup_breadcrumbs";
  }
  const Details* const d = trail.FindDetails(next);
  if (!d)
    return std::string();
  const Entry* const last = trail.Find(finished);
  const bool across = !last || last->submission != e->submission;
  if (e->kind == Kind::kDraw) {
    out += Format("VS modification %016" PRIX64 ", PS modification %016" PRIX64 ", ",
                  d->vertex_shader_modification, d->pixel_shader_modification);
    out += DescribeRenderPass(d->render_pass_key);
    out += "; recorded since " + since + ", before the draw command: " + DescribeWork(d->work);
  } else {
    out += "recorded since " + since + ", with it: " + DescribeWork(d->work);
  }
  if (across) {
    out += last ? Format("; #%" PRIu32 " is in submission %" PRIu64 ", #%" PRIu32
                         " in submission %" PRIu64,
                         finished, last->submission, next, e->submission)
                : std::string("; the last finished entry is no longer in the trail");
  }
  if (e->kind == Kind::kDraw) {
    if (!buffer_markers) {
      out += "; in that work or in the draw (no verdict with VK_NV_device_diagnostic_checkpoints)";
    } else if (d->setup_marked && setup_finished == next) {
      // The marker before the draw command says that everything before it on the queue finished.
      out += "; the GPU finished that work: it hung in the draw itself";
    } else if (d->setup_marked && !across) {
      out +=
          "; the GPU did not finish that work: it hung in it (or in the draw, if the draw ran "
          "alongside it)";
    } else if (d->setup_marked) {
      out +=
          "; the GPU did not finish that work or the other work on the queue between the two "
          "submissions (the presenter's), which is not counted";
    } else if (!d->work.Any() && !across) {
      out += "; nothing else was recorded since " + since + ": it hung in the draw itself";
    } else {
      out +=
          "; in the draw or in the other work on the queue between the two submissions (the "
          "presenter's), which is not counted";
    }
    if (last && last->kind == Kind::kDraw) {
      if (const Details* l = trail.FindDetails(finished);
          l && l->render_pass_key != d->render_pass_key) {
        out += "; last finished in " + DescribeRenderPass(l->render_pass_key);
      }
    }
  }
  return out;
}

}  // namespace rex::graphics::vulkan::fh1_device_lost_trail
