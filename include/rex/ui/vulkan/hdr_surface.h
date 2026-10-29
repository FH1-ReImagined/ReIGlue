// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <rex/ui/vulkan/util.h>
#include <rex/ui/vulkan/device.h>
#include <optional>
#include <utility>

namespace rex::ui::vulkan {
// Resources for one acquired swapchain image. The caller must retire submitted
// work before destruction. A failed construction cleans up only its own partial
// allocation; other images and the current swapchain remain owned by the caller.
class HdrSurface {
 public:
  HdrSurface(const HdrSurface&) = delete;
  HdrSurface& operator=(const HdrSurface&) = delete;
  HdrSurface(HdrSurface&& other) noexcept
      : owner_(std::exchange(other.owner_, nullptr)),
        image_(std::exchange(other.image_, VK_NULL_HANDLE)),
        memory_(std::exchange(other.memory_, VK_NULL_HANDLE)),
        linear_view_(std::exchange(other.linear_view_, VK_NULL_HANDLE)),
        target_view_(std::exchange(other.target_view_, VK_NULL_HANDLE)),
        framebuffer_(std::exchange(other.framebuffer_, VK_NULL_HANDLE)),
        sample_set_(other.sample_set_) {}

  ~HdrSurface() {
    if (!owner_) return;
    const auto& api = owner_->functions();
    const auto device = owner_->device();
    if (framebuffer_) api.vkDestroyFramebuffer(device, framebuffer_, nullptr);
    if (target_view_) api.vkDestroyImageView(device, target_view_, nullptr);
    if (linear_view_) api.vkDestroyImageView(device, linear_view_, nullptr);
    if (image_) api.vkDestroyImage(device, image_, nullptr);
    if (memory_) api.vkFreeMemory(device, memory_, nullptr);
  }

  static std::optional<HdrSurface> Create(const VulkanDevice* owner, VkExtent2D extent,
                                         VkImage target, VkFormat target_format,
                                         VkRenderPass output_pass, VkDescriptorSet samples) {
    if (!owner || !extent.width || !extent.height || !target || !output_pass || !samples)
      return std::nullopt;
    HdrSurface pending(owner);
    pending.sample_set_ = samples;
    const auto& api = owner->functions();
    const auto device = owner->device();
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_B8G8R8A8_UNORM;
    image.extent = {extent.width, extent.height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!util::CreateDedicatedAllocationImage(owner, image, util::MemoryPurpose::kDeviceLocal,
                                              pending.image_, pending.memory_))
      return std::nullopt;

    auto view = [&](VkImage source, VkFormat format, VkImageView& output) {
      VkImageViewCreateInfo description{};
      description.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      description.image = source;
      description.viewType = VK_IMAGE_VIEW_TYPE_2D;
      description.format = format;
      description.subresourceRange = util::InitializeSubresourceRange();
      return api.vkCreateImageView(device, &description, nullptr, &output) == VK_SUCCESS;
    };
    if (!view(pending.image_, VK_FORMAT_B8G8R8A8_SRGB, pending.linear_view_) ||
        !view(target, target_format, pending.target_view_)) return std::nullopt;

    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = output_pass;
    framebuffer_info.attachmentCount = framebuffer_info.layers = 1;
    framebuffer_info.pAttachments = &pending.target_view_;
    framebuffer_info.width = extent.width;
    framebuffer_info.height = extent.height;
    if (api.vkCreateFramebuffer(device, &framebuffer_info, nullptr, &pending.framebuffer_) != VK_SUCCESS)
      return std::nullopt;

    const VkDescriptorImageInfo sampled{VK_NULL_HANDLE, pending.linear_view_,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet descriptor{};
    descriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor.dstSet = samples;
    descriptor.descriptorCount = 1;
    descriptor.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor.pImageInfo = &sampled;
    api.vkUpdateDescriptorSets(device, 1, &descriptor, 0, nullptr);
    return pending;
  }

  VkImage image() const { return image_; }
  VkFramebuffer target() const { return framebuffer_; }
  VkDescriptorSet samples() const { return sample_set_; }

 private:
  explicit HdrSurface(const VulkanDevice* owner) : owner_(owner) {}
  const VulkanDevice* owner_;
  VkImage image_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  VkImageView linear_view_ = VK_NULL_HANDLE;
  VkImageView target_view_ = VK_NULL_HANDLE;
  VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
  VkDescriptorSet sample_set_ = VK_NULL_HANDLE;
};
}  // namespace rex::ui::vulkan
