#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan { class VulkanDevice; }
namespace rex::graphics::vulkan {
struct Fh1ShaderBinding {
  std::string name;
  uint32_t binding;
  bool block;
};
// Resolves a GLSL #include: the requested name and the including file's name in, the
// resolved name and its text out.
using Fh1ShaderInclude = std::function<bool(const std::string& requested, const std::string& includer,
                                            std::string& resolved, std::string_view& text)>;
bool CompileFh1ComputeReflected(std::string_view source, std::string_view preamble,
                                const Fh1ShaderInclude& include, std::vector<uint32_t>& spirv,
                                std::vector<Fh1ShaderBinding>& bindings, std::string& error);

// AMD FidelityFX FSR 3.1 upscaler (no frame generation). The SDK's GLSL passes are
// embedded and compiled at startup; this class owns FSR's internal images and replays
// the dispatch sequence of the SDK's ffx_fsr3upscaler.cpp. All images stay in GENERAL.
class Fh1Fsr3 {
 public:
  struct Frame {
    float jitter_x = 0, jitter_y = 0;  // pixels, the offset the scene was rendered with
    bool reset = true;
    float delta_time_ms = 16.7f;
    // View-space depth = device_to_view[1] / (device depth - device_to_view[0]).
    float device_to_view[2] = {0, 1};
  };
  // Render scale ratio for quality modes; 0 for an unknown mode.
  static float Ratio(std::string_view mode);
  static uint32_t JitterPhases(uint32_t render_width, uint32_t output_width);
  explicit Fh1Fsr3(const ui::vulkan::VulkanDevice* device);
  ~Fh1Fsr3();
  // color, depth (R32F, inverted), motion (RG16F, pixels, current to previous) at the
  // render size; output (RGBA16F) at the output size.
  bool Initialize(uint32_t render_width, uint32_t render_height, uint32_t output_width,
                  uint32_t output_height, VkImageView color, VkImageView depth, VkImageView motion,
                  VkImageView output, float sharpness, PFN_vkCmdUpdateBuffer update_buffer);
  void Execute(VkCommandBuffer command, const Frame& frame);

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace rex::graphics::vulkan
