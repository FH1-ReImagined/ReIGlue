// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#include <rex/ui/vulkan/image_compute_transaction.h>
#include <array>
#include <cassert>
#include <set>
#include <type_traits>

namespace {
template <typename T> T Handle(uintptr_t value) {
  if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
  else return T(value);
}
struct FakeApi {
  mutable unsigned call = 0;
  unsigned fail = 0;
  mutable std::set<unsigned> live;
  mutable uint32_t sampled = 0, storage = 0, max_sets = 0;
  bool Next() const { return ++call != fail; }
  template <typename T> VkResult Make(T* result, unsigned id) const {
    if (!Next()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *result = Handle<T>(id); live.insert(id); return VK_SUCCESS;
  }
  VkResult vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo* info,
      const VkAllocationCallbacks*, VkDescriptorSetLayout* result) const {
    assert(info->bindingCount == 3);
    assert(info->pBindings[2].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    return Make(result, 1);
  }
  VkResult vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo* info,
      const VkAllocationCallbacks*, VkPipelineLayout* result) const {
    assert(info->pushConstantRangeCount == 1 && info->pPushConstantRanges[0].size == 16);
    return Make(result, 2);
  }
  VkResult vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo* info,
      const VkAllocationCallbacks*, VkDescriptorPool* result) const {
    max_sets = info->maxSets;
    for (uint32_t i = 0; i < info->poolSizeCount; ++i) {
      const auto& entry = info->pPoolSizes[i];
      if (entry.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) sampled = entry.descriptorCount;
      if (entry.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) storage = entry.descriptorCount;
    }
    return Make(result, 4);
  }
  VkResult vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo* info,
      VkDescriptorSet* result) const {
    if (!Next()) return VK_ERROR_OUT_OF_POOL_MEMORY;
    for (uint32_t i = 0; i < info->descriptorSetCount; ++i) result[i] = Handle<VkDescriptorSet>(10 + i);
    return VK_SUCCESS;
  }
  void Drop(unsigned id) const { assert(live.erase(id) == 1); }
  void vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*) const { Drop(3); }
  void vkDestroyDescriptorPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*) const { Drop(4); }
  void vkDestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) const { Drop(2); }
  void vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) const { Drop(1); }
};
struct FakeDevice {
  FakeApi api;
  VkDevice device() const { return Handle<VkDevice>(100); }
  const FakeApi& functions() const { return api; }
};
}

int main() {
  using Transaction = rex::ui::vulkan::ImageComputeTransaction<FakeDevice>;
  const std::array<uint32_t, 1> shader{0x07230203};
  const std::array signature{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
  const std::array extra{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3},
                        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}};
  auto pipeline = [](const FakeDevice& device, VkPipelineLayout, std::span<const uint32_t>) {
    VkPipeline result = VK_NULL_HANDLE;
    device.api.Make(&result, 3);
    return result;
  };
  // Every Vulkan allocation failure leaves no resource owned or published.
  for (unsigned failed_call = 1; failed_call <= 5; ++failed_call) {
    FakeDevice device;
    device.api.fail = failed_call;
    auto effect = Transaction::Create(device, shader, signature, 3, 16, pipeline, 6, extra);
    assert(!effect && device.api.live.empty());
    assert(device.api.call == failed_call);
  }
  FakeDevice device;
  {
    auto effect = Transaction::Create(device, shader, signature, 3, 16, pipeline, 6, extra);
    assert(effect && effect->sets.size() == 3 && device.api.live.size() == 4);
    assert(device.api.sampled == 9 && device.api.storage == 6 && device.api.max_sets == 9);
  }
  assert(device.api.live.empty());
  {
    auto effect = Transaction::Create(device, shader, signature, 3, 16, pipeline);
    effect->Release();
  }
  assert(device.api.live.size() == 4); // Explicitly transferred to caller ownership.
  device.api.live.clear();
  const unsigned calls = device.api.call;
  assert(!Transaction::Create(device, shader, signature, 0, 16, pipeline));
  assert(!Transaction::Create(device, shader, signature, 3, 3, pipeline));
  assert(device.api.call == calls);
}
