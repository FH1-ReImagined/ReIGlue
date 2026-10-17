#include <rex/graphics/vulkan/fh1_dlss.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>
#include <rex/logging.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <array>
#if defined(REX_FH1_NGX)
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers.h>
#endif

namespace rex::graphics::vulkan {
struct Fh1Dlss::State {
  const ui::vulkan::VulkanDevice* device;
  uint32_t width = 0, height = 0, output_width = 0, output_height = 0;
  uint32_t backing_scale = 1;
  std::string mode;
#if defined(REX_FH1_NGX)
  NVSDK_NGX_PerfQuality_Value quality = NVSDK_NGX_PerfQuality_Value_MaxPerf;
#endif
  bool dlaa = false;
  const char* test_failure = std::getenv("FORZA_DLSS_TEST_FAILURE");
#if defined(REX_FH1_NGX)
  NVSDK_NGX_Parameter* parameters = nullptr;
  NVSDK_NGX_Handle* feature = nullptr;
  bool initialized = false, images_ready = false, failed = false;
  uint64_t frames = 0;
  uint64_t submissions = 0;
  struct Image { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; VkFormat format{}; uint32_t w{}, h{}; };
  std::array<Image, 4> images; // color, output, depth, motion
  PFN_vkCmdBlitImage blit = nullptr;
  PFN_vkCmdUpdateBuffer update_buffer = nullptr;
  VkDescriptorSetLayout temporal_set_layout{};
  VkDescriptorPool temporal_pool{};
  std::array<VkDescriptorSet, 8> temporal_sets{};
  VkPipelineLayout temporal_layout{};
  VkPipeline temporal_pipeline{};
  VkSampler temporal_sampler{};
  VkBuffer temporal_constants{};
  VkDeviceMemory temporal_memory{};
  bool previous_temporal = false, logged_temporal = false;
  bool CreateTemporalPipeline() {
    const auto& f = device->functions(); const auto d = device->device();
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 4; set_info.pBindings = bindings;
    if (f.vkCreateDescriptorSetLayout(d, &set_info, nullptr, &temporal_set_layout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1; layout.pSetLayouts = &temporal_set_layout;
    if (f.vkCreatePipelineLayout(d, &layout, nullptr, &temporal_layout) != VK_SUCCESS) return false;
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 8; pool.poolSizeCount = 3; pool.pPoolSizes = sizes;
    if (f.vkCreateDescriptorPool(d, &pool, nullptr, &temporal_pool) != VK_SUCCESS) return false;
    std::array<VkDescriptorSetLayout, 8> layouts; layouts.fill(temporal_set_layout);
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = temporal_pool; alloc.descriptorSetCount = 8; alloc.pSetLayouts = layouts.data();
    if (f.vkAllocateDescriptorSets(d, &alloc, temporal_sets.data()) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (f.vkCreateSampler(d, &sampler, nullptr, &temporal_sampler) != VK_SUCCESS) return false;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(device, sizeof(FrameInput::constants),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        ui::vulkan::util::MemoryPurpose::kDeviceLocal, temporal_constants, temporal_memory)) return false;
    // Reproduce the game's depth/stencil decode and unclamped per-object
    // reprojection (PPMotionBlurDepthHorizonVel, E68FE55ECD482EC5). The later
    // speed-blur shaping and clamping in that shader must not affect DLSS MVs.
    static constexpr char source[] = R"glsl(#version 450
layout(local_size_x=8,local_size_y=8) in;
layout(binding=0) uniform sampler2DArray packedDepth;
layout(binding=1,r32f) uniform writeonly image2D depthOut;
layout(binding=2,rg16f) uniform writeonly image2D motionOut;
layout(binding=3,std140) uniform Constants { vec4 c[136]; };
void main() {
  ivec2 p=ivec2(gl_GlobalInvocationID.xy), size=imageSize(depthOut);
  if(any(greaterThanEqual(p,size))) return;
  vec2 uv=(vec2(p)+0.5)/vec2(size);
  uvec4 b=uvec4(round(textureLod(packedDepth,vec3(uv,0),0).wxyz*255.0));
  float depth=exp2(float(int(b.x>>4)-15))*(1.0+float(((b.x&15u)<<16)|(b.y<<8)|b.z)/1048576.0);
  depth=clamp(depth,0.0,1.0);
  uint objectId=b.w>=128u?0u:min((b.w+1u)/3u,24u);
  uint row=32u+4u*objectId;
  vec2 current=(uv-c[134].xy/vec2(size))*vec2(2.0,-2.0)+vec2(-1.0,1.0);
  float denominator=1.0-c[132].x-depth;
  float z=c[132].y/(abs(denominator)>1e-20?denominator:-1e-20);
  // Match the original shader's r3.xyzz products exactly: its homogeneous
  // component (1-depth) is multiplied by reconstructed z as well.
  vec4 position=vec4(current.x*z,current.y*z,z*(1.0-depth),z);
  vec4 a=c[row], y=c[row+1u], w=c[row+3u];
  float previousW=dot(position,w);
  vec2 previous=vec2(dot(position,a),dot(position,y))/previousW;
  vec2 motion=(previous-current)*vec2(size)*vec2(0.5,-0.5);
  if(any(isnan(motion))||any(isinf(motion))||abs(previousW)<1e-20) motion=vec2(0);
  motion=clamp(motion,-vec2(size),vec2(size));
  imageStore(depthOut,p,vec4(depth));
  imageStore(motionOut,p,vec4(motion,0,0));
})glsl";
    std::vector<uint32_t> spirv; std::string error;
    if (!CompileFh1Compute(source, spirv, error)) { REXGPU_ERROR("DLSS temporal shader: {}", error); return false; }
    temporal_pipeline = ui::vulkan::util::CreateComputePipeline(device, temporal_layout,
        spirv.data(), spirv.size() * sizeof(uint32_t));
    return temporal_pipeline != VK_NULL_HANDLE;
  }
  void Temporal(VkCommandBuffer command, const FrameInput& input) {
    const auto& f = device->functions();
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = temporal_constants; barrier.size = VK_WHOLE_SIZE;
    f.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);
    update_buffer(command, temporal_constants, 0, sizeof(input.constants), input.constants.data());
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    f.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);
    const auto set = temporal_sets[submissions % temporal_sets.size()];
    VkDescriptorImageInfo info[] = {{temporal_sampler, input.depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, images[2].view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, images[3].view, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo buffer{temporal_constants, 0, sizeof(input.constants)};
    VkWriteDescriptorSet writes[4]{};
    for (unsigned i = 0; i < 4; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
      writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
          i == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      if (i < 3) writes[i].pImageInfo = &info[i]; else writes[i].pBufferInfo = &buffer;
    }
    f.vkUpdateDescriptorSets(device->device(), 4, writes, 0, nullptr);
    f.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_pipeline);
    f.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_layout, 0, 1, &set, 0, nullptr);
    f.vkCmdDispatch(command, (images[2].w+7)/8, (images[2].h+7)/8, 1);
    for (unsigned i : {2u,3u}) Barrier(command, images[i].image, VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
  }
  bool Create(Image& image, uint32_t w, uint32_t h, VkFormat format) {
    image.w = w; image.h = h; image.format = format;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {w, h, 1};
    info.mipLevels = info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(device, info,
        ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image, image.memory)) return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return device->functions().vkCreateImageView(device->device(), &view, nullptr, &image.view) == VK_SUCCESS;
  }
  void Barrier(VkCommandBuffer command, VkImage image, VkImageLayout old_layout,
               VkImageLayout new_layout, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = src; barrier.dstAccessMask = dst;
    barrier.oldLayout = old_layout; barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    device->functions().vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
  void Blit(VkCommandBuffer command, VkImage src, VkImageLayout src_layout,
            VkImage dst, VkImageLayout dst_layout, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh) {
    VkImageBlit region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {int32_t(sw), int32_t(sh), 1};
    region.dstOffsets[1] = {int32_t(dw), int32_t(dh), 1};
    blit(command, src, src_layout, dst, dst_layout, 1, &region, VK_FILTER_LINEAR);
  }
#endif
};
Fh1Dlss::Fh1Dlss(const ui::vulkan::VulkanDevice* device, uint32_t output_width,
                   uint32_t output_height, std::string_view mode) : state_(std::make_unique<State>()) {
  state_->device = device;
  state_->mode = mode;
  state_->dlaa = mode == "dlaa";
  state_->output_width = output_width;
  state_->output_height = output_height;
}
uint32_t Fh1Dlss::width() const { return state_->width; }
uint32_t Fh1Dlss::height() const { return state_->height; }
uint32_t Fh1Dlss::output_width() const { return state_->output_width; }
uint32_t Fh1Dlss::output_height() const { return state_->output_height; }
void Fh1Dlss::SetSceneBacking(uint32_t scale) { state_->backing_scale = scale; }
Fh1Dlss::~Fh1Dlss() {
#if defined(REX_FH1_NGX)
  auto& s = *state_;
  if (s.feature) NVSDK_NGX_VULKAN_ReleaseFeature(s.feature);
  if (s.parameters) NVSDK_NGX_VULKAN_DestroyParameters(s.parameters);
  if (s.initialized) NVSDK_NGX_VULKAN_Shutdown1(s.device->device());
  for (auto& image : s.images) {
    const auto& f = s.device->functions(); const auto d = s.device->device();
    if (image.view) f.vkDestroyImageView(d, image.view, nullptr);
    if (image.image) f.vkDestroyImage(d, image.image, nullptr);
    if (image.memory) f.vkFreeMemory(d, image.memory, nullptr);
  }
  const auto& f = s.device->functions(); const auto d = s.device->device();
  if (s.temporal_pipeline) f.vkDestroyPipeline(d, s.temporal_pipeline, nullptr);
  if (s.temporal_pool) f.vkDestroyDescriptorPool(d, s.temporal_pool, nullptr);
  if (s.temporal_layout) f.vkDestroyPipelineLayout(d, s.temporal_layout, nullptr);
  if (s.temporal_set_layout) f.vkDestroyDescriptorSetLayout(d, s.temporal_set_layout, nullptr);
  if (s.temporal_sampler) f.vkDestroySampler(d, s.temporal_sampler, nullptr);
  if (s.temporal_constants) f.vkDestroyBuffer(d, s.temporal_constants, nullptr);
  if (s.temporal_memory) f.vkFreeMemory(d, s.temporal_memory, nullptr);
#endif
}
void Fh1Dlss::ReportStatus(std::string_view message) {
#if defined(_WIN32)
  const wchar_t* path = _wgetenv(L"FORZA_DLSS_STATUS_FILE");
#else
  const char* path = std::getenv("FORZA_DLSS_STATUS_FILE");
#endif
  if (!path || !*path) return;
  std::ofstream stream(std::filesystem::path(path), std::ios::trunc);
  stream << message;
}
void Fh1Dlss::BeginFrame(uint64_t frame) {
    input_ = {};
    if (!Evaluating()) {
      if (jitter_was_active_) REXGPU_INFO("DLSS: jitter disabled; offsets={},{}", input_.jitter_x, input_.jitter_y);
      jitter_was_active_ = false;
      return;
    }
    jitter_was_active_ = true;
    auto halton = [](uint64_t n, unsigned radix) {
      float value = 0, factor = 1;
      while (n) { factor /= float(radix); value += factor * float(n % radix); n /= radix; }
      return value;
    };
    input_.jitter_x = halton(frame % 16 + 1, 2) - 0.5f;
    input_.jitter_y = halton(frame % 16 + 1, 3) - 0.5f;
  }

bool Fh1Dlss::Initialize() {
#if defined(REX_FH1_NGX)
  auto& s = *state_;
#if defined(_WIN32)
  const wchar_t* root = _wgetenv(L"FORZA_DLSS_RUNTIME");
#else
  const char* root = std::getenv("FORZA_DLSS_RUNTIME");
#endif
  if (!root || !*root) { REXGPU_ERROR("DLSS: launcher did not supply a verified runtime folder"); return false; }
  const auto folder = std::filesystem::path(root);
  const std::wstring path = folder.wstring(); const wchar_t* search = path.c_str();
  NVSDK_NGX_FeatureCommonInfo common{};
  common.PathListInfo = {&search, 1};
  common.LoggingInfo.LoggingCallback = [](const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    REXGPU_INFO("NGX: {}", message);
  };
  common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
  const auto* instance = s.device->vulkan_instance();
  const auto& functions = instance->functions();
  auto result = NVSDK_NGX_VULKAN_Init_with_ProjectID("34a624c1-4bc3-4c46-923e-e986268895f7",
      NVSDK_NGX_ENGINE_TYPE_CUSTOM, "ReImagined-native", path.c_str(), instance->instance(),
      s.device->physical_device(), s.device->device(), functions.vkGetInstanceProcAddr,
      functions.vkGetDeviceProcAddr, &common);
  if (NVSDK_NGX_FAILED(result)) { REXGPU_ERROR("DLSS: NGX initialization failed: {:08X}", uint32_t(result)); return false; }
  s.initialized = true;
  result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&s.parameters);
  int available = 0;
  int updated_driver = 0, feature_result = 0;
  if (s.parameters) {
    s.parameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &updated_driver);
    s.parameters->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &feature_result);
  }
  if (NVSDK_NGX_FAILED(result) || !s.parameters || NVSDK_NGX_FAILED(s.parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available)) || !available) {
    REXGPU_ERROR("DLSS: unavailable (capabilities={:08X}, feature={:08X}, needs_driver={})",
        uint32_t(result), uint32_t(feature_result), updated_driver); return false;
  }
  if (s.mode == "quality") s.quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
  else if (s.mode == "balanced") s.quality = NVSDK_NGX_PerfQuality_Value_Balanced;
  else if (s.mode == "dlaa") s.quality = NVSDK_NGX_PerfQuality_Value_DLAA;
  else if (s.mode != "dlss" && s.mode != "performance") return false;
  unsigned max_w = 0, max_h = 0, min_w = 0, min_h = 0; float sharpness = 0;
  result = NGX_DLSS_GET_OPTIMAL_SETTINGS(s.parameters, s.output_width, s.output_height,
      s.quality, &s.width, &s.height, &max_w, &max_h, &min_w, &min_h, &sharpness);
  if (NVSDK_NGX_FAILED(result) || !s.width || !s.height || s.width > s.output_width ||
      s.height > s.output_height || (s.dlaa && (s.width != s.output_width || s.height != s.output_height))) {
    REXGPU_ERROR("DLSS: unsupported output/mode or invalid optimal settings: {:08X}", uint32_t(result));
    return false;
  }
  REXGPU_INFO("DLSS: optimal {} input={}x{}, output={}x{}", s.mode, s.width, s.height,
      s.output_width, s.output_height);
  s.blit = reinterpret_cast<PFN_vkCmdBlitImage>(functions.vkGetDeviceProcAddr(s.device->device(), "vkCmdBlitImage"));
  s.update_buffer = reinterpret_cast<PFN_vkCmdUpdateBuffer>(functions.vkGetDeviceProcAddr(s.device->device(), "vkCmdUpdateBuffer"));
  if (!s.blit || !s.Create(s.images[0], s.width, s.height, VK_FORMAT_R16G16B16A16_SFLOAT) ||
      !s.Create(s.images[1], s.output_width, s.output_height, VK_FORMAT_R16G16B16A16_SFLOAT) ||
      !s.Create(s.images[2], s.width, s.height, VK_FORMAT_R32_SFLOAT) ||
      !s.Create(s.images[3], s.width, s.height, VK_FORMAT_R16G16_SFLOAT)) return false;
  if (!s.update_buffer || !s.CreateTemporalPipeline()) return false;
  REXGPU_INFO("DLSS: official NGX Vulkan runtime ready, no ReShade or D3D12 proxy");
  return true;
