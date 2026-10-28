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
  float white, headroom, knee, inverse_span;
  float source_gamma, saturation_weight, split;
  uint32_t calibration;
  float peak, reserved, resolution[2];
};
static_assert(sizeof(HdrParameters) == 48);

inline HdrParameters MakeHdrParameters(HdrSettings settings, uint32_t width, uint32_t height) {
  const auto setting = [](double value, double fallback, double low, double high) {
    return float(std::isfinite(value) ? std::clamp(value, low, high) : fallback);
  };
  const float paper = setting(settings.paper_nits, 200, 80, 1000);
  const float peak = setting(settings.peak_nits, 1000, 80, 10000);
  const float expansion = setting(settings.strength, .5, 0, 1);
  const float available = std::max(peak - paper, 0.0f);
  return {paper * .0125f, expansion * available / paper, .5f, 2,
          setting(settings.gamma, 2.2, 0, 3), .75f,
          settings.compare ? float(width / 2) : -1,
          uint32_t(std::clamp(settings.calibration, 0, 3)),
          peak * .0125f, 0, {float(width), float(height)}};
}
}  // namespace rex::ui
