#pragma once
#include <memory>
#include <rex/ui/vulkan/api.h>
namespace rex::ui::vulkan { class VulkanDevice; }
namespace rex::graphics::vulkan {
// SMAA 1x: edge detection -> blend weights -> neighborhood blending.
// Invoked on the scene before the game draws native-resolution HUD elements.
class Fh1Smaa {
 public:
  explicit Fh1Smaa(const ui::vulkan::VulkanDevice* device);
  ~Fh1Smaa();
  bool Initialize(uint32_t width, uint32_t height, VkImageView color, VkImageView output);
  void Execute(VkCommandBuffer command);
 private:
  struct State;
  std::unique_ptr<State> state_;
};
}
