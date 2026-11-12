#pragma once

// FH1 (patch 0600): the shader pack, <storage root>/shaders/translated/<title>.<key>.vk.xspv.
//
// The SPIR-V the Vulkan pipeline cache's translator made for (microcode hash, modification) pairs,
// with the texture and sampler bindings the translation reports, so that a start takes the
// modules from the file instead of translating them. The launcher's "Prebuild shaders" step
// writes it (shader_prebuild_only), from the player's own shaders, on the player's PC: the files
// hold data derived from the game's shaders and are never distributed.
//
// SPIR-V depends on more than the pair (the device's features, the render target path, the draw
// resolution and raster scales, a few cvars: SpirvShaderTranslator::DescribeConfiguration), and
// on the translator's code. A pack carries that as its identity text: the SDK build and the
// configuration. A reader uses a pack only when the identity is exactly its own, so a pack of
// another build, device or setting is never used (its file name differs too: the key is a hash
// of the identity).
//
// Layout (little-endian):
//   header (64 bytes): magic "FH1XSPV", format version, modification version, pair count,
//     blob count, identity size, binding bytes, blob region size, the metadata checksum (identity,
//     pairs, blob table and bindings), the header's own checksum;
//   identity text;
//   pairs (32 bytes each, sorted by hash, then modification): hash, modification, blob index,
//     binding offset, texture binding count, sampler binding count;
//   blobs (24 bytes each): offset in the blob region, encoded size, SPIR-V words, checksum of the
//     encoded bytes;
//   bindings: per pair, a byte per texture binding, then two bytes per sampler binding;
//   the blob region: each distinct SPIR-V module once (pairs can share one), its 32-bit words as
//     LEB128 (SPIR-V is mostly small numbers: half the size, decoded at memory speed).
//
// Header only and without SDK types, so tests need neither a GPU nor the plugin.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace rex::graphics::vulkan::shader_pack_file {

constexpr uint32_t kFormatVersion = 1;
constexpr size_t kHeaderSize = 64;
constexpr size_t kPairSize = 32;
constexpr size_t kBlobSize = 24;
constexpr uint32_t kMaxPairs = 1u << 20;
constexpr uint32_t kMaxIdentitySize = 64 * 1024;
// The largest FH1 module is 116 KB; a module of 64 MB is damage.
constexpr uint32_t kMaxBlobWords = 16u << 20;
constexpr uint64_t kMaxFileSize = uint64_t(2) << 30;
constexpr char kMagic[8] = {'F', 'H', '1', 'X', 'S', 'P', 'V', 0};
constexpr std::string_view kExtension = ".vk.xspv";

inline uint32_t U16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
inline uint32_t U32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t U64(const uint8_t* p) { return U32(p) | uint64_t(U32(p + 4)) << 32; }
inline void Put16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void Put32(uint8_t* p, uint32_t v) {
  for (size_t i = 0; i < 4; ++i) p[i] = uint8_t(v >> (i * 8));
}
inline void Put64(uint8_t* p, uint64_t v) { Put32(p, uint32_t(v)); Put32(p + 4, uint32_t(v >> 32)); }

// A checksum against damage (not a cryptographic hash): 8 bytes a step, four independent lanes.
inline uint64_t Checksum(const uint8_t* data, size_t size, uint64_t seed = 0) {
  constexpr uint64_t k1 = UINT64_C(0x9E3779B185EBCA87), k2 = UINT64_C(0xC2B2AE3D27D4EB4F);
  auto rotl = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
  uint64_t lane[4] = {seed + k1, seed ^ k2, seed - k1, rotl(seed, 7) + k2};
  size_t i = 0;
  for (; i + 32 <= size; i += 32) {
    for (size_t l = 0; l < 4; ++l) {
      uint64_t v;
      std::memcpy(&v, data + i + l * 8, 8);
      lane[l] = rotl(lane[l] + v * k2, 31) * k1;
    }
  }
  uint64_t h = rotl(lane[0], 1) + rotl(lane[1], 7) + rotl(lane[2], 12) + rotl(lane[3], 18) +
               uint64_t(size) * k1;
  for (; i + 8 <= size; i += 8) {
    uint64_t v;
    std::memcpy(&v, data + i, 8);
    h = rotl(h ^ (rotl(v * k2, 31) * k1), 27) * k1 + k2;
  }
  for (; i < size; ++i) h = rotl(h ^ (uint64_t(data[i]) * k1), 11) * k2;
  h ^= h >> 33;
  h *= k2;
  h ^= h >> 29;
  h *= k1;
  return h ^ (h >> 32);
}

