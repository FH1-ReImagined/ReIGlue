#include <rex/ui/hdr_color.h>
#include <rex/ui/hdr_parameters.h>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
using namespace rex::ui;
int main() {
  for (int code = 0; code <= 255; ++code) {
    const float value = code / 255.0f;
    assert(std::abs(HdrEncodeSrgb(HdrDecodeSrgb(value)) - value) < 0.00001f);
    assert(std::abs(HdrSourceLight(HdrDecodeSrgb(value), 2.2f) - std::pow(value, 2.2f)) < 0.00001f);
  }
  const auto nan = std::numeric_limits<float>::quiet_NaN();
  assert(HdrSourceLight(nan, 2.2f) == 0 && HdrDecodeSrgb(nan) == 0);
  for (double strength : {0.0, 0.01, 0.5, 1.0}) {
    for (double peak : {80.0, 200.0, 1000.0, 10000.0}) {
      const auto p = MakeHdrParameters({200,peak,strength,0,false,0},1280,720);
      const auto mapped = [&](float value) {
        return HdrSceneBrightness(value,p.scene_ceiling,p.diffuse_anchor,p.curve_exponent,p.curve_denominator);
      };
      assert(mapped(0) == 0 && mapped(p.diffuse_anchor) == p.diffuse_anchor);
      assert(std::abs(mapped(1) - p.scene_ceiling) < 0.0001f);
      float previous = 0;
      for (int sample = 0; sample <= 4096; ++sample) {
        const float input = sample / 4096.0f;
        const float output = mapped(input);
        assert(std::isfinite(output) && output >= previous && output <= p.scene_ceiling);
        assert(output >= input);
        if (!strength || peak <= 200) assert(output == input);
        for (float coverage : {0.000001f, 0.25f, 1.0f})
          assert(HdrSceneGain(input,coverage,p.white,p.scene_ceiling,p.diffuse_anchor,
                             p.curve_exponent,p.curve_denominator) == p.white);
        previous = output;
      }
      // The diffuse tangent is continuous, not merely the diffuse value.
      constexpr float epsilon = 0.001f;
      const float slope = (mapped(p.diffuse_anchor + epsilon) - mapped(p.diffuse_anchor)) / epsilon;
      assert(std::abs(slope - 1) < 0.02f);
      // Scaling all channels equally preserves hue ratios and bounds each channel.
      const std::array<float,3> rgb{0.9f,0.3f,0.1f};
      const float gain = HdrSceneGain(rgb[0],0,p.white,p.scene_ceiling,p.diffuse_anchor,
                                      p.curve_exponent,p.curve_denominator);
      assert(std::abs((rgb[1]*gain)/(rgb[0]*gain) - rgb[1]/rgb[0]) < 0.00001f);
      assert(rgb[0]*gain <= p.white*p.scene_ceiling);
    }
  }
}
