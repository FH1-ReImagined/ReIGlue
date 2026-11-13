// FH1 (patches 0620, 0630): the native renderer's shadow mask passes, drawn from the game's draw
// records (FH1 docs/native-renderer.md, "M2: shadow mode" and "M3: the shadow mask natively").
//
// The game's host device layer sends a draw record behind each D3D flush call of the shadow mask
// group (patch 0610). The records of the group's blurs (flag kRecordBlur, with the blur's kind)
// and of the mask's resolves after the first blur (kRecordMaskResolve) are enough to compute the
// blurred mask without the register file: the mask's fetch constant and the quad from the blur's
// record, the depth test from its RB_DEPTHCONTROL, the depth from the cascade passes' records
// (fetch constant 0 of the depth pre-pass resolve).
//
// - Hybrid (0630, island flag kIslandNative): the game skipped those calls (kRecordNative); each
//   blur is one compute dispatch here, in stream order, reading the mask and the depth in guest
//   memory (the shared memory, or the scaled resolve buffer at render scale above 1) and writing
//   either a private linear buffer or, when its input is not the guest's memory, the mask's own
//   memory, which is then marked resolved as a resolve marks it. A resolve only says which buffer
//   holds the mask from then on; at the group's end the mask is copied to its memory if it is not
//   there yet. The softer filter's pair is two dispatches (the horizontal one into a private
//   buffer, the vertical one into the mask's memory), the console filter's box one and a copy.
// - Compare (0620, shadow mode, kIslandCompareContents every native_shadow_compare_every groups):
//   the game made every call; the same dispatches run with private buffers only, and after the
//   world pass's resolve a compare dispatch counts the pixels where the native mask differs from
//   the one the emulated passes produced. The counts are read back once that submission is done.
//
// The game asks before it skips anything: at each group's begin the backend writes what it can
// draw natively with the current settings into the game's status word (kStatus*), and the game
// goes native only while the bit is set (Fh1NativeShadowMaskSupported below). A group the backend
// cannot draw although the game skipped its calls (settings changed in the last frames) keeps the
// mask the cascades resolved, unblurred, and clears the bit for a while.
#include <rex/graphics/vulkan/command_processor.h>

#include <rex/cvar.h>
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

// Common to the passes and the compare: the guest memory span (the shared memory or the scaled
// resolve buffer) and the addressing of a 32-bit tiled texture in it, at any integer render scale
// (as the scaled texture loads read it, texture_load_32bpb_scaled_image.cs.glsl).
constexpr char kCommon[] = R"glsl(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) buffer Mem { uint mem[]; };
layout(set = 1, binding = 0) buffer LinIn { uint lin_in[]; };
layout(set = 2, binding = 0) buffer LinOut { uint lin_out[]; };
layout(push_constant) uniform Params {
  ivec4 size;   // host width, height, guest pitch (texels, 32-aligned), mask endian
  ivec4 scale;  // render scale x, y, kernel (0 copy, 1 box, 2/3 4-tap h/v, 4/5 6-tap h/v), -
  ivec4 offs;   // mask and depth word offsets in mem, the quad's z as 20e4, depth endian (-1: none)
  ivec4 swz;    // the byte of red and of green in a texel (the fetch's swizzle)
  vec4 w;       // the blur's weights (its pixel shader's c255)
} pc;
uint swapEndian(uint v, int e) {
  if (e == 1) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
  if (e == 2) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
  if (e == 3) return (v << 16) | (v >> 16);
  return v;
}
// Xenos 2D tiled word offset of a guest texel (pipeline/texture/util.cpp GetTiledOffset2D).
int tiledIndex(ivec2 p) {
  int pitch = pc.size.z;
  int macro = ((p.x >> 5) + (p.y >> 5) * (pitch >> 5)) << 9;
  int micro = ((p.x & 7) + ((p.y & 0xE) << 2)) << 2;
  int offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((p.y & 1) << 4);
  int b = ((offset & ~0x1FF) << 3) + ((p.y & 16) << 7) + ((offset & 0x1C0) << 2) +
          (((((p.y & 8) >> 2) + (p.x >> 3)) & 3) << 6) + (offset & 0x3F);
  return b >> 2;
}
// A host pixel's word: at render scale, each guest group of 4 texels in a row is scale x * y
// groups of 4 host pixels in a row.
int memIndex(ivec2 p) {
  int sx = pc.scale.x, sy = pc.scale.y;
  if (sx == 1 && sy == 1) return tiledIndex(p);
  int hg = p.x >> 2, gg = hg / sx, gy = p.y / sy;
  return tiledIndex(ivec2(gg << 2, gy)) * sx * sy + ((hg - gg * sx) * sy + (p.y - gy * sy)) * 4 +
         (p.x & 3);
}
uint maskWord(ivec2 p) { return swapEndian(mem[pc.offs.x + memIndex(p)], pc.size.w); }
// The guest draw's depth test: LEQUAL of the quad's z against the pre-pass depth (D24FS8, the
// depth in the upper 24 bits as 20e4, which orders as unsigned integers). No depth writes.
bool depthPasses(ivec2 p) {
  if (pc.offs.w < 0) return true;
  uint d = swapEndian(mem[pc.offs.y + memIndex(p)], pc.offs.w) >> 8;
  return uint(pc.offs.z) <= d;
}
)glsl";

