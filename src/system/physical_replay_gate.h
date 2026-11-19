#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace rex::memory::detail {

// These writers can write through physical aliases, including the raw view
// used by file I/O. Hold only the count during a write. A snapshot refuses
// active writers and closes admission for the duration of the short copy.
class PhysicalReplayGate {
 public:
  class Write {
   public:
    Write(PhysicalReplayGate& gate, bool physical) : gate_(physical ? &gate : nullptr) {
      if (gate_) gate_->EnterWrite();
    }
    ~Write() {
      if (gate_) gate_->LeaveWrite();
    }
    Write(const Write&) = delete;
    Write& operator=(const Write&) = delete;

   private:
    PhysicalReplayGate* gate_;
  };

  void EnterWrite() {
    std::lock_guard lock(mutex_);
    ++writers_;
  }

  void LeaveWrite() {
    std::lock_guard lock(mutex_);
    --writers_;
  }

  std::unique_lock<std::mutex> TryCapture() {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (lock && writers_) lock.unlock();
    return lock;
  }

 private:
  std::mutex mutex_;
  std::size_t writers_ = 0;
};

inline PhysicalReplayGate physical_replay_gate;

template <typename Capture>
bool WithReplayCaptureLocks(PhysicalReplayGate& gate, std::recursive_mutex& allocator,
                            Capture&& capture) {
  auto writers_closed = gate.TryCapture();
  if (!writers_closed) return false;
  // A writer may enter while holding the allocator lock.
  std::unique_lock allocation_stable(allocator, std::try_to_lock);
  return allocation_stable && capture();
}

// The 7F writeback view and A/C/E physical views from Memory::MapViews.
constexpr bool WritesPhysicalAlias(uint32_t address, uint32_t length) {
  const uint64_t end = uint64_t(address) + length;
  return length &&
         ((address < 0x80000000u && end > 0x7f000000u) || end > 0xa0000000u);
}

}  // namespace rex::memory::detail
