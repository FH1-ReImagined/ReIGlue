// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#ifndef REX_UI_HDR_COLOR_H
#define REX_UI_HDR_COLOR_H

// Scalar color contract shared by the output shader and its CPU tests.
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
namespace rex::ui {
#define HDR_FN inline
#define HDR_POW std::pow
#define HDR_CLAMP std::clamp
#else
#define HDR_FN
#define HDR_POW pow
#define HDR_CLAMP clamp
#endif

HDR_FN float HdrDecodeSrgb(float code) {
  code = HDR_CLAMP(code, 0.0f, 1.0f);
  if (code < 0.04045f) return code / 12.92f;
  return HDR_POW((code + 0.055f) / 1.055f, 2.4f);
}

HDR_FN float HdrEncodeSrgb(float light) {
  light = HDR_CLAMP(light, 0.0f, 1.0f);
  if (light < 0.0031308f) return light * 12.92f;
  return 1.055f * HDR_POW(light, 1.0f / 2.4f) - 0.055f;
}

HDR_FN float HdrSourceLight(float sampled_light, float gamma) {
  if (gamma <= 0.0f) return sampled_light;
  return HDR_POW(HdrEncodeSrgb(sampled_light), gamma);
}

// The interface expresses extra headroom relative to paper white. The square
// ramp reaches that headroom at brightness 1 and has zero slope at the knee.
// Coverage interpolates the gain, so fully covered HUD never exceeds SDR white.
HDR_FN float HdrGain(float brightness, float coverage, float paper_scale,
                     float headroom, float knee, float inverse_knee_span) {
  const float ramp = HDR_CLAMP((brightness - knee) * inverse_knee_span, 0.0f, 1.0f);
  const float scene = 1.0f - HDR_CLAMP(coverage, 0.0f, 1.0f);
  return paper_scale * (1.0f + scene * headroom * ramp * ramp);
}

#undef HDR_FN
#undef HDR_POW
#undef HDR_CLAMP
#ifdef __cplusplus
}  // namespace rex::ui
#endif
#endif
