#include <rex/ui/hdr_parameters.h>
#include <rex/ui/hdr_color.h>
#include <cassert>
#include <limits>
using namespace rex::ui;
int main() {
  auto parameters = MakeHdrParameters({200,1000,.5,2.2,false,0},1920,1080);
  assert(parameters.white == 2.5f && parameters.headroom == 2);
  assert(parameters.split == -1 && parameters.resolution[1] == 1080);
  const auto white_nits = [](HdrParameters p) {
    return HdrGain(1,0,p.white,p.headroom,p.knee,p.inverse_span) * 80;
  };
  assert(white_nits(parameters) == 600);
  parameters = MakeHdrParameters({200,80,1,0,true,9},1921,1080);
  assert(white_nits(parameters) == 200);
  assert(parameters.calibration == 3 && parameters.split == 960);
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  parameters = MakeHdrParameters({nan,nan,nan,nan,false,-2},1280,720);
  assert(white_nits(parameters) == 600 && parameters.calibration == 0);
  assert(parameters.source_gamma == 2.2f);
  parameters = MakeHdrParameters({-1,1e9,2,9,false,0},1280,720);
  assert(white_nits(parameters) == 10000 && parameters.source_gamma == 3);
}
