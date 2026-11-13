#pragma once
// FH1 (#858): the bytes the CPU writes into the Vulkan upload rings, by kind, for the log's
// summary once a minute (fh1_device_memory's EndFrame). With the rings in host-visible
// device-local memory (vulkan_rebar_uploads) every byte crosses the bus as a write-combined
// store, so these say which kind costs the draw thread the most.
//
// Each kind is counted from one thread at a time: a relaxed load and store, no locked
// instruction on every draw (a count can be lost when two threads race, which a summary
// tolerates).
#include <atomic>
#include <cstdint>

namespace rex::graphics::vulkan::fh1_upload_stats {

enum class Kind : uint32_t {
  kSystemConstants,
  kFloatConstantsVertex,
  kFloatConstantsPixel,
  kBoolLoopConstants,
  kFetchConstants,
  kConvertedIndices,
  kGuestMemory,
  kCount,
};

struct Stats {
  std::atomic<uint64_t> bytes[size_t(Kind::kCount)];  // zero: static storage
  std::atomic<uint64_t> writes[size_t(Kind::kCount)];
  void Add(Kind kind, uint64_t size) {
    std::atomic<uint64_t>& b = bytes[size_t(kind)];
    std::atomic<uint64_t>& w = writes[size_t(kind)];
    b.store(b.load(std::memory_order_relaxed) + size, std::memory_order_relaxed);
    w.store(w.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }
};

inline Stats& Get() {
  static Stats stats;
  return stats;
}

inline void Add(Kind kind, uint64_t size) { Get().Add(kind, size); }

}  // namespace rex::graphics::vulkan::fh1_upload_stats
