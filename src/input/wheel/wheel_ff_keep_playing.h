/**
 * @file        src/input/wheel/wheel_ff_keep_playing.h
 * @brief       Starting the wheel's effects again for a wheel that ends them by itself (#304),
 *              without SDL.
 *
 * Every effect the driver plays has SDL's endless length (SDL_HAPTIC_INFINITY) and is started for
 * ever. SDL's DirectInput (Windows) and ForceFeedback (macOS) backends multiplied the length by
 * 1000 in 32 bits (`length * 1000UL`, SDL 3.4.14 and SDL's main branch): 4294966296 us, about 71
 * minutes, never their endless value, and more than a HID PID wheel's 16-bit duration field holds.
 * A Simagic Alpha EVO Pro on Windows lost the game's steering force and damper about 30 s after
 * each start, until a pause started them again. SDL patch 0147 (patches/rexglue-sdk/sdl3) sends
 * the endless value there. For a wheel that still ends effects by itself:
 *  - every 10 s the driver asks the wheel whether the game's effects still play (where SDL can ask:
 *    DirectInput, macOS, SDL's HIDAPI Logitech driver) and starts one that stopped again;
 *  - wheel_ffb_keep_playing N starts every effect that plays again every N seconds (starting a
 *    playing effect restarts it unchanged, which some wheels may feel as a bump: 0, the default,
 *    never); -1 starts nothing again, not even an effect the wheel says has stopped.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>

#include "wheel_ff_log.h"

namespace rex::input::wheel {

// The interval of the restarts, in seconds, for the wheel_ffb_keep_playing setting; 0: none.
constexpr int FfKeepPlayingSeconds(int setting) {
  return setting > 0 ? setting : 0;
}

// Whether an effect the wheel says has stopped by itself is started again.
constexpr bool FfRestartStopped(int setting) {
  return setting >= 0;
}

// The status check of one effect the driver started: `playing` is what the wheel says (1 playing,
// 0 stopped by itself, -1 it could not tell); `start` starts it again (true when it worked).
struct FfStoppedCheck {
  bool stopped = false;    // the wheel says it has stopped
  bool restarted = false;  // started again
  bool failed = false;     // starting it again failed
};
template <typename Start>
FfStoppedCheck FfCheckStopped(int playing, int setting, Start&& start) {
  FfStoppedCheck c;
  c.stopped = playing == 0;
  if (c.stopped && FfRestartStopped(setting)) {
    c.restarted = start();
    c.failed = !c.restarted;
  }
  return c;
}

// The check in the log's summary: a restart, or a failure.
inline void FfNoteCheck(FfLog& log, uint32_t block, const FfStoppedCheck& c) {
  if (c.restarted) {
    log.Sdl(block, FfSdlCall::kKeepPlaying);
  } else if (c.failed) {
    log.Sdl(block, FfSdlCall::kFailure);
  }
}

// A warning repeated many times is logged the 1st, 10th, 100th... time.
constexpr bool FfLogThisCount(int n) {
  for (long long k = 1; k <= n; k *= 10) {
    if (k == n) {
      return true;
    }
  }
  return false;
}

// When the effects are next started again: `last` is when they last were (or when the wheel was
// opened), all in seconds.
constexpr bool FfKeepPlayingDue(double last, double now, int seconds) {
  return seconds > 0 && now - last >= seconds;
}

}  // namespace rex::input::wheel
