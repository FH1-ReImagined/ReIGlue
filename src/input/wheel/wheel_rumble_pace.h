/**
 * @file        src/input/wheel/wheel_rumble_pace.h
 * @brief       The game's rumble on a wheel's vibration effects at a fixed pace, whatever the
 *              frame rate (#467), without SDL.
 *
 * FH1 sets its two rumble motors once a frame, and the driver sent every change on to the wheel's
 * sine effects (wheel_rumble.h): 30 updates a second at the console's 30 fps, 60 at a 60 fps cap
 * and 240 at 240. Every update of a playing periodic effect is a report to the wheel, and a wheel
 * that cannot change a playing effect stops and starts it again (DirectInput does so unless the
 * caller passes DIEP_NORESTART, which SDL does not), which begins its sine again. On such a wheel,
 * at 240 updates a second the 40 Hz road sine (25 ms) is cut every 4 ms and never completes a
 * period: what plays is a 240 Hz sawtooth. A Simagic Alpha EVO Pro owner on Windows (#467) felt
 * the vibration grow with the frame rate cap, right at 60 and "through the entire rig" at 240.
 *
 * Now each vibration effect is updated at a fixed rate, whatever the frame rate:
 * wheel_vibration_rate updates a second (60 by default, 15 to 120), on a fixed schedule (at 20 fps
 * too: the driver's state polls between frames send what comes due). The rate is fixed, and not
 * the frame rate, because a wheel that restarts a playing effect on every update buzzes at the
 * update rate: left to the frame rate, the buzz would follow the frame rate cap. 60 is the default
 * because a tester with a Fanatec CSR Elite and a Simagic Alpha EVO Pro found it the smoothest;
 * 30 (the console's frame) can be chosen for a wheel where 60 feels too strong. Each update sends
 * the root mean square of the game's level over the time since the last one (each level held until
 * the game's next), so the vibration's energy per second is the game's, at every frame rate, and a
 * short bump between two updates is still felt. The level lags the game's by up to one pace; a
 * vibration that starts from silence starts at once (the driver, PaceWave), and its schedule from
 * there.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <algorithm>
#include <cmath>

namespace rex::input::wheel {

// wheel_vibration_rate: updates a second, clamped to this range.
constexpr int kRumbleRateMin = 15;
constexpr int kRumbleRateMax = 120;
constexpr int kRumbleRateDefault = 60;
// The pace (seconds between two updates) of a wheel_vibration_rate.
inline double RumblePaceSeconds(int rate) {
  return 1.0 / std::clamp(rate, kRumbleRateMin, kRumbleRateMax);
}
// An update a little early is still on time: frames are not evenly spaced, and at a frame cap
// at the rate each frame must still get its update. 6% of the pace (2 ms at 30 updates a second).
constexpr double kRumbleEarlyFraction = 0.06;

// One vibration effect's level, paced. Times are seconds of a steady clock; `pace` is the seconds
// between two updates (RumblePaceSeconds), read by the caller on each call so a change of the
// setting applies from the next update.
class RumblePace {
 public:
  // The game's level (0 to 1) from `now` on.
  void Set(double now, float level, double pace) {
    Advance(now, pace);
    level_ = level;
  }
  // The level the game asked for last.
  float level() const { return level_; }
  // Whether an update is due at `now` (Take gives its level).
  bool Due(double now, double pace) const {
    return started_ && now + pace * kRumbleEarlyFraction >= due_;
  }
  // The level to send at `now`: the root mean square of the game's level since the last Take (or
  // the first Set). Starts the next interval; the next update is due a pace after this one was.
  float Take(double now, double pace) {
    Advance(now, pace);
    const double span = now - window_start_;
    const float out = span > 0 ? static_cast<float>(std::sqrt(energy_ / span)) : level_;
    window_start_ = now;
    energy_ = 0;
    due_ += pace;
    if (due_ <= now) {
      due_ = now + pace / 2;  // behind after a hitch: no burst of updates
    }
    return out;
  }
  // Forgets the interval: the next Set starts a new one (the wheel stopped the vibration).
  void Reset() { *this = RumblePace{}; }

 private:
  void Advance(double now, double pace) {
    if (!started_) {
      started_ = true;
      window_start_ = last_ = now;
      due_ = now + pace;
      return;
    }
    if (now > last_) {
      energy_ += static_cast<double>(level_) * level_ * (now - last_);
      last_ = now;
    }
  }

  bool started_ = false;
  float level_ = 0.0f;
  double last_ = 0;          // when level_ was last integrated up to
  double window_start_ = 0;  // when the current interval began
  double energy_ = 0;        // the integral of level^2 over the interval so far
  double due_ = 0;           // when the next update is due
};

}  // namespace rex::input::wheel
