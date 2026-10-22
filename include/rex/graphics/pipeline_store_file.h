#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

// The pipeline description store, <title>.<path>.vk.xpso: a header, then fixed-size records
// (the XXH3 of the description, then the description), appended as pipelines are created.
namespace rex::graphics::pipeline_store_file {

// 'XEPS'.
constexpr uint32_t kMagic = 0x53504558;
// Format 2 keeps the description and shader modification versions apart: the original
// 12-byte header holds only the larger of the two, so raising the smaller one went unnoticed
// and left records the code no longer understands.
constexpr uint32_t kFormat2 = 2;
constexpr size_t kLegacyHeaderSize = 12;
constexpr size_t kHeaderSize = 20;

struct Versions {
  uint32_t description = 0;
  uint32_t modification = 0;
};

// Versions are stored big-endian, as in the original header; magic and API are in host order.
inline uint32_t SwapBytes(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

inline std::array<uint8_t, kHeaderSize> EncodeHeader(uint32_t api, Versions versions) {
  const uint32_t words[5] = {kMagic, api, SwapBytes(kFormat2), SwapBytes(versions.description),
                             SwapBytes(versions.modification)};
  std::array<uint8_t, kHeaderSize> header;
  std::memcpy(header.data(), words, kHeaderSize);
  return header;
}

enum class Header {
  // No store for these versions: start an empty one.
  kNone,
  kCurrent,
  // The original header, written with the same versions (the larger one matches): the records
  // are valid and follow at kLegacyHeaderSize; rewrite the store in format 2.
  kLegacy,
};
inline size_t HeaderSize(Header header) {
  return header == Header::kLegacy ? kLegacyHeaderSize : kHeaderSize;
}

// Reads the header from the start of `file`, leaving the file at the first record.
inline Header ReadHeader(std::FILE* file, uint32_t api, Versions versions) {
  std::rewind(file);
  uint32_t words[5];
  if (std::fread(words, sizeof(uint32_t), 3, file) != 3 || words[0] != kMagic ||
      words[1] != api) {
    return Header::kNone;
  }
  if (words[2] == SwapBytes(kFormat2)) {
    return std::fread(words + 3, sizeof(uint32_t), 2, file) == 2 &&
                   words[3] == SwapBytes(versions.description) &&
                   words[4] == SwapBytes(versions.modification)
               ? Header::kCurrent
               : Header::kNone;
  }
  return words[2] == SwapBytes(std::max(versions.description, versions.modification))
             ? Header::kLegacy
             : Header::kNone;
}

template <typename Record>
struct LoadResult {
  // In file order.
  std::vector<Record> kept;
  // Valid records this device cannot create.
  std::vector<Record> unsupported;
  // Records from the first one that fails its hash to the end of the file.
  size_t corrupted = 0;
  // A partial record at the end (a write cut short).
  bool partial = false;
  bool NeedsRewrite(Header header) const {
    return header != Header::kCurrent || !unsupported.empty() || corrupted || partial;
  }
};

// Reads the records that follow the header. A record failing `valid` (its hash) ends the
// store. A record `supported` rejects is left out; all others are kept, in order.
template <typename Record, typename Valid, typename Supported>
LoadResult<Record> LoadRecords(std::FILE* file, Valid valid, Supported supported) {
  LoadResult<Record> result;
  std::vector<uint8_t> bytes;
  uint8_t buffer[65536];
  size_t read;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0) {
    bytes.insert(bytes.end(), buffer, buffer + read);
  }
  const size_t count = bytes.size() / sizeof(Record);
  result.partial = bytes.size() % sizeof(Record) != 0;
  for (size_t i = 0; i < count; ++i) {
    Record record;
    std::memcpy(&record, bytes.data() + i * sizeof(Record), sizeof(Record));
    if (!valid(record)) {
      result.corrupted = count - i;
      break;
    }
    (supported(record) ? result.kept : result.unsupported).push_back(record);
  }
  return result;
}

// Replaces the store at `path` with a format-2 header and `records`. They are written to a
// temporary file that is then renamed over the store, so a crash leaves the old or the new
// store, never a partial one. `file` is closed and reopened for appending (the old store if
// the replacement failed; nullptr if it cannot be reopened). `open` is fopen-like.
template <typename Record, typename Open>
bool Rewrite(const std::filesystem::path& path, std::FILE*& file, uint32_t api, Versions versions,
             const std::vector<Record>& records, Open open) {
  auto temporary = path;
  temporary += ".new";
  std::error_code ec;
  std::FILE* out = open(temporary, "wb");
  if (!out) return false;
  const auto header = EncodeHeader(api, versions);
  bool written = std::fwrite(header.data(), header.size(), 1, out) == 1 &&
                 (records.empty() || std::fwrite(records.data(), sizeof(Record), records.size(),
                                                 out) == records.size());
  written = std::fflush(out) == 0 && written;
  written = std::fclose(out) == 0 && written;
  if (!written) {
    std::filesystem::remove(temporary, ec);
    return false;
  }
  // Windows cannot replace a file that is open.
  if (file) std::fclose(file);
  std::filesystem::rename(temporary, path, ec);
  const bool replaced = !ec;
  if (!replaced) std::filesystem::remove(temporary, ec);
  file = open(path, "a+b");
  return replaced && file;
}

}  // namespace rex::graphics::pipeline_store_file
