#include <rex/graphics/util/presentation_transform.h>
#include <cassert>
#include <limits>
using rex::graphics::PresentationTransform;
struct Viewport { float ndc_scale[3]; float ndc_offset[3]; };
int main() {
  // Test coordinates, rather than just reproducing the composition formula:
  // the guest interval [-1,1] must land in the specified HUD rectangle.
  Viewport viewport{{1, 1, 0.75f}, {0, 0, 0.25f}};
  PresentationTransform::Hud(0.5, 0.75, 0.125, -0.125, 0.25, -1).Apply(viewport);
  assert(viewport.ndc_offset[0] - viewport.ndc_scale[0] == -0.625f);
  assert(viewport.ndc_offset[0] + viewport.ndc_scale[0] == 0.375f);
  assert(viewport.ndc_offset[1] - viewport.ndc_scale[1] == -0.875f);
  assert(viewport.ndc_offset[1] + viewport.ndc_scale[1] == 0.625f);
  assert(viewport.ndc_scale[2] == 0.75f && viewport.ndc_offset[2] == 0.25f);
  // Composing identity cannot alter an existing tile or inverted Y viewport.
  viewport = {{0.25f, -0.5f, 1}, {-0.25f, 0.125f, 0}};
  const auto original = viewport;
  PresentationTransform{}.Apply(viewport);
  for (unsigned i = 0; i < 3; ++i) {
    assert(viewport.ndc_scale[i] == original.ndc_scale[i]);
    assert(viewport.ndc_offset[i] == original.ndc_offset[i]);
  }
  const double nan = std::numeric_limits<double>::quiet_NaN();
  PresentationTransform::Hud(nan, nan, nan, nan, nan, 1).Apply(viewport);
  for (unsigned i = 0; i < 3; ++i) {
    assert(viewport.ndc_scale[i] == original.ndc_scale[i]);
    assert(viewport.ndc_offset[i] == original.ndc_offset[i]);
  }
}
