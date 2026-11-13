// FH1 (patch 0620): the native renderer's shadow mode, content compare (FH1
// docs/native-renderer.md, "M2: shadow mode"). Every native_shadow_compare_every shadow mask
// groups (the game's cvar; the island's begin packet says which), the shadow mask's "softer"
// blur is also computed natively from the draw records alone, and its result is compared on the
// GPU with the mask the emulated passes resolved:
// - the blur's input, its quad and its depth test come from the records: the horizontal blur
//   draw's fetch constant for the mask and its vertex data, the cascade passes' fetch constant
//   0 (the depth pre-pass resolve); nothing from the register file;
// - two compute dispatches (the prototype's, FH1 docs/native-renderer.md): horizontal into a
//   scratch buffer, vertical into a private buffer, right after the guest's horizontal draw
//   (the mask's memory still holds the cascades' resolve);
// - at the group's end, after the world pass's resolve of the mask, a third dispatch compares
//   the two and counts; the counts are read back once that submission is done.
// Nothing the guest sees is written. Softer filter at render scale 1 only (the mask of a scaled
// render target is not in guest memory); otherwise it logs why once and only the state check
// runs.
#include <rex/graphics/vulkan/command_processor.h>

#include <rex/graphics/fh1_host_packet.h>
#include <rex/graphics/vulkan/fh1_dlss.h>
#include <rex/graphics/vulkan/fh1_gpu_profile.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/graphics/vulkan/texture_cache.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

#include "fh1_world_ao.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace rex::graphics::vulkan {

namespace {

namespace fh = fh1_host;

// The softer blur's pixel shaders (BlurShadowMaskH, BlurShadowMaskV).
constexpr uint64_t kBlurH = 0xDBD55E02669D5CB1ull;

constexpr char kBlurShader[] = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
// The shared memory from the lower of the mask and the depth; the horizontal pass's output.
layout(set = 0, binding = 0) buffer Shared { uint shm[]; };
layout(set = 1, binding = 0) buffer Temp { uint tmp[]; };
// Compare mode: the native result (linear), then the statistics.
layout(set = 2, binding = 0) buffer Aux { uint aux[]; };
layout(push_constant) uniform Params {
  ivec4 size;   // width, height, tiled pitch (texels, 32-aligned), endian
  vec4 map;     // texel u = map.x * (pixel x + centre) + map.y; v = map.z * (y + centre) + map.w
  vec4 misc;    // centre, -, -, -
  ivec4 offs;   // mask and depth word offsets in shm, quad z as 20e4, depth endian (-1: no test)
  ivec4 swz;    // the byte of each of red, green, blue, alpha in a texel (the fetch's swizzle)
} pc;
uint swapEndian(uint v, int e) {
  if (e == 1) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
  if (e == 2) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
  if (e == 3) return (v << 16) | (v >> 16);
  return v;
}
// Xenos 2D tiled byte offset (pipeline/texture/util.cpp GetTiledOffset2D), 4 bytes a texel.
int tiledIndex(ivec2 p) {
  int pitch = pc.size.z;
  int macro = ((p.x >> 5) + (p.y >> 5) * (pitch >> 5)) << 9;
  int micro = ((p.x & 7) + ((p.y & 0xE) << 2)) << 2;
  int offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((p.y & 1) << 4);
  int b = ((offset & ~0x1FF) << 3) + ((p.y & 16) << 7) + ((offset & 0x1C0) << 2) +
          (((((p.y & 8) >> 2) + (p.x >> 3)) & 3) << 6) + (offset & 0x3F);
  return b >> 2;
}
uint texelWord(ivec2 p) {
  p = clamp(p, ivec2(0), pc.size.xy - 1);
#if VERTICAL
  return tmp[p.y * pc.size.x + p.x];
#else
  return swapEndian(shm[pc.offs.x + tiledIndex(p)], pc.size.w);
#endif
}
float red(ivec2 p) { return float((texelWord(p) >> (8 * pc.swz.x)) & 0xFFu) / 255.0; }
// A bilinear fetch of red at a texel-space position (centres at +0.5), weights in 1/256 steps.
float tap(vec2 t) {
  vec2 b = t - 0.5;
  vec2 i = floor(b);
  vec2 f = round((b - i) * 256.0) / 256.0;
  ivec2 p = ivec2(i);
  float r00 = red(p), r10 = red(p + ivec2(1, 0)), r01 = red(p + ivec2(0, 1)),
        r11 = red(p + ivec2(1, 1));
  return mix(mix(r00, r10, f.x), mix(r01, r11, f.x), f.y);
}
// The guest draw's depth test: LEQUAL of the quad's z against the pre-pass depth (D24FS8,
// depth in the upper 24 bits as 20e4, which orders as unsigned integers). No depth writes.
bool depthPasses(ivec2 p) {
  if (pc.offs.w < 0) return true;
  uint d = swapEndian(shm[pc.offs.y + tiledIndex(p)], pc.offs.w) >> 8;
  return uint(pc.offs.z) <= d;
}
// The quad maps pixels to texels one to one (texel centre = pixel + 0.5, checked on the CPU),
// so each tap at +-0.5, +-1.5, +-2.5 texels is the mean of two neighbours: seven texels a pixel.
// The horizontal pass also stores the depth test's result after its words (tmp[w * h + i]).
uint linear(ivec2 p) { return uint(p.y * pc.size.x + p.x); }
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, pc.size.xy))) return;
  uint i = linear(p);
  uint n = uint(pc.size.x * pc.size.y);
