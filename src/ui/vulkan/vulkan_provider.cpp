/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/gpu_vendor.h>
#include <rex/ui/vulkan/immediate_drawer.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

#include "vulkan_creation_hooks.h"

REXCVAR_DEFINE_BOOL(vulkan_validation_enabled, false, "UI/Vulkan",
                    "Enable Vulkan validation layers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(vulkan_device, -1, "UI/Vulkan", "Vulkan device index (-1 for auto selection)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(
    gpu_vendor, "", "GPU",
    "Graphics card to use: nvidia, amd, intel or a PCI vendor id such as 0x10de (Vulkan and "
    "Direct3D 12). Empty prefers a discrete GPU over an integrated one. vulkan_device / "
    "d3d12_adapter name an index and win over this.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(vulkan_prefer_geometry_shader, true, "UI/Vulkan",
                    "Prefer physical devices supporting geometryShader when auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    vulkan_prefer_fragment_stores_and_atomics, true, "UI/Vulkan",
    "Prefer physical devices supporting fragmentStoresAndAtomics when auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vulkan_prefer_vertex_pipeline_stores_and_atomics, true, "UI/Vulkan",
                    "Prefer physical devices supporting vertexPipelineStoresAndAtomics when "
                    "auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vulkan_prefer_fill_mode_non_solid, true, "UI/Vulkan",
                    "Prefer physical devices supporting fillModeNonSolid when auto-selecting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex {
namespace ui {
namespace vulkan {

std::unique_ptr<VulkanProvider> VulkanProvider::Create(const bool with_gpu_emulation,
                                                       const bool with_presentation) {
  std::unique_ptr<VulkanProvider> provider(new VulkanProvider());

  provider->vulkan_instance_ =
      VulkanInstance::Create(with_presentation, REXCVAR_GET(vulkan_validation_enabled));
  if (!provider->vulkan_instance_) {
    return nullptr;
  }

  std::vector<VkPhysicalDevice> physical_devices;
  provider->vulkan_instance_->EnumeratePhysicalDevices(physical_devices);

  if (physical_devices.empty()) {
    REXLOG_WARN("No Vulkan physical devices available");
    return nullptr;
  }

  const VulkanInstance::Functions& ifn = provider->vulkan_instance_->functions();

  REXLOG_WARN(
      "Available Vulkan physical devices (use the 'vulkan_device' "
      "configuration variable to force a specific device):");
  for (size_t physical_device_index = 0; physical_device_index < physical_devices.size();
       ++physical_device_index) {
    VkPhysicalDeviceProperties physical_device_properties;
    ifn.vkGetPhysicalDeviceProperties(physical_devices[physical_device_index],
                                      &physical_device_properties);
    REXLOG_WARN("* {}: {}", physical_device_index, physical_device_properties.deviceName);
  }

  // patch 0390: a client (an OpenXR runtime) may name the GPU; it is tried first and alone.
  if (const RexVulkanCreationHooks* hooks = GetVulkanCreationHooks();
      hooks && hooks->physical_device) {
    const VkPhysicalDevice wanted =
        hooks->physical_device(hooks->user, provider->vulkan_instance_->instance());
    if (wanted != VK_NULL_HANDLE) {
      VkPhysicalDeviceProperties wanted_properties;
      ifn.vkGetPhysicalDeviceProperties(wanted, &wanted_properties);
      REXLOG_INFO("Vulkan device chosen by the creation hook (OpenXR runtime): {}",
                  wanted_properties.deviceName);
      if (REXCVAR_GET(vulkan_device) >= 0 || !REXCVAR_GET(gpu_vendor).empty()) {
        REXLOG_WARN("vulkan_device / gpu_vendor are ignored: the OpenXR runtime picks the GPU");
      }
      provider->vulkan_device_ = VulkanDevice::CreateIfSupported(
          provider->vulkan_instance_.get(), wanted, with_gpu_emulation, with_presentation);
      if (!provider->vulkan_device_) {
        REXLOG_WARN("The OpenXR runtime's Vulkan device '{}' cannot run the game; choosing another",
                    wanted_properties.deviceName);
      }
    }
  }

  if (!provider->vulkan_device_ && REXCVAR_GET(vulkan_device) >= 0 &&
      uint32_t(REXCVAR_GET(vulkan_device)) < physical_devices.size()) {
    provider->vulkan_device_ = VulkanDevice::CreateIfSupported(
        provider->vulkan_instance_.get(), physical_devices[REXCVAR_GET(vulkan_device)],
        with_gpu_emulation, with_presentation);
  }

  if (!provider->vulkan_device_) {
    // Order: the vendor asked for with gpu_vendor, then the supported features (the prefer_* cvars),
    // then the kind of device (discrete before integrated before virtual before software), then the
    // driver's own order. A laptop's integrated GPU is often listed first (#89).
    const bool prefer_geometry_shader = REXCVAR_GET(vulkan_prefer_geometry_shader);
    const bool prefer_fragment_stores = REXCVAR_GET(vulkan_prefer_fragment_stores_and_atomics);
    const bool prefer_vertex_stores = REXCVAR_GET(vulkan_prefer_vertex_pipeline_stores_and_atomics);
    const bool prefer_fill_mode_non_solid = REXCVAR_GET(vulkan_prefer_fill_mode_non_solid);
    const uint32_t wanted_vendor = rex::ui::PreferredGpuVendorId(REXCVAR_GET(gpu_vendor));
    struct PhysicalDeviceScore {
      VkPhysicalDevice physical_device;
      uint32_t vendor_match;
      uint32_t score;
      uint32_t type_rank;
    };
    std::vector<PhysicalDeviceScore> scored_devices;
    scored_devices.reserve(physical_devices.size());
    bool vendor_found = false;
    for (const VkPhysicalDevice physical_device : physical_devices) {
      VkPhysicalDeviceProperties properties;
      ifn.vkGetPhysicalDeviceProperties(physical_device, &properties);
      uint32_t score = 0;
      if (with_gpu_emulation) {
        VkPhysicalDeviceFeatures supported_features = {};
        ifn.vkGetPhysicalDeviceFeatures(physical_device, &supported_features);
        if (prefer_geometry_shader && supported_features.geometryShader) {
          ++score;
        }
        if (prefer_fragment_stores && supported_features.fragmentStoresAndAtomics) {
          ++score;
        }
        if (prefer_vertex_stores && supported_features.vertexPipelineStoresAndAtomics) {
          ++score;
        }
        if (prefer_fill_mode_non_solid && supported_features.fillModeNonSolid) {
          ++score;
        }
      }
      uint32_t type_rank = 0;
      switch (properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
          type_rank = 4;
          break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
          type_rank = 3;
          break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
          type_rank = 2;
          break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
          type_rank = 1;
          break;
        default:
          break;
      }
      const uint32_t vendor_match = wanted_vendor && properties.vendorID == wanted_vendor ? 1 : 0;
      vendor_found = vendor_found || vendor_match;
      scored_devices.push_back({physical_device, vendor_match, score, type_rank});
    }
    if (wanted_vendor && !vendor_found) {
      REXLOG_WARN("gpu_vendor '{}' matches no Vulkan physical device; choosing automatically",
                  REXCVAR_GET(gpu_vendor));
    }
    std::stable_sort(scored_devices.begin(), scored_devices.end(),
                     [](const PhysicalDeviceScore& a, const PhysicalDeviceScore& b) {
                       if (a.vendor_match != b.vendor_match) {
                         return a.vendor_match > b.vendor_match;
                       }
                       if (a.score != b.score) {
                         return a.score > b.score;
                       }
                       return a.type_rank > b.type_rank;
                     });

    for (const PhysicalDeviceScore& scored_device : scored_devices) {
      provider->vulkan_device_ =
          VulkanDevice::CreateIfSupported(provider->vulkan_instance_.get(),
                                          scored_device.physical_device, with_gpu_emulation,
                                          with_presentation);
      if (provider->vulkan_device_) {
        break;
      }
    }

    if (!provider->vulkan_device_) {
      REXLOG_WARN(
          "Couldn't choose a compatible Vulkan physical device or initialize a "
          "Vulkan logical device");
      return nullptr;
    }
  }

  {
    // Said once: which card the game runs on (the list above only names what exists).
    const VkPhysicalDevice chosen = provider->vulkan_device_->physical_device();
    VkPhysicalDeviceProperties chosen_properties;
    ifn.vkGetPhysicalDeviceProperties(chosen, &chosen_properties);
    size_t chosen_index = 0;
    for (size_t i = 0; i < physical_devices.size(); ++i) {
      if (physical_devices[i] == chosen) {
        chosen_index = i;
      }
    }
    const char* chosen_type = "other";
    switch (chosen_properties.deviceType) {
      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        chosen_type = "discrete";
        break;
      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        chosen_type = "integrated";
        break;
      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        chosen_type = "virtual";
        break;
      case VK_PHYSICAL_DEVICE_TYPE_CPU:
        chosen_type = "software";
        break;
      default:
        break;
    }
    REXLOG_INFO("Vulkan device in use: {}: {} (vendor 0x{:04X}, device 0x{:04X}, {}, API {}.{}.{})",
                chosen_index, chosen_properties.deviceName, chosen_properties.vendorID,
                chosen_properties.deviceID, chosen_type,
                VK_VERSION_MAJOR(chosen_properties.apiVersion),
                VK_VERSION_MINOR(chosen_properties.apiVersion),
                VK_VERSION_PATCH(chosen_properties.apiVersion));
  }

  if (with_presentation) {
    provider->ui_samplers_ = UISamplers::Create(provider->vulkan_device_.get());
    if (!provider->ui_samplers_) {
      return nullptr;
    }
  }

  return provider;
}

std::unique_ptr<Presenter> VulkanProvider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return VulkanPresenter::Create(host_gpu_loss_callback, vulkan_device(), ui_samplers());
}

std::unique_ptr<ImmediateDrawer> VulkanProvider::CreateImmediateDrawer() {
  return VulkanImmediateDrawer::Create(vulkan_device(), ui_samplers());
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
