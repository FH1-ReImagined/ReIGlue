/**
 * FH1 (#384): texture packs (see pack.h). Fingerprints, the dump writer and the replacement
 * reader; the texture caches call into it, the Vulkan one uploads the replacements.
 */

#include <rex/graphics/pipeline/texture/pack.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <thread>
#include <unordered_set>

#include <xxhash.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/logging.h>

// A private copy (static functions) of the PNG reader the runtime's image_decode.cpp uses.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include <stb_image.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

REXCVAR_DEFINE_BOOL(texture_dump, false, "GPU",
                    "Write each texture the game loads, once, to <texture_pack_root>/dump as "
                    "<fingerprint>.dds (and a .png of its largest level): the names texture pack "
                    "files must have")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(texture_dump_png, true, "GPU",
                    "With texture_dump, also write a .png of the largest level for browsing")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(texture_replace, false, "GPU",
                    "Use texture packs: a texture with a file <fingerprint>.dds or .png in "
                    "texture_pack_root (or a folder in it) is drawn from that file (Vulkan)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(texture_replace_preload, false, "GPU",
                    "Read every texture pack file at start instead of when its texture first "
                    "loads (more memory, no texture shown at its original resolution first)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(texture_pack_root, "", "GPU",
                      "Texture pack folder (default: textures in the user data folder)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::texture_pack {

namespace {

constexpr uint32_t kFingerprintVersion = 1;
// The dump queue: beyond these, new textures are dropped (and dumped when they load again).
constexpr size_t kDumpQueueMaxJobs = 4096;
constexpr uint64_t kDumpQueueMaxBytes = uint64_t(256) << 20;
constexpr uint64_t kSpareBufferBytes = uint64_t(64) << 20;
constexpr uint64_t kMaxPendingDecodedBytes = uint64_t(256) << 20;

std::string Hex(uint64_t value) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

// A fingerprint from a file name: 16 hexadecimal digits.
bool ParseHex(const std::string& text, uint64_t& value) {
  if (text.size() != 16) return false;
  value = 0;
  for (char c : text) {
    uint32_t digit;
    if (c >= '0' && c <= '9') {
      digit = uint32_t(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = uint32_t(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = uint32_t(c - 'A' + 10);
    } else {
      return false;
    }
    value = value << 4 | digit;
  }
  return value != 0;
}

std::string Lower(std::string text) {
  for (char& c : text) c = char(std::tolower(uint8_t(c)));
  return text;
}

void SwapEndian(uint8_t* data, size_t size, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (size_t i = 0; i + 1 < size; i += 2) std::swap(data[i], data[i + 1]);
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& data) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return false;
  const std::streamoff size = file.tellg();
  if (size <= 0 || size > (std::streamoff(1) << 31)) return false;
  data.resize(size_t(size));
  file.seekg(0);
  return bool(file.read(reinterpret_cast<char*>(data.data()), size));
}

// Written next to the target and renamed over it, so a crash never leaves half a file.
bool WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!file) return false;
  }
  std::error_code error;
  std::filesystem::rename(temp, path, error);
  if (error) {
    std::filesystem::remove(temp, error);
    return false;
  }
  return true;
}

// --- Block-compressed decoding (for the dump's PNG) ---------------------------------------

void Rgb565(uint16_t color, uint8_t* rgb) {
  const uint32_t r = color >> 11 & 31, g = color >> 5 & 63, b = color & 31;
  rgb[0] = uint8_t(r << 3 | r >> 2);
  rgb[1] = uint8_t(g << 2 | g >> 4);
  rgb[2] = uint8_t(b << 3 | b >> 2);
}

// out: 16 RGBA texels. allow_alpha: BC1's 3-color mode with transparent black.
void DecodeColorBlock(const uint8_t* block, uint8_t (*out)[4], bool allow_alpha) {
  const uint16_t c0 = uint16_t(block[0] | block[1] << 8);
  const uint16_t c1 = uint16_t(block[2] | block[3] << 8);
  uint8_t colors[4][4] = {};
  Rgb565(c0, colors[0]);
  Rgb565(c1, colors[1]);
  colors[0][3] = colors[1][3] = 255;
  if (c0 > c1 || !allow_alpha) {
    for (int i = 0; i < 3; ++i) {
      colors[2][i] = uint8_t((2 * colors[0][i] + colors[1][i]) / 3);
      colors[3][i] = uint8_t((colors[0][i] + 2 * colors[1][i]) / 3);
    }
    colors[2][3] = colors[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) colors[2][i] = uint8_t((colors[0][i] + colors[1][i]) / 2);
    colors[2][3] = 255;
  }
  const uint32_t indices = uint32_t(block[4]) | uint32_t(block[5]) << 8 |
                           uint32_t(block[6]) << 16 | uint32_t(block[7]) << 24;
  for (int i = 0; i < 16; ++i) std::memcpy(out[i], colors[indices >> (2 * i) & 3], 4);
}