// One pass: SRC_MEM reads the mask from guest memory (else lin_in), DST_MEM writes guest memory
// (else lin_out). Pixels failing the depth test keep the input's word (the render target held the
// mask as resolved). Every tap of the game's blurs is at a half texel of a one-to-one quad (offsets
// in host texels at any render scale, as the translator applies them), so each is the mean of two
// (four for the box) neighbours.
constexpr char kPassShader[] = R"glsl(
uint srcWord(ivec2 p) {
  p = clamp(p, ivec2(0), pc.size.xy - 1);
#if SRC_MEM
  return maskWord(p);
#else
  return lin_in[p.y * pc.size.x + p.x];
#endif
}
float red(ivec2 p) { return float((srcWord(p) >> (8 * pc.swz.x)) & 0xFFu) / 255.0; }
void store(ivec2 p, uint word) {
#if DST_MEM
  mem[pc.offs.x + memIndex(p)] = swapEndian(word, pc.size.w);
#else
  lin_out[p.y * pc.size.x + p.x] = word;
#endif
}
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, pc.size.xy))) return;
  int kernel = pc.scale.z;
  if (kernel == 0 || !depthPasses(p)) {
    store(p, srcWord(p));
    return;
  }
  float acc;
  if (kernel == 1) {
    // BlurShadowMask: four bilinear taps at (+-0.5, +-0.5), dp4 with c255.x; no sqrt.
    float r[9];
    for (int y = 0; y < 3; ++y)
      for (int x = 0; x < 3; ++x) r[3 * y + x] = red(p + ivec2(x - 1, y - 1));
    float q = (r[0] + r[1] + r[3] + r[4]) * 0.25 + (r[1] + r[2] + r[4] + r[5]) * 0.25 +
              (r[3] + r[4] + r[6] + r[7]) * 0.25 + (r[4] + r[5] + r[7] + r[8]) * 0.25;
    acc = q * pc.w.x;
  } else {
    ivec2 d = (kernel & 1) == 0 ? ivec2(1, 0) : ivec2(0, 1);
    const float h = 0.5;
    if (kernel <= 3) {
      // BlurShadowMaskHLQ / VLQ: taps at +-0.5 (c255.y) and +-1.5 (c255.x) of sqrt, squared.
      float r[5];
      for (int k = 0; k < 5; ++k) r[k] = red(p + (k - 2) * d);
      acc = pc.w.y * (sqrt((r[1] + r[2]) * h) + sqrt((r[2] + r[3]) * h)) +
            pc.w.x * (sqrt((r[0] + r[1]) * h) + sqrt((r[3] + r[4]) * h));
    } else {
      // BlurShadowMaskH / V: taps at +-0.5 (c255.x), +-1.5 (c255.z), +-2.5 (c255.y).
      float r[7];
      for (int k = 0; k < 7; ++k) r[k] = red(p + (k - 3) * d);
      acc = pc.w.x * (sqrt((r[3] + r[4]) * h) + sqrt((r[2] + r[3]) * h)) +
            pc.w.z * (sqrt((r[4] + r[5]) * h) + sqrt((r[1] + r[2]) * h)) +
            pc.w.y * (sqrt((r[5] + r[6]) * h) + sqrt((r[0] + r[1]) * h));
    }
    acc = acc * acc;
  }
  // oC0 = (r, 1, 0, 0), to 8 bits as the host render target stores it (round to nearest).
  store(p, (uint(floor(clamp(acc, 0.0, 1.0) * 255.0 + 0.5)) << (8 * pc.swz.x)) |
               (0xFFu << (8 * pc.swz.y)));
}
)glsl";

// Compare mode: the guest's mask (memory, after the world pass's resolve) against the native one
// (lin_in): lin_out[0] pixels, [1] differing words, [2] max |red difference|, [3] sum, [4] other
// channels differ, [5] depth-test failures, [6]/[7] off by 3/255 or more passing/failing the
// test, [8 + d] histogram of |red difference| d (0..7, 7 = 7 or more), samples from [32].
constexpr char kCompareShader[] = R"glsl(
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, pc.size.xy))) return;
  uint g = maskWord(p);
  uint n = lin_in[p.y * pc.size.x + p.x];
  atomicAdd(lin_out[0], 1u);
  bool fails = !depthPasses(p);
  if (fails) atomicAdd(lin_out[5], 1u);
  if (g == n) { atomicAdd(lin_out[8], 1u); return; }
  atomicAdd(lin_out[1], 1u);
  uint sh = uint(8 * pc.swz.x);
  uint dr = uint(abs(int((g >> sh) & 0xFFu) - int((n >> sh) & 0xFFu)));
  atomicMax(lin_out[2], dr);
  atomicAdd(lin_out[3], dr);
  if ((g & ~(0xFFu << sh)) != (n & ~(0xFFu << sh))) atomicAdd(lin_out[4], 1u);
  atomicAdd(lin_out[8 + min(dr, 7u)], 1u);
  if (dr >= 3u) atomicAdd(lin_out[fails ? 7 : 6], 1u);
  uint k = atomicAdd(lin_out[dr >= 3u ? 24 : 25], 1u);
  if (k < 4u) {
    uint b = 32u + (dr >= 3u ? 0u : 16u) + 4u * k;
    lin_out[b] = uint(p.x) | (uint(p.y) << 16);
    lin_out[b + 1u] = g;
    lin_out[b + 2u] = n;
    lin_out[b + 3u] = fails ? 1u : 0u;
  }
}
)glsl";

struct Params {
  int32_t size[4];
  int32_t scale[4];
  int32_t offs[4];
  int32_t swz[4];
  float w[4];
};

