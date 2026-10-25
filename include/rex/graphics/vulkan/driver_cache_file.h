#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace rex::graphics::vulkan::driver_cache_file {
// Files contain opaque, locally generated driver data. Never distribute them.
//
// One entry per pipeline description: the data of a VkPipelineCache that created only that
// pipeline. A single cache seeded with all of the last session's data keeps everything it was
// given, as drivers do not evict: each pipeline the driver did not find in it again (after a
// translator change, or when a driver keys it differently) was added a second time at every
// start, and dead data was never dropped. Entries are replaced instead, and entries of
// descriptions no longer in the pipeline store are dropped, so the file follows the store.
constexpr uint32_t kFormatVersion = 2;
constexpr size_t kHeaderSize = 64;
constexpr size_t kEntryHeaderSize = 24;
// One pipeline's opaque driver data; size varies substantially by driver.
constexpr size_t kMaxEntrySize = 16 * 1024 * 1024;
// Bound disk usage even when a driver produces megabytes per pipeline (#406).
constexpr uint64_t kMaxFileSize = uint64_t(512) * 1024 * 1024;

struct Identity {
  uint32_t vendor = 0, device = 0, driver = 0;
  uint64_t abi = 0;
  std::array<uint8_t, 16> uuid{};
};

struct Entry {
  // XXH3 of the pipeline description, as in the pipeline store.
  uint64_t key = 0;
  // Size of the data when the pipeline was last created from an empty cache.
  uint32_t fresh_size = 0;
  // Of the data.
  uint64_t checksum = 0;
  std::shared_ptr<const std::vector<uint8_t>> data;
  size_t size() const { return data ? data->size() : 0; }
};

inline uint32_t U32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t U64(const uint8_t* p) { return U32(p) | uint64_t(U32(p + 4)) << 32; }
inline void Put32(uint8_t* p, uint32_t v) {
  for (size_t i = 0; i < 4; ++i) p[i] = uint8_t(v >> (i * 8));
}
inline void Put64(uint8_t* p, uint64_t v) { Put32(p, uint32_t(v)); Put32(p + 4, uint32_t(v >> 32)); }
inline uint64_t Checksum(const uint8_t* data, size_t size,
                         uint64_t hash = UINT64_C(14695981039346656037)) {
  for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * UINT64_C(1099511628211);
  return hash;
}
// Driver data from this device that holds more than its header.
inline bool ValidPayload(const uint8_t* p, size_t size, const Identity& id) {
  // Vulkan's VERSION_ONE header uses little-endian fields independent of the host.
  return size >= 32 && size <= kMaxEntrySize && U32(p) >= 32 && U32(p) < size &&
      U32(p + 4) == 1 && U32(p + 8) == id.vendor && U32(p + 12) == id.device &&
      std::equal(id.uuid.begin(), id.uuid.end(), p + 16);
}
inline bool ValidPayload(const std::vector<uint8_t>& p, const Identity& id) {
  return ValidPayload(p.data(), p.size(), id);
}
inline Entry MakeEntry(uint64_t key, std::vector<uint8_t> data, size_t fresh_size) {
  Entry entry;
  entry.key = key;
  entry.fresh_size = uint32_t(std::min(fresh_size, kMaxEntrySize));
  entry.checksum = Checksum(data.data(), data.size());
  entry.data = std::make_shared<const std::vector<uint8_t>>(std::move(data));
  return entry;
}

// What a pipeline's cache, seeded with its stored entry (seed_size 0: none), holds after the
// pipeline was created. Equal data: the driver used the entry. Data much larger than a fresh
// entry: the driver did not find the pipeline in the entry and added it, so the entry is stale
// and must be rebuilt from an empty cache rather than kept growing.
enum class Outcome { kReused, kStored, kStale };
inline size_t StaleLimit(size_t fresh_size) { return fresh_size + fresh_size / 2 + 4096; }
inline Outcome Classify(const Entry* seed, const std::vector<uint8_t>& data) {
  if (!seed || !seed->data) return Outcome::kStored;
  if (data == *seed->data) return Outcome::kReused;
  const size_t fresh = seed->fresh_size ? seed->fresh_size : seed->size();
  return data.size() > StaleLimit(fresh) ? Outcome::kStale : Outcome::kStored;
}

inline std::array<uint8_t, kHeaderSize> EncodeHeader(const Identity& id, uint64_t count) {
  std::array<uint8_t, kHeaderSize> h{};
  const uint8_t magic[8] = {'F','H','1','V','K','P','C',0};
  std::copy_n(magic, 8, h.begin());
  Put32(h.data() + 8, kFormatVersion);
  Put32(h.data() + 12, id.vendor); Put32(h.data() + 16, id.device);
  Put32(h.data() + 20, id.driver); Put64(h.data() + 24, id.abi);
  std::copy(id.uuid.begin(), id.uuid.end(), h.begin() + 32);
  Put64(h.data() + 48, count);
  Put64(h.data() + 56, Checksum(h.data(), 56));
  return h;
}
inline bool DecodeHeader(const uint8_t* h, const Identity& id, uint64_t* count) {
  const uint8_t magic[8] = {'F','H','1','V','K','P','C',0};
  if (!std::equal(magic, magic + 8, h) || U32(h + 8) != kFormatVersion ||
      U32(h + 12) != id.vendor || U32(h + 16) != id.device || U32(h + 20) != id.driver ||
      U64(h + 24) != id.abi || !std::equal(id.uuid.begin(), id.uuid.end(), h + 32) ||
      U64(h + 56) != Checksum(h, 56)) return false;
  *count = U64(h + 48);
  return true;
}