// out: 16 values, every stride-th byte.
void DecodeAlphaBlock(const uint8_t* block, uint8_t* out, size_t stride) {
  uint8_t values[8];
  values[0] = block[0];
  values[1] = block[1];
  if (values[0] > values[1]) {
    for (int i = 1; i < 7; ++i) values[i + 1] = uint8_t(((7 - i) * values[0] + i * values[1]) / 7);
  } else {
    for (int i = 1; i < 5; ++i) values[i + 1] = uint8_t(((5 - i) * values[0] + i * values[1]) / 5);
    values[6] = 0;
    values[7] = 255;
  }
  uint64_t indices = 0;
  for (int i = 0; i < 6; ++i) indices |= uint64_t(block[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) out[i * stride] = values[indices >> (3 * i) & 7];
}

// --- PNG writing ---------------------------------------------------------------------------

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

class BitWriter {
 public:
  explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
  void Put(uint32_t value, int bits) {
    buffer_ |= uint64_t(value) << count_;
    count_ += bits;
    while (count_ >= 8) {
      out_.push_back(uint8_t(buffer_));
      buffer_ >>= 8;
      count_ -= 8;
    }
  }
  // Huffman codes go most significant bit first.
  void PutCode(uint32_t code, int bits) {
    uint32_t reversed = 0;
    for (int i = 0; i < bits; ++i) reversed |= (code >> i & 1) << (bits - 1 - i);
    Put(reversed, bits);
  }
  void Flush() {
    if (count_) out_.push_back(uint8_t(buffer_));
    buffer_ = 0;
    count_ = 0;
  }

 private:
  std::vector<uint8_t>& out_;
  uint64_t buffer_ = 0;
  int count_ = 0;
};

void PutLiteral(BitWriter& bits, uint32_t symbol) {
  if (symbol < 144) {
    bits.PutCode(0x30 + symbol, 8);
  } else if (symbol < 256) {
    bits.PutCode(0x190 + symbol - 144, 9);
  } else if (symbol < 280) {
    bits.PutCode(symbol - 256, 7);
  } else {
    bits.PutCode(0xC0 + symbol - 280, 8);
  }
}

void PutMatch(BitWriter& bits, uint32_t length, uint32_t distance) {
  static const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                           15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                           67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const uint16_t kDistanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,
                                             17,   25,   33,   49,   65,   97,    129,   193,
                                             257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                             4097, 6145, 8193, 12289, 16385, 24577};
  static const uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                             6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  int l = 28;
  while (kLengthBase[l] > length) --l;
  PutLiteral(bits, 257 + l);
  if (kLengthExtra[l]) bits.Put(length - kLengthBase[l], kLengthExtra[l]);
  int d = 29;
  while (kDistanceBase[d] > distance) --d;
  bits.PutCode(d, 5);
  if (kDistanceExtra[d]) bits.Put(distance - kDistanceBase[d], kDistanceExtra[d]);
}

// zlib stream: one fixed-Huffman block, greedy LZ77 over hash chains.
std::vector<uint8_t> Zlib(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out = {0x78, 0x01};
  out.reserve(size / 2 + 64);
  BitWriter bits(out);
  bits.Put(1, 1);  // the final block
  bits.Put(1, 2);  // fixed Huffman codes
  constexpr uint32_t kWindow = 32768, kHashBits = 15, kChain = 16;
  std::vector<int32_t> head(size_t(1) << kHashBits, -1), previous(kWindow, -1);
  auto hash = [&](size_t i) {
    return (uint32_t(data[i]) << 10 ^ uint32_t(data[i + 1]) << 5 ^ data[i + 2]) &
           ((1u << kHashBits) - 1);
  };
  auto insert = [&](size_t i) {
    if (i + 2 >= size) return;
    const uint32_t h = hash(i);
    previous[i & (kWindow - 1)] = head[h];
    head[h] = int32_t(i);
  };
  size_t i = 0;
  while (i < size) {
    uint32_t best_length = 0, best_distance = 0;
    if (i + 2 < size) {
      int32_t candidate = head[hash(i)];
      const uint32_t limit = uint32_t(std::min<size_t>(258, size - i));
      for (uint32_t tries = 0; candidate >= 0 && tries < kChain; ++tries) {
        const size_t distance = i - size_t(candidate);
        if (distance == 0 || distance > kWindow) break;
        uint32_t length = 0;
        while (length < limit && data[size_t(candidate) + length] == data[i + length]) ++length;
        if (length > best_length) {
          best_length = length;
          best_distance = uint32_t(distance);
          if (length == limit) break;
        }
        const int32_t next = previous[size_t(candidate) & (kWindow - 1)];
        if (next >= candidate) break;
        candidate = next;
      }
    }
    if (best_length >= 3) {
      PutMatch(bits, best_length, best_distance);
      for (uint32_t k = 0; k < best_length; ++k) insert(i + k);
      i += best_length;
    } else {
      PutLiteral(bits, data[i]);
      insert(i);
      ++i;
    }
  }
  PutLiteral(bits, 256);
  bits.Flush();
  uint32_t a = 1, b = 0;
  for (size_t k = 0; k < size; ++k) {
    a = (a + data[k]) % 65521;
    b = (b + a) % 65521;
  }
  const uint32_t adler = b << 16 | a;
  for (int k = 3; k >= 0; --k) out.push_back(uint8_t(adler >> (8 * k)));
  return out;
}

void PutChunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
  const uint32_t size = uint32_t(data.size());
  for (int k = 3; k >= 0; --k) png.push_back(uint8_t(size >> (8 * k)));
  const size_t start = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  const uint32_t crc = Crc32(png.data() + start, png.size() - start);
  for (int k = 3; k >= 0; --k) png.push_back(uint8_t(crc >> (8 * k)));
}

