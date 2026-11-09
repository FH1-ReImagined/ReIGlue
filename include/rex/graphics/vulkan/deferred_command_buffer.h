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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/assert.h>
#include <rex/math.h>
#include <rex/ui/vulkan/api.h>
#include <rex/graphics/vulkan/fh1_dlss.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;
class Fh1Dlss;
class Fh1GpuProfile;

class DeferredCommandBuffer {
 public:
  DeferredCommandBuffer(const VulkanCommandProcessor& command_processor,
                        size_t initial_size_bytes = 1024 * 1024);

  void Reset();
  void CmdFh1Dlss(Fh1Dlss* effect, VkImage src, VkImage dst) {
    auto& args = *reinterpret_cast<ArgsFh1Dlss*>(
        WriteCommand(Command::kFh1Dlss, sizeof(ArgsFh1Dlss)));
    args.effect = effect; args.src = src; args.dst = dst; args.input = effect->input();
    native_upscale_ |= effect->UsesMetalFx();
  }
  mutable PFN_vkCmdBlitImage blit_image_ = nullptr;
  void CmdVkBlitImage(VkImage src, VkImage dst, const VkImageBlit& region) {
    auto& args = *reinterpret_cast<ArgsVkBlitImage*>(
        WriteCommand(Command::kVkBlitImage, sizeof(ArgsVkBlitImage)));
    args.src = src; args.dst = dst; args.region = region;
  }
  // A square cube view is populated from an integer-stride 2D-array resolve.
  void CmdFh1NativeCubeBlit(VkImage src, VkImage dst, const VkImageBlit& region) {
    auto& args = *reinterpret_cast<ArgsFh1NativeCubeBlit*>(
        WriteCommand(Command::kFh1NativeCubeBlit, sizeof(ArgsFh1NativeCubeBlit)));
    args.src = src;
    args.dst = dst;
    args.region = region;
  }
  void Execute(VkCommandBuffer command_buffer);

  // FH1 (#551): after a guest draw or resolve, its number in the device-lost trail
  // (fh1_device_lost_trail.h): written by the GPU to `buffer` (offset 4) once finished with
  // VK_AMD_buffer_marker, or a checkpoint holding it (VK_NV_device_diagnostic_checkpoints,
  // reported per pipeline stage: also when issued).
  struct Fh1Breadcrumbs {
    PFN_vkCmdWriteBufferMarkerAMD write_buffer_marker = nullptr;
    PFN_vkCmdSetCheckpointNV set_checkpoint = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    // Test hook (FORZA_TEST_GPU_HANG_AT_FRAME): an event never set, waited for.
    VkEvent hang_event = VK_NULL_HANDLE;
    PFN_vkCmdWaitEvents wait_events = nullptr;
  };
  void SetFh1Breadcrumbs(const Fh1Breadcrumbs* breadcrumbs) { fh1_breadcrumbs_ = breadcrumbs; }
  void CmdFh1Breadcrumb(uint32_t seq) {
    auto& args = *reinterpret_cast<ArgsFh1Breadcrumb*>(
        WriteCommand(Command::kFh1Breadcrumb, sizeof(ArgsFh1Breadcrumb)));
    args.seq = seq;
  }
  // Test hook: makes the GPU wait forever (the driver then resets it: a real device loss).
  void CmdFh1TestHang() { WriteCommand(Command::kFh1TestHang, 0); }

  // FH1 (#640): GPU time per kind of work (fh1_gpu_profile.h). The zone is what the commands
  // recorded from now on are for; with a profile, a timestamp goes before the first command
  // written to the main stream in a new zone. Returns the previous zone.
  void SetFh1GpuProfile(Fh1GpuProfile* profile) {
    fh1_gpu_profile_ = profile;
    fh1_gpu_zone_pending_ = false;
  }
  uint16_t SetFh1GpuZone(uint16_t zone) {
    const uint16_t previous = fh1_gpu_zone_;
    fh1_gpu_zone_ = zone;
    // A zone left without commands of its own back to the marked one needs no timestamp.
    fh1_gpu_zone_pending_ = fh1_gpu_profile_ != nullptr && zone != fh1_gpu_zone_marked_;
    return previous;
  }
  // The profile wrote a timestamp starting `zone`.
  void Fh1GpuZoneMarked(uint16_t zone) {
    fh1_gpu_zone_marked_ = zone;
    fh1_gpu_zone_pending_ = false;
  }
  uint16_t fh1_gpu_zone() const { return fh1_gpu_zone_; }
  Fh1GpuProfile* fh1_gpu_profile() const { return fh1_gpu_profile_; }
  bool fh1_gpu_profiling() const { return fh1_gpu_profile_ != nullptr; }
  void CmdFh1Timestamp(PFN_vkCmdWriteTimestamp write, VkQueryPool pool, uint32_t query) {
    auto& args = *reinterpret_cast<ArgsFh1Timestamp*>(
        WriteCommand(Command::kFh1Timestamp, sizeof(ArgsFh1Timestamp)));
    args.write = write;
    args.pool = pool;
    args.query = query;
  }