// SPIR-V words (little-endian bytes, as the translator returns them) as LEB128.
inline void EncodeWords(const uint8_t* spirv, size_t words, std::vector<uint8_t>& out) {
  out.clear();
  out.reserve(words * 2);
  for (size_t i = 0; i < words; ++i) {
    uint32_t w = U32(spirv + i * 4);
    while (w >= 0x80) {
      out.push_back(uint8_t(w | 0x80));
      w >>= 7;
    }
    out.push_back(uint8_t(w));
  }
}
// Exactly `words` words that use exactly `size` bytes, or false.
inline bool DecodeWords(const uint8_t* data, size_t size, size_t words, std::vector<uint8_t>& out) {
  out.resize(words * 4);
  uint8_t* o = out.data();
  size_t p = 0;
  for (size_t i = 0; i < words; ++i) {
    uint32_t w = 0;
    for (int shift = 0;; shift += 7) {
      if (p >= size || shift > 28) return false;
      const uint8_t b = data[p++];
      w |= uint32_t(b & 0x7F) << shift;
      if (!(b & 0x80)) break;
    }
    Put32(o + i * 4, w);
  }
  return p == size;
}

// A file's identity for a pack's build word: its size and checksum. The Vulkan pipeline cache
// takes the GPU plugin's own file, which the translator is compiled into. "" if unreadable.
inline std::string FileId(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return {};
  const auto end = file.tellg();
  if (end <= 0 || uint64_t(end) > kMaxFileSize) return {};
  std::vector<uint8_t> data(static_cast<size_t>(end));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()))) return {};
  static const char kHex[] = "0123456789abcdef";
  const uint64_t sum = Checksum(data.data(), data.size());
  std::string id;
  for (int i = 15; i >= 0; --i) id += kHex[(sum >> (i * 4)) & 0xF];
  return id + "-" + std::to_string(data.size());
}

// What the pack stands for: the SDK build that translated and the translator's configuration.
inline std::string MakeIdentity(std::string_view build, std::string_view configuration) {
  std::string identity = "format=" + std::to_string(kFormatVersion) + " build=";
  identity += build;
  identity += ' ';
  identity += configuration;
  return identity;
}
// The "build=" word of an identity ("" if there is none).
inline std::string_view BuildOf(std::string_view identity) {
  const size_t at = identity.find(" build=");
  if (at == std::string_view::npos) return {};
  const size_t begin = at + 7;
  const size_t end = identity.find(' ', begin);
  return identity.substr(begin, end == std::string_view::npos ? end : end - begin);
}
inline std::string FileName(uint32_t title_id, std::string_view identity) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string name;
  for (int i = 7; i >= 0; --i) name += kHex[(title_id >> (i * 4)) & 0xF];
  name += '.';
  const uint64_t key = Checksum(reinterpret_cast<const uint8_t*>(identity.data()), identity.size());
  for (int i = 15; i >= 0; --i) name += kHex[(key >> (i * 4)) & 0xF];
  name += kExtension;
  return name;
}

// One translation to store.
struct Translation {
  uint64_t hash = 0, modification = 0;
  // The translator's SPIR-V (a multiple of 4 bytes).
  std::vector<uint8_t> spirv;
  // Encoded by the caller: one byte per texture binding, 16 bits per sampler binding.
  std::vector<uint8_t> textures;
  std::vector<uint16_t> samplers;
};

