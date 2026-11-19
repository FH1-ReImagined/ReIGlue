#pragma once

#include <cstdint>

namespace rex::graphics::vulkan::fh1 {
struct HudSeedSurface {
  uint32_t width, height, format;
  bool single_sample, transfer_source, transfer_destination;
};

constexpr bool CanCopyHudSeed(const HudSeedSurface& source, const HudSeedSurface& destination,
                              int source_width, int source_height,
                              int destination_width, int destination_height, bool distinct,
                              uint32_t packed_color_format) {
  return distinct && source.format == packed_color_format && source.format == destination.format &&
         source.single_sample && destination.single_sample &&
         source.transfer_source && destination.transfer_destination &&
         source_width == 2560 && source_height == 1440 &&
         destination_width == source_width && destination_height == source_height &&
         source.width >= 2560 && source.height >= 1440 &&
         destination.width >= 2560 && destination.height >= 1440;
}
}  // namespace rex::graphics::vulkan::fh1