#if VERTICAL
  bool passes = tmp[n + i] != 0u;
#else
  bool passes = depthPasses(p);
  tmp[n + i] = passes ? 1u : 0u;
#endif
  if (!passes) {
    // The render target keeps what it had: the mask as resolved (the horizontal pass's input).
#if VERTICAL && OUT_AUX
    aux[i] = tmp[i];
#elif VERTICAL
    shm[pc.offs.x + tiledIndex(p)] = swapEndian(tmp[i], pc.size.w);
#else
    tmp[i] = texelWord(p);
#endif
    return;
  }
#if VERTICAL
  ivec2 d = ivec2(0, 1);
#else
  ivec2 d = ivec2(1, 0);
#endif
  float r[7];
  for (int k = 0; k < 7; ++k) r[k] = red(p + (k - 3) * d);
  const float h = 0.5;
  const vec3 w = vec3(0.272727272727, 0.181818181818, 0.0454545454545);
  float acc = w.x * (sqrt((r[3] + r[4]) * h) + sqrt((r[2] + r[3]) * h)) +
              w.y * (sqrt((r[4] + r[5]) * h) + sqrt((r[1] + r[2]) * h)) +
              w.z * (sqrt((r[5] + r[6]) * h) + sqrt((r[0] + r[1]) * h));
  acc = acc * acc;
  // oC0 = (r, 1, 0, 0), to 8 bits as the host render target stores it (misc.y: 0.5 rounds to
  // nearest, 0 truncates).
  uint word = (uint(floor(clamp(acc, 0.0, 1.0) * 255.0 + pc.misc.y)) << (8 * pc.swz.x)) |
              (0xFFu << (8 * pc.swz.y));
#if VERTICAL && OUT_AUX
  aux[i] = word;
#elif VERTICAL
  shm[pc.offs.x + tiledIndex(p)] = swapEndian(word, pc.size.w);
#else
  tmp[i] = word;
#endif
}
)glsl";