// The blurs' pixel shader literals (c255), by kind (fh1_host_packet.h BlurKind). Compare mode
// checks them against the literals the guest's draws load.
constexpr float kWeights[fh::kBlurKindCount][4] = {
    {0.25f, 0.0f, 0.0f, 0.0f},                       // box (58CE51CE56247C16): c255.x
    {0.2f, 0.3f, 0.0f, 0.0f},                        // HLQ (85C2871B0E0A740B): x +-1.5, y +-0.5
    {0.2f, 0.3f, 0.0f, 0.0f},                        // VLQ (F6EE736B656E5942)
    {3.0f / 11.0f, 0.5f / 11.0f, 2.0f / 11.0f, 0.0f},  // H (DBD55E02669D5CB1)
    {3.0f / 11.0f, 0.5f / 11.0f, 2.0f / 11.0f, 0.0f},  // V (D9A1FFC33C029F54)
};
constexpr int32_t kKernel[fh::kBlurKindCount] = {1, 2, 3, 4, 5};
constexpr const char* kKindNames[fh::kBlurKindCount] = {"box", "HLQ", "VLQ", "H", "V"};

// Register shadow words of a record (fh1_host_packet.h kRegisterBlocks).
constexpr uint32_t kShadowRbDepthControl = 109;  // 0x2200
constexpr uint32_t kShadowPaSuVtxCntl = 144;     // 0x2302

constexpr uint32_t kRefusalHold = 300;  // groups with the status bit cleared after a refusal

}  // namespace

// M3's seam with the scene dimensions of ReIGlue #56 (fh1_native_scene_width/height, not in this
// build): the native mask is drawn only where the scene fills the scaled surface (no fractional
// raster) and no independent scene size is set. With such a patch the size check moves here.
static bool Fh1NativeShadowMaskSceneSupported(float raster_scale_x, float raster_scale_y) {
  if (raster_scale_x != 1.0f || raster_scale_y != 1.0f)
    return false;
  for (const char* name : {"fh1_native_scene_width", "fh1_native_scene_height"}) {
    const std::string v = rex::cvar::GetFlagByName(name);
    if (!v.empty() && v != "0")
      return false;
  }
  return true;
}

struct VulkanCommandProcessor::Fh1ShadowCompare {
  bool failed = false;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline passes[4] = {};  // [src from memory * 2 + dst to memory]
  VkPipeline compare = VK_NULL_HANDLE;
  // Private linear masks (host pixels, one word each); enlarged ones are kept until exit.
  VkBuffer lin[2] = {};
  VkDeviceMemory lin_memory[2] = {};
  VkDeviceSize lin_size = 0;
  std::vector<std::pair<VkBuffer, VkDeviceMemory>> retired;
  VkBuffer stats = VK_NULL_HANDLE;
  VkDeviceMemory stats_memory = VK_NULL_HANDLE;
  uint32_t* stats_mapped = nullptr;
  uint64_t stats_submission = 0;
  uint16_t zone = Fh1GpuProfile::kNoZone;

  // The group being recorded.
  bool native = false, compare_wanted = false, any_blur = false;
  enum Where : uint8_t { kMemory, kLinear0, kLinear1 };
  Where texture = kMemory;  // where the mask texture's contents are
  Where target = kMemory;   // where the render target's are (after the first blur)
  bool target_valid = false, broken = false;
  // The mask and the depth: guest addresses, host size, guest pitch, formats.
  uint32_t address = 0, length = 0, width = 0, height = 0, pitch = 0, endian = 0;
  int32_t swz[2] = {};
  uint32_t depth_fetch[6] = {};
  bool depth_valid = false;
  float literal_c255[4] = {};
  bool literal_valid = false;

  // Statistics.
  uint64_t sums[16] = {};
  uint64_t compares = 0, groups_native = 0, blurs_native = 0, refused = 0, unblurred = 0;
  uint64_t kind_blurs[fh::kBlurKindCount] = {};
  uint32_t max_dr = 0;
  uint32_t hold = 0;  // groups left with the status bit cleared
  bool logged_refusal = false, logged_literals[fh::kBlurKindCount] = {};
  std::string last_refusal;

  bool Pipelines(VulkanCommandProcessor& cp);
  bool Buffers(VulkanCommandProcessor& cp, VkDeviceSize size);
  // A dispatch over the mask (kernel, or 0 to copy) from `from` to `to`.
  bool Pass(VulkanCommandProcessor& cp, int32_t kernel, const float* weights, uint32_t quad_z20e4,
            bool depth_test, Where from, Where to);
  bool Refuse(VulkanCommandProcessor& cp, const std::string& why);
  void ReadStats(VulkanCommandProcessor& cp);
};

void VulkanCommandProcessor::Fh1ShadowCompareShutdown() {
  auto* c = fh1_shadow_compare_;
  if (!c)
    return;
  if (c->compares)
    REXGPU_INFO(
        "FH1 native shadow compare (at exit): {} compares, {} pixels: {} differ "
        "({:.4f}%), max |red diff| {}; |diff| histogram 0:{} 1:{} 2:{} 3+:{}; {} refused",
        c->compares, c->sums[0], c->sums[1],
        c->sums[0] ? 100.0 * double(c->sums[1]) / double(c->sums[0]) : 0.0, c->max_dr, c->sums[8],
        c->sums[9], c->sums[10],
        c->sums[11] + c->sums[12] + c->sums[13] + c->sums[14] + c->sums[15], c->refused);
  if (c->groups_native || c->refused)
    REXGPU_INFO(
        "FH1 native shadow mask (at exit): {} groups drawn natively, {} blurs (box {}, HLQ {}, "
        "VLQ {}, H {}, V {}), {} refused, {} left unblurred{}{}",
        c->groups_native, c->blurs_native, c->kind_blurs[0], c->kind_blurs[1], c->kind_blurs[2],
        c->kind_blurs[3], c->kind_blurs[4], c->refused, c->unblurred,
        c->last_refusal.empty() ? "" : "; last refusal: ", c->last_refusal);
  const auto* device = GetVulkanDevice();
  const auto& f = device->functions();
  for (VkPipeline p : c->passes)
    if (p)
      f.vkDestroyPipeline(device->device(), p, nullptr);
  if (c->compare)
    f.vkDestroyPipeline(device->device(), c->compare, nullptr);
  for (int i = 0; i < 2; ++i)
    c->retired.emplace_back(c->lin[i], c->lin_memory[i]);
  c->retired.emplace_back(c->stats, c->stats_memory);
  for (auto [b, m] : c->retired) {
    if (b)
      f.vkDestroyBuffer(device->device(), b, nullptr);
    if (m)
      f.vkFreeMemory(device->device(), m, nullptr);
  }
  if (c->layout)
    f.vkDestroyPipelineLayout(device->device(), c->layout, nullptr);
  delete c;
  fh1_shadow_compare_ = nullptr;
}