#else
  REXGPU_ERROR("DLSS: this renderer was built without the optional NGX SDK");
  return false;
#endif
}
void Fh1Dlss::Execute(VkCommandBuffer command, VkImage scene, VkImage destination, const FrameInput& frame_input) {
#if defined(REX_FH1_NGX)
  auto& s = *state_;
  ++s.submissions;
  if (s.failed) {
    s.Blit(command, scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1280 * s.backing_scale, 720 * s.backing_scale, s.output_width, s.output_height);
    return;
  }
  const auto& f = s.device->functions();
  if (!s.images_ready) {
    for (auto& image : s.images) {
      s.Barrier(command, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
      VkClearColorValue clear{};
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      f.vkCmdClearColorImage(command, image.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
    }
    s.images_ready = true;
  }
  auto& input = s.images[0]; auto& output = s.images[1];
  s.Barrier(command, input.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
      VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  s.Blit(command, scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, input.image,
      VK_IMAGE_LAYOUT_GENERAL, 1280 * s.backing_scale, 720 * s.backing_scale, s.width, s.height);
  for (auto& image : s.images)
    s.Barrier(command, image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  const bool temporal = frame_input.depth_view != VK_NULL_HANDLE &&
      !(s.test_failure && std::string_view(s.test_failure) == "temporal");
  if (temporal) {
    s.Temporal(command, frame_input);
    if (!s.logged_temporal) {
      REXGPU_INFO("DLSS: game reprojection=true");
      s.logged_temporal = true;
    }
  }
  if (!s.feature && !s.failed) {
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature = {s.width, s.height, s.output_width, s.output_height,
        s.quality};
    // This is the game's tonemapped scene, before the HUD. A float Vulkan
    // resource does not make those display-referred colors HDR input.
    create.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
        NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    auto result = NGX_VULKAN_CREATE_DLSS_EXT1(s.device->device(), command, 1, 1, &s.feature, s.parameters, &create);
    if (s.test_failure && std::string_view(s.test_failure) == "create") result = NVSDK_NGX_Result_FAIL_PlatformError;
    if (NVSDK_NGX_FAILED(result)) {
      REXGPU_ERROR("DLSS: Vulkan feature creation failed: {:08X}", uint32_t(result)); s.failed = true;
      ReportStatus("error: NVIDIA feature creation failed. This session uses bilinear scaling; turn DLSS off and restart for native rendering. See the game log.");
    } else REXGPU_INFO("DLSS: created {} feature {}x{} -> {}x{}", s.mode, s.width, s.height, s.output_width, s.output_height);
  }
  bool rendered = false;
  if (s.feature && !s.failed) {
    std::array<NVSDK_NGX_Resource_VK, 4> resources;
    for (unsigned i = 0; i < resources.size(); ++i) {
      const auto& image = s.images[i];
      resources[i] = NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image,
          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, image.format, image.w, image.h, i == 1);
    }
    NVSDK_NGX_VK_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = &resources[0]; eval.Feature.pInOutput = &resources[1];
    eval.pInDepth = &resources[2]; eval.pInMotionVectors = &resources[3];
    eval.InRenderSubrectDimensions = {s.width, s.height}; eval.InMVScaleX = eval.InMVScaleY = 1.0f;
    eval.InJitterOffsetX = frame_input.jitter_x; eval.InJitterOffsetY = frame_input.jitter_y;
    eval.InReset = !temporal || !s.previous_temporal;
    eval.InPreExposure = eval.InExposureScale = 1.0f;
    auto result = NGX_VULKAN_EVALUATE_DLSS_EXT(command, s.feature, s.parameters, &eval);
    if (s.test_failure && std::string_view(s.test_failure) == "evaluate" && s.frames >= 8) result = NVSDK_NGX_Result_FAIL_PlatformError;
    rendered = NVSDK_NGX_SUCCEED(result);
    if (!rendered) {
      REXGPU_ERROR("DLSS: evaluation failed: {:08X}", uint32_t(result)); s.failed = true;
      ReportStatus("error: NVIDIA reconstruction stopped. This session uses bilinear scaling; turn DLSS off and restart for native rendering. See the game log.");
    } else {
      if (++s.frames == 1) ReportStatus("ready: " + s.mode + " active: " +
          std::to_string(s.width) + "x" + std::to_string(s.height) + " NGX input to " +
          std::to_string(s.output_width) + "x" + std::to_string(s.output_height) + ", native UI. Scene raster " +
          std::to_string(1280 * s.backing_scale) + "x" + std::to_string(720 * s.backing_scale) + ".");
      if (s.frames % 120 == 0) REXGPU_DEBUG("DLSS: processed {} native Vulkan frames (game reprojection={})", s.frames, temporal);
    }
    s.previous_temporal = temporal;
  }
  evaluating_.store(rendered && !s.failed, std::memory_order_release);
  if (rendered) {
    s.Barrier(command, output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    s.Blit(command, output.image, VK_IMAGE_LAYOUT_GENERAL, destination,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, s.output_width, s.output_height, s.output_width, s.output_height);
  } else s.Blit(command, scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1280 * s.backing_scale, 720 * s.backing_scale, s.output_width, s.output_height);
#endif
}
}
