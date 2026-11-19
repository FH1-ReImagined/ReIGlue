#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <memory>
#include <array>
#include <span>
#include <utility>

#include <rex/assert.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

class VulkanPrimitiveProcessor final : public PrimitiveProcessor {
 public:
  VulkanPrimitiveProcessor(const RegisterFile& register_file, memory::Memory& memory,
                           SharedMemory& shared_memory, VulkanCommandProcessor& command_processor)
      : PrimitiveProcessor(register_file, memory, shared_memory),
        command_processor_(command_processor) {}
  ~VulkanPrimitiveProcessor();

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() { frame_index_buffer_pool_->ClearCache(); }

  void CompletedSubmissionUpdated();
  void BeginSubmission();
  void BeginFrame();
  void EndSubmission();
  void EndFrame();

  std::pair<VkBuffer, VkDeviceSize> GetBuiltinIndexBuffer(size_t handle) const {
    assert_not_null(builtin_index_buffer_);
    return std::make_pair(builtin_index_buffer_,
                          VkDeviceSize(GetBuiltinIndexBufferOffsetBytes(handle)));
  }
  std::pair<VkBuffer, VkDeviceSize> GetConvertedIndexBuffer(size_t handle) const {
    return frame_index_buffers_[handle];
  }
  // Allocate in the same frame-owned pool used for converted draws. The
  // caller supplies the final bytes for the index format it will bind.
  std::pair<VkBuffer, VkDeviceSize> StageCapturedIndex(std::span<const std::byte> bytes,
                                                      uint32_t width);
  void InitializeReplayVertexBuffer();
  VkBuffer replay_vertex_buffer() const { return replay_vertex_buffer_; }
  void InitializeReplayIndexBuffer();
  VkBuffer replay_index_buffer() const { return replay_index_buffer_; }
  struct ReplayPixelPlane {
    VkImageAspectFlags aspect;
    uint32_t pixel_bytes;
    VkDeviceSize offset;
    bool depth24;
  };
  struct ReplayPixelReadback {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t submission = 0;
    uint32_t memory_type = 0;
    VkRect2D rect{};
    std::array<ReplayPixelPlane, 3> planes{};
    uint32_t plane_count = 0;
    VkDeviceSize stride = 0;
    bool complete = false;
  };
  ReplayPixelReadback* AllocateReplayPixelReadback(
      VkRect2D rect, VkFormat depth_format, VkFormat color_format);
  bool CanSampleReplayPixels(bool color) const { return replay_pixel_samples_[color] < 4; }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;

  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  VulkanCommandProcessor& command_processor_;

  VkDeviceSize builtin_index_buffer_size_ = 0;
  VkBuffer builtin_index_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory builtin_index_buffer_memory_ = VK_NULL_HANDLE;
  // Temporary buffer copied in the beginning of the first submission for
  // uploading to builtin_index_buffer_, destroyed when the submission when it
  // was uploaded is completed.
  VkBuffer builtin_index_buffer_upload_ = VK_NULL_HANDLE;
  VkDeviceMemory builtin_index_buffer_upload_memory_ = VK_NULL_HANDLE;
  // UINT64_MAX means not uploaded yet and needs uploading in the first
  // submission (if the upload buffer exists at all).
  uint64_t builtin_index_buffer_upload_submission_ = UINT64_MAX;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> frame_index_buffer_pool_;
  // Indexed by the backend handles.
  std::deque<std::pair<VkBuffer, VkDeviceSize>> frame_index_buffers_;
  VkBuffer replay_vertex_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory replay_vertex_memory_ = VK_NULL_HANDLE;
  VkBuffer replay_index_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory replay_index_memory_ = VK_NULL_HANDLE;
  void LandReplayPixelReadbacks(uint64_t completed, bool discard = false);
  std::array<ReplayPixelReadback, 8> replay_pixel_readbacks_{};
  uint32_t replay_pixel_samples_[2]{};
};

}  // namespace rex::graphics::vulkan
