/**
 * @file        src/input/wheel/wheel_ff_reject.h
 * @brief       The game's force feedback effects a wheel refuses (#308), without SDL: the
 *              directions an effect is created in, and the effect types not tried again.
 *
 * A Thrustmaster TMX on Windows refused the game's constant force ("Haptic error Unable to create
 * effect"), and the driver tried to create it again every frame: 82,000 warnings in one log, plus
 * thousands for the spring and the damper. Now an effect is created on SDL's steering axis, else in
 * the same direction as Cartesian X and as polar east (a force from the east, which pulls the wheel
 * left, as on the steering axis); the first direction that works is used for the wheel's later
 * effects. A type refused in every direction is logged once and not created again until the game
 * resets its force feedback (no effect block defined) or the wheel is plugged in again; a refused
 * spring falls back to the wheel's own autocentre where it has one.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <array>
#include <cstdint>

#include <rex/input/wheel/wheel_ff.h>

namespace rex::input::wheel {

// The directions an effect is created in: 0 SDL's steering axis, 1 Cartesian X (east), 2 polar east.
constexpr int kFfDirections = 3;

// The order to try them in: the one that worked last, then the others in order.
inline std::array<int, kFfDirections> FfDirectionOrder(int last) {
  std::array<int, kFfDirections> out{};
  last = (last >= 0 && last < kFfDirections) ? last : 0;
  out[0] = last;
  int n = 1;
  for (int d = 0; d < kFfDirections; d++) {
    if (d != last) {
      out[n++] = d;
    }
  }
  return out;
}

// The effect types (SDL's type bits) a wheel refused.
class FfRejections {
 public:
  bool Rejected(uint32_t type) const { return (types_ & type) != 0; }
  // A type refused in every direction; true the first time (log it then).
  bool Reject(uint32_t type) {
    const bool first = !Rejected(type);
    types_ |= type;
    return first;
  }
  void Reset() { types_ = 0; }
  uint32_t types() const { return types_; }

 private:
  uint32_t types_ = 0;
};

// The game reset its force feedback (DeviceControl reset) or has not defined any effect yet.
inline bool FfNothingDefined(const FfState& s) {
  for (uint32_t b = 1; b <= kFfMaxBlocks; b++) {
    if (s.defined(b)) {
      return false;
    }
  }
  return true;
}

}  // namespace rex::input::wheel