// Compare mode: the guest's result (the mask's memory after the world pass's resolve) against
// the native one (set 1 here): aux[0] pixels, [1] differing words, [2] max |red difference|,
// [3] sum |red difference|, [4] other channels differ, [5] depth-test failures,
// [8 + d] histogram of |red difference| d (0..7, 7 = 7 or more).
constexpr char kCompareShader[] = R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) buffer Shared { uint shm[]; };
layout(set = 1, binding = 0) buffer Native { uint nat[]; };
layout(set = 2, binding = 0) buffer Stats { uint stats[]; };
layout(push_constant) uniform Params {
  ivec4 size; vec4 map; vec4 misc; ivec4 offs; ivec4 swz;
} pc;
uint swapEndian(uint v, int e) {
  if (e == 1) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
  if (e == 2) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
  if (e == 3) return (v << 16) | (v >> 16);
  return v;
}
int tiledIndex(ivec2 p) {
  int pitch = pc.size.z;
  int macro = ((p.x >> 5) + (p.y >> 5) * (pitch >> 5)) << 9;
  int micro = ((p.x & 7) + ((p.y & 0xE) << 2)) << 2;
  int offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((p.y & 1) << 4);
  int b = ((offset & ~0x1FF) << 3) + ((p.y & 16) << 7) + ((offset & 0x1C0) << 2) +
          (((((p.y & 8) >> 2) + (p.x >> 3)) & 3) << 6) + (offset & 0x3F);
  return b >> 2;
}
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, pc.size.xy))) return;
  uint g = swapEndian(shm[pc.offs.x + tiledIndex(p)], pc.size.w);
  uint n = nat[p.y * pc.size.x + p.x];
  atomicAdd(stats[0], 1u);
  bool fails = false;
  if (pc.offs.w >= 0) {
    uint d = swapEndian(shm[pc.offs.y + tiledIndex(p)], pc.offs.w) >> 8;
    fails = uint(pc.offs.z) > d;
    if (fails) atomicAdd(stats[5], 1u);
  }
  if (g == n) { atomicAdd(stats[8], 1u); return; }
  atomicAdd(stats[1], 1u);
  uint sh = uint(8 * pc.swz.x);
  uint dr = uint(abs(int((g >> sh) & 0xFFu) - int((n >> sh) & 0xFFu)));
  atomicMax(stats[2], dr);
  atomicAdd(stats[3], dr);
  if ((g & ~(0xFFu << sh)) != (n & ~(0xFFu << sh))) atomicAdd(stats[4], 1u);
  atomicAdd(stats[8 + min(dr, 7u)], 1u);
  if (dr >= 3u) atomicAdd(stats[fails ? 7 : 6], 1u);
  // Samples: 4 of red differences of 3/255 or more, 4 of the others.
  uint k = atomicAdd(stats[dr >= 3u ? 24 : 25], 1u);
  if (k < 4u) {
    uint b = 32u + (dr >= 3u ? 0u : 16u) + 4u * k;
    stats[b] = uint(p.x) | (uint(p.y) << 16);
    stats[b + 1u] = g;
    stats[b + 2u] = n;
    stats[b + 3u] = fails ? 1u : 0u;
  }
}
)glsl";

struct Params {
  int32_t size[4];
  float map[4];
  float misc[4];
  int32_t offs[4];
  int32_t swz[4];
};

}  // namespace

struct VulkanCommandProcessor::Fh1ShadowCompare {
  bool failed = false;
  bool wanted = false, computed = false;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipelines[3] = {};  // horizontal, vertical into `result`, compare
  VkBuffer result = VK_NULL_HANDLE, stats = VK_NULL_HANDLE;
  VkDeviceMemory result_memory = VK_NULL_HANDLE, stats_memory = VK_NULL_HANDLE;
  VkDeviceSize result_size = 0;
  uint32_t* stats_mapped = nullptr;
  uint64_t stats_submission = 0;
  Params last{};
  // The cascade passes' fetch constant 0, from their records.
  uint32_t depth_fetch[6] = {};
  bool depth_valid = false;
  uint64_t sums[16] = {};
  uint64_t frames = 0, refused = 0;
  uint32_t max_dr = 0;
  bool logged_refusal = false;
};

void VulkanCommandProcessor::Fh1ShadowCompareShutdown() {
  auto* c = fh1_shadow_compare_;
  if (!c)
    return;
  if (c->frames)
    REXGPU_INFO(
        "FH1 native shadow compare (at exit): {} compares, {} pixels: {} differ "
        "({:.4f}%), max |red diff| {}; |diff| histogram 0:{} 1:{} 2:{} 3+:{}; {} refused",
        c->frames, c->sums[0], c->sums[1],
        c->sums[0] ? 100.0 * double(c->sums[1]) / double(c->sums[0]) : 0.0, c->max_dr, c->sums[8],
        c->sums[9], c->sums[10],
        c->sums[11] + c->sums[12] + c->sums[13] + c->sums[14] + c->sums[15], c->refused);
  const auto* device = GetVulkanDevice();
  const auto& f = device->functions();
  for (VkPipeline p : c->pipelines)
    if (p)
      f.vkDestroyPipeline(device->device(), p, nullptr);
  if (c->result)
    f.vkDestroyBuffer(device->device(), c->result, nullptr);
  if (c->stats)
    f.vkDestroyBuffer(device->device(), c->stats, nullptr);
  if (c->result_memory)
    f.vkFreeMemory(device->device(), c->result_memory, nullptr);
  if (c->stats_memory)
    f.vkFreeMemory(device->device(), c->stats_memory, nullptr);
  if (c->layout)
    f.vkDestroyPipelineLayout(device->device(), c->layout, nullptr);
  delete c;
  fh1_shadow_compare_ = nullptr;
}