// --- Mip generation (uncompressed replacements without mips) -----------------------------

void GenerateMips(Decoded& decoded) {
  const uint32_t texel = ElementBytes(decoded.format);
  std::vector<std::vector<uint8_t>> levels;
  levels.emplace_back(decoded.data.begin(),
                      decoded.data.begin() + ptrdiff_t(decoded.levels[0].size));
  uint32_t width = decoded.width, height = decoded.height;
  while (width > 1 || height > 1) {
    const uint32_t next_width = std::max(width / 2, 1u), next_height = std::max(height / 2, 1u);
    const std::vector<uint8_t>& source = levels.back();
    std::vector<uint8_t> level(size_t(next_width) * next_height * texel);
    for (uint32_t y = 0; y < next_height; ++y) {
      const uint32_t y0 = std::min(2 * y, height - 1), y1 = std::min(2 * y + 1, height - 1);
      for (uint32_t x = 0; x < next_width; ++x) {
        const uint32_t x0 = std::min(2 * x, width - 1), x1 = std::min(2 * x + 1, width - 1);
        for (uint32_t c = 0; c < texel; ++c) {
          const uint32_t sum = source[(size_t(y0) * width + x0) * texel + c] +
                               source[(size_t(y0) * width + x1) * texel + c] +
                               source[(size_t(y1) * width + x0) * texel + c] +
                               source[(size_t(y1) * width + x1) * texel + c];
          level[(size_t(y) * next_width + x) * texel + c] = uint8_t((sum + 2) / 4);
        }
      }
    }
    levels.push_back(std::move(level));
    width = next_width;
    height = next_height;
  }
  decoded.levels.clear();
  decoded.data.clear();
  width = decoded.width;
  height = decoded.height;
  for (const std::vector<uint8_t>& level : levels) {
    const uint64_t offset = (decoded.data.size() + 15) & ~uint64_t(15);
    decoded.data.resize(size_t(offset));
    decoded.data.insert(decoded.data.end(), level.begin(), level.end());
    decoded.levels.push_back({offset, level.size(), width, height});
    width = std::max(width / 2, 1u);
    height = std::max(height / 2, 1u);
  }
}

}  // namespace

bool IsScreenSized(uint32_t width, uint32_t height) {
  for (uint32_t shift = 0; shift < 4; ++shift) {
    if (width == (1280u >> shift) && (height == (720u >> shift) || height == (736u >> shift))) {
      return true;
    }
  }
  return false;
}

Format GuestFormatFile(xenos::TextureFormat format) {
  // Each holds what the host texture's channels hold (the Vulkan texture cache's host formats),
  // so a replacement in the same layout is sampled through the same swizzle.
  switch (format) {
    case xenos::TextureFormat::k_8:
      return Format::kR8;
    case xenos::TextureFormat::k_8_8:
      return Format::kRG8;
    case xenos::TextureFormat::k_8_8_8_8:
      return Format::kRGBA8;
    case xenos::TextureFormat::k_DXT1:
      return Format::kBC1;
    case xenos::TextureFormat::k_DXT2_3:
      return Format::kBC2;
    case xenos::TextureFormat::k_DXT4_5:
      return Format::kBC3;
    case xenos::TextureFormat::k_DXT5A:
      return Format::kBC4;
    case xenos::TextureFormat::k_DXN:
      return Format::kBC5;
    default:
      return Format::kNone;
  }
}

std::vector<uint8_t> UntileLevel(const GuestTexture& texture,
                                 const texture_util::TextureGuestLayout& layout,
                                 const uint8_t* base, uint32_t base_size, const uint8_t* mips,
                                 uint32_t mips_size, uint32_t level) {
  const FormatInfo* info = FormatInfo::Get(texture.format);
  const uint32_t block_width = info->block_width, block_height = info->block_height;
  const uint32_t block_bytes = info->bytes_per_block();
  if (!block_bytes || (block_bytes & (block_bytes - 1))) return {};
  const uint32_t block_bytes_log2 = uint32_t(std::countr_zero(block_bytes));
  const uint32_t width = std::max(texture.width >> level, 1u);
  const uint32_t height = std::max(texture.height >> level, 1u);
  const uint32_t width_blocks = (width + block_width - 1) / block_width;
  const uint32_t height_blocks = (height + block_height - 1) / block_height;
  const uint32_t packed = layout.packed_level;
  const uint8_t* source;
  size_t source_size;
  const texture_util::TextureGuestLayout::Level* level_layout;
  if (level == 0) {
    source = base;
    source_size = base_size;
    level_layout = &layout.base;
  } else {
    const uint32_t stored = std::min(level, packed);
    if (!mips || stored >= xenos::kTextureMaxMips) return {};
    const uint32_t offset = layout.mip_offsets_bytes[stored];
    if (offset >= mips_size) return {};
    source = mips + offset;
    source_size = mips_size - offset;
    level_layout = &layout.mips[stored];
  }
  uint32_t offset_x = 0, offset_y = 0, offset_z = 0;
  if (level >= packed) {
    texture_util::GetPackedMipOffset(texture.width, texture.height, 1, texture.format, level,
                                     offset_x, offset_y, offset_z);
  }
  const uint32_t pitch_bytes = level_layout->row_pitch_bytes;
  if (!pitch_bytes) return {};
  std::vector<uint8_t> out(size_t(width_blocks) * height_blocks * block_bytes);
  for (uint32_t y = 0; y < height_blocks; ++y) {
    for (uint32_t x = 0; x < width_blocks; ++x) {
      const uint32_t source_x = x + offset_x, source_y = y + offset_y;
      const size_t offset =
          texture.tiled ? size_t(uint32_t(texture_util::GetTiledOffset2D(
                              int32_t(source_x), int32_t(source_y), pitch_bytes / block_bytes,
                              block_bytes_log2)))
                        : size_t(source_y) * pitch_bytes + size_t(source_x) * block_bytes;
      if (offset + block_bytes <= source_size) {
        std::memcpy(&out[(size_t(y) * width_blocks + x) * block_bytes], source + offset,
                    block_bytes);
      }
    }
  }
  return out;
}

