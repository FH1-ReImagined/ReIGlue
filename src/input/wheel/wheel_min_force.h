/**
 * @file        src/input/wheel/wheel_min_force.h
 * @brief       A minimum force for the game's steering force (#303), without SDL: forces weaker
 *              than wheel_ffb_min_strength are scaled up, so a light car still has some weight in
 *              the wheel; stronger ones play as the game sends them.
 *
 * FH1's steering force (its constant force effect, wheel_ff.h) follows the car: in a light car it
 * stays small, in a heavy one (#4: a WRX STI on a Simagic Alpha EVO Pro) it is several times
 * stronger. The minimum lifts the weak end of the range instead of clipping it to a floor: a force
 * x below the minimum m plays as x (1 + 3 (1 - x / m)^2), up to 4 times as strong near 0, and from
 * m up as x. The curve starts at 0 (no step when the force changes sides at the centre), meets the
 * line at m with the same slope, always rises (3 is the largest factor that keeps it rising) and
 * never plays a force weaker than the game's. The centring spring, the damper and the rumble are
 * left alone.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <rex/input/wheel/wheel_ff.h>

namespace rex::input::wheel {

constexpr float kMinForceBoost = 3.0f;  // a force near 0 plays at 1 + kMinForceBoost times

// A force of size x (0 to 1 of the wheel's full force) with the minimum `min` (0 to 1).
inline float MinForce(float x, float min) {
  x = std::clamp(x, 0.0f, 1.0f);
  if (!(min > 0.0f) || x >= min) {
    return x;
  }
  const float u = 1.0f - x / std::min(min, 1.0f);
  return x * (1.0f + kMinForceBoost * u * u);
}

// The game's constant force as played (FfConstantLevel: with the player's strengths and the fade)
// with the minimum applied to the force the game asks for: its magnitude, the effect's gain and the
// device gain, before wheel_ffb_strength and the fade scale it.
inline float FfMinForceLevel(const FfEffect& e, uint32_t device_gain, float level, float min) {
  const float game = std::fabs(FfConstantLevel(e, FfScale(e, device_gain, 1.0f)));
  if (!(game > 0.0f) || !(min > 0.0f)) {
    return level;
  }
  return std::clamp(level * (MinForce(game, min) / game), -1.0f, 1.0f);
}

}  // namespace rex::input::wheel
