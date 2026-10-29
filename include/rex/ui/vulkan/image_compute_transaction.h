// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <vulkan/vulkan_core.h>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace rex::ui::vulkan {
// Transactional construction for a compute effect whose bindings are images.
// Call Release only after handing every handle to an established lifetime owner.
template <typename Device>
class ImageComputeTransaction {
 public:
  ImageComputeTransaction(const ImageComputeTransaction&) = delete;
  ImageComputeTransaction& operator=(const ImageComputeTransaction&) = delete;
  ~ImageComputeTransaction() {
    if (!owner_) return;
    const auto device = owner_->device();
    const auto& api = owner_->functions();
    if (pipeline) api.vkDestroyPipeline(device, pipeline, nullptr);
    if (pool) api.vkDestroyDescriptorPool(device, pool, nullptr);
    if (layout) api.vkDestroyPipelineLayout(device, layout, nullptr);
    if (set_layout) api.vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
  }
  void Release() { owner_ = nullptr; }

  template <typename PipelineBuilder>
  static std::unique_ptr<ImageComputeTransaction> Create(
      const Device& owner, std::span<const uint32_t> shader,
      std::span<const VkDescriptorType> signature, uint32_t frames, uint32_t push_bytes, PipelineBuilder&& build_pipeline,
      uint32_t extra_sets = 0, std::span<const VkDescriptorPoolSize> extra_descriptors = {}) {
    if (shader.empty() || signature.empty() || !frames || push_bytes % 4) return nullptr;
    auto result = std::unique_ptr<ImageComputeTransaction>(new ImageComputeTransaction(owner));
    const auto& api = owner.functions();
    const auto device = owner.device();
    std::vector<VkDescriptorSetLayoutBinding> bindings(signature.size());
    std::map<VkDescriptorType, uint32_t> counts;
    for (uint32_t binding = 0; binding != signature.size(); ++binding) {
      bindings[binding] = {binding, signature[binding], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      counts[signature[binding]] += frames;
    }
    VkDescriptorSetLayoutCreateInfo set_description{};
    set_description.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_description.bindingCount = uint32_t(bindings.size());
    set_description.pBindings = bindings.data();
    if (api.vkCreateDescriptorSetLayout(device, &set_description, nullptr, &result->set_layout) != VK_SUCCESS)
      return nullptr;
    const VkPushConstantRange parameters{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
    VkPipelineLayoutCreateInfo layout_description{};
    layout_description.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_description.setLayoutCount = 1;
    layout_description.pSetLayouts = &result->set_layout;
    layout_description.pushConstantRangeCount = push_bytes ? 1 : 0;
    layout_description.pPushConstantRanges = &parameters;
    if (api.vkCreatePipelineLayout(device, &layout_description, nullptr, &result->layout) != VK_SUCCESS)
      return nullptr;
    result->pipeline = build_pipeline(owner, result->layout, shader);
    if (!result->pipeline) return nullptr;

    for (const auto& extra : extra_descriptors) counts[extra.type] += extra.descriptorCount;
    std::vector<VkDescriptorPoolSize> sizes;
    for (auto [type, count] : counts) sizes.push_back({type, count});
    VkDescriptorPoolCreateInfo pool_description{};
    pool_description.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_description.maxSets = frames + extra_sets;
    pool_description.poolSizeCount = uint32_t(sizes.size());
    pool_description.pPoolSizes = sizes.data();
    if (api.vkCreateDescriptorPool(device, &pool_description, nullptr, &result->pool) != VK_SUCCESS)
      return nullptr;
    const std::vector<VkDescriptorSetLayout> layouts(frames, result->set_layout);
    result->sets.resize(frames);
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = result->pool;
    allocation.descriptorSetCount = frames;
    allocation.pSetLayouts = layouts.data();
    if (api.vkAllocateDescriptorSets(device, &allocation, result->sets.data()) != VK_SUCCESS)
      return nullptr;
    return result;
  }

  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> sets;
 private:
  explicit ImageComputeTransaction(const Device& owner) : owner_(&owner) {}
  const Device* owner_;
};

}  // namespace rex::ui::vulkan
