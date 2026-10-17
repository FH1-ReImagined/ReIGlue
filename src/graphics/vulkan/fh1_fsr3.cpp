// AMD FidelityFX FSR 3.1 upscaler on the FH1 reconstruction inputs. The GPU passes are
// the FidelityFX SDK v1.1.4 GLSL sources (MIT, fetched by cmake/ForzaFsr3Sdk.cmake);
// the host side below follows the SDK's ffx_fsr3upscaler.cpp dispatch sequence with a
// fixed permutation: LDR input, render-size motion vectors without jitter, inverted
// depth, no FP16 and no wave operations, so it runs on any Vulkan 1.0 GPU.
#include <rex/graphics/vulkan/fh1_fsr3.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>

namespace rex::graphics::vulkan {
namespace {
struct Fh1Fsr3Source {
  const char* name;
  const unsigned char* data;
  size_t size;
};
#if defined(REX_FH1_FSR3)
#include "fh1_fsr3_sources.inc"
#endif

enum Resource : uint32_t {
  kColor, kDepth, kMotion, kOutput,  // owned by the caller
  kExposure, kDefaultReactive, kAccumulation1, kAccumulation2, kLuma1, kLuma2, kFp16x1,
  kShadingChange, kNewLocks, kUpscaled1, kUpscaled2, kSpdMips, kFarthestMip1, kLumaHistory1,
  kLumaHistory2, kSpdAtomic, kDilatedReactive, kFrameInfo, kDilatedDepth, kDilatedMotion,
  kPreviousNearestDepth, kResourceCount
};
constexpr uint32_t kFirstInternal = kExposure;

// Shader binding names (ffx_fsr3upscaler.cpp's binding tables) and the resource each
// names on even and odd frames (the SDK's ping-pong of history resources).
struct Name {
  const char* name;
  Resource even, odd;
};
constexpr Name kNames[] = {
    {"r_input_color_jittered", kColor, kColor},
    {"r_input_motion_vectors", kMotion, kMotion},
    {"r_input_depth", kDepth, kDepth},
    {"r_input_exposure", kExposure, kExposure},
    {"r_frame_info", kFrameInfo, kFrameInfo},
    {"r_reactive_mask", kDefaultReactive, kDefaultReactive},
    {"r_transparency_and_composition_mask", kDefaultReactive, kDefaultReactive},
    {"r_reconstructed_previous_nearest_depth", kPreviousNearestDepth, kPreviousNearestDepth},
    {"r_dilated_motion_vectors", kDilatedMotion, kDilatedMotion},
    {"r_dilated_depth", kDilatedDepth, kDilatedDepth},
    {"r_internal_upscaled_color", kUpscaled1, kUpscaled2},
    {"r_accumulation", kAccumulation1, kAccumulation2},
    {"r_luma_history", kLumaHistory1, kLumaHistory2},
    {"r_rcas_input", kUpscaled2, kUpscaled1},
    {"r_spd_mips", kSpdMips, kSpdMips},
    {"r_dilated_reactive_masks", kDilatedReactive, kDilatedReactive},
    {"r_new_locks", kNewLocks, kNewLocks},
    {"r_farthest_depth", kFp16x1, kFp16x1},
    {"r_farthest_depth_mip1", kFarthestMip1, kFarthestMip1},
    {"r_shading_change", kShadingChange, kShadingChange},
    {"r_current_luma", kLuma1, kLuma2},
    {"r_previous_luma", kLuma2, kLuma1},
    {"r_luma_instability", kFp16x1, kFp16x1},
    {"rw_reconstructed_previous_nearest_depth", kPreviousNearestDepth, kPreviousNearestDepth},
    {"rw_dilated_motion_vectors", kDilatedMotion, kDilatedMotion},
    {"rw_dilated_depth", kDilatedDepth, kDilatedDepth},
    {"rw_internal_upscaled_color", kUpscaled2, kUpscaled1},
    {"rw_accumulation", kAccumulation2, kAccumulation1},
    {"rw_luma_history", kLumaHistory2, kLumaHistory1},
    {"rw_upscaled_output", kOutput, kOutput},
    {"rw_dilated_reactive_masks", kDilatedReactive, kDilatedReactive},
    {"rw_frame_info", kFrameInfo, kFrameInfo},
    {"rw_spd_global_atomic", kSpdAtomic, kSpdAtomic},
    {"rw_new_locks", kNewLocks, kNewLocks},
    {"rw_shading_change", kShadingChange, kShadingChange},
    {"rw_farthest_depth", kFp16x1, kFp16x1},
    {"rw_farthest_depth_mip1", kFarthestMip1, kFarthestMip1},
    {"rw_current_luma", kLuma1, kLuma2},
    {"rw_luma_instability", kFp16x1, kFp16x1},
};

enum Pass : uint32_t {
  kPassPrepareInputs, kPassLumaPyramid, kPassShadingChangePyramid, kPassShadingChange,
  kPassPrepareReactivity, kPassLumaInstability, kPassAccumulate, kPassRcas, kPassCount
};
constexpr const char* kPassFiles[kPassCount] = {
    "ffx_fsr3upscaler_prepare_inputs_pass.glsl", "ffx_fsr3upscaler_luma_pyramid_pass.glsl",
    "ffx_fsr3upscaler_shading_change_pyramid_pass.glsl",
    "ffx_fsr3upscaler_shading_change_pass.glsl", "ffx_fsr3upscaler_prepare_reactivity_pass.glsl",
    "ffx_fsr3upscaler_luma_instability_pass.glsl", "ffx_fsr3upscaler_accumulate_pass.glsl",
    "ffx_fsr3upscaler_rcas_pass.glsl"};

enum Constants : uint32_t { kCbMain, kCbSpd, kCbRcas, kCbCount };
constexpr VkDeviceSize kCbStride = 256;

struct MainConstants {  // Fsr3UpscalerConstants, std140-compatible
  int32_t render_size[2], previous_render_size[2], upscale_size[2], previous_upscale_size[2];
  int32_t max_render_size[2], max_upscale_size[2];
  float device_to_view_depth[4];
  float jitter[2], previous_jitter[2], motion_vector_scale[2], downscale_factor[2];
  float motion_vector_jitter_cancellation[2];
  float tan_half_fov, jitter_phase_count, delta_time, delta_pre_exposure;
  float view_space_to_meters, frame_index, velocity_factor, reactiveness_scale;
  float shading_change_scale, accumulation_added_per_frame, min_disocclusion_accumulation;
};
struct SpdConstants {
  uint32_t mips, work_groups, work_group_offset[2], render_size[2];
};

uint16_t ToHalf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
  if (exponent <= 0) return uint16_t(sign);
  if (exponent >= 31) return uint16_t(sign | 0x7C00u);
  return uint16_t(sign | ((uint32_t(exponent) << 10) + (((bits & 0x7FFFFFu) + 0x1000u) >> 13)));
}
}  // namespace