struct WriteResult {
  bool written = false;
  size_t pairs = 0, blobs = 0;
  uint64_t bytes = 0, spirv_bytes = 0;
};

// The temporary file, then a rename over the pack: a reader sees the old pack or the new one.
inline bool ReplaceFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) return false;
  static std::atomic<uint64_t> serial{0};
#ifdef _WIN32
  const uint64_t pid = GetCurrentProcessId();
#else
  const uint64_t pid = uint64_t(getpid());
#endif
  auto temporary = path;
  temporary += ".tmp-" + std::to_string(pid) + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
               std::to_string(serial++);
  bool written = false;
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) {
      file.flush();
      if (file) {
        file.close();
        written = !file.fail();
      }
    }
  }
  if (written) {
#ifdef _WIN32
    written = MoveFileExW(temporary.c_str(), path.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::filesystem::rename(temporary, path, ec);
    written = !ec;
#endif
  }
  if (!written) std::filesystem::remove(temporary, ec);
  return written;
}

// The whole file in memory. Pairs with the same (hash, modification) are stored once (the first).
inline std::vector<uint8_t> Encode(std::string_view identity, uint32_t modification_version,
                                   std::vector<Translation> translations, WriteResult* result = nullptr) {
  std::sort(translations.begin(), translations.end(), [](const Translation& a, const Translation& b) {
    return a.hash != b.hash ? a.hash < b.hash : a.modification < b.modification;
  });
  translations.erase(std::unique(translations.begin(), translations.end(),
                                 [](const Translation& a, const Translation& b) {
                                   return a.hash == b.hash && a.modification == b.modification;
                                 }),
                     translations.end());
  struct Blob {
    uint64_t offset;
    uint32_t encoded_size, words;
    uint64_t checksum;
    const std::vector<uint8_t>* spirv;
  };
  std::vector<Blob> blobs;
  std::unordered_multimap<uint64_t, uint32_t> by_content;
  std::vector<uint32_t> pair_blob;
  std::vector<uint8_t> region, bindings, encoded;
  uint64_t spirv_bytes = 0;
  for (const Translation& t : translations) {
    const uint64_t content = Checksum(t.spirv.data(), t.spirv.size());
    uint32_t index = UINT32_MAX;
    const auto [begin, end] = by_content.equal_range(content);
    for (auto it = begin; it != end; ++it) {
      if (*blobs[it->second].spirv == t.spirv) {
        index = it->second;
        break;
      }
    }
    if (index == UINT32_MAX) {
      EncodeWords(t.spirv.data(), t.spirv.size() / 4, encoded);
      index = uint32_t(blobs.size());
      blobs.push_back({region.size(), uint32_t(encoded.size()), uint32_t(t.spirv.size() / 4),
                       Checksum(encoded.data(), encoded.size()), &t.spirv});
      region.insert(region.end(), encoded.begin(), encoded.end());
      by_content.emplace(content, index);
    }
    pair_blob.push_back(index);
    spirv_bytes += t.spirv.size();
  }
  std::vector<uint8_t> meta(identity.begin(), identity.end());
  const size_t pairs_at = meta.size();
  meta.resize(pairs_at + translations.size() * kPairSize + blobs.size() * kBlobSize);
  for (size_t i = 0; i < translations.size(); ++i) {
    const Translation& t = translations[i];
    uint8_t* p = meta.data() + pairs_at + i * kPairSize;
    Put64(p, t.hash);
    Put64(p + 8, t.modification);
    Put32(p + 16, pair_blob[i]);
    Put32(p + 20, uint32_t(bindings.size()));
    Put16(p + 24, uint32_t(t.textures.size()));
    Put16(p + 26, uint32_t(t.samplers.size()));
    Put32(p + 28, 0);
    bindings.insert(bindings.end(), t.textures.begin(), t.textures.end());
    for (uint16_t s : t.samplers) {
      bindings.push_back(uint8_t(s));
      bindings.push_back(uint8_t(s >> 8));
    }
  }
  const size_t blobs_at = pairs_at + translations.size() * kPairSize;
  for (size_t i = 0; i < blobs.size(); ++i) {
    uint8_t* p = meta.data() + blobs_at + i * kBlobSize;
    Put64(p, blobs[i].offset);
    Put32(p + 8, blobs[i].encoded_size);
    Put32(p + 12, blobs[i].words);
    Put64(p + 16, blobs[i].checksum);
  }
  meta.insert(meta.end(), bindings.begin(), bindings.end());
  std::vector<uint8_t> file(kHeaderSize);
  std::memcpy(file.data(), kMagic, 8);
  Put32(file.data() + 8, kFormatVersion);
  Put32(file.data() + 12, modification_version);
  Put32(file.data() + 16, uint32_t(translations.size()));
  Put32(file.data() + 20, uint32_t(blobs.size()));
  Put32(file.data() + 24, uint32_t(identity.size()));
  Put32(file.data() + 28, uint32_t(bindings.size()));
  Put64(file.data() + 32, region.size());
  Put64(file.data() + 40, 0);
  Put64(file.data() + 48, Checksum(meta.data(), meta.size()));
  Put64(file.data() + 56, Checksum(file.data(), 56));
  file.insert(file.end(), meta.begin(), meta.end());
  file.insert(file.end(), region.begin(), region.end());
  if (result) {
    result->pairs = translations.size();
    result->blobs = blobs.size();
    result->bytes = file.size();
    result->spirv_bytes = spirv_bytes;
  }
  return file;
}

