// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#ifndef REX_UI_HDR_CALIBRATION_H
#define REX_UI_HDR_CALIBRATION_H
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
namespace rex::ui {
#define HDR_CAL_FN inline
#define HDR_CAL_MIN std::min
#define HDR_CAL_ABS std::abs
#define HDR_CAL_EXP2 std::exp2
#define HDR_CAL_FLOOR std::floor
#else
#define HDR_CAL_FN
#define HDR_CAL_MIN min
#define HDR_CAL_ABS abs
#define HDR_CAL_EXP2 exp2
#define HDR_CAL_FLOOR floor
#endif

// Four columns by three rows of square cells, centered in the output. Negative
// means outside the pattern. All values are linear scRGB luminance, independent
// of source gamma and scene expansion. Host/UI coverage is handled by the caller.
HDR_CAL_FN float HdrCalibrationSignal(int mode, float x, float y, float width, float height,
                                     float paper, float peak) {
  if (mode < 1 || mode > 3 || width <= 0.0f || height <= 0.0f) return -1.0f;
  const float cell = HDR_CAL_MIN(width / 5.0f, height / 4.0f);
  const float grid_x = (x - width * 0.5f) / cell + 2.0f;
  const float grid_y = (y - height * 0.5f) / cell + 1.5f;
  if (!(grid_x >= 0.0f && grid_x < 4.0f && grid_y >= 0.0f && grid_y < 3.0f)) return -1.0f;
  const int column = int(HDR_CAL_FLOOR(grid_x)), row = int(HDR_CAL_FLOOR(grid_y));
  const int index = row * 4 + column;
  const float local_x = grid_x - float(column), local_y = grid_y - float(row);
  const bool inset = HDR_CAL_ABS(local_x - 0.5f) < 0.32f && HDR_CAL_ABS(local_y - 0.5f) < 0.32f;
  if (mode == 1) {
    // Absolute black-level signals from zero to one nit, alternating with black
    // inside each cell. A display's source-gamma preference does not change them.
    const float nits = index == 0 ? 0.0f : HDR_CAL_EXP2(float(index - 11));
    const int checker = int(local_x * 8.0f) + int(local_y * 8.0f);
    return inset && (checker % 2 == 0) ? nits / 80.0f : 0.0f;
  }
  if (mode == 2) {
    const float level = column == 0 ? 0.18f : column == 1 ? 0.5f : column == 2 ? 0.75f : 1.0f;
    const float surround = row == 0 ? 0.0f : row == 1 ? 0.18f : 1.0f;
    return paper * (inset ? level : surround);
  }
  // Insets approach the declared peak in sixth-stop increments. Their surround
  // is at the peak, so loss of contrast is visible without exceeding that peak.
  return peak * (inset ? HDR_CAL_EXP2(float(index - 11) / 6.0f) : 1.0f);
}

#undef HDR_CAL_FN
#undef HDR_CAL_MIN
#undef HDR_CAL_ABS
#undef HDR_CAL_EXP2
#undef HDR_CAL_FLOOR
#ifdef __cplusplus
}  // namespace rex::ui
#endif
#endif
