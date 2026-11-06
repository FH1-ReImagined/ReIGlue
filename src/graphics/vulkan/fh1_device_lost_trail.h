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
// Free of the Vulkan headers so that tools/test_device_lost_trail.cpp can test it.
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rex::graphics::vulkan::fh1_device_lost_trail {

enum class Kind : uint8_t { kDraw, kResolve };

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

}  // namespace rex::graphics::vulkan::fh1_device_lost_trail