struct Fh1Fsr3::State {
  const ui::vulkan::VulkanDevice* device;
  PFN_vkCmdUpdateBuffer update_buffer = nullptr;
  uint32_t render_width = 0, render_height = 0, output_width = 0, output_height = 0;
  float sharpness = -1.0f;
  struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    std::array<VkImageView, 6> mips{};
    uint32_t levels = 1;
    bool owned = false;
  };
  std::array<Image, kResourceCount> images;
  struct Binding {
    uint32_t binding;
    VkDescriptorType type;
    Resource even = kColor, odd = kColor;
    uint32_t mip = 0;
    Constants constants = kCbMain;
  };
  struct PassState {
    std::vector<Binding> bindings;
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    std::array<VkDescriptorSet, 2> sets{};
  };
  std::array<PassState, kPassCount> passes;
  VkSampler point_sampler{}, linear_sampler{};
  VkDescriptorPool pool{};
  VkBuffer constants{};
  VkDeviceMemory constants_memory{};
  MainConstants main{};
  bool first = true;
  uint32_t frame_index = 0;

  bool CreateImage(Image& image, uint32_t w, uint32_t h, VkFormat format, bool mip_chain = false) {
    const auto& f = device->functions();
    w = std::max(w, 1u); h = std::max(h, 1u);
    image.levels = mip_chain ? uint32_t(std::floor(std::log2(float(std::max(w, h))))) + 1 : 1;
    VkFormatProperties properties{};
    device->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
        device->physical_device(), format, &properties);
    if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
      REXGPU_ERROR("FSR 3: format {} is not usable as a storage image", uint32_t(format));
      return false;
    }
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {w, h, 1};
    info.mipLevels = image.levels; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(device, info,
        ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image, image.memory)) return false;
    image.owned = true;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.levels, 0, 1};
    if (f.vkCreateImageView(device->device(), &view, nullptr, &image.view) != VK_SUCCESS) return false;
    for (uint32_t mip = 0; mip < image.mips.size(); ++mip) {
      if (mip >= image.levels) { image.mips[mip] = image.mips[image.levels - 1]; continue; }
      if (image.levels == 1) { image.mips[mip] = image.view; continue; }
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
      if (f.vkCreateImageView(device->device(), &view, nullptr, &image.mips[mip]) != VK_SUCCESS) return false;
    }
    return true;
  }

  bool CreatePass(Pass pass, const std::map<std::string, std::string>& sources) {
    const auto& f = device->functions(); const auto d = device->device();
    std::string preamble =
        "#define FFX_GPU 1\n#define FFX_GLSL 1\n#define FFX_HALF 0\n"
        "#define FFX_SPD_NO_WAVE_OPERATIONS 1\n"
        "#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF 0\n"
        "#define FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF 0\n"
        "#define FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF 0\n"
        "#define FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF 0\n"
        "#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE 2\n"
        "#define FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE 0\n"
        "#define FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT 0\n"
        "#define FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS 1\n"
        "#define FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS 0\n"
        "#define FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH 1\n";
    preamble += std::string("#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING ") +
                (sharpness >= 0.0f ? "1\n" : "0\n");
    auto include = [&sources](const std::string& requested, const std::string& includer,
                              std::string& resolved, std::string_view& text) {
      const size_t slash = includer.find_last_of('/');
      const std::string directory = slash == std::string::npos ? "" : includer.substr(0, slash + 1);
      for (const std::string& candidate : {directory + requested, requested}) {
        if (auto it = sources.find(candidate); it != sources.end()) {
          resolved = candidate; text = it->second; return true;
        }
      }
      return false;
    };
    const auto source = sources.find(kPassFiles[pass]);
    if (source == sources.end()) { REXGPU_ERROR("FSR 3: missing {}", kPassFiles[pass]); return false; }
    std::vector<uint32_t> spirv;
    std::vector<Fh1ShaderBinding> reflected;
    std::string error;
    if (!CompileFh1ComputeReflected(source->second, preamble, include, spirv, reflected, error)) {
      REXGPU_ERROR("FSR 3: {}: {}", kPassFiles[pass], error);
      return false;
    }
    auto& state = passes[pass];
    std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
    for (const auto& b : reflected) {
      Binding binding{b.binding};
      VkDescriptorSetLayoutBinding layout_binding{b.binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                  VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      std::string lower = b.name;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return char(std::tolower(c)); });
      if (b.block) {
        binding.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binding.constants = lower.find("spd") != std::string::npos ? kCbSpd :
            lower.find("rcas") != std::string::npos ? kCbRcas : kCbMain;
        if (binding.constants == kCbMain && lower.find("fsr3upscaler") == std::string::npos) {
          REXGPU_ERROR("FSR 3: {} uses unsupported constants {}", kPassFiles[pass], b.name);
          return false;
        }
      } else if (b.name == "s_PointClamp" || b.name == "s_LinearClamp") {
        binding.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        layout_binding.pImmutableSamplers = b.name == "s_PointClamp" ? &point_sampler : &linear_sampler;
      } else {
        binding.type = b.name.rfind("rw_", 0) == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bool found = false;
        if (b.name.rfind("rw_spd_mip", 0) == 0 && b.name.size() == 11) {
          binding.even = binding.odd = kSpdMips; binding.mip = uint32_t(b.name[10] - '0'); found = binding.mip < 6;
        }
        for (const auto& name : kNames) {
          if (!found && b.name == name.name) { binding.even = name.even; binding.odd = name.odd; found = true; }
        }
        if (!found) {
          REXGPU_ERROR("FSR 3: {} uses unsupported resource {}", kPassFiles[pass], b.name);
          return false;
        }
      }
      layout_binding.descriptorType = binding.type;
      layout_bindings.push_back(layout_binding);
      state.bindings.push_back(binding);
    }
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = uint32_t(layout_bindings.size()); set_info.pBindings = layout_bindings.data();
    if (f.vkCreateDescriptorSetLayout(d, &set_info, nullptr, &state.set_layout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1; layout.pSetLayouts = &state.set_layout;
    if (f.vkCreatePipelineLayout(d, &layout, nullptr, &state.layout) != VK_SUCCESS) return false;
    state.pipeline = ui::vulkan::util::CreateComputePipeline(device, state.layout, spirv.data(),
                                                             spirv.size() * sizeof(uint32_t));
    return state.pipeline != VK_NULL_HANDLE;
  }

  bool WriteDescriptors() {
    const auto& f = device->functions(); const auto d = device->device();
    std::array<VkDescriptorPoolSize, 4> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 0}}};
    for (const auto& pass : passes)
      for (const auto& b : pass.bindings)
        for (auto& size : sizes) if (size.type == b.type) size.descriptorCount += 2;
    std::vector<VkDescriptorPoolSize> used;
    for (const auto& size : sizes) if (size.descriptorCount) used.push_back(size);
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 2 * kPassCount; pool_info.poolSizeCount = uint32_t(used.size());
    pool_info.pPoolSizes = used.data();
    if (f.vkCreateDescriptorPool(d, &pool_info, nullptr, &pool) != VK_SUCCESS) return false;
    for (auto& pass : passes) {
      if (!pass.pipeline) continue;
      const std::array<VkDescriptorSetLayout, 2> layouts{pass.set_layout, pass.set_layout};
      VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      alloc.descriptorPool = pool; alloc.descriptorSetCount = 2; alloc.pSetLayouts = layouts.data();
      if (f.vkAllocateDescriptorSets(d, &alloc, pass.sets.data()) != VK_SUCCESS) return false;
      for (uint32_t odd = 0; odd < 2; ++odd) {
        std::vector<VkDescriptorImageInfo> images_info(pass.bindings.size());
        std::vector<VkDescriptorBufferInfo> buffers_info(pass.bindings.size());
        std::vector<VkWriteDescriptorSet> writes;
        for (size_t i = 0; i < pass.bindings.size(); ++i) {
          const auto& b = pass.bindings[i];
          if (b.type == VK_DESCRIPTOR_TYPE_SAMPLER) continue;
          VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          write.dstSet = pass.sets[odd]; write.dstBinding = b.binding; write.descriptorCount = 1;
          write.descriptorType = b.type;
          if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            buffers_info[i] = {constants, kCbStride * b.constants, kCbStride};
            write.pBufferInfo = &buffers_info[i];
          } else {
            const auto& image = images[odd ? b.odd : b.even];
            images_info[i] = {VK_NULL_HANDLE, b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
                image.levels > 1 ? image.mips[b.mip] : image.view, VK_IMAGE_LAYOUT_GENERAL};
            write.pImageInfo = &images_info[i];
          }
          writes.push_back(write);
        }
        f.vkUpdateDescriptorSets(d, uint32_t(writes.size()), writes.data(), 0, nullptr);
      }
    }
    return true;
  }

  void MemoryBarrier(VkCommandBuffer command, VkPipelineStageFlags src_stage, VkAccessFlags src,
                     VkPipelineStageFlags dst_stage, VkAccessFlags dst) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = src; barrier.dstAccessMask = dst;
    device->functions().vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 1, &barrier, 0,
                                             nullptr, 0, nullptr);
  }
  void Clear(VkCommandBuffer command, Resource resource, float r = 0, float g = 0) {
    VkClearColorValue value{};
    value.float32[0] = r; value.float32[1] = g;
    const auto& image = images[resource];
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, image.levels, 0, 1};
    device->functions().vkCmdClearColorImage(command, image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
  }
  void Dispatch(VkCommandBuffer command, Pass pass, uint32_t x, uint32_t y, bool odd) {
    const auto& f = device->functions();
    const auto& state = passes[pass];
    f.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
    f.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, state.layout, 0, 1,
                              &state.sets[odd], 0, nullptr);
    f.vkCmdDispatch(command, x, y, 1);
    MemoryBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
  }
};