bool VulkanCommandProcessor::Fh1ShadowCompare::Pipelines(VulkanCommandProcessor& cp) {
  if (layout)
    return true;
  const auto* device = cp.GetVulkanDevice();
  const auto& f = device->functions();
  const VkDevice d = device->device();
  const VkDescriptorSetLayout set_layout = cp.GetSingleTransientDescriptorLayout(
      SingleTransientDescriptorLayout::kStorageBufferCompute);
  const VkDescriptorSetLayout sets[3] = {set_layout, set_layout, set_layout};
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Params)};
  VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  info.setLayoutCount = 3;
  info.pSetLayouts = sets;
  info.pushConstantRangeCount = 1;
  info.pPushConstantRanges = &push;
  if (f.vkCreatePipelineLayout(d, &info, nullptr, &layout) != VK_SUCCESS) {
    failed = true;
    return false;
  }
  for (int k = 0; k < 5; ++k) {
    std::string source = "#version 450\n";
    if (k < 4)
      source += fmt::format("#define SRC_MEM {}\n#define DST_MEM {}\n", k >> 1, k & 1);
    source += kCommon;
    source += k < 4 ? kPassShader : kCompareShader;
    std::vector<uint32_t> spirv;
    std::string error;
    if (!CompileFh1Compute(source, spirv, error)) {
      REXGPU_ERROR("FH1 native shadow mask: shader {}: {}", k, error);
      failed = true;
      return false;
    }
    VkPipeline p = ui::vulkan::util::CreateComputePipeline(device, layout, spirv.data(),
                                                           spirv.size() * sizeof(uint32_t));
    if (!p) {
      failed = true;
      return false;
    }
    (k < 4 ? passes[k] : compare) = p;
  }
  return true;
}

bool VulkanCommandProcessor::Fh1ShadowCompare::Buffers(VulkanCommandProcessor& cp,
                                                       VkDeviceSize size) {
  const auto* device = cp.GetVulkanDevice();
  const auto& f = device->functions();
  if (!stats) {
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            device, 64 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kReadback, stats, stats_memory) ||
        f.vkMapMemory(device->device(), stats_memory, 0, VK_WHOLE_SIZE, 0,
                      reinterpret_cast<void**>(&stats_mapped)) != VK_SUCCESS) {
      REXGPU_ERROR("FH1 native shadow mask: statistics buffer");
      failed = true;
      return false;
    }
    std::memset(stats_mapped, 0, 64 * sizeof(uint32_t));
  }
  if (lin_size >= size)
    return true;
  // In use by submissions in flight: kept until exit (a change of render scale).
  for (int i = 0; i < 2; ++i) {
    if (lin[i])
      retired.emplace_back(lin[i], lin_memory[i]);
    lin[i] = VK_NULL_HANDLE;
    lin_memory[i] = VK_NULL_HANDLE;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            device, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal, lin[i], lin_memory[i])) {
      REXGPU_ERROR("FH1 native shadow mask: {} MB buffers", (size * 2) >> 20);
      failed = true;
      lin_size = 0;
      return false;
    }
  }
  lin_size = size;
  return true;
}

bool VulkanCommandProcessor::Fh1ShadowCompare::Refuse(VulkanCommandProcessor& cp,
                                                      const std::string& why) {
  ++refused;
  last_refusal = why;
  if (native) {
    // The game skipped its calls: the mask stays as the cascades resolved it this time, and the
    // game is told to draw the next groups itself for a while.
    broken = true;
    hold = kRefusalHold;
  }
  if (!logged_refusal || refused % 1000 == 0) {
    logged_refusal = true;
    REXGPU_INFO("FH1 native shadow mask: not drawn natively ({}){}; {} so far", why,
                native ? ", the mask stays unblurred in this group and the game draws the next "
                         "groups itself for a while"
                       : "; the state check runs",
                refused);
  }
  return false;
}

