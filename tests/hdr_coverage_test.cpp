#include <rex/ui/hdr_coverage.h>
#include <algorithm>
#include <cassert>
#include <cmath>
using namespace rex::ui;
int main() {
  for (float quantum : {1.0f/255, 1.0f/1023}) {
    for (int background=0; background<=100; ++background) {
      const float scene=background/100.0f;
      assert(HdrOverlayLowerBound(scene,scene,quantum) == 0);
      for (float foreground : {0.0f,0.2f,0.8f,1.0f}) {
        for (float alpha : {0.0f,0.01f,0.1f,0.5f,1.0f}) {
          const float actual=scene*(1-alpha)+foreground*alpha;
          const float stored=std::round(actual/quantum)*quantum;
          const float background_stored=std::round(scene*1023)/1023;
          const float bound=HdrOverlayLowerBound(stored,background_stored,quantum);
          assert(std::isfinite(bound) && bound>=0 && bound<=alpha+0.00001f);
          if (!alpha) assert(bound == 0);
        }
      }
    }
  }
  assert(HdrOverlayLowerBound(0,.5f,1.0f/1023) > .99f);
  assert(HdrOverlayLowerBound(1,.5f,1.0f/1023) > .99f);
  assert(HdrOverlayLowerBound(.55f,.5f,1.0f/1023) > 0);
  // A one-pixel overlay does not protect unrelated neighboring pixels.
  for (int pixel=0; pixel<9; ++pixel) {
    const float value=pixel==4?1.0f:.5f;
    assert((HdrOverlayLowerBound(value,.5f,1.0f/1023)>0) == (pixel==4));
  }
}
