#pragma once
#include <array>
#include <cstdint>

namespace rex::input::mnk {
// A state-polling title cannot see a complete press/release between polls.
// Retain only resolved digital controller binds, never steering or triggers.
// Times are monotonic nanoseconds, supplied by the caller for deterministic tests.
class DigitalTaps {
 public:
  static constexpr uint32_t kMask = 0xFF3F;    // PadBinds[6,7] are analog triggers.
  static constexpr int64_t kFrame = 34000000;  // One frame at 30 Hz.
  static constexpr int64_t kExpiry = 100000000;
  void Observe(uint32_t live, int64_t now) {
    live &= kMask;
    const uint32_t rising = live & ~physical_;
    for (unsigned i = 0; i < edges_.size(); ++i) {
      if (rising & (uint32_t(1) << i)) {
        // Coalesce another tap inside the retained frame; do not replay a
        // second action later or extend an already retained pulse.
        if (edges_[i].until <= now && (!edges_[i].pending || edges_[i].expires <= now))
          edges_[i] = {now + kExpiry, 0, true};
      }
    }
    physical_ = live;
  }
  uint32_t Sample(uint32_t live, int64_t now) {
    Observe(live, now);
    uint32_t result = live;
    for (unsigned i = 0; i < edges_.size(); ++i) {
      auto& edge = edges_[i];
      if (edge.pending && now < edge.expires) {
        edge.until = now + kFrame;
      }
      edge.pending = false;
      if (now < edge.until)
        result |= uint32_t(1) << i;
    }
    return result;
  }
  void Reset() {
    edges_ = {};
    physical_ = 0;
  }

 private:
  struct Edge {
    int64_t expires = 0, until = 0;
    bool pending = false;
  };
  std::array<Edge, 16> edges_{};
  uint32_t physical_ = 0;
};
}  // namespace rex::input::mnk
