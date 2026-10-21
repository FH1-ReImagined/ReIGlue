#pragma once
#include <memory>
#include <rex/graphics/vulkan/fh1_dlss.h>
namespace rex::ui::vulkan {
class VulkanDevice;
}
namespace rex::graphics::vulkan {
// Metal objects remain GPU-resident. Two shared timeline events order Vulkan
// preparation, the native upscaler, and Vulkan HUD composition across queues.
class Fh1MetalFx {
 public:
  struct Frame {
    float jitter_x = 0, jitter_y = 0;
    bool reset = true;
  };
  explicit Fh1MetalFx(const ui::vulkan::VulkanDevice* device);
  ~Fh1MetalFx();
  bool Initialize(uint32_t width, uint32_t height, uint32_t output_width, uint32_t output_height,
                  VkImage color, VkImage output, VkImage depth, VkImage motion, bool temporal);
  Fh1Dlss::NativeSubmission Schedule(const Frame& frame);

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace rex::graphics::vulkan