float Fh1Fsr3::Ratio(std::string_view mode) {
  if (mode == "fsr3_quality") return 1.5f;
  if (mode == "fsr3_balanced") return 1.7f;
  if (mode == "fsr3_performance") return 2.0f;
  if (mode == "fsr3_native") return 1.0f;
  return 0.0f;
}

uint32_t Fh1Fsr3::JitterPhases(uint32_t render_width, uint32_t output_width) {
  // ffxFsr3UpscalerGetJitterPhaseCount
  const float ratio = float(output_width) / float(std::max(render_width, 1u));
  return std::max(uint32_t(8.0f * ratio * ratio), 1u);
}

Fh1Fsr3::Fh1Fsr3(const ui::vulkan::VulkanDevice* device) : state_(std::make_unique<State>()) {
  state_->device = device;
}

Fh1Fsr3::~Fh1Fsr3() {
  auto& s = *state_;
  const auto& f = s.device->functions(); const auto d = s.device->device();
  for (auto& pass : s.passes) {
    if (pass.pipeline) f.vkDestroyPipeline(d, pass.pipeline, nullptr);
    if (pass.layout) f.vkDestroyPipelineLayout(d, pass.layout, nullptr);
    if (pass.set_layout) f.vkDestroyDescriptorSetLayout(d, pass.set_layout, nullptr);
  }
  if (s.pool) f.vkDestroyDescriptorPool(d, s.pool, nullptr);
  for (auto& image : s.images) {
    if (!image.owned) continue;
    for (uint32_t mip = 0; mip < image.mips.size() && image.levels > 1 && mip < image.levels; ++mip)
      if (image.mips[mip]) f.vkDestroyImageView(d, image.mips[mip], nullptr);
    if (image.view) f.vkDestroyImageView(d, image.view, nullptr);
    if (image.image) f.vkDestroyImage(d, image.image, nullptr);
    if (image.memory) f.vkFreeMemory(d, image.memory, nullptr);
  }
  if (s.point_sampler) f.vkDestroySampler(d, s.point_sampler, nullptr);
  if (s.linear_sampler) f.vkDestroySampler(d, s.linear_sampler, nullptr);
  if (s.constants) f.vkDestroyBuffer(d, s.constants, nullptr);
  if (s.constants_memory) f.vkFreeMemory(d, s.constants_memory, nullptr);
}