std::vector<uint8_t> ToRgba8(Format format, uint32_t width, uint32_t height, const uint8_t* data) {
  std::vector<uint8_t> rgba(size_t(width) * height * 4, 255);
  auto texel = [&](uint32_t x, uint32_t y) { return &rgba[(size_t(y) * width + x) * 4]; };
  if (!IsBlockCompressed(format)) {
    const uint32_t bytes = ElementBytes(format);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const uint8_t* in = data + (size_t(y) * width + x) * bytes;
        uint8_t* out = texel(x, y);
        switch (format) {
          case Format::kR8:
            out[0] = out[1] = out[2] = in[0];
            break;
          case Format::kRG8:
            out[0] = in[0];
            out[1] = in[1];
            out[2] = 0;
            break;
          case Format::kRGBA8:
            std::memcpy(out, in, 4);
            break;
          case Format::kBGRA8:
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
            out[3] = in[3];
            break;
          default:
            break;
        }
      }
    }
    return rgba;
  }
  if (format == Format::kBC7) return {};
  const uint32_t block_bytes = ElementBytes(format);
  const uint32_t width_blocks = (width + 3) / 4, height_blocks = (height + 3) / 4;
  for (uint32_t by = 0; by < height_blocks; ++by) {
    for (uint32_t bx = 0; bx < width_blocks; ++bx) {
      const uint8_t* block = data + (size_t(by) * width_blocks + bx) * block_bytes;
      uint8_t texels[16][4];
      std::memset(texels, 255, sizeof(texels));
      switch (format) {
        case Format::kBC1:
          DecodeColorBlock(block, texels, true);
          break;
        case Format::kBC2:
          DecodeColorBlock(block + 8, texels, false);
          for (int i = 0; i < 16; ++i) texels[i][3] = uint8_t((block[i / 2] >> (4 * (i & 1)) & 15) * 17);
          break;
        case Format::kBC3:
          DecodeColorBlock(block + 8, texels, false);
          DecodeAlphaBlock(block, &texels[0][3], 4);
          break;
        case Format::kBC4:
          DecodeAlphaBlock(block, &texels[0][0], 4);
          for (int i = 0; i < 16; ++i) texels[i][1] = texels[i][2] = texels[i][0];
          break;
        case Format::kBC5:
          DecodeAlphaBlock(block, &texels[0][0], 4);
          DecodeAlphaBlock(block + 8, &texels[0][1], 4);
          for (int i = 0; i < 16; ++i) texels[i][2] = 0;
          break;
        default:
          break;
      }
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + i / 4;
        if (x < width && y < height) std::memcpy(texel(x, y), texels[i], 4);
      }
    }
  }
  return rgba;
}

std::vector<uint8_t> EncodePng(uint32_t width, uint32_t height, const uint8_t* rgba) {
  // Each row with the filter that gives the smallest sum of absolute differences.
  const size_t stride = size_t(width) * 4;
  std::vector<uint8_t> filtered((stride + 1) * height);
  std::vector<uint8_t> candidate(stride);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* row = rgba + y * stride;
    const uint8_t* above = y ? row - stride : nullptr;
    uint64_t best_cost = UINT64_MAX;
    for (uint8_t filter = 0; filter < 5; ++filter) {
      if (!above && (filter == 2 || filter == 3 || filter == 4)) continue;
      uint64_t cost = 0;
      for (size_t i = 0; i < stride; ++i) {
        const int a = i >= 4 ? row[i - 4] : 0, b = above ? above[i] : 0,
                  c = (above && i >= 4) ? above[i - 4] : 0;
        int predicted = 0;
        switch (filter) {
          case 1:
            predicted = a;
            break;
          case 2:
            predicted = b;
            break;
          case 3:
            predicted = (a + b) / 2;
            break;
          case 4: {
            const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b),
                      pc = std::abs(p - c);
            predicted = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            break;
          }
          default:
            break;
        }
        candidate[i] = uint8_t(row[i] - predicted);
        cost += uint64_t(std::abs(int(int8_t(candidate[i]))));
      }
      if (cost < best_cost) {
        best_cost = cost;
        uint8_t* out = &filtered[y * (stride + 1)];
        out[0] = filter;
        std::memcpy(out + 1, candidate.data(), stride);
      }
    }
  }
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr(13);
  for (int k = 0; k < 4; ++k) {
    ihdr[k] = uint8_t(width >> (24 - 8 * k));
    ihdr[4 + k] = uint8_t(height >> (24 - 8 * k));
  }
  ihdr[8] = 8;  // bits per channel
  ihdr[9] = 6;  // RGBA
  PutChunk(png, "IHDR", ihdr);
  PutChunk(png, "IDAT", Zlib(filtered.data(), filtered.size()));
  PutChunk(png, "IEND", {});
  return png;
}

