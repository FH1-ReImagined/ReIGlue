/**
 * @file        src/input/wheel/wheel_ff_log.h
 * @brief       What the game asks of a wheel's force feedback and what the driver plays on it, for
 *              the log (#304), without SDL: a line when an effect block changes, a summary every
 *              10 s.
 *
 * A Simagic Alpha EVO Pro owner on Windows felt the game's steering force stop about 30 s after
 * every pause, leaving only a centring spring, while the log said nothing: the driver logged each
 * effect block once, at its first definition, and nothing it did with them after. Now:
 *  - a block's line when its type, a condition's coefficients, saturations, dead band or centre,
 *    or the constant force's gain to or from 0 change, at most once a second per block (a change
 *    in between is logged at the next update after it). The steering force's magnitude (every
 *    frame) and a condition's gain (FH1's spring moves with the speed) are left to the summary;
 *  - the game's effect operations and device controls, the first few of each 10 s;
 *  - every 10 s while the game drives the force feedback, one summary: per block what the game
 *    sent (updates, gain and force range, starts and stops, whether it plays) and what the wheel
 *    got (SDL updates, creations, starts, restarts, stops, failures, the level or coefficient
 *    range, whether it runs and, where SDL can ask, whether the wheel says it still plays), then
 *    the game's rumbles and the updates the wheel's vibration got, with how many a second and
 *    their highest level (#467).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <string>

#include <rex/input/wheel/wheel_ff.h>

namespace rex::input::wheel {

constexpr double kFfLogSummarySeconds = 10.0;
constexpr double kFfLogBlockSeconds = 1.0;  // a block's change lines at most this often
constexpr uint32_t kFfLogEventsPerWindow = 8;  // operations and controls logged per summary window

// A change worth its own line: the type; the constant force's gain to or from 0; a condition's
// coefficients, saturations, dead band and centre. Not the constant force's magnitude, nor a
// condition's gain: FH1 moves its spring's gain with the speed, through 0 every few seconds.
inline bool FfBlockChanged(const FfEffect& a, const FfEffect& b) {
  if (a.type != b.type) {
    return true;
  }
  if (a.type == kFfConstant) {
    return (a.gain == 0) != (b.gain == 0);
  }
  return a.center != b.center || a.pos_coeff != b.pos_coeff || a.neg_coeff != b.neg_coeff ||
         a.pos_sat != b.pos_sat || a.neg_sat != b.neg_sat || a.deadband != b.deadband;
}

inline const char* FfBlockTypeName(uint32_t type) {
  switch (type) {
    case kFfConstant: return "constant";
    case kFfSpring: return "spring";
    case kFfDamper: return "damper";
    case kFfInertia: return "inertia";
    case kFfFriction: return "friction";
    default: return "other";
  }
}

inline const char* FfControlName(uint32_t command) {
  switch (command) {
    case kFfEnableActuators: return "actuators on";
    case kFfDisableActuators: return "actuators off";
    case kFfStopAll: return "stop all";
    case kFfReset: return "reset";
    case kFfPause: return "pause";
    case kFfContinue: return "continue";
    default: return "unknown";
  }
}

// What the driver does with a block on the wheel (SDL haptics).
enum class FfSdlCall { kCreate, kUpdate, kStart, kKeepPlaying, kStop, kFailure };

class FfLog {
 public:
  // SetEffect / UpdateEffect of a defined block, at `now` seconds: true when its line is due. Not
  // for a block's first definition, which the driver logs once whatever comes after.
  bool Defined(const FfEffect& e, double now) {
    if (e.block < 1 || e.block > kFfMaxBlocks) {
      return false;
    }
    Block& b = blocks_[e.block - 1];
    Window& w = b.window;
    w.sets++;
    w.min_gain = std::min(w.min_gain, e.gain);
    w.max_gain = std::max(w.max_gain, e.gain);
    if (e.type == kFfConstant) {
      w.min_force = std::min(w.min_force, e.magnitude);
      w.max_force = std::max(w.max_force, e.magnitude);
    }
    active_ = true;
    if (!b.logged) {
      b.logged = true;
      b.logged_at = now;
      b.last = e;
      return false;
    }
    if (!FfBlockChanged(b.last, e) || now - b.logged_at < kFfLogBlockSeconds) {
      return false;  // a change within the second is caught at the next update after it
    }
    b.logged_at = now;
    b.last = e;
    return true;
  }
  // EffectOperation and DeviceControl: true when the call gets its own line.
  bool Operated(uint32_t block, uint32_t op) {
    if (block >= 1 && block <= kFfMaxBlocks) {
      Window& w = blocks_[block - 1].window;
      (op == kFfOpStop ? w.stops : w.starts)++;
    }
    active_ = true;
    return ++events_ <= kFfLogEventsPerWindow;
  }
  bool Controlled() {
    controls_++;
    active_ = true;
    return ++events_ <= kFfLogEventsPerWindow;
  }
  void Rumbled() { rumbles_++; }
  // The driver on the wheel: `value` is the SDL level (constant) or right coefficient (conditions)
  // sent with a creation or an update.
  void Sdl(uint32_t block, FfSdlCall call, int32_t value = 0) {
    if (block < 1 || block > kFfMaxBlocks) {
      return;
    }
    Window& w = blocks_[block - 1].window;
    switch (call) {
      case FfSdlCall::kCreate: w.creates++; break;
      case FfSdlCall::kUpdate: w.updates++; break;
      case FfSdlCall::kStart: w.starts_sdl++; return;
      case FfSdlCall::kKeepPlaying: w.kept++; return;
      case FfSdlCall::kStop: w.stops_sdl++; return;
      case FfSdlCall::kFailure: w.failures++; return;
    }
    w.min_value = std::min(w.min_value, value);
    w.max_value = std::max(w.max_value, value);
  }
  // Rumble restarts that keep the vibration playing (the keep-alive, #304).
  void RumbleKept() { rumble_kept_++; }
  // An update of a vibration effect on the wheel (wheel_rumble_pace.h, #467), to `level` (0 to 1).
  void Vibrated(float level) {
    vibration_updates_++;
    vibration_max_ = std::max(vibration_max_, level);
  }

  // Whether the window is kFfLogSummarySeconds old: Summary then gives its line.
  bool SummaryDue(double now) {
    if (window_start_ < 0) {
      window_start_ = now;
    }
    return now - window_start_ >= kFfLogSummarySeconds;
  }
  // The summary when the window is due and the game did something in it (empty otherwise); then a
  // new window starts. `running` has bit (block - 1) set for each block the first wheel was told
  // to play, `ended` for each of those the wheel says it does not play (where SDL can ask,
  // SDL_HAPTIC_STATUS: DirectInput, macOS, SDL's HIDAPI Logitech driver); `fade` is the fade level
  // (wheel_ff.h FfFade).
  std::string Summary(const FfState& s, uint32_t running, uint32_t ended, float fade, double now) {
    if (!SummaryDue(now)) {
      return {};
    }
    const double seconds = now - window_start_;
    std::string out;
    if (active_) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "the last %.0f s:", seconds);
      out = buf;
      for (uint32_t i = 0; i < kFfMaxBlocks; i++) {
        const Window& b = blocks_[i].window;
        const bool runs = running >> i & 1;
        const bool quiet = !b.sets && !b.starts && !b.stops && !b.creates && !b.updates &&
                           !b.starts_sdl && !b.kept && !b.stops_sdl && !b.failures;
        if (!s.defined(i + 1) && quiet && !runs) {
          continue;
        }
        const FfEffect& e = s.effect(i + 1);
        std::snprintf(buf, sizeof(buf), "%sblock %u %s: ", out.back() == ':' ? " " : " | ", i + 1,
                      s.defined(i + 1) ? FfBlockTypeName(e.type) : "undefined");
        out += buf;
        if (quiet) {  // nothing sent either way: where it stands
          std::snprintf(buf, sizeof(buf), "gain %u, %s, %s%s", e.gain,
                        s.Active(i + 1) ? "playing" : "not playing",
                        runs ? "running" : "not running",
                        ended >> i & 1 ? " (the wheel says it is not playing)" : "");
          out += buf;
          continue;
        }
        std::snprintf(buf, sizeof(buf), "game %u updates", b.sets);
        out += buf;
        out += b.sets ? Range(", gain ", b.min_gain, b.max_gain) : Range(", gain ", e.gain, e.gain);
        if (b.min_force <= b.max_force) {
          out += Range(", force ", b.min_force, b.max_force);
        }
        out += Count(b.starts, "start");
        out += Count(b.stops, "stop");
        out += s.Active(i + 1) ? ", playing" : ", not playing";
        std::snprintf(buf, sizeof(buf), "; wheel %u updates", b.updates);
        out += buf;
        out += Count(b.creates, "creation");
        out += Count(b.starts_sdl, "start");
        out += Count(b.kept, "restart");
        out += Count(b.stops_sdl, "stop");
        out += Count(b.failures, "failure");
        if (b.min_value <= b.max_value) {
          out += Range(s.defined(i + 1) && e.type == kFfConstant ? ", level " : ", coefficient ",
                       b.min_value, b.max_value);
        }
        out += runs ? ", running" : ", not running";
        out += ended >> i & 1 ? " (the wheel says it is not playing)" : "";
      }
      out += " |";
      if (controls_) {
        std::snprintf(buf, sizeof(buf), " %u device control%s,", controls_,
                      controls_ == 1 ? "" : "s");
        out += buf;
      }
      std::snprintf(buf, sizeof(buf), " device gain %u, %u rumbles", s.device_gain(), rumbles_);
      out += buf;
      if (vibration_updates_) {  // what the wheel's vibration got, a second (#467)
        std::snprintf(buf, sizeof(buf), ", %u vibration updates (%.0f a second, up to %.0f%%)",
                      vibration_updates_, vibration_updates_ / seconds, vibration_max_ * 100.0f);
        out += buf;
      }
      out += Count(rumble_kept_, "rumble restart");
      std::snprintf(buf, sizeof(buf), ", forces at %.0f%%", fade * 100.0f);
      out += buf;
    }
    for (Block& b : blocks_) {
      b.window = Window{};
    }
    controls_ = rumbles_ = rumble_kept_ = events_ = vibration_updates_ = 0;
    vibration_max_ = 0.0f;
    active_ = false;
    window_start_ = now;
    return out;
  }

 private:
  // What the game sent and the wheel got, this window.
  struct Window {
    uint32_t sets = 0, starts = 0, stops = 0;
    uint32_t min_gain = UINT32_MAX, max_gain = 0;
    int32_t min_force = INT32_MAX, max_force = INT32_MIN;
    uint32_t creates = 0, updates = 0, starts_sdl = 0, kept = 0, stops_sdl = 0, failures = 0;
    int32_t min_value = INT32_MAX, max_value = INT32_MIN;
  };
  struct Block {
    Window window;
    // The block's last line.
    bool logged = false;
    double logged_at = 0;
    FfEffect last{};
  };
  template <typename T>
  static std::string Range(const char* what, T lo, T hi) {
    char buf[64];
    if (lo == hi) {
      std::snprintf(buf, sizeof(buf), "%s%lld", what, static_cast<long long>(lo));
    } else {
      std::snprintf(buf, sizeof(buf), "%s%lld..%lld", what, static_cast<long long>(lo),
                    static_cast<long long>(hi));
    }
    return buf;
  }
  static std::string Count(uint32_t n, const char* what) {
    if (!n) {
      return {};
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), ", %u %s%s", n, what, n == 1 ? "" : "s");
    return buf;
  }

  Block blocks_[kFfMaxBlocks];
  uint32_t controls_ = 0, rumbles_ = 0, rumble_kept_ = 0, events_ = 0, vibration_updates_ = 0;
  float vibration_max_ = 0.0f;
  bool active_ = false;
  double window_start_ = -1;
};

}  // namespace rex::input::wheel
