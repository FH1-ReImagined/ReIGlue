#pragma once
// FH1 (#268): how much work one Vulkan submission holds, and the submissions the GPU was slow
// to finish.
//
// Windows resets the GPU when one submission keeps it from switching to other work for 2 s
// (TDR, "device lost"). On a Vega APU with its carve-out nearly full the game's frames took
// 50 ms, now and then 200-450 ms, and then one submission 3.8 s. So on integrated GPUs a
// submission is ended after kIntegratedDrawLimit draws (vulkan_submission_draw_limit): the
// next draw starts a new one, as after a resolve that is copied back. And waits of the
// command processor for the GPU of kSlowWaitMs or more are logged with the draws of the
// submission waited for, so a player's log shows whether a slow submission held much work.
//
// Free of the Vulkan headers so that tools/test_submission_watch.cpp can test it.
#include <array>
#include <cstdint>

namespace rex::graphics::vulkan::fh1_submission_watch {

constexpr int32_t kIntegratedDrawLimit = 1000;
constexpr int64_t kSlowWaitMs = 250;
constexpr int64_t kSlowLogIntervalMs = 10000;
constexpr uint32_t kMaxSlowLogs = 50;

// The draws a submission may hold: the setting (-1 = auto: kIntegratedDrawLimit on an
// integrated GPU with a small carve-out, else no limit; 0 = no limit).
inline uint32_t DrawLimit(int32_t setting, bool integrated) {
  if (setting >= 0) return uint32_t(setting);
  return integrated ? uint32_t(kIntegratedDrawLimit) : 0;
}

// Whether to end the open submission before the next draw.
inline bool ShouldSplit(uint32_t draws_in_submission, uint32_t limit) {
  return limit && draws_in_submission >= limit;
}

// The last submissions: their draws and when they were handed to the GPU.
class Watch {
 public:
  struct Record {
    uint64_t submission = 0;
    uint32_t draws = 0;
    int64_t submitted_ms = 0;
  };

  void Submitted(uint64_t submission, uint32_t draws, int64_t now_ms) {
    Record& r = records_[submission % records_.size()];
    r.submission = submission;
    r.draws = draws;
    r.submitted_ms = now_ms;
  }

  // The record of a submission among the last ones, or nullptr.
  const Record* Find(uint64_t submission) const {
    const Record& r = records_[submission % records_.size()];
    return r.submission == submission && submission ? &r : nullptr;
  }

  // A wait for the GPU ended after `waited_ms`: whether to log it (slow, and not too often).
  bool ShouldLogWait(int64_t waited_ms, int64_t now_ms) {
    if (waited_ms < slow_wait_ms_) return false;
    ++slow_waits_;
    if (logged_ >= kMaxSlowLogs || (logged_ && now_ms < last_log_ms_ + kSlowLogIntervalMs)) {
      return false;
    }
    ++logged_;
    last_log_ms_ = now_ms;
    return true;
  }
  // Slow waits since the last logged one (that one included).
  uint32_t TakeSlowWaits() {
    const uint32_t n = slow_waits_;
    slow_waits_ = 0;
    return n;
  }
  // Tests: log shorter waits too (FORZA_TEST_SLOW_WAIT_MS).
  void SetSlowWaitMs(int64_t ms) { slow_wait_ms_ = ms; }

 private:
  std::array<Record, 64> records_{};
  int64_t slow_wait_ms_ = kSlowWaitMs;
  uint32_t slow_waits_ = 0;
  uint32_t logged_ = 0;
  int64_t last_log_ms_ = 0;
};

}  // namespace rex::graphics::vulkan::fh1_submission_watch
