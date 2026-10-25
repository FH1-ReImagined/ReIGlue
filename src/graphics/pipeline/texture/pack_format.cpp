/**
 * FH1 (#384): texture pack file formats (pack_format.h).
 */

#include <rex/graphics/pipeline/texture/pack_format.h>

#include <algorithm>
#include <bit>
#include <cstring>

namespace rex::graphics::texture_pack {

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 |
         uint32_t(uint8_t(d)) << 24;
}

}  // namespace

bool IsBlockCompressed(Format format) { return format >= Format::kBC1; }

uint32_t ElementBytes(Format format) {
  switch (format) {
    case Format::kR8:
      return 1;
    case Format::kRG8:
      return 2;
    case Format::kRGBA8:
    case Format::kBGRA8:
      return 4;
    case Format::kBC1:
    case Format::kBC4:
      return 8;
    case Format::kBC2:
    case Format::kBC3:
    case Format::kBC5:
    case Format::kBC7:
      return 16;
    default:
      return 0;
  }
}

uint64_t LevelBytes(Format format, uint32_t width, uint32_t height) {
  if (IsBlockCompressed(format)) {
    return uint64_t((width + 3) / 4) * ((height + 3) / 4) * ElementBytes(format);
  }
  return uint64_t(width) * height * ElementBytes(format);
}

const char* FormatName(Format format) {
  switch (format) {
    case Format::kR8:
      return "R8";
    case Format::kRG8:
      return "RG8";
    case Format::kRGBA8:
      return "RGBA8";
    case Format::kBGRA8:
      return "BGRA8";
    case Format::kBC1:
      return "BC1";
    case Format::kBC2:
      return "BC2";
    case Format::kBC3:
      return "BC3";
    case Format::kBC4:
      return "BC4";
    case Format::kBC5:
      return "BC5";
    case Format::kBC7:
      return "BC7";
    default:
      return "none";
  }
}

std::vector<uint8_t> EncodeDds(Format format, uint32_t width, uint32_t height,
                               const std::vector<std::vector<uint8_t>>& levels) {
  uint32_t header[31] = {};
  bool dx10 = false;
  uint32_t dxgi = 0;
  header[0] = 124;
  header[1] = 0x1 | 0x2 | 0x4 | 0x1000;  // caps, height, width, pixel format
  header[2] = height;
  header[3] = width;
  header[6] = uint32_t(levels.size());
  if (levels.size() > 1) header[1] |= 0x20000;  // mip map count
  header[18] = 32;
  if (IsBlockCompressed(format)) {
    header[1] |= 0x80000;  // linear size
    header[4] = uint32_t(LevelBytes(format, width, height));
    header[19] = 0x4;  // four CC
  } else {
    header[1] |= 0x8;  // pitch
    header[4] = width * ElementBytes(format);
  }
  switch (format) {
    case Format::kBC1:
      header[20] = FourCC('D', 'X', 'T', '1');
      break;
    case Format::kBC2:
      header[20] = FourCC('D', 'X', 'T', '3');
      break;
    case Format::kBC3:
      header[20] = FourCC('D', 'X', 'T', '5');
      break;
    case Format::kBC4:
      header[20] = FourCC('A', 'T', 'I', '1');
      break;
    case Format::kBC5:
      header[20] = FourCC('A', 'T', 'I', '2');
      break;
    case Format::kBC7:
      dx10 = true;
      dxgi = 98;  // DXGI_FORMAT_BC7_UNORM
      break;
    case Format::kRGBA8:
    case Format::kBGRA8:
      header[19] = 0x40 | 0x1;  // RGB, alpha pixels
      header[21] = 32;
      header[22] = format == Format::kRGBA8 ? 0x000000FF : 0x00FF0000;
      header[23] = 0x0000FF00;
      header[24] = format == Format::kRGBA8 ? 0x00FF0000 : 0x000000FF;
      header[25] = 0xFF000000;
      break;
    case Format::kR8:
      header[19] = 0x20000;  // luminance
      header[21] = 8;
      header[22] = 0xFF;
      break;
    case Format::kRG8:
      dx10 = true;
      dxgi = 49;  // DXGI_FORMAT_R8G8_UNORM
      break;
    default:
      return {};
  }
  if (dx10) {
    header[19] = 0x4;
    header[20] = FourCC('D', 'X', '1', '0');
  }
  header[26] = 0x1000;  // texture
  if (levels.size() > 1) header[26] |= 0x8 | 0x400000;  // complex, mip map
  std::vector<uint8_t> out(4 + sizeof(header));
  std::memcpy(out.data(), "DDS ", 4);
  std::memcpy(out.data() + 4, header, sizeof(header));
  if (dx10) {
    const uint32_t extension[5] = {dxgi, 3 /* 2D */, 0, 1, 0};
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(extension),
               reinterpret_cast<const uint8_t*>(extension) + sizeof(extension));
  }
  for (const std::vector<uint8_t>& level : levels) out.insert(out.end(), level.begin(), level.end());
  return out;
}