std::vector<std::unique_ptr<Decoded>> Client::Take() {
  std::vector<std::unique_ptr<Decoded>> taken;
  std::lock_guard<std::mutex> lock(mutex_);
  taken.swap(ready_);
  ready_count_.store(0, std::memory_order_release);
  return taken;
}

void Client::Put(std::unique_ptr<Decoded> decoded) {
  pending_bytes_.fetch_add(decoded->data.size(), std::memory_order_acq_rel);
  std::lock_guard<std::mutex> lock(mutex_);
  ready_.push_back(std::move(decoded));
  ready_count_.store(uint32_t(ready_.size()), std::memory_order_release);
}

struct Packs::DumpJob {
  uint64_t fingerprint;
  GuestTexture texture;
  texture_util::TextureGuestLayout layout;
  std::vector<uint8_t> base;
  std::vector<uint8_t> mips;
  uint32_t base_size;
  uint32_t mips_size;
};

struct Packs::DecodeJob {
  uint64_t fingerprint;
  uint32_t width;
  uint32_t height;
  std::weak_ptr<Client> client;
};

Packs* Packs::Get() {
  static Packs* packs = [] {
    if (!REXCVAR_GET(texture_dump) && !REXCVAR_GET(texture_replace)) return static_cast<Packs*>(nullptr);
    // Never destroyed: the workers are detached and may be writing when the process ends.
    Packs* created = new Packs();
    if (!created->dump_ && !created->replace_) return static_cast<Packs*>(nullptr);
    return created;
  }();
  return packs;
}

Packs::Packs() {
  dump_ = REXCVAR_GET(texture_dump);
  replace_ = REXCVAR_GET(texture_replace);
  preload_ = replace_ && REXCVAR_GET(texture_replace_preload);
  dump_png_ = REXCVAR_GET(texture_dump_png);
  std::string root = REXCVAR_GET(texture_pack_root);
  if (root.empty()) {
    const std::string user_data = rex::cvar::GetFlagByName("user_data_root");
    if (!user_data.empty()) root = (std::filesystem::path(user_data) / "textures").string();
  }
  if (root.empty()) {
    REXGPU_WARN("Texture packs: no folder (texture_pack_root is not set): off");
    dump_ = replace_ = preload_ = false;
    return;
  }
  root_ = std::filesystem::absolute(std::filesystem::path(root)).lexically_normal();
  dump_dir_ = root_ / "dump";
  std::error_code error;
  std::filesystem::create_directories(dump_ ? dump_dir_ : root_, error);

  const auto scan_start = std::chrono::steady_clock::now();
  size_t duplicates = 0;
  if (replace_) {
    // Every <fingerprint>.dds/.png under the folder, except in dump/; a .dds wins over a .png.
    std::vector<std::pair<uint64_t, std::filesystem::path>> files;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root_, std::filesystem::directory_options::skip_permission_denied, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
      if (it->is_directory(error)) {
        if (it.depth() == 0 && it->path().filename() == "dump") it.disable_recursion_pending();
        continue;
      }
      const std::string extension = Lower(it->path().extension().string());
      uint64_t fingerprint;
      if ((extension == ".dds" || extension == ".png") &&
          ParseHex(it->path().stem().string(), fingerprint)) {
        files.emplace_back(fingerprint, it->path());
      }
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
      if (a.first != b.first) return a.first < b.first;
      // .dds before .png, then by path, so the choice is the same every run.
      const bool a_dds = Lower(a.second.extension().string()) == ".dds";
      const bool b_dds = Lower(b.second.extension().string()) == ".dds";
      if (a_dds != b_dds) return a_dds;
      return a.second < b.second;
    });
    for (auto& file : files) {
      if (!index_.empty() && index_.back().first == file.first) {
        ++duplicates;
        continue;
      }
      index_.push_back(std::move(file));
    }
  }
  if (dump_) {
    std::unordered_set<uint64_t>& dumped = dumped_;
    for (auto it = std::filesystem::directory_iterator(dump_dir_, error);
         !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
      uint64_t fingerprint;
      if (Lower(it->path().extension().string()) == ".dds" &&
          ParseHex(it->path().stem().string(), fingerprint)) {
        dumped.insert(fingerprint);
      }
    }
  }
  const double scan_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - scan_start)
                             .count();
  REXGPU_INFO(
      "Texture packs: folder {}; dump {}{}; replace {} ({} files{}{}){} (listed in {:.1f} ms)",
      root_.string(), dump_ ? "on" : "off",
      dump_ ? " (" + std::to_string(dumped_.size()) + " already dumped)" : std::string(),
      replace_ ? "on" : "off", index_.size(),
      duplicates ? ", " + std::to_string(duplicates) + " duplicate names ignored" : std::string(),
      "", preload_ ? ", preloading" : "", scan_ms);
  if (dump_) std::thread(&Packs::DumpWorker, this).detach();
  if (replace_) std::thread(&Packs::DecodeWorker, this).detach();
}

