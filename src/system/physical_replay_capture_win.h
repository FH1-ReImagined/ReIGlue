#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <span>

namespace rex::memory::detail {

// The caller serializes allocation and mapping changes and supplies every
// writable alias of the selected physical range.
template <typename Read>
bool WithFrozenReplayAliases(const std::array<uint8_t*, 4>& pages, size_t span_bytes, Read read) {
  constexpr size_t kPageSize = 4096;
  if (!span_bytes || span_bytes % kPageSize) return false;
  static_assert(noexcept(read()));

  std::array<DWORD, 4> protect{};
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
  }

  // Each span has one uniform protection, so protect it with one system
  // call. Remember exactly the regions this operation changed.
  std::array<DWORD, 4> restore{};
  bool frozen = true;
  for (size_t i = 0; i < pages.size(); ++i) {
    if (protect[i] == PAGE_READONLY) continue;
    if (!VirtualProtect(pages[i], span_bytes, PAGE_READONLY, &restore[i])) {
      restore[i] = 0;
      frozen = false;
      break;
    }
    if (restore[i] != protect[i]) {
      frozen = false;
      break;
    }
  }
  if (frozen) read();
  for (size_t i = pages.size(); i-- > 0;) {
    if (!restore[i]) continue;
    DWORD previous = 0;
    if (!VirtualProtect(pages[i], span_bytes, restore[i], &previous) ||
        previous != PAGE_READONLY) {
      std::terminate();
    }
  }
  return frozen;
}

inline bool CopyFrozenReplayAliases(const std::array<uint8_t*, 4>& pages, size_t span_bytes,
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
  return WithFrozenReplayAliases(pages, span_bytes, [&]() noexcept {
    std::memcpy(destination.data(), pages.back() + offset, destination.size());
  });
}

}  // namespace rex::memory::detail
