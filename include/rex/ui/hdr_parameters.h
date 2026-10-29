// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rex::ui {
struct HdrSettings {
  double paper_nits, peak_nits, strength, gamma;
  bool compare;
  int calibration;
};

// The shader's stable 48-byte payload begins after the rectangle constants.
struct HdrParameters {
  float white, scene_ceiling, diffuse_anchor, curve_exponent;
  float source_gamma, curve_denominator, split;
  uint32_t calibration;
  float peak, reserved, resolution[2];
};
static_assert(sizeof(HdrParameters) == 48);

inline HdrParameters MakeHdrParameters(HdrSettings settings, uint32_t width, uint32_t height) {
  const auto setting = [](double value, double fallback, double low, double high) {
    return float(std::isfinite(value) ? std::clamp(value, low, high) : fallback);
  };
  const float paper = setting(settings.paper_nits, 200, 80, 1000);
  const float peak = std::max(paper, setting(settings.peak_nits, 1000, 80, 10000));
  const float expansion = setting(settings.strength, .5, 0, 1);
  // Strength is an exposure interpolation: zero retains SDR, one reaches the
  // display peak. This replaces the earlier linear headroom/gain policy.
  const double ceiling = std::exp(expansion * std::log(std::max(double(peak) / paper, 1.0)));
  constexpr double anchor = 0.18;
  double exponent = 0;
  if (ceiling > 1.00001) {
    // Match both the diffuse value and its slope. For k > 0,
    // expm1(k)/k increases strictly from 1, so bisection has one solution.
    const double slope_ratio = (ceiling - anchor) / (1 - anchor);
    double low = 0, high = 16;
    for (unsigned iteration = 0; iteration < 48; ++iteration) {
      const double middle = (low + high) * .5;
      if (std::expm1(middle) / middle < slope_ratio) low = middle;
      else high = middle;
    }
    exponent = (low + high) * .5;
  }
  return {paper * .0125f, float(ceiling), float(anchor), float(exponent),
          setting(settings.gamma, 2.2, 0, 3), float(std::expm1(exponent)),
          settings.compare ? float(width / 2) : -1,
          uint32_t(std::clamp(settings.calibration, 0, 3)),
          peak * .0125f, 0, {float(width), float(height)}};
}
}  // namespace rex::ui