struct ReadResult {
  std::vector<Entry> entries;
  // The file exists but was not written for this identity (another driver or game version,
  // the format-1 file of a single cache) or is damaged. Valid entries before damage are kept.
  bool rejected = false;
  uint64_t file_size = 0;
};
inline ReadResult Read(const std::filesystem::path& path, const Identity& id) {
  ReadResult result;
  // The size of the file opened, which a concurrent save may have replaced since.
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return result;
  const auto end = file.tellg();
  if (end < 0) return result;
  const uint64_t size = uint64_t(end);
  result.file_size = size;
  if (size < kHeaderSize || size > kMaxFileSize) { result.rejected = true; return result; }
  file.seekg(0);
  std::array<uint8_t, kHeaderSize> header;
  uint64_t count = 0;
  if (!file.read(reinterpret_cast<char*>(header.data()), kHeaderSize) ||
      !DecodeHeader(header.data(), id, &count)) {
    result.rejected = true;
    return result;
  }
  uint64_t remaining = size - kHeaderSize;
  for (uint64_t i = 0; i < count; ++i) {
    uint8_t eh[kEntryHeaderSize];
    if (remaining < kEntryHeaderSize || !file.read(reinterpret_cast<char*>(eh), kEntryHeaderSize)) break;
    remaining -= kEntryHeaderSize;
    const uint32_t entry_size = U32(eh + 12);
    if (entry_size > kMaxEntrySize || entry_size > remaining) break;
    std::vector<uint8_t> data(entry_size);
    if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(entry_size))) break;
    remaining -= entry_size;
    // The entry checksum covers the data, then the key and sizes.
    const uint64_t checksum = Checksum(data.data(), data.size());
    if (Checksum(eh, 16, checksum) != U64(eh + 16) || !ValidPayload(data, id)) break;
    Entry entry;
    entry.key = U64(eh);
    entry.fresh_size = U32(eh + 8);
    entry.checksum = checksum;
    entry.data = std::make_shared<const std::vector<uint8_t>>(std::move(data));
    result.entries.push_back(std::move(entry));
  }
  if (result.entries.size() != count || remaining) result.rejected = true;
  return result;
}

struct WriteResult {
  bool written = false;
  // Counts include only valid entries; skipped entries did not fit the file budget.
  size_t entries = 0;
  size_t skipped = 0;
  uint64_t bytes = 0;
};
inline WriteResult Write(const std::filesystem::path& path, const std::vector<Entry>& entries,
                         const Identity& id, uint64_t budget = kMaxFileSize) {
  WriteResult result;
  budget = std::min(budget, kMaxFileSize);
  if (budget < kHeaderSize) return result;
  std::vector<const Entry*> valid;
  valid.reserve(entries.size());
  uint64_t bytes = kHeaderSize;
  for (const auto& entry : entries) {
    if (!entry.data || !ValidPayload(*entry.data, id)) continue;
    const uint64_t entry_bytes = kEntryHeaderSize + entry.size();
    // The caller sorts by description key, so the retained subset is stable across
    // starts. Skip whole entries, then keep looking for smaller ones that fit.
    if (entry_bytes > budget - bytes) { ++result.skipped; continue; }
    valid.push_back(&entry);
    bytes += entry_bytes;
  }
  result.bytes = bytes;
  result.entries = valid.size();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) return result;
  static std::atomic<uint64_t> serial{0};
#ifdef _WIN32
  const uint64_t pid = GetCurrentProcessId();
#else
  const uint64_t pid = uint64_t(getpid());
#endif
  auto temporary = path;
  temporary += ".tmp-" + std::to_string(pid) + "-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial++);
  bool written = false;
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    const auto header = EncodeHeader(id, valid.size());
    bool ok = bool(file.write(reinterpret_cast<const char*>(header.data()), kHeaderSize));
    for (const Entry* entry : valid) {
      if (!ok) break;
      uint8_t eh[kEntryHeaderSize];
      Put64(eh, entry->key);
      Put32(eh + 8, entry->fresh_size);
      Put32(eh + 12, uint32_t(entry->size()));
      Put64(eh + 16, Checksum(eh, 16, entry->checksum));
      ok = file.write(reinterpret_cast<const char*>(eh), kEntryHeaderSize) &&
          file.write(reinterpret_cast<const char*>(entry->data->data()), std::streamsize(entry->size()));
    }
    if (ok) {
      file.flush();
      if (file) { file.close(); written = !file.fail(); }
    }
  }
  if (written) {
#ifdef _WIN32
    written = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::filesystem::rename(temporary, path, ec); written = !ec;
#endif
  }
  if (!written) std::filesystem::remove(temporary, ec);
  result.written = written;
  return result;
}
}  // namespace rex::graphics::vulkan::driver_cache_file
