#pragma once
#include <memory>
#include <atomic>
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan { class VulkanDevice; }
namespace rex::graphics::vulkan {
bool CompileFh1Compute(std::string_view source, std::vector<uint32_t>& spirv, std::string& error);
// Owns NGX and its intermediate images. Calls execute on the submission thread.
class Fh1Dlss {
 public:
  struct NativeSubmission {
    VkSemaphore ready = VK_NULL_HANDLE, complete = VK_NULL_HANDLE;
    uint64_t value = 0;
    std::function<bool()> encode;
  };
  using SplitSubmission = std::function<bool(VkCommandBuffer&, const NativeSubmission&)>;
  struct FrameInput {
    VkImageView depth_view = VK_NULL_HANDLE;
    std::array<float, 136 * 4> constants{};
    float jitter_x = 0, jitter_y = 0;
  };
  explicit Fh1Dlss(const ui::vulkan::VulkanDevice* device, uint32_t output_width, uint32_t output_height, std::string_view mode);
  ~Fh1Dlss();
  bool Initialize();
  uint32_t width() const;
  uint32_t height() const;
  uint32_t output_width() const;
  uint32_t output_height() const;
  void SetSceneBacking(uint32_t scale);
  static void ReportStatus(std::string_view message);
  void Execute(VkCommandBuffer command_buffer, VkImage scene, VkImage destination, const FrameInput& input);
  bool UsesMetalFx() const;
  bool ExecuteMetalFx(VkCommandBuffer& command_buffer, VkImage scene, VkImage destination,
                      const FrameInput& input, const SplitSubmission& split);
  bool Evaluating() const { return evaluating_.load(std::memory_order_acquire); }
  void BeginFrame(uint64_t frame);
  void Observe(VkImageView depth_view, const float* constants) {
    input_.depth_view = depth_view;
    std::copy_n(constants, input_.constants.size(), input_.constants.begin());
    input_.constants[134 * 4] = input_.jitter_x;
    input_.constants[134 * 4 + 1] = input_.jitter_y;
  }
  const FrameInput& input() const { return input_; }
 private:
  std::atomic<bool> evaluating_{false};
  FrameInput input_;
  bool jitter_was_active_ = false;
  struct State;
  std::unique_ptr<State> state_;
};
}