  // The recorded commands, taken out for another thread to execute (threaded submission).
  struct Streams {
    std::vector<uintmax_t> main;
    std::vector<uintmax_t> hoisted;
    bool native_upscale = false;
  };
  // Swaps the recorded commands with `streams` (normally empty ones, whose allocations are
  // reused) and starts recording anew.
  void TakeStreams(Streams& streams) {
    command_stream_.swap(streams.main);
    hoisted_stream_.swap(streams.hoisted);
    streams.native_upscale = native_upscale_;
    Reset();
  }
  // Executes commands taken with TakeStreams. Touches nothing else in the deferred command
  // buffer, so it may run on another thread while new commands are recorded.
  VkCommandBuffer ExecuteStreams(VkCommandBuffer command_buffer, const Streams& streams,
                                 const Fh1Dlss::SplitSubmission& split = {}) const {
    return ExecuteStream(command_buffer, streams.main.data(), streams.main.size(),
                         streams.hoisted.data(), split);
  }

  // Hoisting: commands recorded later but executed earlier, at a hoist point written into the
  // stream before (the shared memory uploads that VulkanSharedMemory moves before the current
  // render pass instead of ending it). The hoisted commands go to a second stream; a hoist
  // point covers a contiguous range of it, which Execute replays where the hoist point is.
  // Writes a hoist point covering nothing yet, and returns its handle.
  size_t CmdHoistPoint();
  // Directs the commands written from now on to the hoisted stream (true) or the main one.
  void SetWritingHoisted(bool hoisted) {
    write_stream_ = hoisted ? &hoisted_stream_ : &command_stream_;
  }
  size_t hoisted_stream_size() const { return hoisted_stream_.size(); }
  // FH1 (#45): the size of the stream commands are written to now.
  size_t write_stream_size() const { return write_stream_->size(); }
  // Drops the hoisted commands from `size` on (only those of the latest hoist point).
  void TruncateHoistedStream(size_t size) {
    assert_true(size <= hoisted_stream_.size());
    hoisted_stream_.resize(size);
  }
  // Makes the hoist point cover the hoisted stream from where it was at CmdHoistPoint to its
  // current end.
  void ExtendHoistPoint(size_t hoist_point);

  // render_pass_begin->pNext of all barriers must be null.
  void CmdVkBeginRenderPass(const VkRenderPassBeginInfo* render_pass_begin,
                            VkSubpassContents contents) {
    assert_null(render_pass_begin->pNext);
    size_t arguments_size = sizeof(ArgsVkBeginRenderPass);
    uint32_t clear_value_count = render_pass_begin->clearValueCount;
    size_t clear_values_offset = 0;
    if (clear_value_count) {
      arguments_size = rex::align(arguments_size, alignof(VkClearValue));
      clear_values_offset = arguments_size;
      arguments_size += sizeof(VkClearValue) * clear_value_count;
    }
    uint8_t* args_ptr =
        reinterpret_cast<uint8_t*>(WriteCommand(Command::kVkBeginRenderPass, arguments_size));
    auto& args = *reinterpret_cast<ArgsVkBeginRenderPass*>(args_ptr);
    args.render_pass = render_pass_begin->renderPass;
    args.framebuffer = render_pass_begin->framebuffer;
    args.render_area = render_pass_begin->renderArea;
    args.clear_value_count = clear_value_count;
    args.contents = contents;
    if (clear_value_count) {
      std::memcpy(args_ptr + clear_values_offset, render_pass_begin->pClearValues,
                  sizeof(VkClearValue) * clear_value_count);
    }
  }

