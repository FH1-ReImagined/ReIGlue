/**
 * @file        src/input/wheel/wheel_handbrake.h
 * @brief       An analog handbrake (#303), without SDL: which device and axis it is read from and
 *              how it reaches the game.
 *
 * FH1's wheel layouts (gamedb.slt WheelLayouts, all three) bind the Handbrake to the left stick up
 * and down (LeftThumbUp, LeftThumbDown) as well as to a face button (B in layout 0), as they bind
 * the clutch to the right stick Y: the Xbox 360 wheel's analog handbrake (a Fanatec shifter's
 * handbrake mode). A control is the strongest of the inputs bound to it, each 0 to 1, and a stick
 * direction is one of them (sub_82CBC118, sub_82CBAA78), so the handbrake axis goes to thumb_ly,
 * pulled as up, and the handbrake button still presses B: both can be bound at once.
 *
 * The axis is on the wheel (wheel_axes handbrake=N: a handbrake plugged into the wheel base) or on
 * a handbrake device (wheel_handbrake_device, its axis wheel_handbrake_axis): a known handbrake
 * (wheel_devices.h, kAuxModels) or one named like one, or the device the setting names, which may
 * also be the pedal or shifter device (Logitech's RS Shifter and Handbrake). An axis set in
 * wheel_axes wins; a number past the wheel's axes (the launcher's 99) reads nothing, so it also
 * turns a handbrake device off.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "wheel_devices.h"

namespace rex::input::wheel {

enum class HandbrakeSource { kNone, kWheel, kDevice };

struct HandbrakePlan {
  HandbrakeSource source = HandbrakeSource::kNone;
  int axis = -1;
};

// Where the handbrake axis is read: wheel_axes' "handbrake" (0 or more) on the wheel, else the
// handbrake device's axis while one is connected, else nowhere.
inline HandbrakePlan PlanHandbrake(const std::map<std::string, int>& wheel_axes,
                                   bool device_connected, int device_axis) {
  const auto it = wheel_axes.find("handbrake");
  if (it != wheel_axes.end() && it->second >= 0) {
    return {HandbrakeSource::kWheel, it->second};
  }
  if (device_connected && device_axis >= 0) {
    return {HandbrakeSource::kDevice, device_axis};
  }
  return {};
}

// The handbrake device among the joysticks (wheel_handbrake_device): a named one (PickNamed), else
// the first known handbrake or one named like one (AuxRole::kOther); -1 for none. It is never taken
// as a wheel.
inline int PickHandbrakeDevice(const std::vector<JoyInfo>& joys, const std::string& setting) {
  switch (ParseDeviceChoice(setting)) {
    case DeviceChoice::kNone:
      return -1;
    case DeviceChoice::kNamed:
      return PickNamed(joys, setting);
    case DeviceChoice::kAuto:
      break;
  }
  for (int i = 0; i < static_cast<int>(joys.size()); i++) {
    if (DetectAuxRole(joys[i].vid, joys[i].pid, joys[i].name) == AuxRole::kOther) {
      return i;
    }
  }
  return -1;
}

// A device's resting axes with the handbrake's taken out of pedal detection (DetectPedalAxes takes
// an axis that rests at an end for a pedal): it reads as centred.
inline std::vector<int16_t> RestWithout(const std::vector<int16_t>& rest, int axis) {
  std::vector<int16_t> out = rest;
  if (axis >= 0 && axis < static_cast<int>(out.size())) {
    out[axis] = 0;
  }
  return out;
}

// thumb_ly for the handbrake pulled `h` (0 released, 1 full): up, the layouts' LeftThumbUp.
inline int16_t HandbrakeThumb(float h) {
  return static_cast<int16_t>(std::lround(std::clamp(h, 0.0f, 1.0f) * 32767.0f));
}

}  // namespace rex::input::wheel
