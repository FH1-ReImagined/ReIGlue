// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <atomic>
#include <cstdint>

namespace rex::ui::vulkan {
// Observations of the last fully constructed connection. During construction
// and retirement both are false. This does not certify GPU completion.
struct HdrOutputState {
  bool offered = false;
  bool active = false;
};

// The presenter serializes connection changes; other threads may read status.
// Publish a complete state in one atomic operation, never independent flags.
// This is diagnostic/policy information, not a resource-lifetime guard.
class HdrOutputPublication {
 public:
  void Clear() { mode_.store(Mode::kUnavailable, std::memory_order_relaxed); }

  // Call only after the connection, including all dependent resources, succeeds.
  // Inconsistent capability/format observations cannot advertise active HDR.
  void CommitConnection(bool offered, bool active) {
    const auto mode = !offered ? Mode::kUnavailable
                             : active ? Mode::kHdr : Mode::kSdrWithHdrAvailable;
    mode_.store(mode, std::memory_order_relaxed);
  }

  HdrOutputState Read() const {
    const auto mode = mode_.load(std::memory_order_relaxed);
    return {mode != Mode::kUnavailable, mode == Mode::kHdr};
  }

 private:
  enum class Mode : uint8_t { kUnavailable, kSdrWithHdrAvailable, kHdr };
  std::atomic<Mode> mode_{Mode::kUnavailable};
};
}  // namespace rex::ui::vulkan
