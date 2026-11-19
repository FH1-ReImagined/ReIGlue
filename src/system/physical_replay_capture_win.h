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
    uint8_t* page;
    size_t bytes;
    DWORD protection;
    bool applied = false;
  };
  bool Freeze(const std::array<uint8_t*, 4>& pages, size_t span_bytes);
  std::vector<Restore> restore_;
};

inline bool FrozenReplayPages::Freeze(const std::array<uint8_t*, 4>& pages, size_t span_bytes) {
  constexpr size_t kPageSize = 4096;
  if (!span_bytes || span_bytes % kPageSize) return false;

  const size_t first_restore = restore_.size();
  for (size_t i = 0; i < pages.size(); ++i) {
    const auto start = reinterpret_cast<uintptr_t>(pages[i]);
    if (!start || start % kPageSize || span_bytes > UINTPTR_MAX - start) return false;
    for (size_t previous = 0; previous < i; ++previous) {
      const auto other = reinterpret_cast<uintptr_t>(pages[previous]);
      if (start < other + span_bytes && other < start + span_bytes) return false;
    }
    // An earlier copy may already have sealed a prefix of this span. Keep
    // those pages and seal only the remaining writable runs.
    const uintptr_t end = start + span_bytes;
    for (uintptr_t base = start; base < end;) {
      MEMORY_BASIC_INFORMATION info{};
      auto* address = reinterpret_cast<uint8_t*>(base);
      if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
          info.Type != MEM_MAPPED ||
          (info.Protect != PAGE_READONLY && info.Protect != PAGE_READWRITE)) return false;
      const auto region = reinterpret_cast<uintptr_t>(info.BaseAddress);
      if (region > base || base - region >= info.RegionSize) return false;
      const size_t remaining = info.RegionSize - (base - region);
      const size_t bytes = remaining < end - base ? remaining : end - base;
      if (info.Protect == PAGE_READWRITE)
        restore_.push_back({address, bytes, PAGE_READWRITE});
      base += bytes;
    }
  }

  // Preflight and allocate the complete restoration list before changing
  // any protection for this copy.
  for (size_t i = first_restore; i < restore_.size(); ++i) {
    auto& run = restore_[i];
    DWORD previous = 0;
    if (!VirtualProtect(run.page, run.bytes, PAGE_READONLY, &previous)) return false;
    run.protection = previous;
    run.applied = true;
    if (previous != PAGE_READWRITE) return false;
  }
  return true;
}

inline FrozenReplayPages::~FrozenReplayPages() {
  for (auto it = restore_.rbegin(); it != restore_.rend(); ++it) {
    if (!it->applied) continue;
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
