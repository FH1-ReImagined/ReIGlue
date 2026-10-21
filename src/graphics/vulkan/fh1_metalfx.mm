#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <rex/graphics/vulkan/fh1_metalfx.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>

namespace rex::graphics::vulkan {
struct Fh1MetalFx::State {
  const ui::vulkan::VulkanDevice* vulkan = nullptr;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLSharedEvent> ready_event = nil, complete_event = nil;
  id<MTLFXTemporalScaler> temporal = nil;
  id<MTLFXSpatialScaler> spatial = nil;
  std::array<id<MTLTexture>, 4> textures{};
  VkSemaphore ready = VK_NULL_HANDLE, complete = VK_NULL_HANDLE;
  uint64_t sequence = 0;
  struct Statistics {
    std::atomic<uint64_t> completed{0};
    std::atomic<bool> failed{false};
  };
  std::shared_ptr<Statistics> statistics = std::make_shared<Statistics>();
  ~State() {
    [temporal release];
    [spatial release];
    for (auto texture : textures)
      [texture release];
    [queue release];
    [ready_event release];
    [complete_event release];
  }
  bool ImportEvent(id<MTLSharedEvent> event, VkSemaphore& semaphore) {
    VkImportMetalSharedEventInfoEXT metal{VK_STRUCTURE_TYPE_IMPORT_METAL_SHARED_EVENT_INFO_EXT};
    metal.mtlSharedEvent = event;
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.pNext = &metal;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    info.pNext = &type;
    return vulkan->functions().vkCreateSemaphore(vulkan->device(), &info, nullptr, &semaphore) ==
           VK_SUCCESS;
  }
};
Fh1MetalFx::Fh1MetalFx(const ui::vulkan::VulkanDevice* device) : state_(std::make_unique<State>()) {
  state_->vulkan = device;
}
Fh1MetalFx::~Fh1MetalFx() {
  auto& s = *state_;
  const auto& f = s.vulkan->functions();
  if (s.ready)
    f.vkDestroySemaphore(s.vulkan->device(), s.ready, nullptr);
  if (s.complete)
    f.vkDestroySemaphore(s.vulkan->device(), s.complete, nullptr);
}
bool Fh1MetalFx::Initialize(uint32_t width, uint32_t height, uint32_t ow, uint32_t oh,
                            VkImage color, VkImage output, VkImage depth, VkImage motion,
                            bool temporal) {
  @autoreleasepool {
    auto& s = *state_;
    if (@available(macOS 13.0, *)) {
      // Scaler support is checked below on the actual Metal device.
    } else {
      REXGPU_ERROR("MetalFX requires macOS 13 or newer");
      return false;
    }
    auto export_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
        s.vulkan->vulkan_instance()->functions().vkGetDeviceProcAddr(s.vulkan->device(),
                                                                     "vkExportMetalObjectsEXT"));
    if (!export_objects) {
      REXGPU_ERROR("MetalFX: VK_EXT_metal_objects unavailable");
      return false;
    }
    const VkImage images[] = {color, output, depth, motion};
    for (size_t i = 0; i < 4; ++i) {
      VkExportMetalTextureInfoEXT texture{VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT};
      texture.image = images[i];
      texture.plane = VK_IMAGE_ASPECT_COLOR_BIT;
      VkExportMetalObjectsInfoEXT info{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
      info.pNext = &texture;
      export_objects(s.vulkan->device(), &info);
      s.textures[i] = [texture.mtlTexture retain];
      if (!s.textures[i]) {
        REXGPU_ERROR("MetalFX: texture {} export failed", i);
        return false;
      }
    }
    s.device = s.textures[0].device;
    s.queue = [s.device newCommandQueue];
    s.queue.label = @"Forza MetalFX";
    s.ready_event = [s.device newSharedEvent];
    s.complete_event = [s.device newSharedEvent];
    if (!s.queue || !s.ready_event || !s.complete_event || !s.ImportEvent(s.ready_event, s.ready) ||
        !s.ImportEvent(s.complete_event, s.complete)) {
      REXGPU_ERROR("MetalFX: could not create shared GPU timeline events");
      return false;
    }
    if (temporal) {
      if (![MTLFXTemporalScalerDescriptor supportsDevice:s.device])
        return false;
      auto d = [MTLFXTemporalScalerDescriptor new];
      d.inputWidth = width;
      d.inputHeight = height;
      d.outputWidth = ow;
      d.outputHeight = oh;
      d.colorTextureFormat = s.textures[0].pixelFormat;
      d.outputTextureFormat = s.textures[1].pixelFormat;
      d.depthTextureFormat = s.textures[2].pixelFormat;
      d.motionTextureFormat = s.textures[3].pixelFormat;
      d.autoExposureEnabled = YES;
      d.requiresSynchronousInitialization = YES;
      s.temporal = [d newTemporalScalerWithDevice:s.device];
      [d release];
      if (!s.temporal) {
        REXGPU_ERROR("MetalFX: temporal scaler creation failed");
        return false;
      }
      s.temporal.colorTexture = s.textures[0];
      s.temporal.outputTexture = s.textures[1];
      s.temporal.depthTexture = s.textures[2];
      s.temporal.motionTexture = s.textures[3];
      s.temporal.inputContentWidth = width;
      s.temporal.inputContentHeight = height;
      s.temporal.depthReversed = YES;
      s.temporal.preExposure = 1.0f;
      s.temporal.motionVectorScaleX = 1.0f;
      s.temporal.motionVectorScaleY = 1.0f;
      const MTLTextureUsage usage[] = {s.temporal.colorTextureUsage, s.temporal.outputTextureUsage,
                                       s.temporal.depthTextureUsage, s.temporal.motionTextureUsage};
      for (size_t i = 0; i < 4; ++i)
        if ((s.textures[i].usage & usage[i]) != usage[i]) {
          REXGPU_ERROR("MetalFX: texture {} usage {} does not include {}", i,
                       uint32_t(s.textures[i].usage), uint32_t(usage[i]));
          return false;
        }
    } else {
      if (![MTLFXSpatialScalerDescriptor supportsDevice:s.device])
        return false;
      auto d = [MTLFXSpatialScalerDescriptor new];
      d.inputWidth = width;
      d.inputHeight = height;
      d.outputWidth = ow;
      d.outputHeight = oh;
      d.colorTextureFormat = s.textures[0].pixelFormat;
      d.outputTextureFormat = s.textures[1].pixelFormat;
      d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModeLinear;
      s.spatial = [d newSpatialScalerWithDevice:s.device];
      [d release];
      if (!s.spatial)
        return false;
      s.spatial.colorTexture = s.textures[0];
      s.spatial.outputTexture = s.textures[1];
      s.spatial.inputContentWidth = width;
      s.spatial.inputContentHeight = height;
    }
    REXGPU_INFO("MetalFX: {} {}x{} -> {}x{} on {}, shared textures and GPU timeline events; no CPU "
                "readback",
                temporal ? "temporal" : "spatial", width, height, ow, oh, s.device.name.UTF8String);
    return true;
  }
}
Fh1Dlss::NativeSubmission Fh1MetalFx::Schedule(const Frame& frame) {
  auto& s = *state_;
  Fh1Dlss::NativeSubmission submission;
  submission.ready = s.ready;
  submission.complete = s.complete;
  submission.value = ++s.sequence;
  const uint64_t value = submission.value;
  submission.encode = [this, frame, value] {
    @autoreleasepool {
      auto& s = *state_;
      if (s.statistics->failed.load())
        return false;
      auto buffer = [s.queue commandBuffer];
      if (!buffer)
        return false;
      buffer.label = @"Forza MetalFX upscale";
      [buffer encodeWaitForEvent:s.ready_event value:value];
      if (s.temporal) {
        s.temporal.jitterOffsetX = frame.jitter_x;
        s.temporal.jitterOffsetY = frame.jitter_y;
        s.temporal.reset = frame.reset;
        [s.temporal encodeToCommandBuffer:buffer];
      } else
        [s.spatial encodeToCommandBuffer:buffer];
      [buffer encodeSignalEvent:s.complete_event value:value];
      const auto statistics = s.statistics;
      id<MTLSharedEvent> complete_event = s.complete_event;
      [buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        if (completed.status == MTLCommandBufferStatusError) {
          REXGPU_ERROR("MetalFX: GPU command failed: {}", completed.error.description.UTF8String);
          statistics->failed.store(true);
          // Release dependent work on failure too; otherwise the Vulkan queue hangs.
          complete_event.signaledValue = std::max(complete_event.signaledValue, value);
        } else {
          const auto n = statistics->completed.fetch_add(1) + 1;
          if (n == 1 || n % 300 == 0)
            REXGPU_INFO("MetalFX: completed {} upscales; main submission GPU interval {:.3f} ms "
                        "(excludes internal passes)",
                        n, (completed.GPUEndTime - completed.GPUStartTime) * 1000.0);
        }
      }];
      [buffer commit];
      return true;
    }
  };
  return submission;
}
} // namespace rex::graphics::vulkan
