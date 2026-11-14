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

// A resolve drawn on the fractional raster, copied back to guest memory (#879). The HUD's
// shaders read every screen texture at 0.75 of its coordinates, so in the scaled resolve a
// picture drawn there fills the top-left 0.75. The CPU reads guest memory as it is: a Big
// Shot's tile, a replay movie's frame, a photo and its thumbnail were copied back from the whole
// scaled surface, one host pixel a block, and came out at three quarters of their size in the
// top left, the rest black. Such a copy takes the raster's part: each guest pixel is the
// average of the host pixels it covers there. Only a whole picture (the copy starts at the
// surface's and at the texture's origin); a predicated tiling tile keeps the copy it had.
struct FractionalReadback {
  unsigned width = 0, height = 0;                // the copy, in guest pixels
  unsigned raster_width = 0, raster_height = 0;  // the scaled copy's host pixels with the picture
  unsigned pitch_tiles = 0;                      // the texture's pitch in 32-pixel tiles
  constexpr bool valid() const { return width != 0; }
};
// False when the raster fills the scaled surface (1440p, 4K), for an empty copy, or when the
// picture's part is not a whole number of host pixels.
constexpr bool GetFractionalReadback(unsigned width, unsigned height, unsigned backing_x,
                                     unsigned backing_y, float scale_x, float scale_y,
                                     unsigned pitch_tiles, FractionalReadback& out) {
  if (!width || !height || !backing_x || !backing_y || !pitch_tiles ||
      !(scale_x > 0.0f && scale_x <= 1.0f && scale_y > 0.0f && scale_y <= 1.0f) ||
      (scale_x == 1.0f && scale_y == 1.0f) || width > pitch_tiles * 32)
    return false;
  const float raster_x = float(width * backing_x) * scale_x;
  const float raster_y = float(height * backing_y) * scale_y;
  const unsigned raster_width = unsigned(raster_x), raster_height = unsigned(raster_y);
  if (float(raster_width) != raster_x || float(raster_height) != raster_y || !raster_width ||
      !raster_height)
    return false;
  out.width = width;
  out.height = height;
  out.raster_width = raster_width;
  out.raster_height = raster_height;
  out.pitch_tiles = pitch_tiles;
  return true;
}
// The host pixels one guest pixel covers along an axis where `size` guest pixels take `raster`
// host pixels: the first one, and each one's share of the guest pixel (they add up to 1). At
// most four, for a backing scale up to 3. The copy's shader does the same sums.
struct RasterTaps {
  unsigned first = 0;
  float weights[4] = {};
};
constexpr RasterTaps GetRasterTaps(unsigned guest, unsigned size, unsigned raster) {
  RasterTaps taps;
  const float ratio = float(raster) / float(size);
  const float low = float(guest) * ratio, high = low + ratio;
  taps.first = unsigned(low);
  for (unsigned i = 0; i < 4; ++i) {
    const float pixel = float(taps.first + i);
    const float from = low > pixel ? low : pixel;
    const float to = high < pixel + 1.0f ? high : pixel + 1.0f;
    taps.weights[i] = to > from ? (to - from) / ratio : 0.0f;
  }
  return taps;
}
}  // namespace rex::graphics::vulkan::fh1
