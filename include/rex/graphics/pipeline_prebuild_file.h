#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

// The shader prebuild's pipeline list (#226): <cache root>/shaders/prebuild/<title>.<fbo|fsi>.vk.xpl,
// written by the launcher from the list it ships, next to <title>.xsh, the shaders it rebuilt from
// the player's own game files. The Vulkan pipeline cache adds the listed pipelines this device and
// render partition can use to the stored ones at every start, so the warm-up builds them too.
//
// A pipeline description's bit fields are laid out differently by MSVC and by Clang/GCC (the stored
// .xpso records are not portable), so the list holds each field by name, one byte per field, and
// the description is assembled field by field (ToDescription). All values are little-endian.
namespace rex::graphics::pipeline_prebuild_file {

// 'XEPL'.
constexpr uint32_t kMagic = 0x4C504558;
constexpr uint32_t kFormat = 1;
constexpr size_t kHeaderSize = 40;
constexpr size_t kRecordSize = 96;

// Record flags. Render partitions with split scene and HUD rendering (fh1 live graphics):
// the whole frame in one store (main), the HUD at output resolution (ui) or the scene (scene).
constexpr uint32_t kPartitionMain = 1u << 0;
constexpr uint32_t kPartitionUi = 1u << 1;
constexpr uint32_t kPartitionScene = 1u << 2;
constexpr uint32_t kPartitionMask = kPartitionMain | kPartitionUi | kPartitionScene;
constexpr uint32_t kKnownFlags = kPartitionMask;

// Field order of a record (tools/pipeline_list.py writes the same order).
enum Field : uint8_t {
  // RenderPassKey.
  kMsaaSamples,
  kDepthAndColorUsed,
  kDepthFormat,
  kColor0ViewFormat,
  kColor1ViewFormat,
  kColor2ViewFormat,
  kColor3ViewFormat,
  kColorRtsUseTransferFormats,
  // Pipeline state.
  kGeometryShader,
  kPrimitiveTopology,
  kPrimitiveRestart,
  kTessellationMode,
  kDepthClampEnable,
  kPolygonMode,
  kCullFront,
  kCullBack,
  kFrontFaceClockwise,
  kRasterizerDiscard,
  kDepthWriteEnable,
  kDepthCompareOp,
  kStencilTestEnable,
  kStencilFrontFailOp,
  kStencilFrontPassOp,
  kStencilFrontDepthFailOp,
  kStencilFrontCompareOp,
  kStencilBackFailOp,
  kStencilBackPassOp,
  kStencilBackDepthFailOp,
  kStencilBackCompareOp,
  kSampleRateShading,
  // Render target 0; render targets 1-3 follow with the same fields.
  kRtSrcColorBlendFactor,
  kRtDstColorBlendFactor,
  kRtColorBlendOp,
  kRtSrcAlphaBlendFactor,
  kRtDstAlphaBlendFactor,
  kRtAlphaBlendOp,
  kRtColorWriteMask,
  kFirstRenderTargetField = kRtSrcColorBlendFactor,
};
constexpr size_t kRenderTargetFieldCount = 7;
constexpr size_t kRenderTargetCount = 4;
constexpr size_t kFieldCount = kFirstRenderTargetField + kRenderTargetFieldCount * kRenderTargetCount;
static_assert(kFieldCount == 58);
static_assert(36 + kFieldCount <= kRecordSize);

// Number of valid values of each field: the enum's values, which may be fewer than its bits hold
// (a blend factor has 4 bits and 15 values). Anything else is never put into a description.
constexpr std::array<uint8_t, kFirstRenderTargetField + kRenderTargetFieldCount> kFieldValueCount = {
    3, 32, 2, 16, 16, 16, 16, 2,                                   // render pass key
    4, 8, 2, 3, 2, 3, 2, 2, 2, 2, 2, 8, 2, 8, 8, 8, 8, 8, 8, 8, 8, 2,  // state
    15, 15, 5, 15, 15, 5, 16,                                      // render target
};
constexpr uint8_t ValueCount(size_t field) {
  return kFieldValueCount[field < kFirstRenderTargetField
                              ? field
                              : kFirstRenderTargetField +
                                    (field - kFirstRenderTargetField) % kRenderTargetFieldCount];
}

struct Entry {
  uint64_t vertex_shader_hash = 0;
  uint64_t vertex_shader_modification = 0;
  uint64_t pixel_shader_hash = 0;
  uint64_t pixel_shader_modification = 0;
  uint32_t flags = 0;
  std::array<uint8_t, kFieldCount> fields{};
  uint8_t RenderTarget(size_t index, size_t field) const {
    return fields[kFirstRenderTargetField + index * kRenderTargetFieldCount + field];
  }
  bool operator<(const Entry& other) const {
    if (vertex_shader_hash != other.vertex_shader_hash)
      return vertex_shader_hash < other.vertex_shader_hash;
    if (vertex_shader_modification != other.vertex_shader_modification)
      return vertex_shader_modification < other.vertex_shader_modification;
    if (pixel_shader_hash != other.pixel_shader_hash)
      return pixel_shader_hash < other.pixel_shader_hash;
    if (pixel_shader_modification != other.pixel_shader_modification)
      return pixel_shader_modification < other.pixel_shader_modification;
    return fields < other.fields;
  }
};

struct Versions {
  uint32_t description = 0;
  uint32_t modification = 0;
};

// FNV-1a, 64 bits, of the records: a damaged or cut list is refused as a whole.
inline uint64_t Checksum(const uint8_t* data, size_t size) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * UINT64_C(1099511628211);
  return hash;
}

