/**
 * @file        src/input/wheel/wheel_primary.h
 * @brief       Which joysticks are wheels and which of them is the wheel (#306), without SDL.
 *
 * Every wheel the driver takes is a controller of its own, in the order it presents them, and the
 * first is the one the game's first player drives with and the one pedal, shifter and handbrake
 * devices are read with (wheel_devices.h). In SDL's order a virtual joystick (vJoy, fed by SimHub
 * with pedals, handbrake and shifter) or a button box came before the real wheel base, so it became
 * the wheel (#306: a Fanatec base with wheel_mode = on). The wheels are now ordered: the device
 * wheel_device names (a name, SDL GUID or vid:pid: exact before part of a name), then those with
 * force feedback, then those SDL lists as wheels, then SDL's order. A device read as the pedal,
 * shifter or handbrake device (named in their settings or found by itself) is no wheel, unless
 * wheel_device names it.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "wheel_devices.h"

namespace rex::input::wheel {

// How well wheel_device names a joystick: 2 exactly (name, GUID or vid:pid), 1 part of its name,
// 0 not at all (or auto).
inline int WheelDeviceMatch(const std::string& wheel_device, const JoyInfo& j) {
  return ParseDeviceChoice(wheel_device) == DeviceChoice::kNamed ? DeviceMatch(wheel_device, j)
                                                                  : 0;
}

// The joysticks taken as wheels, the wheel first. wheel_like: the joystick is a wheel by itself
// (IsWheelDevice); ffb: it has force feedback; aux: it is read as a pedal, shifter or handbrake
// device.
inline std::vector<int> WheelOrder(const std::vector<JoyInfo>& joys,
                                   const std::vector<bool>& wheel_like,
                                   const std::vector<bool>& ffb, const std::vector<bool>& aux,
                                   const std::string& wheel_device) {
  std::vector<int> out;
  for (int i = 0; i < static_cast<int>(joys.size()); i++) {
    const bool named = WheelDeviceMatch(wheel_device, joys[i]) > 0;
    if (named || (wheel_like[i] && !aux[i])) {
      out.push_back(i);
    }
  }
  auto key = [&](int i) {
    return WheelDeviceMatch(wheel_device, joys[i]) * 4 + (ffb[i] ? 2 : 0) +
           (joys[i].sdl_wheel ? 1 : 0);
  };
  std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return key(a) > key(b); });
  return out;
}

}  // namespace rex::input::wheel
