#pragma once

namespace rex::graphics::vulkan::fh1 {
// Screen-pitched EDRAM is reused for square reflection and shadow views.
// Their pixel coordinates must retain the integer resolve storage scale.
constexpr bool IsDisplayViewport(bool x_enabled, bool y_enabled,
                                 float x_scale, float y_scale,
                                 unsigned scissor_width, unsigned scissor_height) {
  if (!x_enabled || !y_enabled)
    return scissor_width == 1280 && scissor_height == 720;
  const float x = x_scale < 0.0f ? -x_scale : x_scale;
  const float y = y_scale < 0.0f ? -y_scale : y_scale;
  return x != y || x == 0.0f;
}
}  // namespace rex::graphics::vulkan::fh1
