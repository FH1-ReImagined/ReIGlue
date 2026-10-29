// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <vulkan/vulkan_core.h>

namespace rex::ui::vulkan {

// This operation deliberately supports only identical, uncompressed single-layer
// 10:10:10:2 images. It neither converts colors nor resolves or rescales pixels.
struct SnapshotSurface {
  VkImage image;
  VkFormat format;
  uint64_t width, height;
  VkSampleCountFlagBits samples;
  VkImageUsageFlags usage;
};

inline std::optional<VkImageCopy> PlanColorSnapshot(const SnapshotSurface& source,
    const SnapshotSurface& destination, uint32_t width, uint32_t height) {
  if (!source.image || !destination.image || source.image == destination.image ||
      source.format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 || destination.format != source.format ||
      source.samples != VK_SAMPLE_COUNT_1_BIT || destination.samples != source.samples ||
      !(source.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
      !(destination.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || !width || !height)
    return std::nullopt;
  for (const auto* image : {&source, &destination}) {
    if (image->width > std::numeric_limits<uint32_t>::max() ||
        image->height > std::numeric_limits<uint32_t>::max() ||
        width > image->width || height > image->height)
      return std::nullopt;
  }
  VkImageCopy region{};
  region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource = region.srcSubresource;
  region.extent = {width, height, 1};
  return region;
}

struct SnapshotAccess {
  VkPipelineStageFlags stage;
  VkAccessFlags access;
  VkImageLayout layout;
};
inline constexpr SnapshotAccess kSnapshotRead{VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};

// The destination is caller-owned, separately allocated, and already has a
// pending TRANSFER_DST transition. Validation happens before disturbing rendering.
// Recorders transition and track the source, flush all pending barriers, copy,
// then schedule its next use. No restoration of an untracked old layout occurs.
template <typename Recorder>
bool RecordColorSnapshot(const SnapshotSurface& source, const SnapshotSurface& destination,
    uint32_t width, uint32_t height, SnapshotAccess next_use, Recorder& recorder) {
  const auto region = PlanColorSnapshot(source, destination, width, height);
  if (!region) return false;
  recorder.Begin(kSnapshotRead);
  recorder.Copy(source.image, destination.image, *region);
  recorder.Finish(next_use);
  return true;
}
}  // namespace rex::ui::vulkan
