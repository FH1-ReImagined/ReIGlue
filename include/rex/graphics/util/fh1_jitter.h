#pragma once
#include <cstdint>

namespace rex::graphics {
// NDC is relative to the cropped host viewport, not the complete guest scene.
inline float Fh1ViewportJitter(float jitter, uint32_t input_extent,
                              uint32_t scene_extent, uint32_t viewport_extent) {
  if (!input_extent || !viewport_extent) return 0.0f;
  return 2.0f * jitter * float(scene_extent) /
      (float(input_extent) * float(viewport_extent));
}
}  // namespace rex::graphics
