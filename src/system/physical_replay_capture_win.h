#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <span>
#include <vector>

namespace rex::memory::detail {

// Construct and destroy under the allocator lock. All changed aliases stay
// frozen until the transaction ends, even after their bytes were copied.
class FrozenReplayPages {
 public:
  FrozenReplayPages() = default;
  FrozenReplayPages(const FrozenReplayPages&) = delete;
  FrozenReplayPages& operator=(const FrozenReplayPages&) = delete;
  ~FrozenReplayPages();
  bool Copy(const std::array<uint8_t*, 4>& pages, size_t span_bytes, size_t offset,
            std::span<uint8_t> destination);

 private:
  struct Restore {
    uint8_t* page = nullptr;
    size_t bytes = 0;
    DWORD protection = 0;
  };
  bool Freeze(const std::array<uint8_t*, 4>& pages, size_t span_bytes);
  std::vector<Restore> restore_;
};

inline bool FrozenReplayPages::Freeze(const std::array<uint8_t*, 4>& pages, size_t span_bytes) {
  constexpr size_t kPageSize = 4096;
  if (!span_bytes || span_bytes % kPageSize) return false;

  std::array<DWORD, 4> protect{};
  size_t writable = 0;
  for (size_t i = 0; i < pages.size(); ++i) {
    const auto start = reinterpret_cast<uintptr_t>(pages[i]);
    if (!start || start % kPageSize || span_bytes > UINTPTR_MAX - start) return false;
    for (size_t previous = 0; previous < i; ++previous) {
      const auto other = reinterpret_cast<uintptr_t>(pages[previous]);
      if (start < other + span_bytes && other < start + span_bytes) return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pages[i], &info, sizeof(info)) || info.State != MEM_COMMIT ||
        info.Type != MEM_MAPPED ||
        (info.Protect != PAGE_READONLY && info.Protect != PAGE_READWRITE)) {
      return false;
    }
    const auto region = reinterpret_cast<uintptr_t>(info.BaseAddress);
    if (region > start || start - region > info.RegionSize ||
        span_bytes > info.RegionSize - (start - region)) {
      return false;
    }
    protect[i] = info.Protect;
    writable += info.Protect == PAGE_READWRITE;
  }

  // Each span has one uniform protection, so protect it with one system
  // call. Remember exactly the regions this operation changed.
  size_t next = restore_.size();
  restore_.resize(next + writable);
  for (size_t i = 0; i < pages.size(); ++i) {
    if (protect[i] == PAGE_READONLY) continue;
    DWORD previous = 0;
    if (!VirtualProtect(pages[i], span_bytes, PAGE_READONLY, &previous)) return false;
    restore_[next++] = {pages[i], span_bytes, previous};
    if (previous != protect[i]) return false;
  }
  return true;
}

inline FrozenReplayPages::~FrozenReplayPages() {
  for (auto it = restore_.rbegin(); it != restore_.rend(); ++it) {
    if (!it->page) continue;
    DWORD previous = 0;
    if (!VirtualProtect(it->page, it->bytes, it->protection, &previous) ||
        previous != PAGE_READONLY) {
      std::terminate();
    }
  }
}

inline bool FrozenReplayPages::Copy(const std::array<uint8_t*, 4>& pages, size_t span_bytes,
                                    size_t offset, std::span<uint8_t> destination) {
  if (offset >= 4096 || offset >= span_bytes || destination.empty() ||
      destination.size() > span_bytes - offset) {
    return false;
  }
  const auto output = reinterpret_cast<uintptr_t>(destination.data());
  if (destination.size() > UINTPTR_MAX - output) return false;
  for (auto* page : pages) {
    const auto start = reinterpret_cast<uintptr_t>(page);
    if (span_bytes > UINTPTR_MAX - start ||
        (output < start + span_bytes && start < output + destination.size())) {
      return false;
    }
  }
  if (!Freeze(pages, span_bytes)) return false;
  std::memcpy(destination.data(), pages.back() + offset, destination.size());
  return true;
}

}  // namespace rex::memory::detail