inline WriteResult Write(const std::filesystem::path& path, std::string_view identity,
                         uint32_t modification_version, std::vector<Translation> translations) {
  WriteResult result;
  const std::vector<uint8_t> file =
      Encode(identity, modification_version, std::move(translations), &result);
  result.written = ReplaceFile(path, file);
  return result;
}

class Reader {
 public:
  enum class Status {
    kNone,      // not opened, or no file
    kOk,
    kOther,     // another build, configuration, format or modification version
    kDamaged,
  };
  struct Pair {
    uint64_t hash, modification;
    uint32_t blob, bindings;
    uint16_t textures, samplers;
  };

  // Reads the header and the metadata (not the SPIR-V). A pack that is not exactly `identity`
  // is kOther and stays closed.
  Status Open(const std::filesystem::path& path, std::string_view identity,
              uint32_t modification_version) {
    Close();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return status_ = Status::kNone;
    const auto end = file.tellg();
    if (end < 0) return status_ = Status::kDamaged;
    const uint64_t size = uint64_t(end);
    uint8_t h[kHeaderSize];
    file.seekg(0);
    if (size < kHeaderSize || size > kMaxFileSize ||
        !file.read(reinterpret_cast<char*>(h), kHeaderSize) || std::memcmp(h, kMagic, 8) ||
        U64(h + 56) != Checksum(h, 56)) {
      return status_ = Status::kDamaged;
    }
    const uint32_t pairs = U32(h + 16), blobs = U32(h + 20), identity_size = U32(h + 24),
                   binding_bytes = U32(h + 28);
    const uint64_t region = U64(h + 32);
    if (U32(h + 8) != kFormatVersion || U32(h + 12) != modification_version ||
        identity_size != identity.size()) {
      return status_ = Status::kOther;
    }
    if (pairs > kMaxPairs || blobs > pairs || identity_size > kMaxIdentitySize) {
      return status_ = Status::kDamaged;
    }
    const uint64_t meta_size =
        uint64_t(identity_size) + uint64_t(pairs) * kPairSize + uint64_t(blobs) * kBlobSize +
        binding_bytes;
    if (kHeaderSize + meta_size + region != size) return status_ = Status::kDamaged;
    std::vector<uint8_t> meta(meta_size);
    if (!file.read(reinterpret_cast<char*>(meta.data()), std::streamsize(meta.size())) ||
        U64(h + 48) != Checksum(meta.data(), meta.size())) {
      return status_ = Status::kDamaged;
    }
    if (std::string_view(reinterpret_cast<const char*>(meta.data()), identity_size) != identity) {
      return status_ = Status::kOther;
    }
    const uint8_t* p = meta.data() + identity_size;
    pairs_.resize(pairs);
    for (uint32_t i = 0; i < pairs; ++i, p += kPairSize) {
      Pair& pair = pairs_[i];
      pair = {U64(p), U64(p + 8), U32(p + 16), U32(p + 20), uint16_t(U16(p + 24)),
              uint16_t(U16(p + 26))};
      if (pair.blob >= blobs || uint64_t(pair.bindings) + pair.textures + 2 * pair.samplers >
                                    binding_bytes ||
          (i && !(std::make_pair(pairs_[i - 1].hash, pairs_[i - 1].modification) <
                  std::make_pair(pair.hash, pair.modification)))) {
        pairs_.clear();
        return status_ = Status::kDamaged;
      }
    }
    blobs_.resize(blobs);
    for (uint32_t i = 0; i < blobs; ++i, p += kBlobSize) {
      Blob& blob = blobs_[i];
      blob = {U64(p), U32(p + 8), U32(p + 12), U64(p + 16)};
      if (blob.offset > region || blob.encoded_size > region - blob.offset ||
          blob.words > kMaxBlobWords || blob.encoded_size < blob.words ||
          uint64_t(blob.encoded_size) > uint64_t(blob.words) * 5) {
        pairs_.clear();
        blobs_.clear();
        return status_ = Status::kDamaged;
      }
    }
    bindings_.assign(p, p + binding_bytes);
    path_ = path;
    region_offset_ = kHeaderSize + meta_size;
    region_size_ = region;
    file_size_ = size;
    return status_ = Status::kOk;
  }