bool VulkanCommandProcessor::Fh1ShadowCompare::Pass(VulkanCommandProcessor& cp, int32_t kernel,
                                                    const float* weights, uint32_t quad_z20e4,
                                                    bool depth_test, Where from, Where to) {
  const auto* device = cp.GetVulkanDevice();
  const auto& f = device->functions();
  VulkanTextureCache& tc = *cp.texture_cache_;
  const bool scaled = tc.IsDrawResolutionScaled();
  const uint32_t sx = tc.draw_resolution_scale_x(), sy = tc.draw_resolution_scale_y();
  xenos::xe_gpu_texture_fetch_t depth;
  std::memcpy(&depth, depth_fetch, sizeof(depth));
  const uint32_t depth_address = (uint32_t(depth.base_address) << 12) & 0x1FFFFFFFu;
  depth_test = depth_test && depth_valid;
  if (!cp.BeginSubmission(true))
    return false;
  // The guest memory span holding the mask and the depth.
  const uint32_t lo = depth_test ? std::min(address, depth_address) : address;
  const uint32_t hi = depth_test ? std::max(address, depth_address) + length : address + length;
  VkBuffer mem_buffer;
  uint64_t span_start, span_length, mask_start, depth_start = 0;
  if (scaled) {
    uint64_t unused;
    if (!tc.GetScaledResolveRange(lo, hi - lo, 2, span_start, span_length) ||
        !tc.GetScaledResolveRange(address, length, 2, mask_start, unused) ||
        (depth_test && !tc.GetScaledResolveRange(depth_address, length, 2, depth_start, unused)) ||
        !tc.CommitScaledResolveRange(address, length, 2) ||
        (depth_test && !tc.CommitScaledResolveRange(depth_address, length, 2)))
      return false;
    mem_buffer = tc.scaled_resolve_buffer();
  } else {
    if (!cp.shared_memory_->RequestRange(address, length) ||
        (depth_test && !cp.shared_memory_->RequestRange(depth_address, length)))
      return false;
    span_start = lo;
    span_length = hi - lo;
    mask_start = address;
    depth_start = depth_address;
    mem_buffer = cp.shared_memory_->buffer();
  }
  if (!mem_buffer)
    return false;
  const uint64_t aligned = span_start & ~uint64_t(255);
  span_length += span_start - aligned;
  span_start = aligned;
  if (span_length > device->properties().maxStorageBufferRange)
    return Refuse(cp, fmt::format("the mask and the depth are {} MB apart", span_length >> 20));

  VkDescriptorSet set[3];
  for (auto& s : set) {
    s = cp.AllocateSingleTransientDescriptor(SingleTransientDescriptorLayout::kStorageBufferCompute);
    if (!s)
      return false;
  }
  const VkDeviceSize lin_bytes = VkDeviceSize(width) * height * 4;
  const VkBuffer in = from == kLinear1 ? lin[1] : lin[0];
  const VkBuffer out = to == kLinear1 ? lin[1] : lin[0];
  const VkDescriptorBufferInfo infos[3] = {
      {mem_buffer, span_start, span_length}, {in, 0, lin_bytes}, {out, 0, lin_bytes}};
  VkWriteDescriptorSet writes[3] = {};
  for (int i = 0; i < 3; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set[i];
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &infos[i];
  }
  f.vkUpdateDescriptorSets(device->device(), 3, writes, 0, nullptr);

  Params params{};
  params.size[0] = int32_t(width);
  params.size[1] = int32_t(height);
  params.size[2] = int32_t(pitch);
  params.size[3] = int32_t(endian);
  params.scale[0] = int32_t(sx);
  params.scale[1] = int32_t(sy);
  params.scale[2] = kernel;
  params.offs[0] = int32_t((mask_start - span_start) >> 2);
  params.offs[1] = depth_test ? int32_t((depth_start - span_start) >> 2) : 0;
  params.offs[2] = int32_t(quad_z20e4);
  params.offs[3] = depth_test ? int32_t(depth.endianness) : -1;
  params.swz[0] = swz[0];
  params.swz[1] = swz[1];
  if (weights)
    std::memcpy(params.w, weights, sizeof(params.w));

  auto* profile = cp.deferred_command_buffer_.fh1_gpu_profile();
  if (profile && zone == Fh1GpuProfile::kNoZone) {
    zone = profile->FindZone(0x46483142'4E534D00ull);
    if (zone == Fh1GpuProfile::kNoZone)
      zone = profile->AddZone(0x46483142'4E534D00ull, "fh1 native shadow mask");
  }
  Fh1GpuZoneScope gpu_zone(cp.deferred_command_buffer_,
                           zone != Fh1GpuProfile::kNoZone ? zone : Fh1GpuProfile::kEffects);
  const bool write_memory = to == kMemory;
  if (scaled) {
    if (write_memory)
      tc.UseScaledResolveBufferForWrite(mask_start, uint64_t(length) * sx * sy);
    else
      tc.UseScaledResolveBufferForRead();
  } else {
    cp.shared_memory_->Use(write_memory ? VulkanSharedMemory::Usage::kComputeWrite
                                        : VulkanSharedMemory::Usage::kRead,
                           write_memory ? std::pair<uint32_t, uint32_t>(address, length)
                                        : std::pair<uint32_t, uint32_t>());
  }
  cp.BindExternalComputePipeline(passes[(from == kMemory ? 2 : 0) + (write_memory ? 1 : 0)]);
  cp.deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
                                                      3, set, 0, nullptr);
  cp.deferred_command_buffer_.CmdVkPushConstants(layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                                 sizeof(params), &params);
  cp.SubmitBarriers(true);
  // The private buffers: the previous pass (or the previous group's compare) is done with them.
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  cp.deferred_command_buffer_.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                                   &barrier, 0, nullptr, 0, nullptr);
  cp.deferred_command_buffer_.CmdVkDispatch((width + 7) / 8, (height + 7) / 8, 1);
  if (write_memory) {
    // As a resolve: textures over the range load again (and, scaled, from the scaled buffer).
    // With ReIGlue #56's resolve provenance this is where the range would be tagged as the
    // scene's resolve of the mask, as the emulated world pass's resolve tags it.
    tc.MarkRangeAsResolved(address, length);
  }
  return true;
}

