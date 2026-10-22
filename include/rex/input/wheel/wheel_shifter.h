/**
 * @file        rex/input/wheel/wheel_shifter.h
 * @brief       Racing wheel shifter and clutch: the gear and button logic of the wheel driver,
 *              kept free of SDL so it can be tested on its own.
 *
 * FH1 has the Xbox 360 wheel's shifter input: when the capabilities' thumb_rx is neither 0 nor
 * 1 the wheel has an H-pattern shifter (sub_82CBAFE0; its high byte is the number of forward
 * gears), and the state's thumb_rx is then the gear the lever is in: -1 reverse, 0 neutral, 1 to
 * 8 (sub_82CBC320, read by the car's input frame sub_824B7360 through control 7, which the
 * wheel layouts bind to it). The analog clutch is the right stick Y (the wheel layouts' Clutch).
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

constexpr int kGearReverse = -1;
constexpr int kGearNeutral = 0;
constexpr int kMaxGear = 7;

struct ShifterMap {
  // SDL button numbers; -1 is unmapped. gear[0] is reverse, gear[1..7] the forward gears.
  int gear[kMaxGear + 1] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int up = -1, down = -1;  // a sequential shifter
};

// The Logitech Driving Force Shifter: gears 1-6 on buttons 12-17 and reverse (pushed down) on
// `reverse`, which depends on the wheel it is plugged into (DefaultShifter).
inline ShifterMap LogitechShifter(int reverse = 18) {
  ShifterMap m;
  for (int g = 1; g <= 6; g++) {
    m.gear[g] = 11 + g;
  }
  m.gear[0] = reverse;
  return m;
}

// The built-in shifter map of a wheel, by USB vendor:product (SDL button numbers, the wheel's HID
// button order on Windows and Linux alike). The Driving Force Shifter's gears 1-6 are buttons 12-17
// on all of them; reverse differs:
//  - G29 (046d:c24f) and G923 for PlayStation and PC (c266; c267 is its PS4 mode before the mode
//    switch): 25 buttons, reverse after the gears on 18 (Logitech's shifter table; the
//    PlayStation G923's own table on PCGamingWiki: reverse button 19, counted from 1);
//  - G920 (c262) and G923 for Xbox and PC (c26e; c26d is its Xbox mode before the mode switch):
//    face buttons, paddles, Menu, View, the stick presses and the Xbox button are 0-10, and
//    reverse comes before the gears, on 11 (PCGamingWiki's G920 and Xbox G923 tables: reverse 12,
//    gears 13-18, counted from 1; a G923 for Xbox owner found reverse on 11, #256).
// Other wheels have no built-in shifter map (wheel_shifter_buttons sets it).
inline ShifterMap DefaultShifter(uint16_t vid, uint16_t pid) {
  if (vid != 0x046D) {
    return ShifterMap{};
  }
  switch (pid) {
    case 0xC24F:  // G29
    case 0xC266:  // G923 for PlayStation and PC
    case 0xC267:  // the same in PS4 mode
      return LogitechShifter(18);
    case 0xC262:  // G920
    case 0xC26E:  // G923 for Xbox and PC
    case 0xC26D:  // the same in Xbox mode
      return LogitechShifter(11);
    default:
      return ShifterMap{};
  }
}

// Overrides "1=12,...,6=17,r=18" and "up=..,down=.." (keys lower case, as ParseKv gives them).
inline ShifterMap ApplyShifterOverrides(ShifterMap m, const std::map<std::string, int>& kv) {
  for (const auto& [key, n] : kv) {
    if (key == "r") {
      m.gear[0] = n;
    } else if (key == "up") {
      m.up = n;
    } else if (key == "down") {
      m.down = n;
    } else if (key.size() == 1 && key[0] >= '1' && key[0] <= '0' + kMaxGear) {
      m.gear[key[0] - '0'] = n;
    }
  }
  return m;
}

// The SDL buttons the shifter reads in this mode ("h": its gears and reverse; "sequential": up and
// down; "off": none). They press no pad button (wheel_buttons.h): a G923 for Xbox has reverse on
// 11, the default left stick press, so engaging reverse opened the friends overlay (#256).
inline std::set<int> ShifterButtons(const ShifterMap& m, const std::string& mode) {
  std::set<int> used;
  if (mode == "h") {
    for (int g = 0; g <= kMaxGear; g++) {
      if (m.gear[g] >= 0) {
        used.insert(m.gear[g]);
      }
    }
  } else if (mode == "sequential") {
    for (int n : {m.up, m.down}) {
      if (n >= 0) {
        used.insert(n);
      }
    }
  }
  return used;
}

// The highest forward gear the map has (what the shifter reports to the game), 0 if none.
inline int TopGear(const ShifterMap& m) {
  for (int g = kMaxGear; g >= 1; g--) {
    if (m.gear[g] >= 0) {
      return g;
    }
  }
  return 0;
}

// The gear the lever is in: the held gear button, neutral when none is (or more than one).
template <typename Held>
int LeverGear(const ShifterMap& m, Held&& held) {
  int gear = kGearNeutral, count = 0;
  for (int g = 0; g <= kMaxGear; g++) {
    if (m.gear[g] >= 0 && held(m.gear[g])) {
      gear = g == 0 ? kGearReverse : g;
      count++;
    }
  }
  return count == 1 ? gear : kGearNeutral;
}

// The gear reported to the game. With need_clutch a new gear engages only while the clutch
// (0 released, 1 pressed) is at the threshold or further; a lever moved into a gear without it
// stays in neutral until the clutch goes down. Neutral needs no clutch.
inline int EngageGear(int lever, int engaged, bool need_clutch, float clutch, float threshold) {
  if (lever == engaged) {
    return engaged;
  }
  if (lever == kGearNeutral || !need_clutch || clutch >= threshold) {
    return lever;
  }
  return kGearNeutral;
}

}  // namespace rex::input::wheel
