#include <rex/ui/hdr_color.h>
#include <cassert>
#include <cmath>
using namespace rex::ui;
int main() {
  // Round-trip every 8-bit code and verify the nonlinear transfer boundaries.
  for (int code = 0; code <= 255; ++code) {
    const float value = code / 255.0f;
    assert(std::abs(HdrEncodeSrgb(HdrDecodeSrgb(value)) - value) < 0.00001f);
    assert(std::abs(HdrSourceLight(HdrDecodeSrgb(value), 2.2f) - std::pow(value, 2.2f)) < 0.00001f);
  }
  assert(HdrDecodeSrgb(0) == 0 && HdrEncodeSrgb(1) <= 1.000001f);
  assert(HdrSourceLight(0.25f, 0) == 0.25f);
  // 200-nit paper, 1000-nit display, 50% highlight strength => 600 nits.
  assert(HdrGain(1, 0, 2.5f, 2, .5f, 2) * 80 == 600);
  assert(HdrGain(1, 1, 2.5f, 2, .5f, 2) * 80 == 200);
  float previous = 0;
  for (int step = 0; step <= 1024; ++step) {
    const float light = step / 1024.0f;
    const float value = light * HdrGain(light, 0, 2.5f, 2, .5f, 2);
    assert(value >= previous && value <= 7.5f);
    assert(HdrGain(light, 1, 2.5f, 2, .5f, 2) == 2.5f);
    assert(HdrGain(light, 0, 2.5f, 0, .5f, 2) == 2.5f);
    previous = value;
  }
}
