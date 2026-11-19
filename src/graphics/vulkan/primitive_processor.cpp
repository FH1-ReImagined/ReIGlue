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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

#include <rex/assert.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/vulkan/primitive_processor.h>
#include "../fh1_replay_benchmark.h"
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

#include "fh1_device_memory.h"
#include "fh1_upload_stats.h"
#include "../fh1_native_replay_plan.h"

REXCVAR_DEFINE_BOOL(vulkan_force_expand_point_sprites_in_vs, false, "GPU/Vulkan",
                    "Force Vulkan point sprite expansion in the vertex shader, even when geometry "
                    "shaders are available")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    vulkan_force_expand_rectangle_lists_in_vs, false, "GPU/Vulkan",
    "Force Vulkan rectangle list expansion in the vertex shader, even when geometry "
    "shaders are available")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vulkan_force_convert_quad_lists_to_triangle_lists, false, "GPU/Vulkan",
                    "Force Vulkan quad list conversion to triangle lists in primitive processing, "
                    "even when geometry shaders are available")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::graphics::vulkan {

namespace {
constexpr VkDeviceSize kReplayIndexCapacity = 256 * 1024;

struct Fh1IndexAuditEntry {
  uint32_t packet;
  fh1_list::ReplayPlan::IndexSnapshot index;
  std::pair<VkBuffer, VkDeviceSize> slice;
};

struct Fh1IndexAudit {
  const VulkanCommandProcessor* owner = nullptr;
  uint64_t lifetime = 0;
  uint64_t frame = 0;
  uint64_t submission = 0;
  std::vector<Fh1IndexAuditEntry> entries;
  const Fh1IndexAuditEntry* selected = nullptr;
  bool witnessed = false;
  bool upload_seen = false;
  size_t bound = 0;
  size_t forced = 0;
};

thread_local Fh1IndexAudit fh1_index_audit;
thread_local bool fh1_index_audit_forced_fallback = false;
}  // namespace

void VulkanCommandProcessor::AuditFh1OwnedIndices(const fh1_list::ReplayPlan& plan,
                                                  bool bind) {
  auto& audit = fh1_index_audit;
  if (bind) audit = {};
  const uint64_t lifetime = plan.producer_lifetime();
  if (!lifetime) return;
  if (!submission_open_ || !frame_open_ || !primitive_processor_) {
    REXGPU_INFO("M4 index upload audit: lifetime={} skipped=no-frame", lifetime);
    return;
  }
  if (bind) {
    audit.owner = this;
    audit.lifetime = lifetime;
    audit.frame = GetCurrentFrame();
    audit.submission = GetCurrentSubmission();
  }
  size_t uploads = 0, bytes = 0, refused = 0;
  for (const auto& op : plan.operations()) {
    const auto* index = plan.index_input(op);
    if (!index) continue;
    const auto [buffer, offset] = primitive_processor_->StageCapturedIndex(index->bytes, index->width);
    if (buffer == VK_NULL_HANDLE || offset % index->width) {
      ++refused;
      continue;
    }
    ++uploads;
    bytes += index->bytes.size();
    if (bind) audit.entries.push_back({op.first_word * 4, *index, {buffer, offset}});
  }
  if (refused || !fh1_list::SustainedReplayRequested()) {
    REXGPU_INFO("M4 index upload audit: lifetime={} frame={} submission={} uploads={} bytes={} refused={}",
                lifetime, GetCurrentFrame(), GetCurrentSubmission(), uploads, bytes, refused);
  }
}

void VulkanCommandProcessor::SelectFh1IndexAuditDraw(uint32_t packet_offset) {
  auto& audit = fh1_index_audit;
  if (audit.owner != this) return;
  audit.selected = nullptr;
  audit.witnessed = false;
  audit.upload_seen = false;
  for (const auto& entry : audit.entries) {
    if (entry.packet == packet_offset) {
      audit.selected = &entry;
      break;
    }
  }
}

bool VulkanCommandProcessor::Fh1ReplayDrawSelected() const {
  const auto& audit = fh1_index_audit;
  return audit.owner == this && audit.selected && frame_open_ && submission_open_ &&
         audit.frame == GetCurrentFrame() && audit.submission == GetCurrentSubmission();
}

void VulkanCommandProcessor::ObserveFh1IndexAuditUpload(uint32_t address,
                                                       std::span<const uint8_t> bytes) {
  auto& audit = fh1_index_audit;
  if (audit.owner != this || !audit.selected || audit.frame != GetCurrentFrame() ||
      audit.submission != GetCurrentSubmission()) return;
  const auto& index = audit.selected->index;
  const uint64_t upload_end = uint64_t(address) + bytes.size();
  const uint64_t index_end = uint64_t(index.address) + index.bytes.size();
  if (upload_end <= index.address || index_end <= address) return;
  audit.upload_seen = true;
  // A later overlapping transfer supersedes any earlier witness.
  audit.witnessed = address <= index.address && index_end <= upload_end &&
                    std::memcmp(bytes.data() + (index.address - address), index.bytes.data(),
                                index.bytes.size()) == 0;
}

std::pair<VkBuffer, VkDeviceSize> VulkanCommandProcessor::TakeFh1IndexAudit(
    uint32_t address, uint32_t count, uint32_t width, uint32_t endian) {
  auto& audit = fh1_index_audit;
  if (audit.owner != this) return {};
  const auto* entry = std::exchange(audit.selected, nullptr);
  if (!entry || !frame_open_ || !submission_open_ ||
      audit.frame != GetCurrentFrame() || audit.submission != GetCurrentSubmission()) return {};
  const auto& index = entry->index;
  if (index.address != address || index.width != width || index.endian != endian ||
      index.bytes.size() != uint64_t(count) * width ||
      shared_memory_->IsRangeGpuWritten(address, uint32_t(index.bytes.size()))) return {};
  const VkBuffer replay_index = primitive_processor_->replay_index_buffer();
  VkBufferCopy resident_copy = {};
  const uint32_t bias = address & 3;
  if (!audit.witnessed) {
    // A resident copy starts from the GPU bytes the stock draw would read. An
    // observed nonmatching upload is a disagreement and stays on the stock path.
    if (audit.upload_seen || replay_index == VK_NULL_HANDLE) return {};
    resident_copy.srcOffset = address - bias;
    resident_copy.size = (VkDeviceSize(bias) + index.bytes.size() + 3) & ~VkDeviceSize(3);
    if (resident_copy.size > kReplayIndexCapacity ||
        resident_copy.srcOffset + resident_copy.size > SharedMemory::kBufferSize) return {};
    const auto lock = thread::global_critical_region::AcquireDirect();
    if (!shared_memory_->IsRangeValid(uint32_t(resident_copy.srcOffset),
                                      uint32_t(resident_copy.size))) return {};
  }
  if (!std::exchange(fh1_index_audit_forced_fallback, true)) {
    ++audit.forced;
    REXGPU_INFO("M4 index bind audit: lifetime={} packet={} forced-stock bytes={}",
                audit.lifetime, entry->packet, index.bytes.size());
    return {};
  }
  if (!audit.witnessed) {
    shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
    PushBufferMemoryBarrier(replay_index, 0, resident_copy.size,
                            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT);
    SubmitBarriers(true);
    deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(), replay_index, 1,
                                             &resident_copy);
    PushBufferMemoryBarrier(replay_index, 0, resident_copy.size, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_INDEX_READ_BIT);
  }
  ++audit.bound;
  if (!fh1_list::SustainedReplayRequested()) {
    REXGPU_INFO("M4 index bind audit: lifetime={} packet={} owned bytes={} source={}",
                audit.lifetime, entry->packet, index.bytes.size(),
                audit.witnessed ? "matched-upload" : "resident-gpu-copy");
  }
  if (!audit.witnessed) return {replay_index, bias};
  return entry->slice;
}