void VulkanCommandProcessor::Fh1ShadowCompare::ReadStats(VulkanCommandProcessor& cp) {
  if (!stats_submission || cp.GetCompletedSubmission() < stats_submission)
    return;
  const auto* vdev = cp.GetVulkanDevice();
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = stats_memory;
  range.size = VK_WHOLE_SIZE;
  vdev->functions().vkInvalidateMappedMemoryRanges(vdev->device(), 1, &range);
  uint32_t st[64];
  std::memcpy(st, stats_mapped, sizeof(st));
  for (int i = 0; i < 16; ++i)
    sums[i] += st[i];
  max_dr = std::max(max_dr, st[2]);
  ++compares;
  std::memset(stats_mapped, 0, 64 * sizeof(uint32_t));
  vdev->functions().vkFlushMappedMemoryRanges(vdev->device(), 1, &range);
  stats_submission = 0;
  // Every compare with a pixel off by 3/255 or more is logged with its samples (the first 20).
  const bool large = st[11] + st[12] + st[13] + st[14] + st[15] != 0;
  static int large_logged = 0;
  if (compares == 1 || compares % 50 == 0 || (large && large_logged++ < 20)) {
    const auto& u = sums;
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
        compares, u[0], u[1], u[0] ? 100.0 * double(u[1]) / double(u[0]) : 0.0, max_dr, u[4],
        u[0] ? 100.0 * double(u[5]) / double(u[0]) : 0.0, u[8], u[9], u[10], u[11], u[12], u[13],
        u[14], u[15], st[1], st[2], samples);
  }
}

void VulkanCommandProcessor::Fh1HostPacket(const uint32_t* words, uint32_t count) {
  CommandProcessor::Fh1HostPacket(words, count);  // the state check, and the messages
  if (count < 2)
    return;
  const uint32_t op = words[1];
  if (op != fh::kOpIslandBegin && op != fh::kOpRecord && op != fh::kOpIslandEnd &&
      op != fh::kOpLiterals)
    return;
  if (!fh1_shadow_compare_)
    fh1_shadow_compare_ = new Fh1ShadowCompare;
  auto& c = *fh1_shadow_compare_;
  if (op == fh::kOpLiterals) {
    // The pixel shader's c255 (words 2044-2047 from register 0x4000), for the next record.
    const uint32_t first = count > 4 ? words[3] : 0, n = count > 4 ? words[4] : 0;
    if (count >= fh::kLiteralsHeaderWords + n && first <= 2044 && first + n >= 2048) {
      std::memcpy(c.literal_c255, words + fh::kLiteralsHeaderWords + (2044 - first), 16);
      c.literal_valid = true;
    }
    return;
  }
  if (op == fh::kOpIslandBegin) {
    const uint32_t flags = count > 4 ? words[4] : 0;
    c.native = (flags & fh::kIslandNative) != 0;
    c.compare_wanted = !c.native && (flags & fh::kIslandCompareContents);
    c.any_blur = c.target_valid = c.broken = c.depth_valid = c.literal_valid = false;
    c.texture = c.target = Fh1ShadowCompare::kMemory;
    // What the game may skip from now on.
    if (count > 5 && words[5]) {
      if (c.hold)
        --c.hold;
      const bool supported =
          !c.failed && !c.hold &&
          Fh1NativeShadowMaskSceneSupported(render_target_cache_->raster_scale_x(),
                                            render_target_cache_->raster_scale_y()) &&
          (!texture_cache_->IsDrawResolutionScaled() || texture_cache_->scaled_resolve_buffer());
      if (auto* status = memory_->TranslateVirtual<uint32_t*>(words[5]))
        *status = rex::byte_swap(uint32_t(supported ? fh::kStatusShadowMaskBlur : 0u));
    }
    return;
  }
  if (op == fh::kOpIslandEnd) {
    Fh1ShadowCompareRun();
    c.literal_valid = false;
    return;
  }
  constexpr uint32_t kFixed = fh::kRecordHeaderWords + fh::kRegisterShadowWords + fh::kFetchWords;
  if (count < kFixed)
    return;
  const uint32_t flags = words[fh::kRecFlags];
  const bool literal_valid = c.literal_valid;
  c.literal_valid = false;
  // The cascade passes' depth (their tf0), from the records of the emulated draws.
  if (!(flags & fh::kRecordNative) && !c.any_blur) {
    const Shader* ps = active_pixel_shader();
    if (ps && fh1_world_ao::IsShadowPass(ps->ucode_data_hash())) {
      std::memcpy(c.depth_fetch, words + fh::kRecordHeaderWords + fh::kRegisterShadowWords,
                  sizeof(c.depth_fetch));
      c.depth_valid = true;
    }
  }
  if (!(flags & (fh::kRecordBlur | fh::kRecordMaskResolve)))
    return;
  // Compare mode checks the weights against the literals the guest's draw loaded.
  if ((flags & fh::kRecordBlur) && literal_valid && !(flags & fh::kRecordNative)) {
    const uint32_t kind = (flags >> fh::kRecordBlurKindShift) & 15;
    if (kind < fh::kBlurKindCount && !c.logged_literals[kind]) {
      c.logged_literals[kind] = true;
      const float* w = kWeights[kind];
      const bool same = std::fabs(c.literal_c255[0] - w[0]) < 1e-6f &&
                        std::fabs(c.literal_c255[1] - w[1]) < 1e-6f &&
                        (kind < fh::kBlurHorizontal6 || std::fabs(c.literal_c255[2] - w[2]) < 1e-6f);
      REXGPU_INFO("FH1 native shadow mask: blur {} loads c255 = ({}, {}, {}, {}){}",
                  kKindNames[kind], c.literal_c255[0], c.literal_c255[1], c.literal_c255[2],
                  c.literal_c255[3], same ? "" : " -- NOT the native weights");
    }
  }
  const bool run = c.native ? (flags & fh::kRecordNative) != 0 : c.compare_wanted;
  if (!run || c.broken)
    return;
  if (flags & fh::kRecordMaskResolve) {
    // The render target is copied to the texture: the mask is where the last blur put it.
    if (c.target_valid)
      c.texture = c.target;
    return;
  }
  if (!Fh1ShadowCompareCompute(words, count) && c.native) {
    c.broken = true;
    ++c.unblurred;
  }
}

