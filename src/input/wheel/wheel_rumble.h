/**
 * @file        src/input/wheel/wheel_rumble.h
 * @brief       The game's two rumble motors on a force feedback wheel's motor (#273), without
 *              SDL: classic (one coarse 25 Hz sine, as before) or fine (a light, quicker sine for
 *              each motor).
 *
 * On the Xbox 360 the wheel's rumble went to small motors in the rim, which had to shake hard to
 * be felt. Played through a force feedback base's main motor, the same levels rattle the whole
 * wheel (#4: a Fanatec CSR Elite with the engine and tyre-slip rumble together). Fine plays the
 * engine motor (the game's right motor) as a ~71 Hz sine and the road motor (left) as a 40 Hz one,
 * each at most kFineGain of the force feedback strength, so they are felt as a texture in the rim.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include <rex/input/wheel/wheel_ff.h>

namespace rex::input::wheel {

enum class RumbleStyle { kClassic, kFine };

// wheel_rumble_style: "classic", anything else fine (the default).
inline RumbleStyle ParseRumbleStyle(const std::string& s) {
  return s == "classic" ? RumbleStyle::kClassic : RumbleStyle::kFine;
}

// One periodic effect: its period (SDL and Linux take milliseconds) and level, 0 to 1 of the
// motor's full force.
struct RumbleWave {
  uint16_t period_ms = 0;
  float level = 0.0f;
};

constexpr uint16_t kClassicPeriodMs = 40;  // 25 Hz
constexpr float kClassicGain = 0.6f;
constexpr uint16_t kFineRoadPeriodMs = 25;    // 40 Hz
constexpr uint16_t kFineEnginePeriodMs = 14;  // ~71 Hz
constexpr float kFineGain = 0.25f;

// Classic: the stronger motor, each by its own strength, as one 25 Hz sine (as before #273).
inline RumbleWave ClassicRumble(uint16_t left_motor, uint16_t right_motor, const FfStrengths& s,
                                float fade) {
  return {kClassicPeriodMs, FfRumbleLevel(left_motor, right_motor, s) * kClassicGain * fade};
}

// Fine: each motor its own sine, by its own strength and the overall strength.
struct FineRumble {
  RumbleWave road;    // the game's left motor: road surface, kerbs, impacts, tyre slip
  RumbleWave engine;  // the right motor: mostly the engine
};
inline FineRumble FineRumbleOf(uint16_t left_motor, uint16_t right_motor, const FfStrengths& s,
                               float fade) {
  auto level = [&](uint16_t motor, float strength) {
    return std::clamp(motor / 65535.0f * strength * s.overall, 0.0f, 1.0f) * kFineGain * fade;
  };
  return {{kFineRoadPeriodMs, level(left_motor, s.road)},
          {kFineEnginePeriodMs, level(right_motor, s.engine)}};
}

// An SDL periodic magnitude (0 to 32767) for a level.
inline int16_t RumbleMagnitude(float level) {
  return static_cast<int16_t>(std::clamp(level, 0.0f, 1.0f) * 32767.0f);
}

}  // namespace rex::input::wheel
