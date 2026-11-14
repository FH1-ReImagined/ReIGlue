#include <rex/graphics/vulkan/fh1_screen_raster.h>

#include <cstdio>
#include <initializer_list>

int main() {
  using namespace rex::graphics::vulkan::fh1;
  unsigned failures = 0;
  auto check = [&](bool result) { failures += !result; };
  // Actual cockpit producer and downsample chain, with either viewport Y sign.
  for (unsigned width : {512u, 256u, 128u, 64u}) {
    const unsigned height = width * 9 / 16;
    for (float sign : {-1.0f, 1.0f}) {
      const float x = width / 2.0f, y = sign * height / 2.0f;
      check(IsInstrumentViewport(1280, true, true, x, y, width, height));
      check(!IsNativeScreenViewport(1280, true, true, x, y, width, height));
      check(!IsInstrumentViewport(640, true, true, x, y, width, height));
      check(!IsInstrumentViewport(1280, true, true, x, y, 1280, 720));
      check(!IsInstrumentViewport(1280, false, true, x, y, width, height));
      check(!IsInstrumentViewport(1280, true, true, x + 0.5f, y, width, height));
    }
  }
  // Preserve the existing policy for display, padded bloom, square reflections,
  // software viewports, unsupported pitches and dimensions close to the family.
  for (unsigned pitch : {1280u, 640u, 320u, 160u, 80u, 512u}) {
    for (unsigned width : {1280u, 640u, 512u, 320u, 256u, 160u, 128u, 80u, 64u}) {
      for (unsigned height : {720u, 360u, 288u, 192u, 144u, 96u, 72u, 45u, 36u, width}) {
        for (bool xe : {false, true})
          for (bool ye : {false, true}) {
            const float x = width / 2.0f, y = -height / 2.0f;
            const bool supported = pitch == 1280 || pitch == 640 || pitch == 320 || pitch == 160;
            const bool prior = supported && IsDisplayViewport(xe, ye, x, y, width, height);
            const bool instrument = IsInstrumentViewport(pitch, xe, ye, x, y, width, height);
            check(IsNativeScreenViewport(pitch, xe, ye, x, y, width, height) ==
                  (prior && !instrument));
          }
      }
    }
  }
  // Square faces remain integer; padded bloom and cinematic display remain native.
  check(!IsNativeScreenViewport(1280, true, true, 256, -256, 512, 512));
  check(IsNativeScreenViewport(1280, true, true, 160, -96, 320, 192));
  check(IsNativeScreenViewport(1280, true, true, 80, -96, 160, 192));
  check(IsNativeScreenViewport(1280, true, true, 640, -232, 1280, 464));
  check(IsNativeScreenViewport(1280, false, false, 1, 1, 1280, 720));
  check(!IsNativeScreenViewport(1280, false, false, 1, 1, 512, 288));
  // #785: the race intro's three 1280x256 tiles on the 1080p HUD target (raster 0.75).
  FractionalTileResolve tile{};
  check(GetFractionalTileResolve(0, -256, 0.75f, 0.75f, 1280, 256, tile) && tile.x == 0 &&
        tile.y == 192 && tile.width == 960 && tile.height == 192);
  check(GetFractionalTileResolve(0, -512, 0.75f, 0.75f, 1280, 208, tile) && tile.x == 0 &&
        tile.y == 384 && tile.width == 960 && tile.height == 160);
  // Columns too; the moved copy never grows past the tile.
  check(GetFractionalTileResolve(-640, 0, 0.75f, 0.75f, 640, 720, tile) && tile.x == 480 &&
        tile.y == 0 && tile.width == 480 && tile.height == 544);
  check(GetFractionalTileResolve(0, -256, 1.0f, 0.75f, 1280, 8, tile) && tile.width == 1280 &&
        tile.height == 8);
  // Nothing moves: the first tile, an untiled resolve, an integer raster (1440p, 4K), a
  // position off the 8-pixel alignment (0.75 * 8 = 6), a positive offset.
  check(!GetFractionalTileResolve(0, 0, 0.75f, 0.75f, 1280, 256, tile));
  check(!GetFractionalTileResolve(0, -256, 1.0f, 1.0f, 1280, 256, tile));
  check(!GetFractionalTileResolve(0, -8, 0.75f, 0.75f, 1280, 256, tile));
  check(!GetFractionalTileResolve(0, 256, 0.75f, 0.75f, 1280, 256, tile));
  check(!GetFractionalTileResolve(0, -256, 0.75f, 0.75f, 1280, 0, tile));
  check(!GetFractionalTileResolve(0, -256, 1.5f, 0.75f, 1280, 256, tile));
  // #879: a whole picture drawn on the 1080p HUD target (raster 0.75 of render scale 2) is
  // copied back from the raster's part: a Big Shot's tile, a movie's frame, a photo's thumbnail.
  FractionalReadback copy{};
  check(GetFractionalReadback(1280, 720, 2, 2, 0.75f, 0.75f, 40, copy) && copy.width == 1280 &&
        copy.height == 720 && copy.raster_width == 1920 && copy.raster_height == 1080 &&
        copy.pitch_tiles == 40 && copy.valid());
  check(GetFractionalReadback(640, 360, 2, 2, 0.75f, 0.75f, 20, copy) && copy.raster_width == 960 &&
        copy.raster_height == 540);
  check(GetFractionalReadback(416, 240, 2, 2, 0.75f, 0.75f, 13, copy) && copy.raster_width == 624 &&
        copy.raster_height == 360);
  // Nothing changes: an integer raster (1440p, 4K), an empty copy, a part that is not whole
  // host pixels, a copy wider than its texture.
  copy = {};
  check(!GetFractionalReadback(1280, 720, 2, 2, 1.0f, 1.0f, 40, copy) && !copy.valid());
  check(!GetFractionalReadback(1280, 720, 3, 3, 1.0f, 1.0f, 40, copy));
  check(!GetFractionalReadback(0, 720, 2, 2, 0.75f, 0.75f, 40, copy));
  check(!GetFractionalReadback(1280, 0, 2, 2, 0.75f, 0.75f, 40, copy));
  check(!GetFractionalReadback(1280, 720, 2, 2, 0.77f, 0.75f, 40, copy));
  check(!GetFractionalReadback(1280, 720, 2, 2, 0.75f, 1.5f, 40, copy));
  check(!GetFractionalReadback(1280, 720, 2, 2, 0.75f, 0.75f, 39, copy));
  check(!GetFractionalReadback(1280, 720, 2, 2, 0.75f, 0.75f, 0, copy) && !copy.valid());
  // A guest pixel covers 1.5 host pixels there: one whole and half of its neighbour, in turn.
  const auto near = [](float a, float b) { return a - b < 1e-5f && b - a < 1e-5f; };
  RasterTaps taps = GetRasterTaps(0, 1280, 1920);
  check(taps.first == 0 && near(taps.weights[0], 2.0f / 3) && near(taps.weights[1], 1.0f / 3) &&
        taps.weights[2] == 0.0f && taps.weights[3] == 0.0f);
  taps = GetRasterTaps(1, 1280, 1920);
  check(taps.first == 1 && near(taps.weights[0], 1.0f / 3) && near(taps.weights[1], 2.0f / 3) &&
        taps.weights[2] == 0.0f);
  taps = GetRasterTaps(1279, 1280, 1920);
  check(taps.first == 1918 && near(taps.weights[0], 1.0f / 3) && near(taps.weights[1], 2.0f / 3));
  // Every host pixel of the picture is used once in all, and nothing outside it.
  for (unsigned size : {1280u, 720u, 640u, 360u, 416u, 240u}) {
    for (unsigned raster : {size * 3 / 2, size * 2, size * 9 / 4, size * 3, size}) {
      float used[8] = {};
      bool inside = true;
      for (unsigned guest = 0; guest < 2; ++guest) {
        taps = GetRasterTaps(guest, size, raster);
        float sum = 0.0f;
        for (unsigned i = 0; i < 4; ++i) {
          sum += taps.weights[i];
          if (taps.weights[i] > 0.0f)
            used[taps.first + i] += taps.weights[i] * raster / size;
        }
        check(near(sum, 1.0f));
      }
      taps = GetRasterTaps(size - 1, size, raster);
      for (unsigned i = 0; i < 4; ++i)
        inside &= taps.weights[i] <= 1e-5f || taps.first + i < raster;
      check(inside);
      for (unsigned host = 0; host + 1 < 2 * raster / size; ++host)
        check(near(used[host], 1.0f));
    }
  }
  std::printf("FH1 instrument/native raster policy: %u failures\n", failures);
  return failures ? 1 : 0;
}