bool Fh1Fsr3::Initialize(uint32_t render_width, uint32_t render_height, uint32_t output_width,
                         uint32_t output_height, VkImageView color, VkImageView depth,
                         VkImageView motion, VkImageView output, float sharpness,
                         PFN_vkCmdUpdateBuffer update_buffer) {
#if defined(REX_FH1_FSR3)
  auto& s = *state_;
  const auto& f = s.device->functions(); const auto d = s.device->device();
  const auto start = std::chrono::steady_clock::now();
  s.render_width = render_width; s.render_height = render_height;
  s.output_width = output_width; s.output_height = output_height;
  s.sharpness = sharpness; s.update_buffer = update_buffer;
  if (!update_buffer) return false;
  const VkImageView external[] = {color, depth, motion, output};
  for (uint32_t i = 0; i < kFirstInternal; ++i) {
    s.images[i].view = external[i];
    s.images[i].mips.fill(external[i]);
  }
  const uint32_t rw = render_width, rh = render_height, ow = output_width, oh = output_height;
  struct Desc { Resource id; uint32_t w, h; VkFormat format; bool mips; };
  const Desc descs[] = {
      {kExposure, 1, 1, VK_FORMAT_R32G32_SFLOAT, false},
      {kDefaultReactive, 1, 1, VK_FORMAT_R8_UNORM, false},
      {kAccumulation1, rw, rh, VK_FORMAT_R8_UNORM, false},
      {kAccumulation2, rw, rh, VK_FORMAT_R8_UNORM, false},
      {kLuma1, rw, rh, VK_FORMAT_R16_SFLOAT, false},
      {kLuma2, rw, rh, VK_FORMAT_R16_SFLOAT, false},
      {kFp16x1, rw, rh, VK_FORMAT_R16_SFLOAT, false},
      {kShadingChange, rw / 2, rh / 2, VK_FORMAT_R8_UNORM, false},
      {kNewLocks, ow, oh, VK_FORMAT_R8_UNORM, false},
      {kUpscaled1, ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT, false},
      {kUpscaled2, ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT, false},
      {kSpdMips, rw / 2, rh / 2, VK_FORMAT_R16G16_SFLOAT, true},
      {kFarthestMip1, rw / 2, rh / 2, VK_FORMAT_R16_SFLOAT, false},
      // The GLSL declares the luma history rgba8.
      {kLumaHistory1, rw, rh, VK_FORMAT_R8G8B8A8_UNORM, false},
      {kLumaHistory2, rw, rh, VK_FORMAT_R8G8B8A8_UNORM, false},
      {kSpdAtomic, 1, 1, VK_FORMAT_R32_UINT, false},
      {kDilatedReactive, rw, rh, VK_FORMAT_R8G8B8A8_UNORM, false},
      {kFrameInfo, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, false},
      {kDilatedDepth, rw, rh, VK_FORMAT_R32_SFLOAT, false},
      {kDilatedMotion, rw, rh, VK_FORMAT_R16G16_SFLOAT, false},
      {kPreviousNearestDepth, rw, rh, VK_FORMAT_R32_UINT, false},
  };
  for (const auto& desc : descs)
    if (!s.CreateImage(s.images[desc.id], desc.w, desc.h, desc.format, desc.mips)) return false;
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.maxLod = VK_LOD_CLAMP_NONE;
  if (f.vkCreateSampler(d, &sampler, nullptr, &s.point_sampler) != VK_SUCCESS) return false;
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  if (f.vkCreateSampler(d, &sampler, nullptr, &s.linear_sampler) != VK_SUCCESS) return false;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(s.device, kCbStride * kCbCount,
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      ui::vulkan::util::MemoryPurpose::kDeviceLocal, s.constants, s.constants_memory)) return false;

  std::map<std::string, std::string> sources;
  for (const auto& source : kFsr3Sources)
    sources[source.name].assign(reinterpret_cast<const char*>(source.data), source.size);
  // The app chooses the output format: this renderer's output image is RGBA16F.
  auto& callbacks = sources["fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h"];
  const std::string_view marker = "/* app controlled format */)";
  const size_t at = callbacks.find(marker);
  if (at == std::string::npos) { REXGPU_ERROR("FSR 3: unexpected callbacks source"); return false; }
  callbacks.replace(at, marker.size(), ", rgba16f)");
  for (uint32_t pass = 0; pass < kPassCount; ++pass) {
    if (pass == kPassRcas && sharpness < 0.0f) continue;
    if (!s.CreatePass(Pass(pass), sources)) return false;
  }
  if (!s.WriteDescriptors()) return false;
  s.main.max_render_size[0] = int32_t(rw); s.main.max_render_size[1] = int32_t(rh);
  s.main.max_upscale_size[0] = int32_t(ow); s.main.max_upscale_size[1] = int32_t(oh);
  s.main.velocity_factor = 1.0f; s.main.reactiveness_scale = 1.0f;
  s.main.shading_change_scale = 1.0f;
  s.main.accumulation_added_per_frame = 1.0f / 3.0f;
  s.main.min_disocclusion_accumulation = -1.0f / 3.0f;
  REXGPU_INFO("FSR 3: FidelityFX FSR 3.1.4 upscaler ready, {}x{} -> {}x{}, sharpening {}, "
              "shaders compiled in {} ms", rw, rh, ow, oh, sharpness,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start).count());
  return true;