uint64_t Packs::Fingerprint(const GuestTexture& texture,
                            const texture_util::TextureGuestLayout& layout, const uint8_t* base,
                            uint32_t base_size) const {
  // Only the visible blocks of the base level: the padding around them (to the tile, the
  // pitch, the packed mip tail) can hold whatever the memory held before, which differs between
  // runs. When the tiled layout is a permutation of exactly the visible blocks (32-block
  // multiples, 128 for 1- and 2-byte blocks), the guest data is hashed as it is; otherwise the
  // blocks are gathered first (small and odd-sized textures).
  const FormatInfo* info = FormatInfo::Get(texture.format);
  const uint32_t block_bytes = info->bytes_per_block();
  const uint32_t width_blocks = (texture.width + info->block_width - 1) / info->block_width;
  const uint32_t height_blocks = (texture.height + info->block_height - 1) / info->block_height;
  const uint64_t visible_bytes = uint64_t(width_blocks) * height_blocks * block_bytes;
  const uint32_t multiple = block_bytes >= 4 ? 32 : 128;
  uint64_t data;
  if (layout.packed_level != 0 && layout.base.row_pitch_bytes == width_blocks * block_bytes &&
      (!texture.tiled || (width_blocks % multiple == 0 && height_blocks % multiple == 0)) &&
      visible_bytes <= base_size) {
    data = XXH3_64bits(base, size_t(visible_bytes));
  } else {
    const std::vector<uint8_t> blocks = UntileLevel(texture, layout, base, base_size, nullptr, 0, 0);
    data = XXH3_64bits(blocks.data(), blocks.size());
  }
  struct {
    uint32_t version, format, endianness, width, height, pitch, tiled, packed_mips;
    uint64_t data;
  } fields = {kFingerprintVersion,
              uint32_t(texture.format),
              uint32_t(texture.endianness),
              texture.width,
              texture.height,
              texture.pitch,
              uint32_t(texture.tiled),
              uint32_t(texture.packed_mips),
              data};
  static_assert(sizeof(fields) == 40, "no padding in the hashed fields");
  const uint64_t fingerprint = XXH3_64bits(&fields, sizeof(fields));
  return fingerprint ? fingerprint : 1;
}

void Packs::Dump(uint64_t fingerprint, const GuestTexture& texture,
                 const texture_util::TextureGuestLayout& layout, const uint8_t* base,
                 uint32_t base_size, const uint8_t* mips, uint32_t mips_size) {
  if (!dump_) return;
  // Copied here, rounded up to 16 bytes as the texture cache reads it (within the same pages):
  // the worker must not read guest memory, which the game may free (and protect) meanwhile.
  // The copies go into buffers the worker gave back, so they rarely touch new pages.
  const uint32_t base_copy = (base_size + 15) & ~15u;
  const uint32_t mips_copy = mips ? (mips_size + 15) & ~15u : 0;
  auto job = std::make_unique<DumpJob>();
  {
    std::lock_guard<std::mutex> lock(dump_mutex_);
    if (dumped_.count(fingerprint)) return;
    if (dump_queue_.size() >= kDumpQueueMaxJobs ||
        dump_queue_bytes_ + base_copy + mips_copy > kDumpQueueMaxBytes) {
      ++stats_.dumps_dropped;
      return;
    }
    dumped_.insert(fingerprint);
    dump_queue_bytes_ += base_copy + mips_copy;
    for (std::vector<uint8_t>* buffer : {&job->base, &job->mips}) {
      if (spare_buffers_.empty()) break;
      spare_bytes_ -= spare_buffers_.back().capacity();
      buffer->swap(spare_buffers_.back());
      spare_buffers_.pop_back();
    }
  }
  const auto start = std::chrono::steady_clock::now();
  job->fingerprint = fingerprint;
  job->texture = texture;
  job->layout = layout;
  job->base.assign(base, base + base_copy);
  job->mips.assign(mips, mips + mips_copy);
  job->base_size = base_size;
  job->mips_size = mips ? mips_size : 0;
  stats_.dump_copy_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count());
  {
    std::lock_guard<std::mutex> lock(dump_mutex_);
    dump_queue_.push_back(std::move(job));
    ++stats_.dumps_queued;
  }
  dump_cv_.notify_one();
}

bool Packs::HasReplacement(uint64_t fingerprint) const {
  auto it = std::lower_bound(index_.begin(), index_.end(), fingerprint,
                             [](const auto& entry, uint64_t value) { return entry.first < value; });
  return it != index_.end() && it->first == fingerprint;
}

std::vector<uint64_t> Packs::Replacements() const {
  std::vector<uint64_t> fingerprints;
  fingerprints.reserve(index_.size());
  for (const auto& entry : index_) fingerprints.push_back(entry.first);
  return fingerprints;
}

void Packs::RequestDecode(uint64_t fingerprint, uint32_t width, uint32_t height,
                          const std::shared_ptr<Client>& client) {
  if (!replace_) return;
  auto job = std::make_unique<DecodeJob>();
  job->fingerprint = fingerprint;
  job->width = width;
  job->height = height;
  job->client = client;
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    decode_queue_.push_back(std::move(job));
  }
  decode_cv_.notify_one();
}

