#pragma once
// FH1 (#384): the texture pack file formats: DDS reading and writing. Nothing else of the SDK, so
// tools/test_texture_pack_dds.cpp builds it alone.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rex::graphics::texture_pack {

// Host formats of dumped and replacement files.
enum class Format : uint8_t {
  kNone,
  kR8,
  kRG8,
  kRGBA8,
  kBGRA8,
  kBC1,
  kBC2,
  kBC3,
  kBC4,
  kBC5,
  kBC7,
};
bool IsBlockCompressed(Format format);
// Bytes of one 4x4 block (compressed formats) or one texel.
uint32_t ElementBytes(Format format);
uint64_t LevelBytes(Format format, uint32_t width, uint32_t height);
const char* FormatName(Format format);

// The largest replacement, either way.
constexpr uint32_t kMaxFileSize = 16384;

struct Level {
  uint64_t offset;
  uint64_t size;
  uint32_t width;
  uint32_t height;
};

// A replacement file, read and decoded on a worker; each level's rows packed, the levels starting
// 16-byte aligned in data.
struct Decoded {
  uint64_t fingerprint = 0;
  bool ok = false;
  Format format = Format::kNone;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<Level> levels;
  std::vector<uint8_t> data;
};

// A DDS file of the levels (rows packed, as written by EncodeDds).
std::vector<uint8_t> EncodeDds(Format format, uint32_t width, uint32_t height,
                               const std::vector<std::vector<uint8_t>>& levels);
// Reads a 2D DDS file: legacy headers (DXT1-5, ATI1/2, BC4U/BC5U, 32-bit RGB masks, luminance)
// and DX10 ones (BC1-5, BC7, RGBA8, BGRA8, R8, RG8). Uncompressed rows may be padded as the
// header's pitch says (see the code). false and a reason in error when it cannot be used.
bool DecodeDds(const uint8_t* data, size_t size, Decoded& out, std::string& error);

}  // namespace rex::graphics::texture_pack