#else
  REXGPU_ERROR("FSR 3: this renderer was built without the FidelityFX sources");
  return false;
#endif
}

void Fh1Fsr3::Execute(VkCommandBuffer command, const Frame& frame) {
  auto& s = *state_;
  const bool odd = s.frame_index & 1;
  const bool reset = frame.reset || s.first;
  // The color blit and the depth/motion pass precede this; also wait for the previous
  // frame's reads of the images cleared below.
  s.MemoryBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                  VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  if (s.first) {
    for (uint32_t i = kFirstInternal; i < kResourceCount; ++i) s.Clear(command, Resource(i));
  }
  if (reset) {
    s.Clear(command, odd ? kAccumulation2 : kAccumulation1);
    s.Clear(command, kFrameInfo, -1.0f, 1.0f);
  }
  s.Clear(command, kPreviousNearestDepth);  // inverted depth: far is 0
  s.Clear(command, kSpdAtomic);
  s.Clear(command, kSpdMips);

  auto& c = s.main;
  const uint32_t rw = s.render_width, rh = s.render_height, ow = s.output_width, oh = s.output_height;
  c.previous_jitter[0] = c.jitter[0]; c.previous_jitter[1] = c.jitter[1];
  c.jitter[0] = frame.jitter_x; c.jitter[1] = frame.jitter_y;
  c.previous_render_size[0] = c.render_size[0]; c.previous_render_size[1] = c.render_size[1];
  c.render_size[0] = int32_t(rw); c.render_size[1] = int32_t(rh);
  c.previous_upscale_size[0] = c.upscale_size[0]; c.previous_upscale_size[1] = c.upscale_size[1];
  c.upscale_size[0] = int32_t(ow); c.upscale_size[1] = int32_t(oh);
  // FH1 does not expose its projection here; a 60 degree vertical field of view only
  // scales FSR's disocclusion and depth-change thresholds.
  const float aspect = float(rw) / float(rh);
  const float tan_half_fov_y = std::tan(0.5f * 1.0472f);
  c.tan_half_fov = tan_half_fov_y * aspect;
  c.view_space_to_meters = 1.0f;
  c.device_to_view_depth[0] = frame.device_to_view[0];
  c.device_to_view_depth[1] = frame.device_to_view[1];
  c.device_to_view_depth[2] = tan_half_fov_y * aspect;
  c.device_to_view_depth[3] = tan_half_fov_y;
  c.downscale_factor[0] = float(rw) / float(ow); c.downscale_factor[1] = float(rh) / float(oh);
  c.delta_pre_exposure = 1.0f;
  // Motion vectors are in render pixels.
  c.motion_vector_scale[0] = 1.0f / float(rw); c.motion_vector_scale[1] = 1.0f / float(rh);
  c.motion_vector_jitter_cancellation[0] = c.motion_vector_jitter_cancellation[1] = 0.0f;
  const float phases = float(JitterPhases(rw, ow));
  if (reset || c.jitter_phase_count == 0.0f) c.jitter_phase_count = phases;
  else if (phases > c.jitter_phase_count) c.jitter_phase_count += 1.0f;
  else if (phases < c.jitter_phase_count) c.jitter_phase_count -= 1.0f;
  c.delta_time = std::clamp(frame.delta_time_ms / 1000.0f, 0.0f, 1.0f);
  c.frame_index = reset ? 0.0f : c.frame_index + 1.0f;

  // ffxSpdSetup over the whole render rectangle.
  const uint32_t spd_x = (rw - 1) / 64 + 1, spd_y = (rh - 1) / 64 + 1;
  SpdConstants spd{};
  spd.work_groups = spd_x * spd_y;
  spd.mips = uint32_t(std::min(std::floor(std::log2(float(std::max(rw, rh)))), 12.0f));
  spd.render_size[0] = rw; spd.render_size[1] = rh;
  // FsrRcasCon with the SDK's sharpness remapping.
  uint32_t rcas[4] = {};
  const float stops = std::exp2(-((-2.0f * std::max(s.sharpness, 0.0f)) + 2.0f));
  std::memcpy(&rcas[0], &stops, sizeof(float));
  rcas[1] = uint32_t(ToHalf(stops)) | (uint32_t(ToHalf(stops)) << 16);

  s.MemoryBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_UNIFORM_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  s.update_buffer(command, s.constants, kCbStride * kCbMain, sizeof(c), &c);
  s.update_buffer(command, s.constants, kCbStride * kCbSpd, sizeof(spd), &spd);
  s.update_buffer(command, s.constants, kCbStride * kCbRcas, sizeof(rcas), rcas);
  s.MemoryBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

  const uint32_t src_x = (rw + 7) / 8, src_y = (rh + 7) / 8;
  s.Dispatch(command, kPassPrepareInputs, src_x, src_y, odd);
  s.Dispatch(command, kPassLumaPyramid, spd_x, spd_y, odd);
  s.Dispatch(command, kPassShadingChangePyramid, spd_x, spd_y, odd);
  s.Dispatch(command, kPassShadingChange, (uint32_t(rw * 0.5f) + 7) / 8, (uint32_t(rh * 0.5f) + 7) / 8, odd);
  s.Dispatch(command, kPassPrepareReactivity, src_x, src_y, odd);
  s.Dispatch(command, kPassLumaInstability, src_x, src_y, odd);
  s.Dispatch(command, kPassAccumulate, (ow + 7) / 8, (oh + 7) / 8, odd);
  if (s.sharpness >= 0.0f) s.Dispatch(command, kPassRcas, (ow + 15) / 16, (oh + 15) / 16, odd);
  s.frame_index = (s.frame_index + 1) % 16;
  s.first = false;
}
}  // namespace rex::graphics::vulkan
