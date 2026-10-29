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
#define HDR_EXP std::exp
#define HDR_CLAMP std::clamp
#else
#define HDR_FN
#define HDR_POW pow
#define HDR_EXP exp
#define HDR_CLAMP clamp
#endif

HDR_FN float HdrDecodeSrgb(float code) {
  if (!(code >= 0.0f)) return 0.0f;
  code = HDR_CLAMP(code, 0.0f, 1.0f);
  if (code < 0.04045f) return code / 12.92f;
  return HDR_POW((code + 0.055f) / 1.055f, 2.4f);
}

HDR_FN float HdrEncodeSrgb(float light) {
  if (!(light >= 0.0f)) return 0.0f;
  light = HDR_CLAMP(light, 0.0f, 1.0f);
  if (light < 0.0031308f) return light * 12.92f;
  return 1.055f * HDR_POW(light, 1.0f / 2.4f) - 0.055f;
}

HDR_FN float HdrSourceLight(float sampled_light, float gamma) {
  if (!(sampled_light >= 0.0f)) return 0.0f;
  sampled_light = HDR_CLAMP(sampled_light, 0.0f, 1.0f);
  if (!(gamma > 0.0f)) return sampled_light;
  return HDR_POW(HdrEncodeSrgb(sampled_light), gamma);
}

// The scene uses an exponential segment above a diffuse anchor. Its endpoint is
// the requested scene ceiling, and its tangent at the anchor is the SDR tangent.
// MakeHdrParameters solves that boundary-value problem once per presentation.
HDR_FN float HdrSceneBrightness(float brightness, float ceiling, float anchor,
                               float exponent, float denominator) {
  if (!(brightness >= 0.0f)) return 0.0f;
  brightness = HDR_CLAMP(brightness, 0.0f, 1.0f);
  if (brightness <= anchor || ceiling <= 1.0f || exponent <= 0.0001f)
    return brightness;
  const float position = (brightness - anchor) / (1.0f - anchor);
  const float segment = (HDR_EXP(exponent * position) - 1.0f) / denominator;
  return HDR_CLAMP(anchor + (ceiling - anchor) * segment, brightness, ceiling);
}

// Coverage is a conservative classification, not an interpolation coefficient:
// any overlay protects that pixel. A common RGB multiplier preserves hue ratios.
HDR_FN float HdrSceneGain(float strongest, float coverage, float paper_scale,
                         float ceiling, float anchor, float exponent, float denominator) {
  if (!(strongest > 0.0f) || !(coverage <= 0.0f)) return paper_scale;
  return paper_scale * HdrSceneBrightness(strongest, ceiling, anchor, exponent, denominator) / strongest;
}

#undef HDR_FN
#undef HDR_POW
#undef HDR_EXP
#undef HDR_CLAMP
#ifdef __cplusplus
}  // namespace rex::ui
#endif
#endif
