// Copyright (c) 2026 ReIGlue contributors.
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <algorithm>
#include <iterator>
namespace rex::ui::vulkan {
template <typename Format>
struct HdrFormatChoice { Format format; bool available, active; };
// Format is the host API's pair of pixel format and color space. Keeping this
// selection independent of Vulkan headers also makes it safe to include before
// the platform-specific Vulkan API configuration.
template <typename Range, typename Format>
HdrFormatChoice<Format> ChooseHdrFormat(const Range& offered, Format fallback,
                                      bool extension, bool requested, Format desired) {
  if (!extension) return {fallback, false, false};
  const auto first = std::begin(offered), last = std::end(offered);
  const auto selected = std::find_if(first, last, [desired](auto candidate) {
    return candidate.colorSpace == desired.colorSpace && candidate.format == desired.format;
  });
  const bool available = selected != last;
  return {available && requested ? *selected : fallback, available, available && requested};
}
}  // namespace rex::ui::vulkan