  void Close() {
    pairs_.clear();
    blobs_.clear();
    bindings_.clear();
    std::vector<uint8_t>().swap(region_);
    path_.clear();
    status_ = Status::kNone;
  }

  Status status() const { return status_; }
  bool is_open() const { return status_ == Status::kOk; }
  const std::filesystem::path& path() const { return path_; }
  size_t pair_count() const { return pairs_.size(); }
  size_t blob_count() const { return blobs_.size(); }
  uint64_t file_size() const { return file_size_; }
  const std::vector<Pair>& pairs() const { return pairs_; }

  const Pair* Find(uint64_t hash, uint64_t modification) const {
    auto it = std::lower_bound(pairs_.begin(), pairs_.end(), std::make_pair(hash, modification),
                               [](const Pair& p, const std::pair<uint64_t, uint64_t>& key) {
                                 return std::make_pair(p.hash, p.modification) < key;
                               });
    return it != pairs_.end() && it->hash == hash && it->modification == modification ? &*it
                                                                                       : nullptr;
  }
  const uint8_t* textures(const Pair& pair) const { return bindings_.data() + pair.bindings; }
  uint16_t sampler(const Pair& pair, size_t i) const {
    return uint16_t(U16(bindings_.data() + pair.bindings + pair.textures + 2 * i));
  }

  // Reads all the SPIR-V at once (a start's warm-up takes most of it); Decode then reads from
  // memory, from any number of threads. Without it, each Decode reads its module from the file.
  bool Load() {
    if (!is_open()) return false;
    if (region_.size() == region_size_ && region_size_) return true;
    std::ifstream file(path_, std::ios::binary);
    std::vector<uint8_t> region(region_size_);
    if (!file.seekg(std::streamoff(region_offset_)) ||
        !file.read(reinterpret_cast<char*>(region.data()), std::streamsize(region.size()))) {
      return false;
    }
    region_ = std::move(region);
    return true;
  }
  // Frees what Load read (not while another thread decodes).
  void Unload() { std::vector<uint8_t>().swap(region_); }

