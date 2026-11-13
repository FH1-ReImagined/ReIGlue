#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/ui/graphics_upload_buffer_pool.h>
#include <rex/ui/vulkan/device.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanUploadBufferPool : public GraphicsUploadBufferPool {
 public:
  // preferred_memory_types (FH1 #858): memory types tried before the usual host-visible
  // choice (device-local ones the CPU writes directly, with Resizable BAR); 0 for the usual
  // choice only. They must be host-coherent: when a page cannot be allocated in them, the pool
  // goes on in the usual type, and its earlier pages are not flushed.
  VulkanUploadBufferPool(const VulkanDevice* vulkan_device, VkBufferUsageFlags usage,
                         size_t page_size = kDefaultPageSize, uint32_t preferred_memory_types = 0);

  uint8_t* Request(uint64_t submission_index, size_t size, size_t alignment, VkBuffer& buffer_out,
                   VkDeviceSize& offset_out);
  uint8_t* RequestPartial(uint64_t submission_index, size_t size, size_t alignment,
                          VkBuffer& buffer_out, VkDeviceSize& offset_out, VkDeviceSize& size_out);

 protected:
  Page* CreatePageImplementation() override;

  void FlushPageWrites(Page* page, size_t offset, size_t size) override;

 private:
  struct VulkanPage : public Page {
    // Takes ownership of the buffer and its memory and mapping.
    VulkanPage(const VulkanDevice* vulkan_device, VkBuffer buffer, VkDeviceMemory memory,
               void* mapping)
        : vulkan_device_(vulkan_device), buffer_(buffer), memory_(memory), mapping_(mapping) {}
    ~VulkanPage() override;
    const VulkanDevice* vulkan_device_;
    VkBuffer buffer_;
    VkDeviceMemory memory_;
    void* mapping_;
  };

  const VulkanDevice* vulkan_device_;

  VkDeviceSize allocation_size_;
  static constexpr uint32_t kMemoryTypeUnknown = UINT32_MAX;
  static constexpr uint32_t kMemoryTypeUnavailable = kMemoryTypeUnknown - 1;
  uint32_t memory_type_ = UINT32_MAX;
  // FH1 (#858): the usual host-visible type when memory_type_ is a preferred one (the fallback
  // when a page cannot be allocated there), else UINT32_MAX.
  uint32_t fallback_memory_type_ = UINT32_MAX;

  VkBufferUsageFlags usage_;
  uint32_t preferred_memory_types_;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