void VulkanCommandProcessor::EndFh1IndexAudit() {
  auto& audit = fh1_index_audit;
  if (audit.owner != this) return;
  if (!fh1_list::SustainedReplayRequested()) {
    REXGPU_INFO("M4 index bind summary: lifetime={} staged={} owned={} forced={}",
                audit.lifetime, audit.entries.size(), audit.bound, audit.forced);
  }
  audit = {};
}

VulkanPrimitiveProcessor::~VulkanPrimitiveProcessor() {
  Shutdown(true);
}

bool VulkanPrimitiveProcessor::Initialize() {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();
  // Keep triangle fan handling aligned with D3D12 by always converting fans to
  // lists in the shared primitive processor path.
  constexpr bool triangle_fans_supported_without_conversion = false;

  // Default to D3D12-like geometry-shader-based handling when available, but
  // allow opting into Vulkan fallback paths for debugging and downlevel
  // compatibility testing.
  bool geometry_shader_primitive_emulation_allowed = device_properties.geometryShader;
  bool quad_lists_supported_without_conversion =
      geometry_shader_primitive_emulation_allowed &&
      !REXCVAR_GET(vulkan_force_convert_quad_lists_to_triangle_lists);
  bool point_sprites_supported_without_vs_expansion =
      geometry_shader_primitive_emulation_allowed &&
      !REXCVAR_GET(vulkan_force_expand_point_sprites_in_vs);
  bool rectangle_lists_supported_without_vs_expansion =
      geometry_shader_primitive_emulation_allowed &&
      !REXCVAR_GET(vulkan_force_expand_rectangle_lists_in_vs);

  if (!InitializeCommon(
          device_properties.fullDrawIndexUint32, triangle_fans_supported_without_conversion, false,
          quad_lists_supported_without_conversion, point_sprites_supported_without_vs_expansion,
          rectangle_lists_supported_without_vs_expansion)) {
    Shutdown();
    return false;
  }
  frame_index_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      std::max(size_t(kMinRequiredConvertedIndexBufferSize),
               ui::GraphicsUploadBufferPool::kDefaultPageSize),
      fh1_device_memory::Get().upload_types());
  return true;
}

