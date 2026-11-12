#pragma once

#include <cstdint>

namespace rex::graphics::vulkan::fh1 {
struct RasterExtent {
  unsigned width = 0;
  unsigned height = 0;

  constexpr bool IsSpecified() const { return width || height; }
  constexpr bool IsComplete() const { return width && height; }
};

struct NativeSceneRaster {
  RasterExtent scene;
  RasterExtent hud;
  RasterExtent backing_scale;

  constexpr bool IsEnabled() const { return backing_scale.IsComplete(); }
};

constexpr unsigned RasterCeiling(unsigned value, unsigned divisor) {
  return value / divisor + unsigned(value % divisor != 0);
}

// Scaled resolves use private storage only when at least one axis is scaled.
// Keep at least one backing axis at 2 for subnative rasters; prefer the smaller
// height axis to fit devices with a tighter width limit.
constexpr NativeSceneRaster GetNativeSceneRaster(RasterExtent scene, RasterExtent hud,
                                                  RasterExtent max_backing_pixels,
                                                  unsigned max_backing_scale) {
  if (!scene.IsComplete() || !hud.IsComplete())
    return {};
  const RasterExtent max_extent{scene.width > hud.width ? scene.width : hud.width,
                                scene.height > hud.height ? scene.height : hud.height};
  RasterExtent scale{RasterCeiling(max_extent.width, 1280),
                     RasterCeiling(max_extent.height, 720)};
  if (scale.width == 1 && scale.height == 1 &&
      (scene.width != 1280 || scene.height != 720 || hud.width != 1280 || hud.height != 720)) {
    if (max_backing_pixels.height >= 1440)
      scale.height = 2;
    else
      scale.width = 2;
  }
  if (scale.width > max_backing_scale || scale.height > max_backing_scale ||
      scale.width > max_backing_pixels.width / 1280 ||
      scale.height > max_backing_pixels.height / 720)
    return {};
  return {scene, hud, scale};
}

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

// The smallest bloom pass also uses a screen pitch. Cockpit textures and
// square reflection and shadow views keep the integer raster.
constexpr bool IsFractionalSceneViewport(unsigned pitch, bool x_enabled, bool y_enabled,
                                         float x_scale, float y_scale, unsigned width,
                                         unsigned height) {
  if (pitch == 80)
    return IsDisplayViewport(x_enabled, y_enabled, x_scale, y_scale, width, height);
  return IsNativeScreenViewport(pitch, x_enabled, y_enabled, x_scale, y_scale, width, height);
}

// Raster scissor coverage is rounded at the edges. Scaling offset and size
// independently loses a pixel when both have a fractional remainder.
constexpr void ScaleRasterScissor(unsigned& offset, unsigned& extent,
                                 unsigned active_pixels, unsigned backing_pixels) {
  const uint64_t high = (uint64_t(offset) + extent) * active_pixels;
  const unsigned end = unsigned((high + backing_pixels - 1) / backing_pixels);
  offset = unsigned(uint64_t(offset) * active_pixels / backing_pixels);
  if (extent) extent = end - offset;
}

// FH1's known tiled raster is the 0.75 path fixed by 0490. Its tile offsets
// have been checked against the resolve's eight-guest-pixel alignment.
constexpr bool SupportsFractionalPredicatedTiles(RasterExtent active, RasterExtent backing) {
  const auto supported_axis = [](unsigned raster, unsigned storage) {
    return raster == storage || uint64_t(raster) * 4 == uint64_t(storage) * 3;
  };
  return supported_axis(active.width, backing.width) &&
         supported_axis(active.height, backing.height);
}

// The split has already identified this frame's final HUD color target.
template <typename ColorFormat>
constexpr bool IsNative720pHudTarget(RasterExtent raster, unsigned expected_base,
                                     unsigned color_base, unsigned pitch, bool one_sample,
                                     ColorFormat color_format, bool offset_window) {
  const bool packed_color = color_format == ColorFormat::k_2_10_10_10 ||
      color_format == ColorFormat::k_2_10_10_10_AS_10_10_10_10;
  return raster.width == 1280 && raster.height == 720 && expected_base != UINT32_MAX &&
         color_base == expected_base && pitch == 1280 && one_sample && packed_color &&
         !offset_window;
}

constexpr bool KeepIntegerRaster(RasterExtent active, RasterExtent backing, bool predicated,
                                 bool pixel_memexport, bool native_hud_target) {
  return pixel_memexport ||
         (predicated && !native_hud_target && !SupportsFractionalPredicatedTiles(active, backing));
}

constexpr bool IsNative720pHudResolve(bool hud_target, bool color_zero, RasterExtent extent,
                                      bool source_offset) {
  return hud_target && color_zero && !source_offset && extent.width == 1280 &&
         extent.height == 720;
}

// A single coordinate transform is valid only when all owners in the EDRAM
// copy rectangle wrote with the same raster. Unknown owners use integer UVs.
struct ResolveProducerRaster {
  bool has_fractional = false;
  bool has_full_or_unknown = false;

  constexpr void Add(bool fractional) {
    has_fractional |= fractional;
    has_full_or_unknown |= !fractional;
  }
  constexpr bool IsFractional() const { return has_fractional && !has_full_or_unknown; }
  constexpr bool IsMixed() const { return has_fractional && has_full_or_unknown; }
};

// Predicated tiling on the fractional raster (#785). FH1 draws some scenes (the street race
// intro's crowd shot) in three 1280x256 EDRAM tiles with 4x MSAA, each moved into place by a
// negative window offset and resolved to its own rows of the 1280x720 texture. On the HUD
// target at a 1080p output (raster 0.75 of render scale 2) a tile's display-scale draws fill
// only the top-left 0.75 of the tile, so the tile at screen row 256 must land at row 192 of the
// texture and only its first 192 rows hold pixels: the HUD's shaders read every screen texture
// at 0.75 of its coordinates. Kept at row 256, the texture holds the tiles' unrendered rows at
// 192-256 and 448-512, the two grey bands across the screen. Gives the destination position
// and the size of the copy for a tile at screen position (-window_x_offset, -window_y_offset);
// false when nothing moves (the first tile, an untiled resolve) or the moved position is not a
// whole multiple of the 8-pixel resolve alignment.
struct FractionalTileResolve {
  unsigned x, y, width, height;
};
constexpr bool GetFractionalTileResolve(int window_x_offset, int window_y_offset, float scale_x,
                                        float scale_y, unsigned width, unsigned height,
                                        FractionalTileResolve& out) {
  if (window_x_offset > 0 || window_y_offset > 0 || (!window_x_offset && !window_y_offset) ||
      !(scale_x > 0.0f && scale_x <= 1.0f && scale_y > 0.0f && scale_y <= 1.0f) ||
      (scale_x == 1.0f && scale_y == 1.0f) || !width || !height)
    return false;
  const float x = float(-window_x_offset) * scale_x;
  const float y = float(-window_y_offset) * scale_y;
  const unsigned xi = unsigned(x), yi = unsigned(y);
  if (float(xi) != x || float(yi) != y || (xi & 7) || (yi & 7))
    return false;
  const auto shrink = [](unsigned size, float scale) {
    const float scaled = float(size) * scale;
    unsigned aligned = unsigned(scaled);
    if (float(aligned) < scaled)
      ++aligned;
    aligned = (aligned + 7) & ~7u;
    return aligned < 8 ? 8u : (aligned > size ? size : aligned);
  };
  out.x = xi;
  out.y = yi;
  out.width = shrink(width, scale_x);
  out.height = shrink(height, scale_y);
  return true;
}
}  // namespace rex::graphics::vulkan::fh1