  void CmdVkBindDescriptorSets(VkPipelineBindPoint pipeline_bind_point, VkPipelineLayout layout,
                               uint32_t first_set, uint32_t descriptor_set_count,
                               const VkDescriptorSet* descriptor_sets,
                               uint32_t dynamic_offset_count, const uint32_t* dynamic_offsets) {
    size_t arguments_size = rex::align(sizeof(ArgsVkBindDescriptorSets), alignof(VkDescriptorSet));
    size_t descriptor_sets_offset = arguments_size;
    arguments_size += sizeof(VkDescriptorSet) * descriptor_set_count;
    size_t dynamic_offsets_offset = 0;
    if (dynamic_offset_count) {
      arguments_size = rex::align(arguments_size, alignof(uint32_t));
      dynamic_offsets_offset = arguments_size;
      arguments_size += sizeof(uint32_t) * dynamic_offset_count;
    }
    uint8_t* args_ptr =
        reinterpret_cast<uint8_t*>(WriteCommand(Command::kVkBindDescriptorSets, arguments_size));
    auto& args = *reinterpret_cast<ArgsVkBindDescriptorSets*>(args_ptr);
    args.pipeline_bind_point = pipeline_bind_point;
    args.layout = layout;
    args.first_set = first_set;
    args.descriptor_set_count = descriptor_set_count;
    args.dynamic_offset_count = dynamic_offset_count;
    std::memcpy(args_ptr + descriptor_sets_offset, descriptor_sets,
                sizeof(VkDescriptorSet) * descriptor_set_count);
    if (dynamic_offset_count) {
      std::memcpy(args_ptr + dynamic_offsets_offset, dynamic_offsets,
                  sizeof(uint32_t) * dynamic_offset_count);
    }
  }

