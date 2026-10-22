/**
 * @file        rex/input/wheel/wheel_ff.h
 * @brief       The Xbox 360 wheel's force feedback as games drive it through the kernel's
 *              XInputdFF* calls: effect blocks, their state and what they ask of the wheel,
 *              without SDL (the wheel driver plays them on SDL haptics).
 *
 * Forza Horizon looks XInputdFFGetDeviceInfo, SetEffect, UpdateEffect, EffectOperation,
 * DeviceControl, SetDeviceGain, CancelIo and SetRumble (xboxkrnl ordinals 0x282-0x289) up at
 * run time once it sees a wheel with force feedback, through its XInputFF library
 * (sub_831CD7F8 and the wrappers after it). The handle comes from XamUserGetDeviceContext.
 * The game (sub_82CBB2D8, sub_82CB76B8, sub_82CBB790) then:
 *  - needs GetDeviceInfo's second word (effects the device plays at once) to be at least 5;
 *  - stops everything with DeviceControl(3), defines effect blocks 1-5 with one SetEffect and
 *    starts them with EffectOperation({block, 2}..., count, 1): inertia, friction, damper and
 *    spring, scaled by Game:\Media\ControllerFFB.ini and the wheel options' force feedback
 *    scale (inertia and friction are 0 there), and a constant force;
 *  - each frame updates the constant force's magnitude (UpdateEffect flags 0x400: the
 *    type-specific parameters) with the steering force of the car, and the spring's gain
 *    (flags 0x100) with the speed and the rear wheels' slip;
 *  - sends its rumble with SetRumble, a 360 controller's two motor speeds.
 * An effect is 29 big-endian words: block (1-based), type, a word FH1 sets to 4, gain (0-255),
 * seven words FH1 sets to fixed values (0xFF0, 0, 0, 0xFF, 0xFF, 0, 0), then the type's
 * parameters from word 11. The types are numbered in the HID PID order: 1 constant, 8 spring,
 * 9 damper, 10 inertia, 11 friction.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace rex::input::wheel {

constexpr uint32_t kFfConstant = 1;
constexpr uint32_t kFfSpring = 8;
constexpr uint32_t kFfDamper = 9;
constexpr uint32_t kFfInertia = 10;
constexpr uint32_t kFfFriction = 11;

constexpr size_t kFfEffectWords = 29;  // 0x74 bytes
constexpr uint32_t kFfMaxBlocks = 16;  // what GetDeviceInfo reports; FH1 uses 1-5

// EffectOperation: FH1 starts its effects with 2. 3 stops (as in HID PID); 1 is taken as a start.
constexpr uint32_t kFfOpStop = 3;
// DeviceControl, in the HID PID order. FH1 sends 3 before it defines its effects.
constexpr uint32_t kFfEnableActuators = 1, kFfDisableActuators = 2, kFfStopAll = 3, kFfReset = 4,
                   kFfPause = 5, kFfContinue = 6;

struct FfEffect {
  uint32_t block = 0;  // 1-based effect block
  uint32_t type = 0;
  uint32_t gain = 0;   // 0-255
  // Constant: word 11 is the magnitude, -127 to 127. Conditions (spring, damper, inertia,
  // friction): centre offset and the coefficients on the positive and negative side (-127 to
  // 127), the saturations on each side and the dead band (0-255).
  int32_t magnitude = 0;
  int32_t center = 0;
  int32_t pos_coeff = 0, neg_coeff = 0;
  int32_t pos_sat = 0, neg_sat = 0;
  int32_t deadband = 0;
};

inline uint32_t FfWord(const uint8_t* be, size_t i) {
  const uint8_t* p = be + 4 * i;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// One effect as the game laid it out in guest memory.
inline FfEffect DecodeFfEffect(const uint8_t* be) {
  FfEffect e;
  e.block = FfWord(be, 0);
  e.type = FfWord(be, 1);
  e.gain = FfWord(be, 3);
  e.magnitude = e.center = static_cast<int32_t>(FfWord(be, 11));
  e.pos_coeff = static_cast<int32_t>(FfWord(be, 12));
  e.neg_coeff = static_cast<int32_t>(FfWord(be, 13));
  e.pos_sat = static_cast<int32_t>(FfWord(be, 14));
  e.neg_sat = static_cast<int32_t>(FfWord(be, 15));
  e.deadband = static_cast<int32_t>(FfWord(be, 16));
  return e;
}

inline bool FfIsCondition(uint32_t type) {
  return type == kFfSpring || type == kFfDamper || type == kFfInertia || type == kFfFriction;
}

// What the game has asked of the device: its effect blocks and whether each plays.
class FfState {
 public:
  // SetEffect / UpdateEffect: (re)defines the effect's block. False for a block out of range.
  bool Define(const FfEffect& e) {
    if (e.block < 1 || e.block > kFfMaxBlocks) {
      return false;
    }
    fx_[e.block - 1] = e;
    defined_[e.block - 1] = true;
    return true;
  }
  // EffectOperation for one block. False for a block that was never defined.
  bool Operate(uint32_t block, uint32_t op) {
    if (block < 1 || block > kFfMaxBlocks || !defined_[block - 1]) {
      return false;
    }
    playing_[block - 1] = op != kFfOpStop;
    return true;
  }
  void Control(uint32_t command) {
    switch (command) {
      case kFfEnableActuators: actuators_ = true; break;
      case kFfDisableActuators: actuators_ = false; break;
      case kFfStopAll: std::fill(playing_, playing_ + kFfMaxBlocks, false); break;
      case kFfReset:
        std::fill(playing_, playing_ + kFfMaxBlocks, false);
        std::fill(defined_, defined_ + kFfMaxBlocks, false);
        paused_ = false;
        actuators_ = true;
        break;
      case kFfPause: paused_ = true; break;
      case kFfContinue: paused_ = false; break;
      default: break;
    }
  }
  void SetDeviceGain(uint32_t gain) { device_gain_ = std::min<uint32_t>(gain, 255); }

  bool defined(uint32_t block) const { return block >= 1 && block <= kFfMaxBlocks && defined_[block - 1]; }
  const FfEffect& effect(uint32_t block) const { return fx_[block - 1]; }
  // The block's effect should be felt now.
  bool Active(uint32_t block) const {
    return defined(block) && playing_[block - 1] && actuators_ && !paused_;
  }
  uint32_t device_gain() const { return device_gain_; }

 private:
  FfEffect fx_[kFfMaxBlocks]{};
  bool defined_[kFfMaxBlocks]{};
  bool playing_[kFfMaxBlocks]{};
  bool actuators_ = true;
  bool paused_ = false;
  uint32_t device_gain_ = 255;
};

// The player's strengths, 0 to 1 (100 percent is 1): wheel_ffb_strength scales everything, the
// others one part on top of it (#256).
struct FfStrengths {
  float overall = 1.0f;  // wheel_ffb_strength
  float spring = 1.0f;   // wheel_ffb_spring_strength: the centring spring and the damper
  float engine = 1.0f;   // wheel_rumble_engine_strength: the rumble's right motor (FH1: the engine)
  float road = 1.0f;     // wheel_rumble_road_strength: the left motor (road surface, kerbs, impacts)
};

// The strength an effect block plays at: the spring and the damper also take the spring strength,
// so the centring can be softened without the steering force and the road feel.
inline float FfTypeStrength(uint32_t type, const FfStrengths& s) {
  return s.overall * ((type == kFfSpring || type == kFfDamper) ? s.spring : 1.0f);
}

// The game's rumble as one vibration, 0 to 1: the stronger of the two motors, each scaled by its
// own strength (in a car that revs quickly the engine motor near the redline drowned out the road),
// then by the overall strength.
inline float FfRumbleLevel(uint16_t left_motor, uint16_t right_motor, const FfStrengths& s) {
  const float road = left_motor / 65535.0f * s.road;
  const float engine = right_motor / 65535.0f * s.engine;
  return std::clamp(std::max(road, engine) * s.overall, 0.0f, 1.0f);
}

// The game's forces fade out while the player is not driving and come back when they are (#256).
// FH1 keeps its spring and damper playing with their last values in the pause menu, rewinds and
// replays, and plays the steering force and rumble of the car going backwards while it rewinds;
// the wheel then pulled and buzzed. `level` multiplies every force and the rumble.
constexpr float kFfFadeOutSeconds = 0.25f;
constexpr float kFfFadeInSeconds = 0.5f;
struct FfFade {
  float level = 1.0f;
  // Moves level towards 1 (driving) or 0 by `dt` seconds' worth; true when it changed.
  bool Step(bool driving, float dt) {
    const float before = level;
    dt = std::max(dt, 0.0f);
    level = driving ? std::min(1.0f, level + dt / kFfFadeInSeconds)
                    : std::max(0.0f, level - dt / kFfFadeOutSeconds);
    return level != before;
  }
};

// How strongly an effect plays, 0 to 1: its gain, the device gain and the player's strength.
inline float FfScale(const FfEffect& e, uint32_t device_gain, float strength) {
  const float g = std::min<uint32_t>(e.gain, 255) / 255.0f * std::min<uint32_t>(device_gain, 255) / 255.0f;
  return std::clamp(g * strength, 0.0f, 1.0f);
}

// A constant force on the steering axis, -1 to 1, in the game's direction (FH1: positive
// turns the wheel to the right; wheel_input_driver.cpp maps it onto SDL's steering axis).
inline float FfConstantLevel(const FfEffect& e, float scale) {
  return std::clamp(e.magnitude / 127.0f, -1.0f, 1.0f) * scale;
}

// A condition on the steering axis: coefficients -1 to 1 (positive pulls back to the centre),
// saturations, dead band and centre as fractions of the full range.
struct FfCondition {
  float right_coeff = 0, left_coeff = 0;
  float right_sat = 0, left_sat = 0;
  float deadband = 0;
  float center = 0;
};

inline FfCondition FfConditionOf(const FfEffect& e, float scale) {
  FfCondition c;
  c.right_coeff = std::clamp(e.pos_coeff / 127.0f, -1.0f, 1.0f) * scale;
  c.left_coeff = std::clamp(e.neg_coeff / 127.0f, -1.0f, 1.0f) * scale;
  c.right_sat = std::clamp(e.pos_sat / 255.0f, 0.0f, 1.0f) * scale;
  c.left_sat = std::clamp(e.neg_sat / 255.0f, 0.0f, 1.0f) * scale;
  c.deadband = std::clamp(e.deadband / 255.0f, 0.0f, 1.0f);
  c.center = std::clamp(e.center / 127.0f, -1.0f, 1.0f);
  return c;
}

// True when the condition can push at all (FH1's inertia and friction are all zero).
inline bool FfConditionFelt(const FfCondition& c) {
  return (c.right_coeff != 0 && c.right_sat > 0) || (c.left_coeff != 0 && c.left_sat > 0);
}

// A spring as a self-centring strength, 0 to 1, for wheels that can centre themselves but have no
// spring effect (the Linux driver of the G25, G27 and G29): the weaker side's pull.
inline float FfSpringAutocenter(const FfCondition& c) {
  const float right = std::min(std::max(c.right_coeff, 0.0f), c.right_sat);
  const float left = std::min(std::max(c.left_coeff, 0.0f), c.left_sat);
  return std::clamp(std::min(right, left), 0.0f, 1.0f);
}

// The self-centring for the game's springs on a wheel without a spring effect, 0 to 1: the
// strongest spring block that is defined and playing; 0 once none is (stopped, paused, reset, or
// the block redefined as another type).
inline float FfAutocenter(const FfState& s, float strength) {
  float level = 0.0f;
  for (uint32_t b = 1; b <= kFfMaxBlocks; b++) {
    if (!s.Active(b) || s.effect(b).type != kFfSpring) {
      continue;
    }
    const FfEffect& e = s.effect(b);
    level = std::max(level, FfSpringAutocenter(FfConditionOf(e, FfScale(e, s.device_gain(), strength))));
  }
  return level;
}

// The kernel's XInputdFF* calls (xboxkrnl_hid.cpp), played on every wheel the wheel driver
// has open (wheel_input_driver.cpp). Each is false when there is no wheel.
bool GameFfDeviceInfo();
bool GameFfSetEffect(const FfEffect& effect);
bool GameFfUpdateEffect(const FfEffect& effect, uint32_t flags);
bool GameFfOperate(uint32_t block, uint32_t op);
bool GameFfControl(uint32_t command);
bool GameFfSetGain(uint32_t gain);
bool GameFfSetRumble(uint16_t left_motor, uint16_t right_motor);

}  // namespace rex::input::wheel
