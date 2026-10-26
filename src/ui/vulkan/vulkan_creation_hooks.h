/**
 * Vulkan instance and device creation through a client (Forza Horizon patch 0390).
 *
 * An OpenXR runtime with XR_KHR_vulkan_enable2 has to create the Vulkan instance and device
 * itself (it adds the extensions it needs) and pick the physical device (the GPU the headset
 * is connected to). A client registers these callbacks before the graphics provider exists,
 * through the C export rexui_SetVulkanCreationHooks. No public header declares them: clients
 * compiled against the stock SDK headers declare the same structure and find the export at run
 * time; the structure starts with its size so it can grow.
 */
#pragma once

#include <cstdint>

#include <rex/ui/vulkan/api.h>

extern "C" {
struct RexVulkanCreationHooks {
  uint32_t size;  // sizeof(RexVulkanCreationHooks)
  void* user;
  // Replaces vkCreateInstance (null: the SDK's call).
  VkResult (*create_instance)(void* user, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                              const VkInstanceCreateInfo* create_info, VkInstance* instance);
  // The physical device to try first and alone (VK_NULL_HANDLE: the SDK chooses as usual).
  VkPhysicalDevice (*physical_device)(void* user, VkInstance instance);
  // Replaces vkCreateDevice (null: the SDK's call).
  VkResult (*create_device)(void* user, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                            VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create_info,
                            VkDevice* device);
};
}

namespace rex::ui::vulkan {
// The registered hooks, or null.
const RexVulkanCreationHooks* GetVulkanCreationHooks();
}  // namespace rex::ui::vulkan