inline uint32_t ReadU32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t ReadU64(const uint8_t* p) { return ReadU32(p) | uint64_t(ReadU32(p + 4)) << 32; }
inline void WriteU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
inline void WriteU64(uint8_t* p, uint64_t v) {
  WriteU32(p, uint32_t(v));
  WriteU32(p + 4, uint32_t(v >> 32));
}

enum class Status {
  kOk,
  // Not a pipeline list, an unknown format, or damaged (size, checksum).
  kDamaged,
  // A list for the other render target path (fbo or fsi).
  kOtherApi,
  // Written for other description or modification versions: the fields may mean something else.
  kOtherVersions,
};
inline const char* StatusText(Status status) {
  switch (status) {
    case Status::kOk:
      return "ok";
    case Status::kDamaged:
      return "damaged or not a pipeline list";
    case Status::kOtherApi:
      return "for the other render target path";
    case Status::kOtherVersions:
      return "for other pipeline description versions";
  }
  return "?";
}

struct ParseResult {
  Status status = Status::kDamaged;
  uint32_t api = 0;
  Versions versions;
  // In file order.
  std::vector<Entry> entries;
  // Records with a value no description can hold, or unknown flags: skipped.
  size_t invalid = 0;
};

inline ParseResult Parse(const uint8_t* data, size_t size, uint32_t api, Versions versions) {
  ParseResult result;
  if (size < kHeaderSize || ReadU32(data) != kMagic || ReadU32(data + 4) != kFormat) return result;
  result.api = ReadU32(data + 8);
  result.versions = {ReadU32(data + 12), ReadU32(data + 16)};
  const uint32_t field_count = ReadU32(data + 20);
  const uint64_t count = ReadU32(data + 24);
  const uint32_t record_size = ReadU32(data + 28);
  if (field_count != kFieldCount || record_size != kRecordSize ||
      size != kHeaderSize + count * kRecordSize ||
      ReadU64(data + 32) != Checksum(data + kHeaderSize, size - kHeaderSize)) {
    return result;
  }
  if (result.api != api) {
    result.status = Status::kOtherApi;
    return result;
  }
  if (result.versions.description != versions.description ||
      result.versions.modification != versions.modification) {
    result.status = Status::kOtherVersions;
    return result;
  }
  result.status = Status::kOk;
  result.entries.reserve(size_t(count));
  for (uint64_t i = 0; i < count; ++i) {
    const uint8_t* p = data + kHeaderSize + i * kRecordSize;
    Entry entry;
    entry.vertex_shader_hash = ReadU64(p);
    entry.vertex_shader_modification = ReadU64(p + 8);
    entry.pixel_shader_hash = ReadU64(p + 16);
    entry.pixel_shader_modification = ReadU64(p + 24);
    entry.flags = ReadU32(p + 32);
    std::memcpy(entry.fields.data(), p + 36, kFieldCount);
    bool valid = !(entry.flags & ~kKnownFlags) && entry.vertex_shader_hash;
    for (size_t f = 0; valid && f < kFieldCount; ++f) valid = entry.fields[f] < ValueCount(f);
    if (valid) {
      result.entries.push_back(entry);
    } else {
      ++result.invalid;
    }
  }
  return result;
}

