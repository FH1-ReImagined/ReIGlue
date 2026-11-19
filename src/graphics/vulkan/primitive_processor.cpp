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
  size_t bound = 0;
  size_t forced = 0;
};

thread_local Fh1IndexAudit fh1_index_audit;
thread_local bool fh1_index_audit_forced_fallback = false;
}  // namespace

void VulkanCommandProcessor::AuditFh1OwnedIndices(const fh1_list::ReplayPlan& plan,
                                                  uint64_t lifetime, bool bind) {
  if (!submission_open_ || !frame_open_ || !primitive_processor_) {
    REXGPU_INFO("M4 index upload audit: lifetime={} skipped=no-frame", lifetime);
    return;
  }
  auto& audit = fh1_index_audit;
  if (bind) {
    audit = {};
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
  REXGPU_INFO("M4 index upload audit: lifetime={} frame={} submission={} uploads={} bytes={} refused={}",
              lifetime, GetCurrentFrame(), GetCurrentSubmission(), uploads, bytes, refused);
}

void VulkanCommandProcessor::SelectFh1IndexAuditDraw(uint32_t packet_offset) {
  auto& audit = fh1_index_audit;
  if (audit.owner != this) return;
  audit.selected = nullptr;
  audit.witnessed = false;
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
  if (!entry || !audit.witnessed || !frame_open_ || !submission_open_ ||
      audit.frame != GetCurrentFrame() || audit.submission != GetCurrentSubmission()) return {};
  const auto& index = entry->index;
  if (index.address != address || index.width != width || index.endian != endian ||
      index.bytes.size() != uint64_t(count) * width ||
      shared_memory_->IsRangeGpuWritten(address, uint32_t(index.bytes.size()))) return {};
  if (!std::exchange(fh1_index_audit_forced_fallback, true)) {
    ++audit.forced;
    REXGPU_INFO("M4 index bind audit: lifetime={} packet={} forced-stock bytes={}",
                audit.lifetime, entry->packet, index.bytes.size());
    return {};
  }
  ++audit.bound;
  REXGPU_INFO("M4 index bind audit: lifetime={} packet={} owned bytes={}",
              audit.lifetime, entry->packet, index.bytes.size());
  return entry->slice;
}

void VulkanCommandProcessor::EndFh1IndexAudit() {
  auto& audit = fh1_index_audit;
  if (audit.owner != this) return;
  REXGPU_INFO("M4 index bind summary: lifetime={} staged={} owned={} forced={}",
              audit.lifetime, audit.entries.size(), audit.bound, audit.forced);
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

  frame_index_buffers_.clear();
  frame_index_buffer_pool_.reset();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, replay_vertex_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, replay_vertex_memory_);
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

void VulkanPrimitiveProcessor::CompletedSubmissionUpdated() {
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