void VulkanCommandProcessor::Fh1HostPacket(const uint32_t* words, uint32_t count) {
  CommandProcessor::Fh1HostPacket(words, count);  // the state check, and the messages
  if (count < 2)
    return;
  const uint32_t op = words[1];
  if (op != fh::kOpIslandBegin && op != fh::kOpRecord && op != fh::kOpIslandEnd)
    return;
  if (!fh1_shadow_compare_)
    fh1_shadow_compare_ = new Fh1ShadowCompare;
  auto& c = *fh1_shadow_compare_;
  if (op == fh::kOpIslandBegin) {
    c.wanted = count > 4 && (words[4] & fh::kIslandCompareContents);
    c.computed = false;
    return;
  }
  if (op == fh::kOpIslandEnd) {
    if (c.computed)
      Fh1ShadowCompareRun();
    c.computed = false;
    return;
  }
  constexpr uint32_t kFixed = fh::kRecordHeaderWords + fh::kRegisterShadowWords + fh::kFetchWords;
  if (count < kFixed)
    return;
  const Shader* ps = active_pixel_shader();
  if (!ps)
    return;
  const uint32_t* fetch = words + fh::kRecordHeaderWords + fh::kRegisterShadowWords;
  if (fh1_world_ao::IsShadowPass(ps->ucode_data_hash())) {
    std::memcpy(c.depth_fetch, fetch, sizeof(c.depth_fetch));
    c.depth_valid = true;
    return;
  }
  if (c.wanted && !c.computed && ps->ucode_data_hash() == kBlurH &&
      (words[fh::kRecFlags] & fh::kRecordVertexData) && count >= kFixed + fh::kVertexDataWords &&
      ps->is_ucode_analyzed() && !ps->texture_bindings().empty()) {
    c.computed = Fh1ShadowCompareCompute(words, count);
    if (!c.computed)
      ++c.refused;
  }
}