// One blur record (kRecordBlur).
bool VulkanCommandProcessor::Fh1ShadowCompareCompute(const uint32_t* words, uint32_t count) {
  auto& c = *fh1_shadow_compare_;
  if (c.failed || !c.Pipelines(*this))
    return c.Refuse(*this, "no pipelines");
  const uint32_t flags = words[fh::kRecFlags];
  const uint32_t kind = (flags >> fh::kRecordBlurKindShift) & 15;
  if (kind >= fh::kBlurKindCount)
    return c.Refuse(*this, fmt::format("blur kind {}", kind));
  if (!(flags & fh::kRecordVertexData) ||
      count < fh::kRecordHeaderWords + fh::kRegisterShadowWords + fh::kFetchWords +
                  fh::kVertexDataWords)
    return c.Refuse(*this, "no quad");
  const uint32_t* shadow = words + fh::kRecordHeaderWords;
  const uint32_t* fetch_words = shadow + fh::kRegisterShadowWords;
  // The mask: the blurs sample it through fetch constant 1.
  xenos::xe_gpu_texture_fetch_t fetch;
  static_assert(sizeof(fetch) == 6 * sizeof(uint32_t));
  std::memcpy(&fetch, fetch_words + 6 * 1, sizeof(fetch));
  float v[20];
  std::memcpy(v, fetch_words + fh::kFetchWords, sizeof(v));
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
  // A quad that covers nothing draws nothing (the game's no-sun path draws a zeroed one; the game
  // does not go native there, but a later blur of a native group could).
  if (!(rect[2] > rect[0]) || !(rect[3] > rect[1]))
    return true;
  const uint32_t gw = uint32_t(fetch.size_2d.width) + 1, gh = uint32_t(fetch.size_2d.height) + 1;
  const uint32_t pitch = uint32_t(fetch.pitch) << 5;
  const uint32_t address = (uint32_t(fetch.base_address) << 12) & 0x1FFFFFFFu;
  if (fetch.format != xenos::TextureFormat::k_8_8_8_8 || !fetch.tiled || !pitch ||
      uint32_t(fetch.dimension) != 1)
    return c.Refuse(*this, fmt::format("mask format {}, tiled {}", uint32_t(fetch.format),
                                       uint32_t(fetch.tiled)));
  if (!Fh1NativeShadowMaskSceneSupported(render_target_cache_->raster_scale_x(),
                                         render_target_cache_->raster_scale_y()))
    return c.Refuse(*this, "a fractional raster or an independent scene size");
  // Screen pixel -> texel, as the guest's quad maps it: it must be one to one.
  const float centre = (shadow[kShadowPaSuVtxCntl] & 1) ? 0.5f : 0.0f;
  const float sx0 = (rect[0] + 1.0f) * 0.5f * gw, sx1 = (rect[2] + 1.0f) * 0.5f * gw;
  const float sy0 = (1.0f - rect[3]) * 0.5f * gh, sy1 = (1.0f - rect[1]) * 0.5f * gh;
  const float du = (rect[6] - rect[4]) * gw / (sx1 - sx0);
  const float dv = (rect[7] - rect[5]) * gh / (sy1 - sy0);
  const float u0 = rect[4] * gw - sx0 * du, v0 = rect[5] * gh - sy0 * dv;
  if (!(std::fabs(du - 1.0f) <= 1e-4f) || !(std::fabs(dv - 1.0f) <= 1e-4f) ||
      !(std::fabs(du * centre + u0 - 0.5f) <= 1e-3f) ||
      !(std::fabs(dv * centre + v0 - 0.5f) <= 1e-3f) || sx0 > 0.5f || sy0 > 0.5f ||
      sx1 < gw - 0.5f || sy1 < gh - 0.5f)
    return c.Refuse(*this, "the quad does not map the screen to texels one to one");
  // The depth test: off, or LEQUAL without writes or stencil (the game's).
  const uint32_t depth_control = shadow[kShadowRbDepthControl];
  const bool z_test = (depth_control >> 1) & 1;
  if ((depth_control & 1) || (z_test && (((depth_control >> 4) & 7) != 3 ||
                                         ((depth_control >> 2) & 1))))
    return c.Refuse(*this, fmt::format("depth control {:08X}", depth_control));
  if (z_test) {
    xenos::xe_gpu_texture_fetch_t depth;
    std::memcpy(&depth, c.depth_fetch, sizeof(depth));
    if (!c.depth_valid || !depth.tiled || uint32_t(depth.size_2d.width) + 1 != gw ||
        uint32_t(depth.size_2d.height) + 1 != gh || (uint32_t(depth.pitch) << 5) != pitch)
      return c.Refuse(*this, "no depth of the mask's size from the cascade passes");
  }
  const uint32_t length = pitch * ((gh + 31) & ~31u) * 4;
  const uint32_t sx = texture_cache_->draw_resolution_scale_x(),
                 sy = texture_cache_->draw_resolution_scale_y();
  if (c.any_blur && (address != c.address || gw * sx != c.width || gh * sy != c.height))
    return c.Refuse(*this, "the group's blurs draw different masks");
  c.address = address;
  c.length = length;
  c.width = gw * sx;
  c.height = gh * sy;
  c.pitch = pitch;
  c.endian = uint32_t(fetch.endianness);
  c.swz[0] = int32_t((fetch.swizzle >> 0) & 7) & 3;
  c.swz[1] = int32_t((fetch.swizzle >> 3) & 7) & 3;
  if (!c.Buffers(*this, VkDeviceSize(c.width) * c.height * 4))
    return c.Refuse(*this, "no buffers");

  // From the texture into a buffer it is not in; natively, straight into the mask's memory when
  // the texture is a private buffer (the render target's word is the texture's where the depth
  // test fails, so writing over the old contents of memory is what the guest's resolve does).
  using W = Fh1ShadowCompare::Where;
  const W from = c.texture;
  W to;
  if (from == W::kMemory)
    to = (c.target_valid && c.target == W::kLinear0) ? W::kLinear1 : W::kLinear0;
  else if (c.native)
    to = W::kMemory;
  else
    to = from == W::kLinear0 ? W::kLinear1 : W::kLinear0;
  if (!c.Pass(*this, kKernel[kind], kWeights[kind], xenos::Float32To20e4(v[2], true), z_test, from,
              to))
    return false;
  c.target = to;
  c.target_valid = true;
  c.any_blur = true;
  if (c.native) {
    ++c.blurs_native;
    ++c.kind_blurs[kind];
  }
  return true;
}