// The file as the launcher writes it (tests; tools/pipeline_list.py has the same encoder).
inline std::vector<uint8_t> Encode(const std::vector<Entry>& entries, uint32_t api,
                                   Versions versions) {
  std::vector<uint8_t> out(kHeaderSize + entries.size() * kRecordSize, 0);
  for (size_t i = 0; i < entries.size(); ++i) {
    uint8_t* p = out.data() + kHeaderSize + i * kRecordSize;
    const Entry& e = entries[i];
    WriteU64(p, e.vertex_shader_hash);
    WriteU64(p + 8, e.vertex_shader_modification);
    WriteU64(p + 16, e.pixel_shader_hash);
    WriteU64(p + 24, e.pixel_shader_modification);
    WriteU32(p + 32, e.flags);
    std::memcpy(p + 36, e.fields.data(), kFieldCount);
  }
  WriteU32(out.data(), kMagic);
  WriteU32(out.data() + 4, kFormat);
  WriteU32(out.data() + 8, api);
  WriteU32(out.data() + 12, versions.description);
  WriteU32(out.data() + 16, versions.modification);
  WriteU32(out.data() + 20, uint32_t(kFieldCount));
  WriteU32(out.data() + 24, uint32_t(entries.size()));
  WriteU32(out.data() + 28, uint32_t(kRecordSize));
  WriteU64(out.data() + 32, Checksum(out.data() + kHeaderSize, out.size() - kHeaderSize));
  return out;
}

// What the device and the render partition take from a list.
struct Target {
  // kPartitionMain, kPartitionUi or kPartitionScene. The rest of a description does not depend
  // on the device: the render pass key holds the guest's depth format whatever host format
  // stands for it, and what a device cannot create is left to ArePipelineRequirementsMet.
  uint32_t partition = kPartitionMain;
  // Modification bits this partition never sets (the FH1 fractional raster variant, bit 63, when
  // the raster is not scaled): pipelines with any of them are never used here.
  uint64_t unused_modification_bits = 0;
};
struct SelectResult {
  // Without duplicates, in a stable order.
  std::vector<Entry> entries;
  size_t other_partition = 0;
  size_t unused_variant = 0;
};

inline SelectResult Select(const std::vector<Entry>& entries, const Target& target) {
  SelectResult result;
  std::set<Entry> seen;
  auto add = [&](const Entry& e) {
    if (seen.insert(e).second) result.entries.push_back(e);
  };
  for (Entry e : entries) {
    if (!(e.flags & target.partition)) {
      ++result.other_partition;
      continue;
    }
    if ((e.vertex_shader_modification | e.pixel_shader_modification) &
        target.unused_modification_bits) {
      ++result.unused_variant;
      continue;
    }
    // Flags say where the record may be used, not what it is: they are not part of the result.
    e.flags = 0;
    add(e);
  }
  return result;
}