void Packs::DumpWorker() {
  std::ofstream index(dump_dir_ / "index.tsv", std::ios::app);
  uint32_t errors_logged = 0;
  while (true) {
    std::unique_ptr<DumpJob> job;
    {
      std::unique_lock<std::mutex> lock(dump_mutex_);
      dump_cv_.wait(lock, [this] { return !dump_queue_.empty(); });
      job = std::move(dump_queue_.front());
      dump_queue_.pop_front();
    }
    WriteDump(*job, index, errors_logged);
    // The buffers go back for the next copies (at most 64 MiB kept).
    std::lock_guard<std::mutex> lock(dump_mutex_);
    dump_queue_bytes_ -= job->base.size() + job->mips.size();
    for (std::vector<uint8_t>* buffer : {&job->base, &job->mips}) {
      if (buffer->capacity() && spare_bytes_ + buffer->capacity() <= kSpareBufferBytes) {
        spare_bytes_ += buffer->capacity();
        buffer->clear();
        spare_buffers_.push_back(std::move(*buffer));
      }
    }
  }
}

void Packs::WriteDump(DumpJob& job, std::ofstream& index, uint32_t& errors_logged) {
  const GuestTexture& texture = job.texture;
  SwapEndian(job.base.data(), job.base.size(), texture.endianness);
  SwapEndian(job.mips.data(), job.mips.size(), texture.endianness);
  const Format format = GuestFormatFile(texture.format);
  const uint32_t level_count = job.mips.empty() ? 1 : texture.mip_levels;
  std::vector<std::vector<uint8_t>> levels;
  for (uint32_t level = 0; level < level_count; ++level) {
    std::vector<uint8_t> data =
        UntileLevel(texture, job.layout, job.base.data(), job.base_size,
                    job.mips.empty() ? nullptr : job.mips.data(), job.mips_size, level);
    if (data.empty()) break;
    levels.push_back(std::move(data));
  }
  const std::string name = Hex(job.fingerprint);
  bool ok = !levels.empty();
  std::vector<uint8_t> dds;
  if (ok) {
    dds = EncodeDds(format, texture.width, texture.height, levels);
    ok = !dds.empty() && WriteFile(dump_dir_ / (name + ".dds"), dds);
  }
  uint64_t written = dds.size();
  if (ok && dump_png_) {
    std::vector<uint8_t> rgba = ToRgba8(format, texture.width, texture.height, levels[0].data());
    if (!rgba.empty()) {
      std::vector<uint8_t> png = EncodePng(texture.width, texture.height, rgba.data());
      if (WriteFile(dump_dir_ / (name + ".png"), png)) written += png.size();
    }
  }
  if (!ok) {
    ++dumps_failed_;
    if (errors_logged++ < 8) {
      REXGPU_WARN("Texture packs: could not dump {} ({} {}x{}) to {}", name,
                  FormatInfo::Get(texture.format)->name, texture.width, texture.height,
                  dump_dir_.string());
    }
    return;
  }
  ++dumps_written_;
  dump_bytes_written_ += written;
  index << name << '\t' << FormatInfo::Get(texture.format)->name << '\t' << texture.width << 'x'
        << texture.height << '\t' << levels.size() << '\t' << FormatName(format) << '\n';
  index.flush();
}

