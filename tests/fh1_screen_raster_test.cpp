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
  // 0900 (FH1 #827): the scene tiles' place in the frame for the marked FM4 shadow fetch. The
  // frame's viewport (scale 640, -360; offset 640, 360) starts in the target: no offset.
  check(TileFrameOrigin(true, 640.0f, 640.0f, 0, 1.0f) == 0.0f);
  check(TileFrameOrigin(true, -360.0f, 360.0f, 0, 1.0f) == 0.0f);
  check(TileFrameOrigin(true, -360.0f, 360.0f, 0, 2.0f) == 0.0f);
  // The second and third tiles, moved up by the window offset: 256 and 512 rows, in host
  // pixels at render scale 1, at render scale 2 and on the 1080p raster (0.75 of scale 2).
  check(TileFrameOrigin(true, -360.0f, 360.0f, -256, 1.0f) == 256.0f);
  check(TileFrameOrigin(true, -360.0f, 360.0f, -512, 1.0f) == 512.0f);
  check(TileFrameOrigin(true, -360.0f, 360.0f, -256, 2.0f) == 512.0f);
  check(TileFrameOrigin(true, -360.0f, 360.0f, -512, 2.0f) == 1024.0f);
  check(TileFrameOrigin(true, -360.0f, 360.0f, -256, 1.5f) == 384.0f);
  check(TileFrameOrigin(true, -360.0f, 360.0f, -512, 1.5f) == 768.0f);
  // The same tiles moved by the viewport's own offset, either scale sign, columns too.
  check(TileFrameOrigin(true, -360.0f, 104.0f, 0, 1.0f) == 256.0f);
  check(TileFrameOrigin(true, 360.0f, -152.0f, 0, 1.0f) == 512.0f);
  check(TileFrameOrigin(true, 640.0f, 0.0f, 0, 1.0f) == 640.0f);
  check(TileFrameOrigin(true, 640.0f, 640.0f, -640, 2.0f) == 1280.0f);
  // A half-pixel offset is not a tile, and a tile with one is still 256 rows up.
  check(TileFrameOrigin(true, -360.0f, 359.5f, 0, 1.0f) == 0.0f);
  check(TileFrameOrigin(true, -360.0f, 360.5f, 0, 1.0f) == 0.0f);
  check(TileFrameOrigin(true, -360.0f, 360.5f, -256, 1.0f) == 256.0f);
  // No offset: a viewport inside the target (a small view further down), a disabled axis, a
  // value no frame has.
  check(TileFrameOrigin(true, -64.0f, 400.0f, 0, 1.0f) == 0.0f);
  check(TileFrameOrigin(true, -360.0f, 360.0f, 256, 1.0f) == 0.0f);
  check(TileFrameOrigin(false, -360.0f, 360.0f, -256, 1.0f) == 0.0f);
  check(TileFrameOrigin(true, -360.0f, 360.0f, -8192, 1.0f) == 8192.0f);
  check(TileFrameOrigin(true, -360.0f, 360.0f, -8193, 1.0f) == 0.0f);
  {
    const float zero = 0.0f, not_a_number = zero / zero;
    check(TileFrameOrigin(true, not_a_number, 360.0f, -256, 1.0f) == 0.0f);
    check(TileFrameOrigin(true, -360.0f, not_a_number, -256, 1.0f) == 0.0f);
  }
  std::printf("FH1 instrument/native raster policy: %u failures\n", failures);
  return failures ? 1 : 0;
}