void VulkanPrimitiveProcessor::Shutdown(bool from_destructor) {
  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  LandReplayPixelReadbacks(command_processor_.GetCompletedSubmission(), true);

  frame_index_buffers_.clear();
  frame_index_buffer_pool_.reset();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, replay_vertex_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, replay_vertex_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, replay_index_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, replay_index_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_upload_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         builtin_index_buffer_upload_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, builtin_index_buffer_memory_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanPrimitiveProcessor::InitializeReplayVertexBuffer() {
  ui::vulkan::util::CreateDedicatedAllocationBuffer(
      command_processor_.GetVulkanDevice(), SpirvShaderTranslator::kFh1ReplayVertexCapacity,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      ui::vulkan::util::MemoryPurpose::kDeviceLocal, replay_vertex_buffer_, replay_vertex_memory_);
}

void VulkanPrimitiveProcessor::InitializeReplayIndexBuffer() {
  ui::vulkan::util::CreateDedicatedAllocationBuffer(
      command_processor_.GetVulkanDevice(), kReplayIndexCapacity,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      ui::vulkan::util::MemoryPurpose::kDeviceLocal, replay_index_buffer_, replay_index_memory_);
}

void VulkanPrimitiveProcessor::CompletedSubmissionUpdated() {
  LandReplayPixelReadbacks(command_processor_.GetCompletedSubmission());
  if (builtin_index_buffer_upload_ != VK_NULL_HANDLE &&
      command_processor_.GetCompletedSubmission() >= builtin_index_buffer_upload_submission_) {
    const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           builtin_index_buffer_upload_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           builtin_index_buffer_upload_memory_);
  }
}

VulkanPrimitiveProcessor::ReplayPixelReadback*
VulkanPrimitiveProcessor::AllocateReplayPixelReadback(VkRect2D rect, VkFormat depth_format,
                                                       VkFormat color_format) {
  ReplayPixelReadback sample;
  const VkDeviceSize pixels = VkDeviceSize(rect.extent.width) * rect.extent.height;
  const bool color = color_format != VK_FORMAT_UNDEFINED;
  if (!pixels || pixels > 1280 * 2048 || replay_pixel_samples_[color] >= 4) return nullptr;
  auto add = [&](VkImageAspectFlags aspect, uint32_t bytes, bool d24 = false) {
    sample.planes[sample.plane_count++] = {aspect, bytes, sample.stride, d24};
    sample.stride = (sample.stride + pixels * bytes + 7) & ~VkDeviceSize(7);
  };
  if (depth_format != VK_FORMAT_UNDEFINED) {
    if (depth_format != VK_FORMAT_D24_UNORM_S8_UINT &&
        depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) return nullptr;
    add(VK_IMAGE_ASPECT_DEPTH_BIT, 4, depth_format == VK_FORMAT_D24_UNORM_S8_UINT);
    add(VK_IMAGE_ASPECT_STENCIL_BIT, 1);
  }
  if (color) {
    switch (color_format) {
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_SRGB:
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
      case VK_FORMAT_R16G16_SNORM:
      case VK_FORMAT_R16G16_SFLOAT:
      case VK_FORMAT_R32_SFLOAT:
        add(VK_IMAGE_ASPECT_COLOR_BIT, 4);
        break;
      case VK_FORMAT_R16G16B16A16_UNORM:
      case VK_FORMAT_R16G16B16A16_SNORM:
      case VK_FORMAT_R16G16B16A16_SFLOAT:
      case VK_FORMAT_R32G32_SFLOAT:
        add(VK_IMAGE_ASPECT_COLOR_BIT, 8);
        break;
      default:
        REXGPU_INFO("M4 pixel parity unsupported color format={}", uint32_t(color_format));
        return nullptr;
    }
  }
  assert_not_zero(sample.plane_count);
  for (auto& slot : replay_pixel_readbacks_) {
    if (slot.buffer) continue;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            command_processor_.GetVulkanDevice(), sample.stride * 3,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, ui::vulkan::util::MemoryPurpose::kReadback,
            sample.buffer, sample.memory, &sample.memory_type)) return nullptr;
    sample.submission = command_processor_.GetCurrentSubmission();
    sample.rect = rect;
    slot = sample;
    ++replay_pixel_samples_[color];
    return &slot;
  }
  return nullptr;
}