void Packs::DecodeWorker() {
  uint32_t errors_logged = 0;
  while (true) {
    std::unique_ptr<DecodeJob> job;
    {
      std::unique_lock<std::mutex> lock(decode_mutex_);
      decode_cv_.wait(lock, [this] { return !decode_queue_.empty(); });
      job = std::move(decode_queue_.front());
      decode_queue_.pop_front();
    }
    std::shared_ptr<Client> client = job->client.lock();
    if (!client) continue;
    // At most this much decoded and waiting for the frame starts to upload it (32 MiB a frame).
    while (client->pending_bytes() > kMaxPendingDecodedBytes && job->client.use_count() > 1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto decoded = std::make_unique<Decoded>();
    decoded->fingerprint = job->fingerprint;
    std::string error;
    auto it = std::lower_bound(
        index_.begin(), index_.end(), job->fingerprint,
        [](const auto& entry, uint64_t value) { return entry.first < value; });
    std::vector<uint8_t> file;
    if (it == index_.end() || it->first != job->fingerprint) {
      error = "not in the pack";
    } else if (!ReadFile(it->second, file)) {
      error = "could not read the file";
    } else if (Lower(it->second.extension().string()) == ".png") {
      int width = 0, height = 0, channels = 0;
      stbi_uc* pixels = stbi_load_from_memory(file.data(), int(file.size()), &width, &height,
                                              &channels, 4);
      if (!pixels) {
        error = "not a PNG file this reader understands";
      } else if (width <= 0 || height <= 0 || uint32_t(width) > kMaxFileSize ||
                 uint32_t(height) > kMaxFileSize) {
        error = "bad size";
        stbi_image_free(pixels);
      } else {
        decoded->format = Format::kRGBA8;
        decoded->width = uint32_t(width);
        decoded->height = uint32_t(height);
        const uint64_t size = uint64_t(width) * uint64_t(height) * 4;
        decoded->data.assign(pixels, pixels + size);
        decoded->levels.push_back({0, size, uint32_t(width), uint32_t(height)});
        stbi_image_free(pixels);
      }
    } else if (!DecodeDds(file.data(), file.size(), *decoded, error)) {
      // error set
    }
    if (error.empty() && job->width && job->height) {
      // The same picture at another size: the aspect ratio must match (within a texel).
      const int64_t cross = int64_t(decoded->width) * job->height - int64_t(decoded->height) * job->width;
      if (std::abs(cross) > int64_t(std::max(decoded->width, decoded->height))) {
        error = "its aspect ratio differs from the game's " + std::to_string(job->width) + "x" +
                std::to_string(job->height);
      }
    }
    if (error.empty() && !IsBlockCompressed(decoded->format) && decoded->levels.size() == 1) {
      GenerateMips(*decoded);
    }
    decoded->ok = error.empty();
    if (decoded->ok) {
      ++decodes_ok_;
    } else {
      ++decodes_failed_;
      if (errors_logged++ < 32) {
        REXGPU_WARN("Texture packs: {} not used: {} ({})", Hex(job->fingerprint), error,
                    it != index_.end() && it->first == job->fingerprint ? it->second.string()
                                                                        : std::string());
      }
      decoded->data.clear();
      decoded->data.shrink_to_fit();
    }
    client->Put(std::move(decoded));
  }
}

void Packs::CountSkip(Skip skip, xenos::TextureFormat format) {
  ++stats_.skipped[size_t(skip)];
  if (skip == Skip::kFormat) ++stats_.skipped_formats[uint32_t(format) & 63];
}

void Packs::CountLoad(uint64_t nanoseconds, uint64_t hash_nanoseconds, uint64_t bytes) {
  stats_.fingerprint_ns += nanoseconds;
  stats_.hash_ns += hash_nanoseconds;
  stats_.fingerprint_max_ns = std::max(stats_.fingerprint_max_ns, nanoseconds);
  stats_.fingerprint_bytes += bytes;
  if (bytes) ++stats_.fingerprints;
}

void Packs::CountReplacementUsed(uint64_t fingerprint) {
  if (used_.insert(fingerprint).second) ++stats_.replacements_used;
}

void Packs::CountUploaded(uint64_t gpu_bytes) {
  ++uploads_;
  upload_bytes_ += gpu_bytes;
}

void Packs::LogSummary(bool force) {
  const uint64_t now = rex::chrono::Clock::QueryHostUptimeMillis();
  if (!force && now - last_summary_ms_ < 60000) return;
  const uint64_t written = dumps_written_.load();
  const bool changed = stats_.fingerprint_ns != stats_logged_.fingerprint_ns ||
                       written != dumps_written_logged_ ||
                       stats_.replacements_used != stats_logged_.replacements_used;
  last_summary_ms_ = now;
  if (!changed) return;
  std::string formats;
  {
    std::vector<std::pair<uint32_t, uint32_t>> counts;
    for (uint32_t i = 0; i < 64; ++i) {
      if (stats_.skipped_formats[i]) counts.emplace_back(stats_.skipped_formats[i], i);
    }
    std::sort(counts.rbegin(), counts.rend());
    for (size_t i = 0; i < counts.size() && i < 6; ++i) {
      formats += (i ? ", " : "") + std::string(FormatInfo::Get(xenos::TextureFormat(counts[i].second))->name) +
                 " " + std::to_string(counts[i].first);
    }
  }
  const auto& s = stats_;
  REXGPU_INFO(
      "Texture packs: command processor time {:.1f} ms ({:.2f} ms since the last summary; "
      "hashing {:.1f} ms, dump copies {:.1f} ms; at most {:.0f} us for a load); {} loads "
      "fingerprinted ({:.1f} MiB); dumped {} ({:.1f} MiB written, {} queued, {} dropped, {} "
      "failed); replacements for {} textures ({} files read, "
      "{} not usable, {} uploaded, "
      "{:.1f} MiB of GPU memory); textures skipped: {} format, {} cube/3D/array/scaled, {} "
      "screen-sized, {} dynamic, {} mips only, {} tiny; {} GPU-written loads{}{}",
      double(s.fingerprint_ns) / 1e6,
      double(s.fingerprint_ns - stats_logged_.fingerprint_ns) / 1e6, double(s.hash_ns) / 1e6,
      double(s.dump_copy_ns) / 1e6, double(s.fingerprint_max_ns) / 1e3, s.fingerprints,
      double(s.fingerprint_bytes) / 1048576.0, written,
      double(dump_bytes_written_.load()) / 1048576.0, s.dumps_queued, s.dumps_dropped,
      dumps_failed_.load(), s.replacements_used,
      decodes_ok_.load() + decodes_failed_.load(), decodes_failed_.load() + unusable_.load(),
      uploads_.load(), double(upload_bytes_.load()) / 1048576.0,
      s.skipped[size_t(Skip::kFormat)], s.skipped[size_t(Skip::kShape)],
      s.skipped[size_t(Skip::kScreen)], s.skipped[size_t(Skip::kDynamic)],
      s.skipped[size_t(Skip::kNoBase)], s.skipped[size_t(Skip::kTiny)],
      s.skipped[size_t(Skip::kGpuWritten)], formats.empty() ? "" : "; formats not handled: ",
      formats);
  stats_logged_ = stats_;
  dumps_written_logged_ = written;
}

}  // namespace rex::graphics::texture_pack
