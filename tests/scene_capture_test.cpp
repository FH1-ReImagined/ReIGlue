// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#include <rex/graphics/util/scene_capture.h>
#include <cassert>
using namespace rex::graphics::scene_capture;
int main() {
  for (bool attempted : {false, true}) {
    for (auto draw : {DrawKind::Other, DrawKind::Composite, DrawKind::Overlay}) {
      assert(Evaluate(draw, false, 42, 42, attempted) == Action::Ignore);
    }
    assert(Evaluate(DrawKind::Composite, true, 42, 7, attempted) == Action::RememberSurface);
    assert(Evaluate(DrawKind::Overlay, true, 42, 7, attempted) == Action::Ignore);
  }
  assert(Evaluate(DrawKind::Overlay, true, 42, 42, false) == Action::Capture);
  assert(Evaluate(DrawKind::Overlay, true, 42, 42, true) == Action::Ignore);
  assert(Evaluate(DrawKind::Other, true, 42, 42, false) == Action::Ignore);
  const auto native = Resolution(1, 1, 0, 0);
  assert(native.width == 1280 && native.height == 720);
  const auto clipped = Resolution(3, 2, 1920, 1080);
  assert(clipped.width == 1920 && clipped.height == 1080);
  const auto small = Resolution(1, 1, 1920, 1080);
  assert(small.width == 1280 && small.height == 720);
  const auto overflow = Resolution(UINT32_MAX, UINT32_MAX, 0, -1);
  assert(overflow.width == UINT32_MAX && overflow.height == UINT32_MAX);
}
