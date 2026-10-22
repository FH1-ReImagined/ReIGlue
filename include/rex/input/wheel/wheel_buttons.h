/**
 * @file        rex/input/wheel/wheel_buttons.h
 * @brief       Racing wheel buttons: which SDL button presses which pad button, kept free of SDL
 *              so it can be tested on its own.
 *
 * The game takes the wheel for an Xbox 360 wheel, so its wheel layouts apply (gamedb.slt
 * WheelLayouts, layout 0): the bumpers are the 360 wheel's paddles and shift up (RB) and down
 * (LB), A is the clutch, B the handbrake, X Activate, Y Rewind and Back the camera.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace rex::input::wheel {

// Which order a wheel's buttons come in (SDL numbers follow the wheel's HID button order).
enum class ButtonLayout {
  kGeneric,      // face buttons, right and left paddle, Back, Start, the stick presses at 8-11
  kPlayStation,  // G29, G923 for PlayStation: cross, square, circle, triangle, paddles, R2, L2, ...
  kXbox,         // G920, G923 for Xbox: A, B, X, Y, paddles, Menu, View, RSB, LSB, Xbox, ...
};

// The layout of a known wheel by USB vendor:product (the generic one otherwise).
inline ButtonLayout ButtonLayoutOf(uint16_t vid, uint16_t pid) {
  if (vid == 0x046D) {
    switch (pid) {
      case 0xC24F:  // G29
      case 0xC266:  // G923 for PlayStation and PC
      case 0xC267:  // the same in PS4 mode
        return ButtonLayout::kPlayStation;
      case 0xC262:  // G920
      case 0xC26E:  // G923 for Xbox and PC
      case 0xC26D:  // the same in Xbox mode
        return ButtonLayout::kXbox;
      default:
        break;
    }
  }
  return ButtonLayout::kGeneric;
}

// Pad button name -> SDL button number (-1: none). The usual wheel order: face buttons, the right
// and left paddle, then Share/Back and Options/Start and the stick presses. On a G29 or G923
// (PlayStation version, 046d:c24f and c266) buttons 1 and 2 are square and circle, so B and X
// swap; 6 and 7 (R2, L2) are not used. The paddles sat on B and X while the game took every wheel
// for a gamepad, whose layout shifts with those (#219). A G920 or G923 for Xbox (c262, c26e) has
// Menu (Start) on 6, View (Back) on 7, RSB 8, LSB 9 and the Xbox button 10; 11-17 are the
// Driving Force Shifter (PCGamingWiki's G920 and Xbox G923 tables, counted from 1 there), so the
// generic stick presses on 10 and 11 were the Xbox button and the shifter's reverse (#256).
// `overrides` is wheel_buttons. A button in `taken` (the shifter's, wheel_shifter.h) presses no pad
// button, whatever the defaults or the overrides say.
inline std::map<std::string, int> WheelButtons(ButtonLayout layout,
                                               const std::map<std::string, int>& overrides,
                                               const std::set<int>& taken = {}) {
  std::map<std::string, int> map = {{"a", 0},     {"b", 1},     {"x", 2},     {"y", 3},
                                    {"rb", 4},    {"lb", 5},    {"back", 8},  {"start", 9},
                                    {"rs", 10},   {"ls", 11},   {"up", -1},   {"down", -1},
                                    {"left", -1}, {"right", -1}};
  if (layout == ButtonLayout::kPlayStation) {
    map["b"] = 2;
    map["x"] = 1;
  } else if (layout == ButtonLayout::kXbox) {
    map["start"] = 6;
    map["back"] = 7;
    map["rs"] = 8;
    map["ls"] = 9;
  }
  for (const auto& [k, v] : overrides) {
    if (map.count(k)) {
      map[k] = v;
    }
  }
  for (auto& [k, v] : map) {
    if (taken.count(v)) {
      v = -1;
    }
  }
  return map;
}

// The d-pad bits from the wheel's first hat (SDL hat bits: up 1, right 2, down 4, left 8) for the
// directions wheel_buttons leaves out. A d-pad direction it sets replaces the hat's (#279): a
// button number presses it instead, -1 turns it off (the launcher's Clear on a d-pad row).
inline uint16_t HatDpad(uint8_t hat, const std::map<std::string, int>& overrides) {
  static constexpr struct {
    uint8_t hat;
    const char* key;
    uint16_t bit;
  } kDirs[] = {{0x01, "up", 0x0001}, {0x04, "down", 0x0002}, {0x08, "left", 0x0004},
               {0x02, "right", 0x0008}};
  uint16_t out = 0;
  for (const auto& d : kDirs) {
    if ((hat & d.hat) && !overrides.count(d.key)) {
      out |= d.bit;
    }
  }
  return out;
}

}  // namespace rex::input::wheel