// Fills a pipeline description (the SDK's VulkanPipelineCache::PipelineDescription, all bits
// zero) from an entry, by field name: the compiler places the bits.
template <typename Description>
void ToDescription(const Entry& e, Description& d) {
  d.vertex_shader_hash = e.vertex_shader_hash;
  d.vertex_shader_modification = e.vertex_shader_modification;
  d.pixel_shader_hash = e.pixel_shader_hash;
  d.pixel_shader_modification = e.pixel_shader_modification;
  auto& k = d.render_pass_key;
  const auto& f = e.fields;
#define REX_PREBUILD_SET(target, value) target = static_cast<decltype(target)>(value)
  REX_PREBUILD_SET(k.msaa_samples, f[kMsaaSamples]);
  REX_PREBUILD_SET(k.depth_and_color_used, f[kDepthAndColorUsed]);
  REX_PREBUILD_SET(k.depth_format, f[kDepthFormat]);
  REX_PREBUILD_SET(k.color_0_view_format, f[kColor0ViewFormat]);
  REX_PREBUILD_SET(k.color_1_view_format, f[kColor1ViewFormat]);
  REX_PREBUILD_SET(k.color_2_view_format, f[kColor2ViewFormat]);
  REX_PREBUILD_SET(k.color_3_view_format, f[kColor3ViewFormat]);
  REX_PREBUILD_SET(k.color_rts_use_transfer_formats, f[kColorRtsUseTransferFormats]);
  REX_PREBUILD_SET(d.geometry_shader, f[kGeometryShader]);
  REX_PREBUILD_SET(d.primitive_topology, f[kPrimitiveTopology]);
  REX_PREBUILD_SET(d.primitive_restart, f[kPrimitiveRestart]);
  REX_PREBUILD_SET(d.tessellation_mode, f[kTessellationMode]);
  REX_PREBUILD_SET(d.depth_clamp_enable, f[kDepthClampEnable]);
  REX_PREBUILD_SET(d.polygon_mode, f[kPolygonMode]);
  REX_PREBUILD_SET(d.cull_front, f[kCullFront]);
  REX_PREBUILD_SET(d.cull_back, f[kCullBack]);
  REX_PREBUILD_SET(d.front_face_clockwise, f[kFrontFaceClockwise]);
  REX_PREBUILD_SET(d.rasterizer_discard, f[kRasterizerDiscard]);
  REX_PREBUILD_SET(d.depth_write_enable, f[kDepthWriteEnable]);
  REX_PREBUILD_SET(d.depth_compare_op, f[kDepthCompareOp]);
  REX_PREBUILD_SET(d.stencil_test_enable, f[kStencilTestEnable]);
  REX_PREBUILD_SET(d.stencil_front_fail_op, f[kStencilFrontFailOp]);
  REX_PREBUILD_SET(d.stencil_front_pass_op, f[kStencilFrontPassOp]);
  REX_PREBUILD_SET(d.stencil_front_depth_fail_op, f[kStencilFrontDepthFailOp]);
  REX_PREBUILD_SET(d.stencil_front_compare_op, f[kStencilFrontCompareOp]);
  REX_PREBUILD_SET(d.stencil_back_fail_op, f[kStencilBackFailOp]);
  REX_PREBUILD_SET(d.stencil_back_pass_op, f[kStencilBackPassOp]);
  REX_PREBUILD_SET(d.stencil_back_depth_fail_op, f[kStencilBackDepthFailOp]);
  REX_PREBUILD_SET(d.stencil_back_compare_op, f[kStencilBackCompareOp]);
  REX_PREBUILD_SET(d.sample_rate_shading, f[kSampleRateShading]);
  for (size_t i = 0; i < kRenderTargetCount; ++i) {
    auto& rt = d.render_targets[i];
    REX_PREBUILD_SET(rt.src_color_blend_factor, e.RenderTarget(i, 0));
    REX_PREBUILD_SET(rt.dst_color_blend_factor, e.RenderTarget(i, 1));
    REX_PREBUILD_SET(rt.color_blend_op, e.RenderTarget(i, 2));
    REX_PREBUILD_SET(rt.src_alpha_blend_factor, e.RenderTarget(i, 3));
    REX_PREBUILD_SET(rt.dst_alpha_blend_factor, e.RenderTarget(i, 4));
    REX_PREBUILD_SET(rt.alpha_blend_op, e.RenderTarget(i, 5));
    REX_PREBUILD_SET(rt.color_write_mask, e.RenderTarget(i, 6));
  }
#undef REX_PREBUILD_SET
}

}  // namespace rex::graphics::pipeline_prebuild_file
