// FH1 (#380): the decisions of the world ambient occlusion (fh1_world_ao.cpp), without Vulkan,
// so tools/test_world_ao.cpp can check them.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace rex::graphics::vulkan::fh1_world_ao {

// Replacing images may wait for the queue and close the current draw's submission. Resume
// and refresh its texture references before continuing, including when allocation failed
// and the guest draw will proceed without AO. A failed resume must abort the draw.
enum class ResourceStatus { kReady, kUnavailable, kSubmissionFailed };
template <typename Update, typename IsOpen, typename Resume, typename Refresh>
ResourceStatus PrepareResourcesForDraw(Update&& update, IsOpen&& is_open, Resume&& resume,
                                      Refresh&& refresh) {
  const bool ready = update();
  if (!is_open()) {
    if (!resume()) return ResourceStatus::kSubmissionFailed;
    refresh();
  }
  return ready ? ResourceStatus::kReady : ResourceStatus::kUnavailable;
}

// The game's `world_ambient_occlusion`: 0 off, 1 low, 2 high; anything else is off.
inline int Level(std::string_view value) {
  return value == "high" ? 2 : value == "low" ? 1 : 0;
}
// The occlusion's strength (SAO intensity) for a level.
inline float Intensity(int level) { return level >= 2 ? 1.0f : 0.6f; }

// The engine effect's ApplyShadows techniques: the pixel shaders of the shadow mask's passes
// (tf0 the scene depth, tf1 the cascade's shadow map).
inline constexpr uint64_t kShadowPasses[] = {0x93626E75D17576C5ull, 0x26EB620936001876ull,
                                             0x11824C2EC1B156C6ull, 0x26C4FD34AECBE4DEull,
                                             0xFCDF9BE8C57F7D01ull, 0x8418C40F121D7EA7ull,
                                             0x22DA22B5639EBAE4ull};
inline bool IsShadowPass(uint64_t hash) {
  return std::find(std::begin(kShadowPasses), std::end(kShadowPasses), hash) !=
         std::end(kShadowPasses);
}

// Where the occlusion is computed: half the scene's resolution from 1920 pixels wide (render
// scale 2 and above), the scene's own below.
struct Grid {
  uint32_t downsample, width, height;
};
inline Grid GridFor(uint32_t width, uint32_t height) {
  const uint32_t d = width >= 1920 ? 2 : 1;
  return {d, (width + d - 1) / d, (height + d - 1) / d};
}

// The shadow passes rebuild a pixel's view position from the depth with their pixel constants:
// view z = c25.y / (1 - c25.x - depth), x = z * ndc.x / c0.x, y = z * ndc.y / c1.y. Out: A, B
// and the tangents of half the field of view in x and y.
inline bool Projection(float c0x, float c1y, float c25x, float c25y, float out[4]) {
  if (!std::isfinite(c0x) || !std::isfinite(c1y) || !std::isfinite(c25x) ||
      !std::isfinite(c25y) || c25y == 0.0f || std::fabs(c0x) < 1e-3f || std::fabs(c1y) < 1e-3f)
    return false;
  out[0] = c25x;
  out[1] = c25y;
  out[2] = 1.0f / std::fabs(c0x);
  out[3] = 1.0f / std::fabs(c1y);
  return true;
}
// The view distance of a depth value with those constants (the compute shaders' viewZ); 0 for
// the sky.
inline float ViewZ(const float projection[4], float depth) {
  const float den = 1.0f - projection[0] - depth;
  const float z = std::fabs(projection[1] / (std::fabs(den) > 1e-20f ? den : 1e-20f));
  return depth <= 0.0f || !std::isfinite(z) ? 0.0f : z;
}

// The depth pre-pass's stencil (the low byte of the resolved depth): 0 the static world,
// 3 x an object id for the cars, 128 the ground around the player's car, 129 the
// alpha-tested trees. Cars and trees keep their own shading (the compose shader's
// keepsItsShading).
inline bool KeepsItsShading(uint32_t stencil) {
  return (stencil >= 1 && stencil < 128) || stencil == 129;
}

// Radius (m), and the distance range over which the occlusion fades out (m).
inline constexpr float kRadius = 1.5f, kFadeStart = 60.0f, kFadeEnd = 160.0f;

}  // namespace rex::graphics::vulkan::fh1_world_ao
