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
 * Now each vibration effect is updated kRumblePaceSeconds apart (the console's frame, 1/30 s), on a
 * fixed schedule, whatever the frame rate: about 30 updates a second at every cap (at 20 fps too:
 * the driver's state polls between frames send what comes due). Each update sends the root mean
 * square of the game's level over the time since the last one (each level held until the game's
 * next), so the vibration's energy per second is the game's, at every frame rate, and a short bump
 * between two updates is still felt. The level lags the game's by up to one pace; a vibration that
 * starts from silence starts at once (the driver, PaceWave), and its schedule from there.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cmath>

namespace rex::input::wheel {

constexpr double kRumblePaceSeconds = 1.0 / 30.0;
// An update a little early is still on time: frames are not evenly spaced, and at a 30 fps cap
// each frame must still get its update.
constexpr double kRumblePaceEarlySeconds = 0.002;

// One vibration effect's level, paced. Times are seconds of a steady clock.
class RumblePace {
 public:
  // The game's level (0 to 1) from `now` on.
  void Set(double now, float level) {
    Advance(now);
    level_ = level;
  }
  // The level the game asked for last.
  float level() const { return level_; }
  // Whether an update is due at `now` (Take gives its level).
  bool Due(double now) const { return started_ && now + kRumblePaceEarlySeconds >= due_; }
  // The level to send at `now`: the root mean square of the game's level since the last Take (or
  // the first Set). Starts the next interval; the next update is due a pace after this one was.
  float Take(double now) {
    Advance(now);
    const double span = now - window_start_;
    const float out = span > 0 ? static_cast<float>(std::sqrt(energy_ / span)) : level_;
    window_start_ = now;
    energy_ = 0;
    due_ += kRumblePaceSeconds;
    if (due_ <= now) {
      due_ = now + kRumblePaceSeconds / 2;  // behind after a hitch: no burst of updates
    }
    return out;
  }
  // Forgets the interval: the next Set starts a new one (the wheel stopped the vibration).
  void Reset() { *this = RumblePace{}; }

 private:
  void Advance(double now) {
    if (!started_) {
      started_ = true;
      window_start_ = last_ = now;
      due_ = now + kRumblePaceSeconds;
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
