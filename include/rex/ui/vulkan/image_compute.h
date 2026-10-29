// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <rex/ui/vulkan/util.h>
#include <rex/ui/vulkan/image_compute_transaction.h>
#include <array>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace rex::ui::vulkan {
using ImageComputeObjects = ImageComputeTransaction<VulkanDevice>;

inline VkPipeline BuildImageComputePipeline(const VulkanDevice& device, VkPipelineLayout layout,
                                             std::span<const uint32_t> shader) {
  return util::CreateComputePipeline(&device, layout, shader.data(), shader.size_bytes());
}

struct ComputeImageBinding {
  VkImageView view;
  VkDescriptorType type;
  VkImageLayout layout;
};
template <size_t Count>
void WriteComputeImages(const VulkanDevice& owner, VkDescriptorSet set,
                        const std::array<ComputeImageBinding, Count>& images) {
  std::array<VkDescriptorImageInfo, Count> values{};
  std::array<VkWriteDescriptorSet, Count> writes{};
  for (uint32_t binding = 0; binding != images.size(); ++binding) {
    values[binding] = {VK_NULL_HANDLE, images[binding].view, images[binding].layout};
    auto& write = writes[binding];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = images[binding].type;
    write.pImageInfo = &values[binding];
  }
  owner.functions().vkUpdateDescriptorSets(owner.device(), uint32_t(writes.size()), writes.data(), 0, nullptr);
}

inline bool AllocateImageSets(const VulkanDevice& owner, VkDescriptorPool pool,
                              VkDescriptorSetLayout layout, std::span<VkDescriptorSet> result) {
  const std::vector<VkDescriptorSetLayout> layouts(result.size(), layout);
  VkDescriptorSetAllocateInfo description{};
  description.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  description.descriptorPool = pool;
  description.descriptorSetCount = uint32_t(result.size());
  description.pSetLayouts = layouts.data();
  return owner.functions().vkAllocateDescriptorSets(owner.device(), &description, result.data()) == VK_SUCCESS;
}

template <typename Recorder>
void RecordCoverageDispatch(Recorder& commands, VkPipelineLayout layout, VkDescriptorSet set,
                            uint32_t width, uint32_t height, bool swap_rb, float source_quantum) {
  const struct Parameters { uint32_t width, height, swap_rb; float source_quantum; }
      size{width, height, uint32_t(swap_rb), source_quantum};
  static_assert(sizeof(size) == 16);
  commands.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
  commands.CmdVkPushConstants(layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), &size);
  commands.CmdVkDispatch((width + 15) / 16, (height + 7) / 8, 1);
}
}  // namespace rex::ui::vulkan
