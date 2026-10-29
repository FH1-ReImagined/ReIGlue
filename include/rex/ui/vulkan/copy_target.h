// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <rex/ui/vulkan/util.h>

namespace rex::ui::vulkan {
// A local guard owns an unpublished allocation until its image and view both
// exist. Target is a caller-owned record with image, memory, view and dimensions.
template <typename Target>
bool InstallCopyTarget(const VulkanDevice& device, uint32_t width, uint32_t height, Target& target,
                       VkFormat format = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                       VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) {
  if (!width || !height || target.image) return false;
  struct Pending {
    const VulkanDevice& owner;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    bool published = false;
    ~Pending() {
      if (published) return;
      const auto& api = owner.functions();
      if (view) api.vkDestroyImageView(owner.device(), view, nullptr);
      if (image) api.vkDestroyImage(owner.device(), image, nullptr);
      if (memory) api.vkFreeMemory(owner.device(), memory, nullptr);
    }
  } pending{device};
  VkImageCreateInfo image{};
  image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = format;
  image.extent = {width, height, 1};
  image.mipLevels = image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  image.usage = usage;
  if (!util::CreateDedicatedAllocationImage(&device, image, util::MemoryPurpose::kDeviceLocal,
                                             pending.image, pending.memory)) return false;
  VkImageViewCreateInfo view{};
  view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view.image = pending.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = image.format;
  view.subresourceRange = util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
  if (device.functions().vkCreateImageView(device.device(), &view, nullptr, &pending.view) != VK_SUCCESS)
    return false;
  target.image = pending.image;
  target.memory = pending.memory;
  target.view = pending.view;
  if constexpr (requires { target.width; target.height; }) {
    target.width = width;
    target.height = height;
  }
  pending.published = true;
  return true;
}
}  // namespace rex::ui::vulkan
