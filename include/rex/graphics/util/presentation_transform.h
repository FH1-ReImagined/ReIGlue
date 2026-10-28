// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <array>
#include <cmath>

namespace rex::graphics {
// Axis-aligned affine mapping in normalized device coordinates. The first
// component maps displacements; the second maps the origin. This representation
// lets both graphics backends compose the same host layout with the guest map.
struct PresentationTransform {
  std::array<float, 2> extent{1, 1};
  std::array<float, 2> origin{};

  static PresentationTransform Hud(double width, double height, double x, double y,
                                   double edge_distance, float anchor) {
    const auto finite = [](double value, float fallback) {
      return std::isfinite(value) && std::abs(value) <= 16 ? float(value) : fallback;
    };
    PresentationTransform result;
    result.extent = {finite(width, 1), finite(height, 1)};
    result.origin = {finite(x, 0), finite(y, 0)};
    result.origin[0] += finite(edge_distance, 0) * finite(anchor, 0);
    return result;
  }

  template <typename Viewport>
  void Apply(Viewport& viewport) const {
    for (unsigned axis = 0; axis != 2; ++axis) {
      viewport.ndc_offset[axis] = origin[axis] + extent[axis] * viewport.ndc_offset[axis];
      viewport.ndc_scale[axis] *= extent[axis];
    }
  }
};
}  // namespace rex::graphics
