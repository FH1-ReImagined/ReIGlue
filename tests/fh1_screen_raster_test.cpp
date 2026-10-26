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
  std::printf("FH1 instrument/native raster policy: %u failures\n", failures);
  return failures ? 1 : 0;
}