  void CmdVkBindIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType index_type) {
    auto& args = *reinterpret_cast<ArgsVkBindIndexBuffer*>(
        WriteCommand(Command::kVkBindIndexBuffer, sizeof(ArgsVkBindIndexBuffer)));
    args.buffer = buffer;
    args.offset = offset;
    args.index_type = index_type;
  }

  void CmdVkBindPipeline(VkPipelineBindPoint pipeline_bind_point, VkPipeline pipeline) {
    auto& args = *reinterpret_cast<ArgsVkBindPipeline*>(
        WriteCommand(Command::kVkBindPipeline, sizeof(ArgsVkBindPipeline)));
    args.pipeline_bind_point = pipeline_bind_point;
    args.pipeline = pipeline;
  }

  void CmdVkBindVertexBuffers(uint32_t first_binding, uint32_t binding_count,
                              const VkBuffer* buffers, const VkDeviceSize* offsets) {
    size_t arguments_size = rex::align(sizeof(ArgsVkBindVertexBuffers), alignof(VkBuffer));
    size_t buffers_offset = arguments_size;
    arguments_size =
        rex::align(arguments_size + sizeof(VkBuffer) * binding_count, alignof(VkDeviceSize));
    size_t offsets_offset = arguments_size;
    arguments_size += sizeof(VkDeviceSize) * binding_count;
    uint8_t* args_ptr =
        reinterpret_cast<uint8_t*>(WriteCommand(Command::kVkBindVertexBuffers, arguments_size));
    auto& args = *reinterpret_cast<ArgsVkBindVertexBuffers*>(args_ptr);
    args.first_binding = first_binding;
    args.binding_count = binding_count;
    std::memcpy(args_ptr + buffers_offset, buffers, sizeof(VkBuffer) * binding_count);
    std::memcpy(args_ptr + offsets_offset, offsets, sizeof(VkDeviceSize) * binding_count);
  }

  void CmdClearAttachmentsEmplace(uint32_t attachment_count, VkClearAttachment*& attachments_out,
                                  uint32_t rect_count, VkClearRect*& rects_out) {
    size_t arguments_size = rex::align(sizeof(ArgsVkClearAttachments), alignof(VkClearAttachment));
    size_t attachments_offset = arguments_size;
    arguments_size = rex::align(arguments_size + sizeof(VkClearAttachment) * attachment_count,
                                alignof(VkClearRect));
    size_t rects_offset = arguments_size;
    arguments_size += sizeof(VkClearRect) * rect_count;
    uint8_t* args_ptr =
        reinterpret_cast<uint8_t*>(WriteCommand(Command::kVkClearAttachments, arguments_size));
    auto& args = *reinterpret_cast<ArgsVkClearAttachments*>(args_ptr);
    args.attachment_count = attachment_count;
    args.rect_count = rect_count;
    attachments_out = reinterpret_cast<VkClearAttachment*>(args_ptr + attachments_offset);
    rects_out = reinterpret_cast<VkClearRect*>(args_ptr + rects_offset);
  }
  void CmdVkClearAttachments(uint32_t attachment_count, const VkClearAttachment* attachments,
                             uint32_t rect_count, const VkClearRect* rects) {
    VkClearAttachment* attachments_arg;
    VkClearRect* rects_arg;
    CmdClearAttachmentsEmplace(attachment_count, attachments_arg, rect_count, rects_arg);
    std::memcpy(attachments_arg, attachments, sizeof(VkClearAttachment) * attachment_count);
    std::memcpy(rects_arg, rects, sizeof(VkClearRect) * rect_count);
  }

  VkImageSubresourceRange* CmdClearColorImageEmplace(VkImage image, VkImageLayout image_layout,
                                                     const VkClearColorValue* color,
                                                     uint32_t range_count) {
    const size_t header_size =
        rex::align(sizeof(ArgsVkClearColorImage), alignof(VkImageSubresourceRange));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(WriteCommand(
        Command::kVkClearColorImage, header_size + sizeof(VkImageSubresourceRange) * range_count));
    auto& args = *reinterpret_cast<ArgsVkClearColorImage*>(args_ptr);
    args.image = image;
    args.image_layout = image_layout;
    args.color = *color;
    args.range_count = range_count;
    return reinterpret_cast<VkImageSubresourceRange*>(args_ptr + header_size);
  }
  void CmdVkClearColorImage(VkImage image, VkImageLayout image_layout,
                            const VkClearColorValue* color, uint32_t range_count,
                            const VkImageSubresourceRange* ranges) {
    std::memcpy(CmdClearColorImageEmplace(image, image_layout, color, range_count), ranges,
                sizeof(VkImageSubresourceRange) * range_count);
  }

  VkBufferCopy* CmdCopyBufferEmplace(VkBuffer src_buffer, VkBuffer dst_buffer,
                                     uint32_t region_count) {
    const size_t header_size = rex::align(sizeof(ArgsVkCopyBuffer), alignof(VkBufferCopy));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkCopyBuffer, header_size + sizeof(VkBufferCopy) * region_count));
    auto& args = *reinterpret_cast<ArgsVkCopyBuffer*>(args_ptr);
    args.src_buffer = src_buffer;
    args.dst_buffer = dst_buffer;
    args.region_count = region_count;
    return reinterpret_cast<VkBufferCopy*>(args_ptr + header_size);
  }
  void CmdVkCopyBuffer(VkBuffer src_buffer, VkBuffer dst_buffer, uint32_t region_count,
                       const VkBufferCopy* regions) {
    std::memcpy(CmdCopyBufferEmplace(src_buffer, dst_buffer, region_count), regions,
                sizeof(VkBufferCopy) * region_count);
  }

  void CmdVkBeginQuery(VkQueryPool query_pool, uint32_t query, VkQueryControlFlags flags) {
    auto& args = *reinterpret_cast<ArgsVkBeginQuery*>(
        WriteCommand(Command::kVkBeginQuery, sizeof(ArgsVkBeginQuery)));
    args.query_pool = query_pool;
    args.query = query;
    args.flags = flags;
  }

  VkBufferImageCopy* CmdCopyBufferToImageEmplace(VkBuffer src_buffer, VkImage dst_image,
                                                 VkImageLayout dst_image_layout,
                                                 uint32_t region_count) {
    const size_t header_size =
        rex::align(sizeof(ArgsVkCopyBufferToImage), alignof(VkBufferImageCopy));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(WriteCommand(
        Command::kVkCopyBufferToImage, header_size + sizeof(VkBufferImageCopy) * region_count));
    auto& args = *reinterpret_cast<ArgsVkCopyBufferToImage*>(args_ptr);
    args.src_buffer = src_buffer;
    args.dst_image = dst_image;
    args.dst_image_layout = dst_image_layout;
    args.region_count = region_count;
    return reinterpret_cast<VkBufferImageCopy*>(args_ptr + header_size);
  }
  void CmdVkCopyBufferToImage(VkBuffer src_buffer, VkImage dst_image,
                              VkImageLayout dst_image_layout, uint32_t region_count,
                              const VkBufferImageCopy* regions) {
    std::memcpy(CmdCopyBufferToImageEmplace(src_buffer, dst_image, dst_image_layout, region_count),
                regions, sizeof(VkBufferImageCopy) * region_count);
  }

  void CmdVkCopyQueryPoolResults(VkQueryPool query_pool, uint32_t first_query, uint32_t query_count,
                                 VkBuffer dst_buffer, VkDeviceSize dst_offset, VkDeviceSize stride,
                                 VkQueryResultFlags flags) {
    auto& args = *reinterpret_cast<ArgsVkCopyQueryPoolResults*>(
        WriteCommand(Command::kVkCopyQueryPoolResults, sizeof(ArgsVkCopyQueryPoolResults)));
    args.query_pool = query_pool;
    args.first_query = first_query;
    args.query_count = query_count;
    args.dst_buffer = dst_buffer;
    args.dst_offset = dst_offset;
    args.stride = stride;
    args.flags = flags;
  }

  void CmdVkDispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) {
    auto& args = *reinterpret_cast<ArgsVkDispatch*>(
        WriteCommand(Command::kVkDispatch, sizeof(ArgsVkDispatch)));
    args.group_count_x = group_count_x;
    args.group_count_y = group_count_y;
    args.group_count_z = group_count_z;
  }

  void CmdVkDraw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
                 uint32_t first_instance) {
    auto& args = *reinterpret_cast<ArgsVkDraw*>(WriteCommand(Command::kVkDraw, sizeof(ArgsVkDraw)));
    args.vertex_count = vertex_count;
    args.instance_count = instance_count;
    args.first_vertex = first_vertex;
    args.first_instance = first_instance;
  }

  void CmdVkDrawIndexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                        int32_t vertex_offset, uint32_t first_instance) {
    auto& args = *reinterpret_cast<ArgsVkDrawIndexed*>(
        WriteCommand(Command::kVkDrawIndexed, sizeof(ArgsVkDrawIndexed)));
    args.index_count = index_count;
    args.instance_count = instance_count;
    args.first_index = first_index;
    args.vertex_offset = vertex_offset;
    args.first_instance = first_instance;
  }

  void CmdVkEndQuery(VkQueryPool query_pool, uint32_t query) {
    auto& args = *reinterpret_cast<ArgsVkEndQuery*>(
        WriteCommand(Command::kVkEndQuery, sizeof(ArgsVkEndQuery)));
    args.query_pool = query_pool;
    args.query = query;
  }

  void CmdVkEndRenderPass() { WriteCommand(Command::kVkEndRenderPass, 0); }

  // Dynamic rendering (VK_KHR_dynamic_rendering / Vulkan 1.3).
  void CmdVkBeginRendering(const VkRenderingInfo* rendering_info);
  void CmdVkEndRendering() { WriteCommand(Command::kVkEndRendering, 0); }

  void CmdVkResetQueryPool(VkQueryPool query_pool, uint32_t first_query, uint32_t query_count) {
    auto& args = *reinterpret_cast<ArgsVkResetQueryPool*>(
        WriteCommand(Command::kVkResetQueryPool, sizeof(ArgsVkResetQueryPool)));
    args.query_pool = query_pool;
    args.first_query = first_query;
    args.query_count = query_count;
  }

  // pNext of all barriers must be null.
  void CmdVkPipelineBarrier(VkPipelineStageFlags src_stage_mask,
                            VkPipelineStageFlags dst_stage_mask, VkDependencyFlags dependency_flags,
                            uint32_t memory_barrier_count, const VkMemoryBarrier* memory_barriers,
                            uint32_t buffer_memory_barrier_count,
                            const VkBufferMemoryBarrier* buffer_memory_barriers,
                            uint32_t image_memory_barrier_count,
                            const VkImageMemoryBarrier* image_memory_barriers);

  void CmdVkPushConstants(VkPipelineLayout layout, VkShaderStageFlags stage_flags, uint32_t offset,
                          uint32_t size, const void* values) {
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkPushConstants, sizeof(ArgsVkPushConstants) + size));
    auto& args = *reinterpret_cast<ArgsVkPushConstants*>(args_ptr);
    args.layout = layout;
    args.stage_flags = stage_flags;
    args.offset = offset;
    args.size = size;
    std::memcpy(args_ptr + sizeof(ArgsVkPushConstants), values, size);
  }

  void CmdVkSetBlendConstants(const float* blend_constants) {
    auto& args = *reinterpret_cast<ArgsVkSetBlendConstants*>(
        WriteCommand(Command::kVkSetBlendConstants, sizeof(ArgsVkSetBlendConstants)));
    std::memcpy(args.blend_constants, blend_constants, sizeof(float) * 4);
  }

  void CmdVkSetDepthBias(float depth_bias_constant_factor, float depth_bias_clamp,
                         float depth_bias_slope_factor) {
    auto& args = *reinterpret_cast<ArgsVkSetDepthBias*>(
        WriteCommand(Command::kVkSetDepthBias, sizeof(ArgsVkSetDepthBias)));
    args.depth_bias_constant_factor = depth_bias_constant_factor;
    args.depth_bias_clamp = depth_bias_clamp;
    args.depth_bias_slope_factor = depth_bias_slope_factor;
  }

  void CmdVkSetScissor(uint32_t first_scissor, uint32_t scissor_count, const VkRect2D* scissors) {
    const size_t header_size = rex::align(sizeof(ArgsVkSetScissor), alignof(VkRect2D));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkSetScissor, header_size + sizeof(VkRect2D) * scissor_count));
    auto& args = *reinterpret_cast<ArgsVkSetScissor*>(args_ptr);
    args.first_scissor = first_scissor;
    args.scissor_count = scissor_count;
    std::memcpy(args_ptr + header_size, scissors, sizeof(VkRect2D) * scissor_count);
  }

  void CmdVkSetStencilCompareMask(VkStencilFaceFlags face_mask, uint32_t compare_mask) {
    auto& args = *reinterpret_cast<ArgsSetStencilMaskReference*>(
        WriteCommand(Command::kVkSetStencilCompareMask, sizeof(ArgsSetStencilMaskReference)));
    args.face_mask = face_mask;
    args.mask_reference = compare_mask;
  }

  void CmdVkSetStencilReference(VkStencilFaceFlags face_mask, uint32_t reference) {
    auto& args = *reinterpret_cast<ArgsSetStencilMaskReference*>(
        WriteCommand(Command::kVkSetStencilReference, sizeof(ArgsSetStencilMaskReference)));
    args.face_mask = face_mask;
    args.mask_reference = reference;
  }

  void CmdVkSetStencilWriteMask(VkStencilFaceFlags face_mask, uint32_t write_mask) {
    auto& args = *reinterpret_cast<ArgsSetStencilMaskReference*>(
        WriteCommand(Command::kVkSetStencilWriteMask, sizeof(ArgsSetStencilMaskReference)));
    args.face_mask = face_mask;
    args.mask_reference = write_mask;
  }

  void CmdVkSetViewport(uint32_t first_viewport, uint32_t viewport_count,
                        const VkViewport* viewports) {
    const size_t header_size = rex::align(sizeof(ArgsVkSetViewport), alignof(VkViewport));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkSetViewport, header_size + sizeof(VkViewport) * viewport_count));
    auto& args = *reinterpret_cast<ArgsVkSetViewport*>(args_ptr);
    args.first_viewport = first_viewport;
    args.viewport_count = viewport_count;
    std::memcpy(args_ptr + header_size, viewports, sizeof(VkViewport) * viewport_count);
  }

 private:
  enum class Command {
    kFh1Dlss,
    kVkBlitImage,
    kFh1NativeCubeBlit,
    kVkBeginRenderPass,
    kVkBeginQuery,
    kVkBindDescriptorSets,
    kVkBindIndexBuffer,
    kVkBindPipeline,
    kVkBindVertexBuffers,
    kVkClearAttachments,
    kVkClearColorImage,
    kVkCopyBuffer,
    kVkCopyBufferToImage,
    kVkCopyQueryPoolResults,
    kVkDispatch,
    kVkDraw,
    kVkDrawIndexed,
    kVkEndQuery,
    kVkEndRenderPass,
    kVkBeginRendering,
    kVkEndRendering,
    kVkPipelineBarrier,
    kVkPushConstants,
    kVkResetQueryPool,
    kVkSetBlendConstants,
    kVkSetDepthBias,
    kVkSetScissor,
    kVkSetStencilCompareMask,
    kVkSetStencilReference,
    kVkSetStencilWriteMask,
    kVkSetViewport,
    kHoistedCommands,
    kFh1Breadcrumb,
    kFh1TestHang,
    kFh1Timestamp,
  };

  struct CommandHeader {
    Command command;
    uint32_t arguments_size_elements;
  };
  struct ArgsFh1Dlss {
    Fh1Dlss* effect;
    VkImage src, dst;
    Fh1Dlss::FrameInput input;
  };
  struct ArgsVkBlitImage {
    VkImage src, dst;
    VkImageBlit region;
  };
  static constexpr size_t kCommandHeaderSizeElements =
      (sizeof(CommandHeader) + sizeof(uintmax_t) - 1) / sizeof(uintmax_t);

  struct ArgsFh1NativeCubeBlit {
    VkImage src, dst;
    VkImageBlit region;
  };
  struct ArgsVkBeginRenderPass {
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkRect2D render_area;
    uint32_t clear_value_count;
    VkSubpassContents contents;
    // Followed by aligned optional VkClearValue[].
    static_assert(alignof(VkClearValue) <= alignof(uintmax_t));
  };

  struct ArgsVkBeginRendering {
    VkRenderingFlags flags;
    VkRect2D render_area;
    uint32_t layer_count;
    uint32_t view_mask;
    uint32_t color_attachment_count;
    bool has_depth_attachment;
    bool has_stencil_attachment;
    // Followed by:
    // - VkRenderingAttachmentInfo[color_attachment_count]
    // - VkRenderingAttachmentInfo depth (if has_depth_attachment)
    // - VkRenderingAttachmentInfo stencil (if has_stencil_attachment)
    static_assert(alignof(VkRenderingAttachmentInfo) <= alignof(uintmax_t));
  };

  struct ArgsVkBeginQuery {
    VkQueryPool query_pool;
    uint32_t query;
    VkQueryControlFlags flags;
  };

  struct ArgsVkBindDescriptorSets {
    VkPipelineBindPoint pipeline_bind_point;
    VkPipelineLayout layout;
    uint32_t first_set;
    uint32_t descriptor_set_count;
    uint32_t dynamic_offset_count;
    // Followed by aligned VkDescriptorSet[], optional uint32_t[].
    static_assert(alignof(VkDescriptorSet) <= alignof(uintmax_t));
  };

  struct ArgsVkBindIndexBuffer {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkIndexType index_type;
  };

  struct ArgsVkBindPipeline {
    VkPipelineBindPoint pipeline_bind_point;
    VkPipeline pipeline;
  };

  struct ArgsVkBindVertexBuffers {
    uint32_t first_binding;
    uint32_t binding_count;
    // Followed by aligned VkBuffer[], VkDeviceSize[].
    static_assert(alignof(VkBuffer) <= alignof(uintmax_t));
    static_assert(alignof(VkDeviceSize) <= alignof(uintmax_t));
  };

  struct ArgsVkClearAttachments {
    uint32_t attachment_count;
    uint32_t rect_count;
    // Followed by aligned VkClearAttachment[], VkClearRect[].
    static_assert(alignof(VkClearAttachment) <= alignof(uintmax_t));
    static_assert(alignof(VkClearRect) <= alignof(uintmax_t));
  };

  struct ArgsVkClearColorImage {
    VkImage image;
    VkImageLayout image_layout;
    VkClearColorValue color;
    uint32_t range_count;
    // Followed by aligned VkImageSubresourceRange[].
    static_assert(alignof(VkImageSubresourceRange) <= alignof(uintmax_t));
  };

  struct ArgsVkCopyBuffer {
    VkBuffer src_buffer;
    VkBuffer dst_buffer;
    uint32_t region_count;
    // Followed by aligned VkBufferCopy[].
    static_assert(alignof(VkBufferCopy) <= alignof(uintmax_t));
  };

  struct ArgsVkCopyBufferToImage {
    VkBuffer src_buffer;
    VkImage dst_image;
    VkImageLayout dst_image_layout;
    uint32_t region_count;
    // Followed by aligned VkBufferImageCopy[].
    static_assert(alignof(VkBufferImageCopy) <= alignof(uintmax_t));
  };

  struct ArgsVkCopyQueryPoolResults {
    VkQueryPool query_pool;
    uint32_t first_query;
    uint32_t query_count;
    VkBuffer dst_buffer;
    VkDeviceSize dst_offset;
    VkDeviceSize stride;
    VkQueryResultFlags flags;
  };

  struct ArgsVkDispatch {
    uint32_t group_count_x;
    uint32_t group_count_y;
    uint32_t group_count_z;
  };

  struct ArgsVkDraw {
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
  };

  struct ArgsVkDrawIndexed {
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    int32_t vertex_offset;
    uint32_t first_instance;
  };

  struct ArgsVkEndQuery {
    VkQueryPool query_pool;
    uint32_t query;
  };

  struct ArgsVkResetQueryPool {
    VkQueryPool query_pool;
    uint32_t first_query;
    uint32_t query_count;
  };

  struct ArgsVkPipelineBarrier {
    VkPipelineStageFlags src_stage_mask;
    VkPipelineStageFlags dst_stage_mask;
    VkDependencyFlags dependency_flags;
    uint32_t memory_barrier_count;
    uint32_t buffer_memory_barrier_count;
    uint32_t image_memory_barrier_count;
    // Followed by aligned optional VkMemoryBarrier[],
    // optional VkBufferMemoryBarrier[], optional VkImageMemoryBarrier[].
    static_assert(alignof(VkMemoryBarrier) <= alignof(uintmax_t));
    static_assert(alignof(VkBufferMemoryBarrier) <= alignof(uintmax_t));
    static_assert(alignof(VkImageMemoryBarrier) <= alignof(uintmax_t));
  };

  struct ArgsVkPushConstants {
    VkPipelineLayout layout;
    VkShaderStageFlags stage_flags;
    uint32_t offset;
    uint32_t size;
    // Followed by `size` bytes of values.
  };

  struct ArgsVkSetBlendConstants {
    float blend_constants[4];
  };

  struct ArgsVkSetDepthBias {
    float depth_bias_constant_factor;
    float depth_bias_clamp;
    float depth_bias_slope_factor;
  };

  struct ArgsVkSetScissor {
    uint32_t first_scissor;
    uint32_t scissor_count;
    // Followed by aligned VkRect2D[].
    static_assert(alignof(VkRect2D) <= alignof(uintmax_t));
  };

  struct ArgsSetStencilMaskReference {
    VkStencilFaceFlags face_mask;
    uint32_t mask_reference;
  };

  struct ArgsFh1Breadcrumb {
    uint32_t seq;
  };

  struct ArgsFh1Timestamp {
    PFN_vkCmdWriteTimestamp write;
    VkQueryPool pool;
    uint32_t query;
  };

  struct ArgsHoistedCommands {
    // Range of hoisted_stream_, in elements.
    size_t begin;
    size_t end;
  };

  struct ArgsVkSetViewport {
    uint32_t first_viewport;
    uint32_t viewport_count;
    // Followed by aligned VkViewport[].
    static_assert(alignof(VkViewport) <= alignof(uintmax_t));
  };

  void* WriteCommand(Command command, size_t arguments_size_bytes);
  void Fh1GpuZoneMark();
  VkCommandBuffer ExecuteStream(VkCommandBuffer command_buffer, const uintmax_t* stream,
                     size_t stream_remaining, const uintmax_t* hoisted_stream,
                     const Fh1Dlss::SplitSubmission& split = {}) const;

  const VulkanCommandProcessor& command_processor_;

  // uintmax_t to ensure uint64_t and pointer alignment of all structures.
  std::vector<uintmax_t> command_stream_;
  std::vector<uintmax_t> hoisted_stream_;
  std::vector<uintmax_t>* write_stream_ = &command_stream_;
  bool native_upscale_ = false;
  const Fh1Breadcrumbs* fh1_breadcrumbs_ = nullptr;
  Fh1GpuProfile* fh1_gpu_profile_ = nullptr;
  uint16_t fh1_gpu_zone_ = 0;
  uint16_t fh1_gpu_zone_marked_ = 0;
  bool fh1_gpu_zone_pending_ = false;
};

// FH1 (#640): the zone of the commands recorded in a scope.
class Fh1GpuZoneScope {
 public:
  Fh1GpuZoneScope(DeferredCommandBuffer& commands, uint16_t zone)
      : commands_(commands), previous_(commands.SetFh1GpuZone(zone)) {}
  ~Fh1GpuZoneScope() { commands_.SetFh1GpuZone(previous_); }
  Fh1GpuZoneScope(const Fh1GpuZoneScope&) = delete;
  Fh1GpuZoneScope& operator=(const Fh1GpuZoneScope&) = delete;

 private:
  DeferredCommandBuffer& commands_;
  uint16_t previous_;
};

}  // namespace rex::graphics::vulkan
