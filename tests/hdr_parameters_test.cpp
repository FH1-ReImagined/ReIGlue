#include <rex/ui/hdr_parameters.h>
#include <cassert>
#include <limits>
using namespace rex::ui;
int main() {
  auto p = MakeHdrParameters({200,1000,.5,2.2,false,0},1920,1080);
  assert(p.white == 2.5f && std::abs(p.scene_ceiling - std::sqrt(5.0f)) < 0.00001f);
  assert(p.split == -1 && p.resolution[1] == 1080 && p.diffuse_anchor == .18f);
  // A half-strength exposure lies halfway in stops, not halfway in nits.
  assert(std::abs(80*p.white*p.scene_ceiling - std::sqrt(200.0f*1000)) < .001f);
  p = MakeHdrParameters({200,80,1,0,true,9},1921,1080);
  assert(p.scene_ceiling == 1 && p.curve_exponent == 0 && p.curve_denominator == 0);
  assert(p.calibration == 3 && p.split == 960);
  for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity()}) {
    p = MakeHdrParameters({invalid,invalid,invalid,invalid,false,-2},1280,720);
    assert(std::isfinite(p.curve_exponent) && std::isfinite(p.curve_denominator));
    assert(p.white == 2.5f && p.source_gamma == 2.2f && p.calibration == 0);
    assert(std::abs(p.scene_ceiling - std::sqrt(5.0f)) < .00001f);
  }
  p = MakeHdrParameters({-1,1e9,2,9,false,0},1280,720);
  assert(std::abs(80*p.white*p.scene_ceiling - 10000) < .001f && p.source_gamma == 3);
}