void VulkanPrimitiveProcessor::LandReplayPixelReadbacks(uint64_t completed, bool discard) {
  if (!replay_pixel_samples_[0] && !replay_pixel_samples_[1]) return;
  const auto* device = command_processor_.GetVulkanDevice();
  const auto& dfn = device->functions();
  for (auto& slot : replay_pixel_readbacks_) {
    if (!slot.buffer || (!discard && slot.submission > completed)) continue;
    void* mapped = nullptr;
    const bool mapped_ok = slot.complete && slot.submission <= completed &&
        dfn.vkMapMemory(device->device(), slot.memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS;
    bool readable = mapped_ok;
    if (readable && !(device->memory_types().host_coherent & (1u << slot.memory_type))) {
      const VkMappedMemoryRange range{
          VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, slot.memory, 0, VK_WHOLE_SIZE};
      readable = dfn.vkInvalidateMappedMemoryRanges(device->device(), 1, &range) == VK_SUCCESS;
    }
    const auto pixels = size_t(slot.rect.extent.width) * slot.rect.extent.height;
    for (uint32_t i = 0; i < slot.plane_count; ++i) {
      const auto& plane = slot.planes[i];
      size_t changed = 0;
      size_t mismatch = 0;
      if (readable) {
        const auto* before = static_cast<const uint8_t*>(mapped) + plane.offset;
        const auto* stock = before + slot.stride;
        const auto* replay = stock + slot.stride;
        // X8_D24 has an unused high byte in the transferred depth aspect.
        const uint32_t meaningful = plane.depth24 ? 3 : plane.pixel_bytes;
        for (size_t pixel = 0; pixel < pixels; ++pixel) {
          const auto offset = pixel * plane.pixel_bytes;
          changed += std::memcmp(before + offset, stock + offset, meaningful) != 0;
          mismatch += std::memcmp(stock + offset, replay + offset, meaningful) != 0;
        }
      }
      REXGPU_INFO("M4 pixel parity: submission={} completed={} aspect={} pixels={} readable={} "
                  "stock_changed={} replay_mismatch={} x={} y={} w={} h={}",
                  slot.submission, completed, uint32_t(plane.aspect), pixels, readable, changed,
                  mismatch, slot.rect.offset.x, slot.rect.offset.y, slot.rect.extent.width,
                  slot.rect.extent.height);
    }
    if (mapped_ok) dfn.vkUnmapMemory(device->device(), slot.memory);
    dfn.vkDestroyBuffer(device->device(), slot.buffer, nullptr);
    dfn.vkFreeMemory(device->device(), slot.memory, nullptr);
    slot = {};
  }
}

void VulkanPrimitiveProcessor::BeginSubmission() {
  if (builtin_index_buffer_upload_ != VK_NULL_HANDLE &&
      builtin_index_buffer_upload_submission_ == UINT64_MAX) {
    // No need to submit deferred barriers - builtin_index_buffer_ has never
    // been used yet, and builtin_index_buffer_upload_ is written before
    // submitting commands reading it.

    command_processor_.EndRenderPass();

    DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();

    VkBufferCopy* copy_region =
        command_buffer.CmdCopyBufferEmplace(builtin_index_buffer_upload_, builtin_index_buffer_, 1);
    copy_region->srcOffset = 0;
    copy_region->dstOffset = 0;
    copy_region->size = builtin_index_buffer_size_;

    command_processor_.PushBufferMemoryBarrier(
        builtin_index_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_INDEX_READ_BIT);

    builtin_index_buffer_upload_submission_ = command_processor_.GetCurrentSubmission();
  }
}

void VulkanPrimitiveProcessor::BeginFrame() {
  frame_index_buffer_pool_->Reclaim(command_processor_.GetCompletedFrame());
}

void VulkanPrimitiveProcessor::EndSubmission() {
  frame_index_buffer_pool_->FlushWrites();
}

void VulkanPrimitiveProcessor::EndFrame() {
  ClearPerFrameCache();
  frame_index_buffers_.clear();
}

std::pair<VkBuffer, VkDeviceSize> VulkanPrimitiveProcessor::StageCapturedIndex(
    std::span<const std::byte> bytes, uint32_t width) {
  if ((width != 2 && width != 4) || bytes.empty() || bytes.size() % width ||
      bytes.size() / width > UINT16_MAX) return {};
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
  uint8_t* mapping = frame_index_buffer_pool_->Request(
      command_processor_.GetCurrentFrame(), bytes.size(), width, buffer, offset);
  if (!mapping) return {};
  std::memcpy(mapping, bytes.data(), bytes.size());
  return {buffer, offset};
}

bool VulkanPrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  assert_not_zero(size_bytes);
  assert_true(builtin_index_buffer_ == VK_NULL_HANDLE);
  assert_true(builtin_index_buffer_memory_ == VK_NULL_HANDLE);
  assert_true(builtin_index_buffer_upload_ == VK_NULL_HANDLE);
  assert_true(builtin_index_buffer_upload_memory_ == VK_NULL_HANDLE);

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  builtin_index_buffer_size_ = VkDeviceSize(size_bytes);
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, builtin_index_buffer_size_,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, builtin_index_buffer_,
          builtin_index_buffer_memory_)) {
    REXGPU_ERROR(
        "Vulkan primitive processor: Failed to create the built-in index "
        "buffer GPU resource with {} bytes",
        size_bytes);
    return false;
  }
  uint32_t upload_memory_type;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, builtin_index_buffer_size_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          ui::vulkan::util::MemoryPurpose::kUpload, builtin_index_buffer_upload_,
          builtin_index_buffer_upload_memory_, &upload_memory_type)) {
    REXGPU_ERROR(
        "Vulkan primitive processor: Failed to create the built-in index "
        "buffer upload resource with {} bytes",
        size_bytes);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, builtin_index_buffer_memory_);
    return false;
  }

  void* mapping;
  if (dfn.vkMapMemory(device, builtin_index_buffer_upload_memory_, 0, VK_WHOLE_SIZE, 0, &mapping) !=
      VK_SUCCESS) {
    REXGPU_ERROR(
        "Vulkan primitive processor: Failed to map the built-in index buffer "
        "upload resource with {} bytes",
        size_bytes);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           builtin_index_buffer_upload_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           builtin_index_buffer_upload_memory_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, builtin_index_buffer_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, builtin_index_buffer_memory_);
    return false;
  }
  fill_callback(mapping);
  ui::vulkan::util::FlushMappedMemoryRange(vulkan_device, builtin_index_buffer_memory_,
                                           upload_memory_type);
  dfn.vkUnmapMemory(device, builtin_index_buffer_upload_memory_);

  // Schedule uploading in the first submission.
  builtin_index_buffer_upload_submission_ = UINT64_MAX;
  return true;
}

void* VulkanPrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
  VkBuffer buffer;
  VkDeviceSize offset;
  uint8_t* mapping = frame_index_buffer_pool_->Request(
      command_processor_.GetCurrentFrame(),
      index_size * index_count + (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0),
      index_size, buffer, offset);
  if (!mapping) {
    return nullptr;
  }
  fh1_upload_stats::Add(fh1_upload_stats::Kind::kConvertedIndices, index_size * index_count);
  if (coalign_for_simd) {
    ptrdiff_t coalignment_offset = GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    mapping += coalignment_offset;
    offset = VkDeviceSize(offset + coalignment_offset);
  }
  backend_handle_out = frame_index_buffers_.size();
  frame_index_buffers_.emplace_back(buffer, offset);
  return mapping;
}

}  // namespace rex::graphics::vulkan
