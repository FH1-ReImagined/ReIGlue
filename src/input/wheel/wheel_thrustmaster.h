/**
 * @file        src/input/wheel/wheel_thrustmaster.h
 * @brief       Thrustmaster wheels by USB id (#308), and a wheel whose axes all read 0 when it is
 *              opened, without SDL.
 *
 * A Thrustmaster TMX (044f:b67f, whose Windows driver gives it no name: SDL calls it "44F B67F")
 * reported 0 on all four axes when the game opened it, before its first report, so detection from
 * where each axis rests found no pedal and a pedal could be taken for the steering (#308: 90
 * degrees of wheel before anything happened). Known models now have their axes and buttons built
 * in, and a wheel whose axes all read exactly 0 when opened has its rest read again once a report
 * comes in (RestPending, RestArrived).
 *
 * Source: Project Motor Racing's device maps ([Pitwall](https://github.com/Apkallu-Industries/Pitwall)
 * `PMR/shared/inputDevices/Thrustmaster_*.xml`, commit 01acf41; rawInput backend): each wheel's
 * product id, the HID usage of its steering (X), throttle (Rz), brake (Y) and clutch (Slider), and
 * its 0-based HID buttons. SDL numbers a joystick's axes in HID usage order (X, Y, Z, Rx, Ry, Rz,
 * Slider, Dial; Linux evdev keeps that order) and its buttons by HID button number, so the TMX's X,
 * Y, Rz and Slider are SDL axes 0 to 3. The numbers hold only with the axis count the map assumes.
 * The left and right paddle (buttons 0 and 1 on all of them) are the left and right bumper, which
 * the game's wheel layouts shift down and up with. Where the pedals rest is read from the wheel, not
 * assumed. The T248 is left out: no source gives its axes (hid-tmff2's report descriptor marks them
 * as guesses).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rex::input::wheel {

struct ThrustmasterModel {
  uint16_t pid;
  const char* name;
  int axes;  // the axis count the map assumes
  int steer, throttle, brake, clutch;
  // SDL buttons for A B X Y, the left and right bumper, Back, Start, the stick presses (-1: none).
  int a, b, x, y, lb, rb, back, start, ls, rs;
};

constexpr uint16_t kThrustmasterVid = 0x044F;

// clang-format off
inline constexpr ThrustmasterModel kThrustmasterModels[] = {
    // Xbox wheels: A 4, B 8, X 2, Y 9 (PMR); their own bumpers (TMX 3 and 11, TX 6 and 7, TS-XW 3
    // and 5) stay free, the paddles shift.
    {0xB67F, "Thrustmaster TMX", 4, 0, 2, 1, 3, 4, 8, 2, 9, 0, 1, 6, 7, 5, 10},
    {0xB669, "Thrustmaster TX", 4, 0, 2, 1, 3, 4, 8, 2, 9, 0, 1, 3, 5, -1, -1},
    {0xB692, "Thrustmaster TS-XW", 4, 0, 2, 1, 3, 12, 8, 2, 9, 0, 1, 6, 7, -1, -1},
    {0xB689, "Thrustmaster TS-PC Racer", 4, 0, 2, 1, 3, 12, 8, 2, 9, 0, 1, 6, 7, -1, -1},
    // PlayStation wheels: cross 5 (A), circle 4 (B), square 3 (X), triangle 2 (Y), SET 6 (Back),
    // Options 7 (Start), L3 10, R3 11; R2 8 and L2 9 stay free.
    {0xB66E, "Thrustmaster T300 RS", 4, 0, 2, 1, 3, 5, 4, 3, 2, 0, 1, 6, 7, 10, 11},
    {0xB677, "Thrustmaster T150", 4, 0, 2, 1, 3, 5, 4, 3, 2, 0, 1, 6, 7, 10, 11},
    // T-GT: X, Y, Z, Rx, Ry, Rz, Slider, Dial (its rim's two thumbsticks on Rx, Ry, Z and Dial).
    {0xB684, "Thrustmaster T-GT", 8, 0, 5, 1, 6, 5, 4, 3, 2, 0, 1, 6, 7, 10, 11},
};
// clang-format on

inline const ThrustmasterModel* FindThrustmaster(uint16_t vid, uint16_t pid) {
  if (vid != kThrustmasterVid) {
    return nullptr;
  }
  for (const auto& m : kThrustmasterModels) {
    if (m.pid == pid) {
      return &m;
    }
  }
  return nullptr;
}

// The model's axes when the wheel has the axis count its map assumes, else none (detected).
inline const ThrustmasterModel* ThrustmasterAxes(const ThrustmasterModel* m, int num_axes) {
  return m && num_axes == m->axes ? m : nullptr;
}

// The model's buttons as wheel_buttons overrides; `user` (wheel_buttons) wins over them.
inline std::map<std::string, int> ThrustmasterButtons(const ThrustmasterModel* m,
                                                      const std::map<std::string, int>& user) {
  if (!m) {
    return user;
  }
  std::map<std::string, int> out = {{"a", m->a},       {"b", m->b},       {"x", m->x},
                                    {"y", m->y},       {"lb", m->lb},     {"rb", m->rb},
                                    {"back", m->back}, {"start", m->start}, {"ls", m->ls},
                                    {"rs", m->rs}};
  for (const auto& [k, v] : user) {
    out[k] = v;
  }
  return out;
}

// Every axis read exactly 0 when the wheel was opened: it has not reported yet (a centred stick
// reads about 0, but a pedal rests at an end), so where its axes rest is unknown.
inline bool RestPending(const std::vector<int16_t>& rest) {
  if (rest.size() < 2) {
    return false;
  }
  for (int16_t v : rest) {
    if (v != 0) {
      return false;
    }
  }
  return true;
}

// The wheel's first report has come in: some axis no longer reads 0. Its values are then the rest.
inline bool RestArrived(const std::vector<int16_t>& now) {
  for (int16_t v : now) {
    if (v != 0) {
      return true;
    }
  }
  return false;
}

}  // namespace rex::input::wheel
