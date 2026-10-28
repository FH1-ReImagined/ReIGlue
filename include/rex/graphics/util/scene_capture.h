// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace rex::graphics::scene_capture {
enum class DrawKind { Other, Composite, Overlay };
enum class Action { Ignore, RememberSurface, Capture };

// FH1's composite and overlay views address the same EDRAM surface. A snapshot
// belongs to the first overlay after a composite, at most once per frame.
constexpr Action Evaluate(DrawKind draw, bool eligible, uint32_t surface,
                          uint32_t remembered, bool attempted) {
  if (!eligible || draw == DrawKind::Other) return Action::Ignore;
  if (draw == DrawKind::Composite) return Action::RememberSurface;
  return !attempted && remembered == surface ? Action::Capture : Action::Ignore;
}

struct Extent { uint32_t width, height; };
constexpr Extent Resolution(uint32_t scale_x, uint32_t scale_y, int32_t limit_x, int32_t limit_y) {
  auto dimension = [](uint32_t base, uint32_t scale, int32_t limit) {
    uint64_t value = uint64_t(base) * scale;
    const uint64_t maximum = limit > 0 ? uint32_t(limit) : std::numeric_limits<uint32_t>::max();
    return uint32_t(std::min(value, maximum));
  };
  return {dimension(1280, scale_x, limit_x), dimension(720, scale_y, limit_y)};
}
}  // namespace rex::graphics::scene_capture
