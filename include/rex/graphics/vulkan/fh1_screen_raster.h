#pragma once

namespace rex::graphics::vulkan::fh1 {
// Screen-pitched EDRAM is reused for square reflection and shadow views.
// Their pixel coordinates must retain the integer resolve storage scale.
constexpr bool IsDisplayViewport(bool x_enabled, bool y_enabled, float x_scale, float y_scale,
                                 unsigned scissor_width, unsigned scissor_height) {
  if (!x_enabled || !y_enabled)
    return scissor_width == 1280 && scissor_height == 720;
  const float x = x_scale < 0.0f ? -x_scale : x_scale;
  const float y = y_scale < 0.0f ? -y_scale : y_scale;
  return x != y || x == 0.0f;
}

// The cockpit builds its instrument texture in a screen-pitched surface, then
// samples the 512x288 image and its smaller resolves in car materials. These
// textures use integer resolve coordinates, with no native-screen UV correction.
// Match active viewport AND scissor dimensions, not the padded allocation pitch.
constexpr bool IsInstrumentViewport(unsigned pitch, bool x_enabled, bool y_enabled, float x_scale,
                                    float y_scale, unsigned scissor_width,
                                    unsigned scissor_height) {
  if (pitch != 1280 || !x_enabled || !y_enabled)
    return false;
  const float x = x_scale < 0.0f ? -x_scale : x_scale;
  const float y = y_scale < 0.0f ? -y_scale : y_scale;
  if (scissor_width != 2.0f * x || scissor_height != 2.0f * y)
    return false;
  return (scissor_width == 512 && scissor_height == 288) ||
         (scissor_width == 256 && scissor_height == 144) ||
         (scissor_width == 128 && scissor_height == 72) ||
         (scissor_width == 64 && scissor_height == 36);
}

constexpr bool IsNativeScreenViewport(unsigned pitch, bool x_enabled, bool y_enabled, float x_scale,
                                      float y_scale, unsigned scissor_width,
                                      unsigned scissor_height) {
  if (pitch != 1280 && pitch != 640 && pitch != 320 && pitch != 160)
    return false;
  return !IsInstrumentViewport(pitch, x_enabled, y_enabled, x_scale, y_scale, scissor_width,
                               scissor_height) &&
         IsDisplayViewport(x_enabled, y_enabled, x_scale, y_scale, scissor_width, scissor_height);
}
}  // namespace rex::graphics::vulkan::fh1