  // The pair's SPIR-V, checked against its checksum.
  bool Decode(const Pair& pair, std::vector<uint8_t>& spirv, std::vector<uint8_t>& scratch) const {
    const Blob& blob = blobs_[pair.blob];
    const uint8_t* data;
    if (region_.size() == region_size_ && region_size_) {
      data = region_.data() + blob.offset;
    } else {
      std::lock_guard<std::mutex> lock(file_mutex_);
      std::ifstream file(path_, std::ios::binary | std::ios::ate);
      // The file this reader opened: a pack written since then may lay its modules out otherwise.
      if (!file || uint64_t(file.tellg()) != file_size_) return false;
      scratch.resize(blob.encoded_size);
      if (!file.seekg(std::streamoff(region_offset_ + blob.offset)) ||
          !file.read(reinterpret_cast<char*>(scratch.data()), std::streamsize(scratch.size()))) {
        return false;
      }
      data = scratch.data();
    }
    return Checksum(data, blob.encoded_size) == blob.checksum &&
           DecodeWords(data, blob.encoded_size, blob.words, spirv);
  }

 private:
  struct Blob {
    uint64_t offset;
    uint32_t encoded_size, words;
    uint64_t checksum;
  };
  Status status_ = Status::kNone;
  std::filesystem::path path_;
  std::vector<Pair> pairs_;
  std::vector<Blob> blobs_;
  std::vector<uint8_t> bindings_;
  std::vector<uint8_t> region_;
  uint64_t region_offset_ = 0, region_size_ = 0, file_size_ = 0;
  mutable std::mutex file_mutex_;
};

// The identity text of a pack file ("" if it is not a readable pack).
inline std::string ReadIdentity(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  uint8_t h[kHeaderSize];
  if (!file.read(reinterpret_cast<char*>(h), kHeaderSize) || std::memcmp(h, kMagic, 8) ||
      U64(h + 56) != Checksum(h, 56) || U32(h + 24) > kMaxIdentitySize) {
    return {};
  }
  std::string identity(U32(h + 24), '\0');
  if (!file.read(identity.data(), std::streamsize(identity.size()))) return {};
  return identity;
}

// After writing `keep`: removes the title's other packs in its folder that another build wrote
// (or that cannot be read), and leftover temporary files. Packs of this build for other
// configurations stay (split rendering's second cache, a setting switched back), the newest
// `max_others` of them (a pack of FH1's list is about 22 MB). Returns the number of files removed.
inline size_t RemoveStale(const std::filesystem::path& keep, std::string_view identity,
                          size_t max_others = 3) {
  std::error_code ec;
  const std::string prefix = keep.filename().string().substr(0, 9);  // "<TITLE>."
  const std::string build(BuildOf(identity));
  std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> same_build;
  size_t removed = 0;
  for (std::filesystem::directory_iterator it(keep.parent_path(), ec), end; !ec && it != end;
       it.increment(ec)) {
    const std::filesystem::path path = it->path();
    const std::string name = path.filename().string();
    if (name.rfind(prefix, 0) != 0 || path == keep) continue;
    const bool temporary = name.find(std::string(kExtension) + ".tmp-") != std::string::npos;
    const bool pack = name.size() > kExtension.size() &&
                      name.compare(name.size() - kExtension.size(), kExtension.size(), kExtension) == 0;
    if (!temporary && !pack) continue;
    if (temporary) {
      // Another process may be writing it right now.
      const auto written = std::filesystem::last_write_time(path, ec);
      if (ec || std::filesystem::file_time_type::clock::now() - written < std::chrono::minutes(10)) {
        ec.clear();
        continue;
      }
    }
    if (pack && !build.empty() && BuildOf(ReadIdentity(path)) == build) {
      same_build.emplace_back(std::filesystem::last_write_time(path, ec), path);
      continue;
    }
    std::error_code remove_ec;
    removed += std::filesystem::remove(path, remove_ec) ? 1 : 0;
  }
  std::sort(same_build.begin(), same_build.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  for (size_t i = max_others; i < same_build.size(); ++i) {
    std::error_code remove_ec;
    removed += std::filesystem::remove(same_build[i].second, remove_ec) ? 1 : 0;
  }
  return removed;
}

}  // namespace rex::graphics::vulkan::shader_pack_file
