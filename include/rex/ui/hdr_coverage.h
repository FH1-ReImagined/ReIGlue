// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#ifndef REX_UI_HDR_COVERAGE_H
#define REX_UI_HDR_COVERAGE_H
#ifdef __cplusplus
#include <algorithm>
namespace rex::ui {
#define HDR_COV_FN inline
#define HDR_COV_CLAMP std::clamp
#else
#define HDR_COV_FN
#define HDR_COV_CLAMP clamp
#endif

// Lower bound on overlay opacity from F = S*(1-a) + C*a, 0 <= C <= 1.
// Treat each stored UNORM value as a rounding interval, not an exact color.
// A source/snapshot difference explainable by those intervals is not evidence
// of an overlay. The snapshot has ten bits per color channel.
HDR_COV_FN float HdrOverlayLowerBound(float finished, float scene, float source_quantum) {
  finished = HDR_COV_CLAMP(finished, 0.0f, 1.0f);
  scene = HDR_COV_CLAMP(scene, 0.0f, 1.0f);
  const float source_radius = source_quantum * 0.5f;
  const float scene_radius = 0.5f / 1023.0f;
  const float low_finished = HDR_COV_CLAMP(finished - source_radius, 0.0f, 1.0f);
  const float high_finished = HDR_COV_CLAMP(finished + source_radius, 0.0f, 1.0f);
  const float low_scene = HDR_COV_CLAMP(scene - scene_radius, 0.0f, 1.0f);
  const float high_scene = HDR_COV_CLAMP(scene + scene_radius, 0.0f, 1.0f);
  // Four single-precision ULPs at one bound the interval arithmetic's rounding.
  const float arithmetic_error = 0.000000476837158203125f;
  if (low_finished > high_scene + arithmetic_error)
    return (low_finished - high_scene) / (1.0f - high_scene);
  if (low_scene > high_finished + arithmetic_error)
    return (low_scene - high_finished) / low_scene;
  return 0.0f;
}

#undef HDR_COV_FN
#undef HDR_COV_CLAMP
#ifdef __cplusplus
}  // namespace rex::ui
#endif
#endif