bool DecodeDds(const uint8_t* data, size_t size, Decoded& out, std::string& error) {
  if (size < 128 || std::memcmp(data, "DDS ", 4)) {
    error = "not a DDS file";
    return false;
  }
  uint32_t header[31];
  std::memcpy(header, data + 4, sizeof(header));
  if (header[0] != 124) {
    error = "bad DDS header";
    return false;
  }
  const uint32_t flags = header[1], height = header[2], width = header[3];
  const uint32_t pixel_flags = header[19], four_cc = header[20], bits = header[21];
  if (header[27] & 0x200) {
    error = "cube maps are not supported yet";
    return false;
  }
  if ((header[27] & 0x200000) || ((flags & 0x800000) && header[5] > 1)) {
    error = "volume textures are not supported yet";
    return false;
  }
  size_t offset = 128;
  Format format = Format::kNone;
  bool fill_alpha = false;
  if (pixel_flags & 0x4) {
    if (four_cc == FourCC('D', 'X', 'T', '1')) {
      format = Format::kBC1;
    } else if (four_cc == FourCC('D', 'X', 'T', '2') || four_cc == FourCC('D', 'X', 'T', '3')) {
      format = Format::kBC2;
    } else if (four_cc == FourCC('D', 'X', 'T', '4') || four_cc == FourCC('D', 'X', 'T', '5')) {
      format = Format::kBC3;
    } else if (four_cc == FourCC('A', 'T', 'I', '1') || four_cc == FourCC('B', 'C', '4', 'U')) {
      format = Format::kBC4;
    } else if (four_cc == FourCC('A', 'T', 'I', '2') || four_cc == FourCC('B', 'C', '5', 'U')) {
      format = Format::kBC5;
    } else if (four_cc == FourCC('D', 'X', '1', '0')) {
      if (size < 148) {
        error = "truncated DX10 header";
        return false;
      }
      uint32_t extension[5];
      std::memcpy(extension, data + 128, sizeof(extension));
      offset = 148;
      if (extension[1] != 3 || extension[3] > 1 || (extension[2] & 0x4)) {
        error = "only single 2D textures are supported (no arrays or cube maps)";
        return false;
      }
      switch (extension[0]) {
        case 71:  // BC1_UNORM
        case 72:  // BC1_UNORM_SRGB (sampled as UNORM: the game does its own gamma)
          format = Format::kBC1;
          break;
        case 74:
        case 75:
          format = Format::kBC2;
          break;
        case 77:
        case 78:
          format = Format::kBC3;
          break;
        case 80:
          format = Format::kBC4;
          break;
        case 83:
          format = Format::kBC5;
          break;
        case 98:
        case 99:
          format = Format::kBC7;
          break;
        case 28:
        case 29:
          format = Format::kRGBA8;
          break;
        case 87:
        case 91:
          format = Format::kBGRA8;
          break;
        case 61:
          format = Format::kR8;
          break;
        case 49:
          format = Format::kRG8;
          break;
        default:
          error = "unsupported DXGI format " + std::to_string(extension[0]);
          return false;
      }
    } else {
      error = "unsupported four CC";
      return false;
    }
  } else if ((pixel_flags & 0x40) && bits == 32) {
    if (header[22] == 0xFF && header[23] == 0xFF00 && header[24] == 0xFF0000) {
      format = Format::kRGBA8;
    } else if (header[22] == 0xFF0000 && header[23] == 0xFF00 && header[24] == 0xFF) {
      format = Format::kBGRA8;
    } else {
      error = "unsupported 32-bit RGB channel order";
      return false;
    }
    fill_alpha = !(pixel_flags & 0x1);
  } else if ((pixel_flags & 0x20000) && bits == 8) {
    format = Format::kR8;
  } else if ((pixel_flags & 0x20000) && (pixel_flags & 0x1) && bits == 16) {
    format = Format::kRG8;  // luminance + alpha: R and G
  } else {
    error = "unsupported pixel format (use BC1-5/7, RGBA8, BGRA8, R8 or RG8)";
    return false;
  }
  if (!width || !height || width > kMaxFileSize || height > kMaxFileSize) {
    error = "bad size " + std::to_string(width) + "x" + std::to_string(height);
    return false;
  }
  uint32_t level_count = (flags & 0x20000) && header[6] ? header[6] : 1;
  level_count = std::min(level_count, uint32_t(std::bit_width(std::max(width, height))));
  // Rows of an uncompressed image are packed (as DirectXTex reads them) unless the header's
  // pitch (DDSD_PITCH, dwPitchOrLinearSize) says they are padded. A padded file's mip levels
  // must be padded the known way: rows to 4 bytes (Direct3D 9 surfaces, what writers that pad
  // do); any other padding is accepted in a file without mips only. Block-compressed data has no
  // row padding, and its linear size is not checked (writers disagree on its value).
  enum class Rows { kPacked, kFourBytes, kStated };
  Rows rows = Rows::kPacked;
  uint64_t stated_pitch = 0;
  const uint32_t element_bytes = ElementBytes(format);
  auto row_bytes = [&](uint32_t level_width) {
    return IsBlockCompressed(format) ? uint64_t((level_width + 3) / 4) * element_bytes
                                     : uint64_t(level_width) * element_bytes;
  };
  auto rows_of = [&](uint32_t level_height) {
    return IsBlockCompressed(format) ? (level_height + 3) / 4 : level_height;
  };
  auto pitch_of = [&](uint32_t level_width, Rows layout) -> uint64_t {
    const uint64_t packed = row_bytes(level_width);
    switch (layout) {
      case Rows::kFourBytes:
        return (packed + 3) & ~uint64_t(3);
      case Rows::kStated:
        return stated_pitch;
      default:
        return packed;
    }
  };
  auto total_of = [&](Rows layout) {
    uint64_t total = 0;
    for (uint32_t level = 0; level < level_count; ++level) {
      total += pitch_of(std::max(width >> level, 1u), layout) *
               rows_of(std::max(height >> level, 1u));
    }
    return total;
  };
  if (!IsBlockCompressed(format)) {
    const uint64_t packed_row = row_bytes(width);
    const uint64_t four_byte_row = (packed_row + 3) & ~uint64_t(3);
    if ((flags & 0x8) && header[4] && header[4] != packed_row) {
      if (header[4] < packed_row) {
        error = "its header's row pitch (" + std::to_string(header[4]) +
                " bytes) is shorter than a row (" + std::to_string(packed_row) + " bytes)";
        return false;
      }
      if (header[4] == four_byte_row) {
        rows = Rows::kFourBytes;
      } else if (level_count == 1) {
        rows = Rows::kStated;
        stated_pitch = header[4];
      } else {
        error = "its rows are padded to " + std::to_string(header[4]) + " bytes (a row is " +
                std::to_string(packed_row) +
                "), which says nothing about how its mip levels are padded: save it without row "
                "padding";
        return false;
      }
    } else if (level_count > 1 && total_of(Rows::kFourBytes) != total_of(Rows::kPacked) &&
               size - offset == total_of(Rows::kFourBytes)) {
      // The first row needs no padding, but the file is exactly the size of levels with rows
      // padded to 4 bytes (old Direct3D 9 writers): its small levels have them.
      rows = Rows::kFourBytes;
    }
  }
  const uint64_t needed = total_of(rows);
  if (needed > size - offset) {
    error = "the file is shorter than its header says (" + std::to_string(level_count) +
            " levels of " + std::to_string(width) + "x" + std::to_string(height) + " " +
            FormatName(format) + " need " + std::to_string(needed) + " bytes after the header, it has " +
            std::to_string(size - offset) + ")";
    return false;
  }
  out.format = format;
  out.width = width;
  out.height = height;
  out.levels.clear();
  out.data.clear();
  for (uint32_t level = 0; level < level_count; ++level) {
    const uint32_t level_width = std::max(width >> level, 1u);
    const uint32_t level_height = std::max(height >> level, 1u);
    const uint64_t row = row_bytes(level_width), pitch = pitch_of(level_width, rows);
    const uint32_t row_count = rows_of(level_height);
    const uint64_t out_offset = (out.data.size() + 15) & ~uint64_t(15);
    out.data.resize(size_t(out_offset));
    for (uint32_t y = 0; y < row_count; ++y) {
      const uint8_t* source = data + offset + y * pitch;
      out.data.insert(out.data.end(), source, source + row);
    }
    out.levels.push_back({out_offset, row * row_count, level_width, level_height});
    offset += size_t(pitch * row_count);
  }
  if (fill_alpha) {
    for (const Level& level : out.levels) {
      for (uint64_t i = 3; i < level.size; i += 4) out.data[size_t(level.offset + i)] = 255;
    }
  }
  return true;
}

std::vector<std::string> ParsePackOrder(const std::string& value, size_t& refused) {
  std::vector<std::string> names;
  size_t start = 0;
  while (start <= value.size()) {
    size_t end = value.find('|', start);
    if (end == std::string::npos) end = value.size();
    std::string name = value.substr(start, end - start);
    start = end + 1;
    if (name.empty()) continue;
    std::string lower = name;
    for (char& c : lower) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (name == ".." || name.find_first_of("/\\:") != std::string::npos || lower == "dump") {
      ++refused;
      continue;
    }
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
  }
  return names;
}

}  // namespace rex::graphics::texture_pack
