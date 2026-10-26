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

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include <rex/graphics/shared_memory.h>
#include <rex/memory.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

class VulkanSharedMemory : public SharedMemory {
 public:
  VulkanSharedMemory(VulkanCommandProcessor& command_processor, memory::Memory& memory,
                     VkPipelineStageFlags guest_shader_pipeline_stages);
  ~VulkanSharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  void CompletedSubmissionUpdated();
  void EndSubmission();

  enum class Usage {
    // Index buffer, vfetch, compute read, transfer source.
    kRead,
    // Index buffer, vfetch, memexport.
    kGuestDrawReadWrite,
    kComputeWrite,
    kTransferDestination,
  };
  // Inserts a pipeline barrier for the target usage, also ensuring consecutive
  // read-write accesses are ordered with each other.
  void Use(Usage usage, std::pair<uint32_t, uint32_t> written_range = {});

  VkBuffer buffer() const { return buffer_; }

  using SharedMemory::page_size_log2;

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;

  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;
  bool Fh1UploadData(uint32_t start, const uint8_t* data, uint32_t length) override;  // patch 0396

 private:
  void GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                     VkAccessFlags& access_mask) const;
  // A barrier on the whole buffer, written into the deferred command buffer directly.
  void WriteUsageBarrier(Usage from, Usage to);

  VulkanCommandProcessor& command_processor_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  VkBuffer buffer_ = VK_NULL_HANDLE;
  uint32_t buffer_memory_type_;
  // Single for non-sparse, every allocation so far for sparse.
  std::vector<VkDeviceMemory> buffer_memory_;

  Usage last_usage_;
  std::pair<uint32_t, uint32_t> last_written_range_;

  // Uploads hoisted before a render pass (VulkanCommandProcessor::CanHoistSharedMemoryUpload)
  // share one barrier before their copies and one after: the render pass they were hoisted
  // before, and where its closing barrier is in the hoisted command stream (dropped and
  // written again after the next upload's copies).
  uint64_t hoist_render_pass_serial_ = 0;
  size_t hoist_closing_barrier_offset_ = 0;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> upload_buffer_pool_;
  std::vector<VkBufferCopy> upload_regions_;
};

}  // namespace rex::graphics::vulkan