bool VulkanCommandProcessor::Fh1ShadowCompareCompute(const uint32_t* words, uint32_t count) {
  auto& c = *fh1_shadow_compare_;
  if (c.failed)
    return false;
  const auto* device = GetVulkanDevice();
  const auto& f = device->functions();
  const VkDevice d = device->device();
  if (!c.layout) {
    const VkDescriptorSetLayout set_layout =
        GetSingleTransientDescriptorLayout(SingleTransientDescriptorLayout::kStorageBufferCompute);
    const VkDescriptorSetLayout sets[3] = {set_layout, set_layout, set_layout};
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Params)};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 3;
    info.pSetLayouts = sets;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (f.vkCreatePipelineLayout(d, &info, nullptr, &c.layout) != VK_SUCCESS) {
      c.failed = true;
      return false;
    }
    for (int k = 0; k < 3; ++k) {
      std::string source = k == 2 ? kCompareShader : kBlurShader;
      if (k < 2)
        source.insert(source.find('\n') + 1, k == 0 ? "#define VERTICAL 0\n#define OUT_AUX 0\n"
                                                    : "#define VERTICAL 1\n#define OUT_AUX 1\n");
      std::vector<uint32_t> spirv;
      std::string error;
      if (!CompileFh1Compute(source, spirv, error)) {
        REXGPU_ERROR("FH1 native shadow compare: shader {}: {}", k, error);
        c.failed = true;
        return false;
      }
      c.pipelines[k] = ui::vulkan::util::CreateComputePipeline(device, c.layout, spirv.data(),
                                                               spirv.size() * sizeof(uint32_t));
      if (!c.pipelines[k]) {
        c.failed = true;
        return false;
      }
    }
  }

  // The mask: the fetch constant the horizontal blur's pixel shader samples, from the record.
  const uint32_t* fetch_words = words + fh::kRecordHeaderWords + fh::kRegisterShadowWords;
  const uint32_t slot = active_pixel_shader()->texture_bindings()[0].fetch_constant & 31;
  xenos::xe_gpu_texture_fetch_t fetch;
  static_assert(sizeof(fetch) == 6 * sizeof(uint32_t));
  std::memcpy(&fetch, fetch_words + 6 * slot, sizeof(fetch));
  // The quad: 4 vertices of (x, y, z, u, v), from the record's vertex data.
  float v[20];
  std::memcpy(v, words + fh::kRecordHeaderWords + fh::kRegisterShadowWords + fh::kFetchWords,
              sizeof(v));
  float rect[8] = {v[0], v[1], v[0], v[1], v[3], v[4], v[3], v[4]};
  for (int k = 1; k < 4; ++k) {
    const float* q = v + 5 * k;
    rect[0] = std::min(rect[0], q[0]);
    rect[1] = std::min(rect[1], q[1]);
    rect[2] = std::max(rect[2], q[0]);
    rect[3] = std::max(rect[3], q[1]);
    rect[4] = std::min(rect[4], q[3]);
    rect[5] = std::min(rect[5], q[4]);
    rect[6] = std::max(rect[6], q[3]);
    rect[7] = std::max(rect[7], q[4]);
  }
  const float quad_z = v[2];
  const uint32_t width = uint32_t(fetch.size_2d.width) + 1;
  const uint32_t height = uint32_t(fetch.size_2d.height) + 1;
  const uint32_t pitch = uint32_t(fetch.pitch) << 5;
  const uint32_t address = (uint32_t(fetch.base_address) << 12) & 0x1FFFFFFFu;
  const uint32_t length = pitch * ((height + 31) & ~31u) * 4;
  auto refuse = [&](const std::string& why) {
    if (!c.logged_refusal) {
      c.logged_refusal = true;
      REXGPU_INFO("FH1 native shadow compare: not compared ({}); the state check runs", why);
    }
    return false;
  };
  if (fetch.format != xenos::TextureFormat::k_8_8_8_8 || !fetch.tiled || !pitch ||
      uint32_t(fetch.dimension) != 1)
    return refuse(
        fmt::format("mask format {}, tiled {}", uint32_t(fetch.format), uint32_t(fetch.tiled)));
  if (texture_cache_->IsDrawResolutionScaled())
    return refuse("render scale above 1");
  if (!c.result || c.result_size < VkDeviceSize(width) * height * 4) {
    if (c.result)
      return refuse("the mask changed size");
    c.result_size = VkDeviceSize(width) * height * 4;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            device, c.result_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal, c.result, c.result_memory) ||
        !ui::vulkan::util::CreateDedicatedAllocationBuffer(
            device, 64 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kReadback, c.stats, c.stats_memory) ||
        f.vkMapMemory(d, c.stats_memory, 0, VK_WHOLE_SIZE, 0,
                      reinterpret_cast<void**>(&c.stats_mapped)) != VK_SUCCESS) {
      REXGPU_ERROR("FH1 native shadow compare: buffers");
      c.failed = true;
      return false;
    }
    std::memset(c.stats_mapped, 0, 64 * sizeof(uint32_t));
  }

  // Screen pixel -> texel, as the guest's quad maps it (it must be one to one).
  const int32_t centre_mode =
      uint32_t(register_file_->Get<reg::PA_SU_VTX_CNTL>().pix_center) ? 1 : 0;
  const float sx0 = (rect[0] + 1.0f) * 0.5f * width, sx1 = (rect[2] + 1.0f) * 0.5f * width;
  const float sy0 = (1.0f - rect[3]) * 0.5f * height, sy1 = (1.0f - rect[1]) * 0.5f * height;
  Params params{};
  params.size[0] = int32_t(width);
  params.size[1] = int32_t(height);
  params.size[2] = int32_t(pitch);
  params.size[3] = int32_t(fetch.endianness);
  const float du = (rect[6] - rect[4]) * width / (sx1 - sx0);
  const float dv = (rect[7] - rect[5]) * height / (sy1 - sy0);
  params.map[0] = du;
  params.map[1] = rect[4] * width - sx0 * du;
  params.map[2] = dv;
  params.map[3] = rect[5] * height - sy0 * dv;
  params.misc[0] = centre_mode ? 0.5f : 0.0f;
  const float cx = params.misc[0];
  if (!(std::fabs(params.map[0] - 1.0f) <= 1e-4f) || !(std::fabs(params.map[2] - 1.0f) <= 1e-4f) ||
      !(std::fabs(params.map[0] * cx + params.map[1] - 0.5f) <= 1e-3f) ||
      !(std::fabs(params.map[2] * cx + params.map[3] - 0.5f) <= 1e-3f))
    return refuse("the quad does not map pixels to texels one to one");
  params.misc[1] = 0.5f;  // round to nearest, as the host render target stores it

  if (!BeginSubmission(true))
    return false;
  Fh1GpuZoneScope gpu_zone(deferred_command_buffer_, Fh1GpuProfile::kEffects);
  if (!shared_memory_->RequestRange(address, length))
    return false;
  xenos::xe_gpu_texture_fetch_t depth;
  std::memcpy(&depth, c.depth_fetch, sizeof(depth));
  const uint32_t depth_address = (uint32_t(depth.base_address) << 12) & 0x1FFFFFFFu;
  const bool depth_ok =
      c.depth_valid && depth.tiled && uint32_t(depth.size_2d.width) + 1 == width &&
      uint32_t(depth.size_2d.height) + 1 == height && (uint32_t(depth.pitch) << 5) == pitch &&
      shared_memory_->RequestRange(depth_address, length);
  const uint32_t span_start = depth_ok ? std::min(address, depth_address) : address;
  const uint32_t span_end = depth_ok ? std::max(address, depth_address) + length : address + length;
  const VkDeviceSize temp_size = VkDeviceSize(width) * height * 4 * 2;
  ScratchBufferAcquisition scratch(AcquireScratchGpuBuffer(
      temp_size, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT));
  if (scratch.buffer() == VK_NULL_HANDLE)
    return false;
  VkDescriptorSet set[3];
  for (auto& s : set) {
    s = AllocateSingleTransientDescriptor(SingleTransientDescriptorLayout::kStorageBufferCompute);
    if (!s)
      return false;
  }
  const VkDescriptorBufferInfo infos[3] = {
      {shared_memory_->buffer(), span_start, span_end - span_start},
      {scratch.buffer(), 0, temp_size},
      {c.result, 0, VkDeviceSize(width) * height * 4}};
  VkWriteDescriptorSet writes[3] = {};
  for (int i = 0; i < 3; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set[i];
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &infos[i];
  }
  f.vkUpdateDescriptorSets(d, 3, writes, 0, nullptr);
  params.offs[0] = int32_t((address - span_start) >> 2);
  params.offs[1] = depth_ok ? int32_t((depth_address - span_start) >> 2) : 0;
  params.offs[2] = int32_t(xenos::Float32To20e4(quad_z, true));
  params.offs[3] = depth_ok ? int32_t(depth.endianness) : -1;
  for (int k = 0; k < 4; ++k)
    params.swz[k] = int32_t((fetch.swizzle >> (3 * k)) & 7) & 3;

  const uint32_t groups[2] = {(width + 7) / 8, (height + 7) / 8};
  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  BindExternalComputePipeline(c.pipelines[0]);
  deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, c.layout, 0, 3,
                                                   set, 0, nullptr);
  deferred_command_buffer_.CmdVkPushConstants(c.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                              sizeof(params), &params);
  SubmitBarriers(true);
  deferred_command_buffer_.CmdVkDispatch(groups[0], groups[1], 1);
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  deferred_command_buffer_.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                                &barrier, 0, nullptr, 0, nullptr);
  BindExternalComputePipeline(c.pipelines[1]);
  deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, c.layout, 0, 3,
                                                   set, 0, nullptr);
  deferred_command_buffer_.CmdVkPushConstants(c.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                              sizeof(params), &params);
  deferred_command_buffer_.CmdVkDispatch(groups[0], groups[1], 1);
  scratch.SetStageMask(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  scratch.SetAccessMask(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  c.last = params;
  c.last.offs[0] = int32_t(address >> 2);  // absolute: the compare binds its own span
  c.last.offs[1] = depth_ok ? int32_t(depth_address >> 2) : 0;
  return true;
}

// At the group's end, after the world pass's resolve of the mask.
bool VulkanCommandProcessor::Fh1ShadowCompareRun() {
  auto& c = *fh1_shadow_compare_;
  if (!c.stats_mapped)
    return false;
  // The previous compare's counts, once its submission is done (one in flight at most).
  if (c.stats_submission) {
    if (GetCompletedSubmission() < c.stats_submission)
      return true;
    const auto* vdev = GetVulkanDevice();
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = c.stats_memory;
    range.size = VK_WHOLE_SIZE;
    vdev->functions().vkInvalidateMappedMemoryRanges(vdev->device(), 1, &range);
    uint32_t st[64];
    std::memcpy(st, c.stats_mapped, sizeof(st));
    for (int i = 0; i < 16; ++i)
      c.sums[i] += st[i];
    c.max_dr = std::max(c.max_dr, st[2]);
    ++c.frames;
    std::memset(c.stats_mapped, 0, 64 * sizeof(uint32_t));
    vdev->functions().vkFlushMappedMemoryRanges(vdev->device(), 1, &range);
    c.stats_submission = 0;
    // Every compare with a pixel off by 3/255 or more is logged with its samples.
    const bool large = st[11] + st[12] + st[13] + st[14] + st[15] != 0;
    static int large_logged = 0;
    if (c.frames == 1 || c.frames % 50 == 0 || (large && large_logged++ < 20)) {
      const auto& u = c.sums;
      std::string samples;
      for (int k = 0; k < 8; ++k) {
        const uint32_t* q = st + 32 + 4 * k;
        if (!q[0] && !q[1] && !q[2])
          continue;
        samples += fmt::format(" ({},{}) emulated {:08X} native {:08X}{}", q[0] & 0xFFFF,
                               q[0] >> 16, q[1], q[2], q[3] ? " depth-fail" : "");
      }
      REXGPU_INFO(
          "FH1 native shadow compare: {} compares, {} pixels: {} differ ({:.4f}%), max |red "
          "diff| {}, other channels differ {}, depth-test fails {:.2f}%; |diff| histogram 0:{} "
          "1:{} 2:{} 3:{} 4:{} 5:{} 6:{} 7+:{}; this one: {} differ, max {}{}",
          c.frames, u[0], u[1], u[0] ? 100.0 * double(u[1]) / double(u[0]) : 0.0, c.max_dr, u[4],
          u[0] ? 100.0 * double(u[5]) / double(u[0]) : 0.0, u[8], u[9], u[10], u[11], u[12], u[13],
          u[14], u[15], st[1], st[2], samples);
    }
  }
  if (!BeginSubmission(true))
    return false;
  const Params& params = c.last;
  const uint32_t width = uint32_t(params.size[0]), height = uint32_t(params.size[1]);
  const uint32_t length = uint32_t(params.size[2]) * ((height + 31) & ~31u) * 4;
  const uint32_t address = uint32_t(params.offs[0]) << 2;
  if (!shared_memory_->RequestRange(address, length))
    return false;
  VkDescriptorSet set[3];
  for (auto& s : set) {
    s = AllocateSingleTransientDescriptor(SingleTransientDescriptorLayout::kStorageBufferCompute);
    if (!s)
      return false;
  }
  const uint32_t depth_address = uint32_t(params.offs[1]) << 2;
  const uint32_t span_start = params.offs[3] >= 0 ? std::min(address, depth_address) : address;
  const uint32_t span_end =
      params.offs[3] >= 0 ? std::max(address, depth_address) + length : address + length;
  const VkDescriptorBufferInfo infos[3] = {
      {shared_memory_->buffer(), span_start, span_end - span_start},
      {c.result, 0, VkDeviceSize(width) * height * 4},
      {c.stats, 0, 64 * sizeof(uint32_t)}};
  VkWriteDescriptorSet writes[3] = {};
  for (int i = 0; i < 3; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set[i];
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &infos[i];
  }
  const auto* device = GetVulkanDevice();
  device->functions().vkUpdateDescriptorSets(device->device(), 3, writes, 0, nullptr);
  Params p = params;
  p.offs[0] = int32_t((address - span_start) >> 2);
  p.offs[1] = params.offs[3] >= 0 ? int32_t((depth_address - span_start) >> 2) : 0;
  Fh1GpuZoneScope gpu_zone(deferred_command_buffer_, Fh1GpuProfile::kEffects);
  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  BindExternalComputePipeline(c.pipelines[2]);
  deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, c.layout, 0, 3,
                                                   set, 0, nullptr);
  deferred_command_buffer_.CmdVkPushConstants(c.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p),
                                              &p);
  SubmitBarriers(true);
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  deferred_command_buffer_.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                                &barrier, 0, nullptr, 0, nullptr);
  deferred_command_buffer_.CmdVkDispatch((width + 7) / 8, (height + 7) / 8, 1);
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  deferred_command_buffer_.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                                                nullptr, 0, nullptr);
  c.stats_submission = GetCurrentSubmission();
  return true;
}

}  // namespace rex::graphics::vulkan
