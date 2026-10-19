// Host integration of the official MIT SMAA 1x algorithm by Jorge Jimenez et al.
// Shader and lookup tables are pinned/hash checked by cmake/ForzaSmaa.cmake.
#include <rex/graphics/vulkan/fh1_smaa.h>
#include <rex/graphics/vulkan/fh1_dlss.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>
#include <rex/logging.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "AreaTex.h"
#include "SearchTex.h"

namespace rex::graphics::vulkan {
namespace {
#include "fh1_smaa_source.inc"
constexpr VkImageSubresourceRange kColorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}
struct Fh1Smaa::State {
  const ui::vulkan::VulkanDevice* device;
  uint32_t width{}, height{};
  struct Image { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; };
  std::array<Image, 4> images; // edges, weights, area lookup, search lookup
  VkBuffer upload{};
  VkDeviceMemory upload_memory{};
  VkSampler sampler{};
  VkDescriptorSetLayout set_layout{};
  VkPipelineLayout layout{};
  VkDescriptorPool pool{};
  std::array<VkDescriptorSet, 3> sets{};
  std::array<VkPipeline, 3> pipelines{};
  bool first = true;

  void Barrier(VkCommandBuffer command, VkImage image, VkImageLayout old_layout,
               VkImageLayout new_layout, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = src; barrier.dstAccessMask = dst;
    barrier.oldLayout = old_layout; barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = kColorRange;
    device->functions().vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
  bool Create(Image& image, uint32_t w, uint32_t h, VkFormat format, bool storage) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {w,h,1};
    info.mipLevels = info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(device, info,
        ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image, image.memory)) return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
    view.subresourceRange = kColorRange;
    return device->functions().vkCreateImageView(device->device(), &view, nullptr, &image.view) == VK_SUCCESS;
  }
  bool Pipeline(unsigned pass) {
    std::string source = R"glsl(#version 450
layout(local_size_x=8, local_size_y=8) in;
layout(push_constant) uniform Constants { vec4 metrics; } pc;
#define SMAA_GLSL_3 1
#define SMAA_PRESET_HIGH 1
#define SMAA_RT_METRICS pc.metrics
// Pixel-shader discard means a zero edge in a compute pass. Every pixel is
// written, so an edge from the preceding frame can never remain in the buffer.
#define discard return vec2(0.0)
)glsl";
    source.append(reinterpret_cast<const char*>(kSmaaSource), sizeof(kSmaaSource));
    source += R"glsl(
layout(binding=0) uniform sampler2D colorTex;
layout(binding=1) uniform sampler2D edgesTex;
layout(binding=2) uniform sampler2D blendTex;
layout(binding=3) uniform sampler2D areaTex;
layout(binding=4) uniform sampler2D searchTex;
)glsl";
    source += pass == 2 ? "layout(binding=5,rgba16f) writeonly uniform image2D resultImage;\n" :
                          "layout(binding=5,rgba8) writeonly uniform image2D resultImage;\n";
    source += R"glsl(void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(p, imageSize(resultImage)))) return;
  vec2 uv = (vec2(p)+0.5)*pc.metrics.xy;
)glsl";
    if (pass == 0) source += R"glsl(
  vec4 offsets[3]; SMAAEdgeDetectionVS(uv,offsets);
  imageStore(resultImage,p,vec4(SMAALumaEdgeDetectionPS(uv,offsets,colorTex),0,0));
)glsl";
    else if (pass == 1) source += R"glsl(
  vec2 pixel; vec4 offsets[3]; SMAABlendingWeightCalculationVS(uv,pixel,offsets);
  imageStore(resultImage,p,SMAABlendingWeightCalculationPS(uv,pixel,offsets,edgesTex,areaTex,searchTex,vec4(0)));
)glsl";
    else source += R"glsl(
  vec4 offset; SMAANeighborhoodBlendingVS(uv,offset);
  imageStore(resultImage,p,SMAANeighborhoodBlendingPS(uv,offset,colorTex,blendTex));
)glsl";
    source += "}\n";
    std::vector<uint32_t> spirv; std::string error;
    if (!CompileFh1Compute(source,spirv,error)) {
      REXGPU_ERROR("SMAA pass {}: {}",pass,error); return false;
    }
    pipelines[pass] = ui::vulkan::util::CreateComputePipeline(device,layout,
        spirv.data(),spirv.size()*sizeof(uint32_t));
    return pipelines[pass] != VK_NULL_HANDLE;
  }
};
Fh1Smaa::Fh1Smaa(const ui::vulkan::VulkanDevice* device) : state_(std::make_unique<State>()) {
  state_->device = device;
}
Fh1Smaa::~Fh1Smaa() {
  auto& s=*state_; const auto& f=s.device->functions(); const auto d=s.device->device();
  for (auto pipeline:s.pipelines) if (pipeline) f.vkDestroyPipeline(d,pipeline,nullptr);
  if (s.pool) f.vkDestroyDescriptorPool(d,s.pool,nullptr);
  if (s.layout) f.vkDestroyPipelineLayout(d,s.layout,nullptr);
  if (s.set_layout) f.vkDestroyDescriptorSetLayout(d,s.set_layout,nullptr);
  if (s.sampler) f.vkDestroySampler(d,s.sampler,nullptr);
  if (s.upload) f.vkDestroyBuffer(d,s.upload,nullptr);
  if (s.upload_memory) f.vkFreeMemory(d,s.upload_memory,nullptr);
  for (auto& image:s.images) {
    if (image.view) f.vkDestroyImageView(d,image.view,nullptr);
    if (image.image) f.vkDestroyImage(d,image.image,nullptr);
    if (image.memory) f.vkFreeMemory(d,image.memory,nullptr);
  }
}
bool Fh1Smaa::Initialize(uint32_t width,uint32_t height,VkImageView color,VkImageView output) {
  auto& s=*state_; s.width=width;s.height=height;
  const auto& f=s.device->functions(); const auto d=s.device->device();
  if (!s.Create(s.images[0],width,height,VK_FORMAT_R8G8B8A8_UNORM,true) ||
      !s.Create(s.images[1],width,height,VK_FORMAT_R8G8B8A8_UNORM,true) ||
      !s.Create(s.images[2],AREATEX_WIDTH,AREATEX_HEIGHT,VK_FORMAT_R8G8_UNORM,false) ||
      !s.Create(s.images[3],SEARCHTEX_WIDTH,SEARCHTEX_HEIGHT,VK_FORMAT_R8_UNORM,false)) return false;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(s.device,AREATEX_SIZE+SEARCHTEX_SIZE,
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,ui::vulkan::util::MemoryPurpose::kUpload,
      s.upload,s.upload_memory)) return false;
  void* mapped=nullptr;
  if (f.vkMapMemory(d,s.upload_memory,0,VK_WHOLE_SIZE,0,&mapped)!=VK_SUCCESS) return false;
  std::memcpy(mapped,areaTexBytes,AREATEX_SIZE);
  std::memcpy(static_cast<unsigned char*>(mapped)+AREATEX_SIZE,searchTexBytes,SEARCHTEX_SIZE);
  // Upload memory may be non-coherent on a discrete GPU.
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory=s.upload_memory;range.size=VK_WHOLE_SIZE;
  const auto flushed=f.vkFlushMappedMemoryRanges(d,1,&range);
  f.vkUnmapMemory(d,s.upload_memory);
  if (flushed!=VK_SUCCESS) return false;
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
  sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (f.vkCreateSampler(d,&sampler,nullptr,&s.sampler)!=VK_SUCCESS) return false;
  std::array<VkDescriptorSetLayoutBinding,6> bindings{};
  for (unsigned i=0;i<6;++i) bindings[i]={i,i==5?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
  VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set.bindingCount=uint32_t(bindings.size());set.pBindings=bindings.data();
  if (f.vkCreateDescriptorSetLayout(d,&set,nullptr,&s.set_layout)!=VK_SUCCESS) return false;
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,16};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount=1;layout.pSetLayouts=&s.set_layout;
  layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&push;
  if (f.vkCreatePipelineLayout(d,&layout,nullptr,&s.layout)!=VK_SUCCESS) return false;
  const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,15},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets=3;pool.poolSizeCount=2;pool.pPoolSizes=sizes;
  if (f.vkCreateDescriptorPool(d,&pool,nullptr,&s.pool)!=VK_SUCCESS) return false;
  const VkDescriptorSetLayout layouts[]={s.set_layout,s.set_layout,s.set_layout};
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool=s.pool;alloc.descriptorSetCount=3;alloc.pSetLayouts=layouts;
  if (f.vkAllocateDescriptorSets(d,&alloc,s.sets.data())!=VK_SUCCESS) return false;
  // Sets are immutable after initialization. Concurrent frames don't update
  // descriptors that may still be referenced by an earlier submission.
  for (unsigned pass=0;pass<3;++pass) {
    const VkImageView views[]={color,s.images[0].view,s.images[1].view,
                              s.images[2].view,s.images[3].view,
                              pass==0?s.images[0].view:pass==1?s.images[1].view:output};
    std::array<VkDescriptorImageInfo,6> info{};
    std::array<VkWriteDescriptorSet,6> writes{};
    for (unsigned i=0;i<6;++i) {
      info[i]={i==5?VK_NULL_HANDLE:s.sampler,views[i],VK_IMAGE_LAYOUT_GENERAL};
      writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet=s.sets[pass];writes[i].dstBinding=i;writes[i].descriptorCount=1;
      writes[i].descriptorType=bindings[i].descriptorType;writes[i].pImageInfo=&info[i];
    }
    f.vkUpdateDescriptorSets(d,uint32_t(writes.size()),writes.data(),0,nullptr);
    if (!s.Pipeline(pass)) return false;
  }
  return true;
}
void Fh1Smaa::Execute(VkCommandBuffer command) {
  auto& s=*state_;const auto& f=s.device->functions();
  if (s.first) {
    for (auto& image:s.images) s.Barrier(command,image.image,VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
    for (unsigned i=2;i<4;++i) {
      VkBufferImageCopy copy{};copy.bufferOffset=i==2?0:AREATEX_SIZE;
      copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
      copy.imageExtent=i==2?VkExtent3D{AREATEX_WIDTH,AREATEX_HEIGHT,1}:VkExtent3D{SEARCHTEX_WIDTH,SEARCHTEX_HEIGHT,1};
      f.vkCmdCopyBufferToImage(command,s.upload,s.images[i].image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
      s.Barrier(command,s.images[i].image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    }
    s.first=false;
  } else {
    for (unsigned i=0;i<2;++i) s.Barrier(command,s.images[i].image,VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_SHADER_WRITE_BIT);
  }
  const float metrics[]={1.0f/s.width,1.0f/s.height,float(s.width),float(s.height)};
  for (unsigned pass=0;pass<3;++pass) {
    f.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,s.pipelines[pass]);
    f.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,s.layout,0,1,&s.sets[pass],0,nullptr);
    f.vkCmdPushConstants(command,s.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(metrics),metrics);
    f.vkCmdDispatch(command,(s.width+7)/8,(s.height+7)/8,1);
    if (pass<2) s.Barrier(command,s.images[pass].image,VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
  }
}
}
