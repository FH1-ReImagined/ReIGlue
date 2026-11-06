#pragma once
// FH1 (#551): the extension the GPU emulation device leaves breadcrumbs with, so that a lost
// device names the draw the GPU was on (graphics/vulkan/fh1_device_lost_trail.h). Set when the
// device is created (cvar gpu_device_lost_breadcrumbs, default on).

namespace rex::ui::vulkan {

enum class Fh1BreadcrumbExtension : int {
  kNone = 0,
  kAmdBufferMarker = 1,  // VK_AMD_buffer_marker (AMD's drivers, RADV)
  kNvCheckpoints = 2,    // VK_NV_device_diagnostic_checkpoints (NVIDIA)
};

Fh1BreadcrumbExtension Fh1GetBreadcrumbExtension();

}  // namespace rex::ui::vulkan