// The group's end (after the world pass's resolve of the mask): natively, the mask's memory gets
// the mask if it is still in a private buffer; in compare mode the two masks are compared.
bool VulkanCommandProcessor::Fh1ShadowCompareRun() {
  auto& c = *fh1_shadow_compare_;
  c.ReadStats(*this);
  if (!c.any_blur || c.broken)
    return false;
  using W = Fh1ShadowCompare::Where;
  if (c.native) {
    ++c.groups_native;
    if (c.texture != W::kMemory && !c.Pass(*this, 0, nullptr, 0, false, c.texture, W::kMemory))
      return false;
    if (c.groups_native == 1 || c.groups_native % 3000 == 0)
      REXGPU_INFO(
          "FH1 native shadow mask: {} groups drawn natively, {} blurs (box {}, HLQ {}, VLQ {}, H "
          "{}, V {}); {} refused, {} left unblurred; {}x{} at render scale {}x{}",
          c.groups_native, c.blurs_native, c.kind_blurs[0], c.kind_blurs[1], c.kind_blurs[2],
          c.kind_blurs[3], c.kind_blurs[4], c.refused, c.unblurred, c.width, c.height,
          texture_cache_->draw_resolution_scale_x(), texture_cache_->draw_resolution_scale_y());
    return true;
  }
  if (!c.compare_wanted || c.texture == W::kMemory || c.stats_submission)
    return false;
  // Compare: the guest's mask in memory against the native one in its private buffer.
  if (!BeginSubmission(true))
    return false;
  VulkanTextureCache& tc = *texture_cache_;
  const bool scaled = tc.IsDrawResolutionScaled();
  xenos::xe_gpu_texture_fetch_t depth;
  std::memcpy(&depth, c.depth_fetch, sizeof(depth));
  const uint32_t depth_address = (uint32_t(depth.base_address) << 12) & 0x1FFFFFFFu;
  const bool depth_test = c.depth_valid;
  const uint32_t lo = depth_test ? std::min(c.address, depth_address) : c.address;
  const uint32_t hi =
      depth_test ? std::max(c.address, depth_address) + c.length : c.address + c.length;
  VkBuffer mem_buffer;
  uint64_t span_start, span_length, mask_start, depth_start = 0, unused;
  if (scaled) {
    if (!tc.GetScaledResolveRange(lo, hi - lo, 2, span_start, span_length) ||
        !tc.GetScaledResolveRange(c.address, c.length, 2, mask_start, unused) ||
        (depth_test && !tc.GetScaledResolveRange(depth_address, c.length, 2, depth_start, unused)))
      return false;
    mem_buffer = tc.scaled_resolve_buffer();
  } else {
    if (!shared_memory_->RequestRange(c.address, c.length))
      return false;
    span_start = lo;
    span_length = hi - lo;
    mask_start = c.address;
    depth_start = depth_address;
    mem_buffer = shared_memory_->buffer();
  }
  const uint64_t aligned = span_start & ~uint64_t(255);
  span_length += span_start - aligned;
  span_start = aligned;
  VkDescriptorSet set[3];
  for (auto& s : set) {
    s = AllocateSingleTransientDescriptor(SingleTransientDescriptorLayout::kStorageBufferCompute);
    if (!s)
      return false;
  }
  const VkDescriptorBufferInfo infos[3] = {
      {mem_buffer, span_start, span_length},
      {c.texture == W::kLinear1 ? c.lin[1] : c.lin[0], 0, VkDeviceSize(c.width) * c.height * 4},
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
  Params p{};
  p.size[0] = int32_t(c.width);
  p.size[1] = int32_t(c.height);
  p.size[2] = int32_t(c.pitch);
  p.size[3] = int32_t(c.endian);
  p.scale[0] = int32_t(tc.draw_resolution_scale_x());
  p.scale[1] = int32_t(tc.draw_resolution_scale_y());
  p.offs[0] = int32_t((mask_start - span_start) >> 2);
  p.offs[1] = depth_test ? int32_t((depth_start - span_start) >> 2) : 0;
  p.offs[2] = 0;
  p.offs[3] = -1;  // depth-test failures are not split out here
  p.swz[0] = c.swz[0];
  p.swz[1] = c.swz[1];
  Fh1GpuZoneScope gpu_zone(deferred_command_buffer_, Fh1GpuProfile::kEffects);
  if (scaled)
    tc.UseScaledResolveBufferForRead();
  else
    shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  BindExternalComputePipeline(c.compare);
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
  deferred_command_buffer_.CmdVkDispatch((c.width + 7) / 8, (c.height + 7) / 8, 1);
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  deferred_command_buffer_.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                                                nullptr, 0, nullptr);
  c.stats_submission = GetCurrentSubmission();
  return true;
}

}  // namespace rex::graphics::vulkan
