/**
 * @file        src/input/wheel/wheel_devices.h
 * @brief       Pedals and shifters on their own USB device, read together with a wheel base as
 *              one wheel (#272): which joystick is the pedal set or the shifter (known models by
 *              USB vendor:product, names, the wheel_pedal_device / wheel_shifter_device
 *              settings), which device and axis each pedal reads, and a known shifter's gear
 *              buttons. No SDL: the wheel driver (wheel_input_driver.cpp) feeds it what SDL
 *              reports.
 *
 * Known models and where their ids come from (docs/sdk_patches.md, patch 0128):
 *  - HWDB: JacKeTUs/simracing-hwdb, 90-<maker>.hwdb (commit ea6e23f), the udev hardware
 *    database Linux distributions ship for sim racing devices;
 *  - PMR: Project Motor Racing's device maps (Apkallu-Industries/Pitwall,
 *    PMR/shared/inputDevices, commit 01acf41): the USB id, each pedal's HID axis and a
 *    shifter's gear buttons;
 *  - FTEC: gotzl/hid-fanatecff hid-ftec.h (commit d83c2a2), the Linux Fanatec driver;
 *  - HPR: mherbold/SimagicHPR README (Simagic's haptic pedal ids).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <rex/input/wheel/wheel_shifter.h>

namespace rex::input::wheel {

enum class AuxRole { kNone, kPedals, kShifter, kOther };

// A pedal set or shifter on its own USB device. Pedal axes are SDL axis numbers that hold when the
// device has exactly `axes` axes: SDL numbers a device's axes in HID usage order (X, Y, Z, Rx, Ry,
// Rz, then sliders) on Windows and Linux alike, so the rank of each pedal's usage among the
// device's three is its number. With a different count the axes are detected (DetectPedalAxes).
// -1: unknown. A known shifter's gear buttons are 0-based HID buttons (PMR), which are SDL's.
struct AuxModel {
  uint16_t vid, pid;
  AuxRole role;
  const char* name;
  int axes = 0;                     // pedals: how many axes the layout below assumes
  int throttle = -1, brake = -1, clutch = -1;
  int first_gear = -1;              // shifter: button of gear 1; gears follow on the next buttons
  int gears = 0;                    // forward gears on consecutive buttons from first_gear
  int reverse = -1;                 // shifter: reverse's button
  int up = -1, down = -1;           // shifter: its sequential paddles, when it has them
};

// clang-format off
inline constexpr AuxModel kAuxModels[] = {
    // Simagic. Pedals: Rx clutch, Ry brake, Rz throttle (PMR).
    {0x0483, 0x0525, AuxRole::kPedals, "Simagic P1000", 3, 2, 1, 0},           // HPR, PMR
    {0x0483, 0x0524, AuxRole::kPedals, "Simagic P2000", 3, 2, 1, 0},           // PMR
    {0x3670, 0x0902, AuxRole::kPedals, "Simagic P2000 (haptic)", 3, 2, 1, 0},  // HPR, PMR
    {0x3670, 0x0903, AuxRole::kPedals, "Simagic P500"},                        // HPR
    {0x3670, 0x0905, AuxRole::kPedals, "Simagic P700"},                        // HPR
    // DS-8X: gears 1-8 on 0-7 (the game has 7), reverse 8, paddles 9 down and 10 up (PMR).
    {0x0483, 0x0531, AuxRole::kShifter, "Simagic DS-8X", 0, -1, -1, -1, 0, 7, 8, 10, 9},
    {0x0483, 0x0530, AuxRole::kOther, "Simagic TB-1 handbrake"},              // HWDB
    {0x3670, 0x0A04, AuxRole::kOther, "Simagic TB-RS handbrake"},             // HWDB
    // Fanatec. Pedals: X throttle, Y brake, Z clutch (PMR).
    {0x0EB7, 0x6204, AuxRole::kPedals, "Fanatec CSL Elite Pedals LC", 3, 0, 1, 2},     // FTEC, HWDB, PMR
    {0x0EB7, 0x6205, AuxRole::kPedals, "Fanatec CSL Pedals"},                          // FTEC, HWDB
    {0x0EB7, 0x6206, AuxRole::kPedals, "Fanatec CSL Elite Pedals LC V2", 3, 0, 1, 2},  // FTEC, HWDB, PMR
    {0x0EB7, 0x183B, AuxRole::kPedals, "Fanatec ClubSport Pedals V3", 3, 0, 1, 2},     // FTEC, PMR
    // ClubSport Shifter SQ: reverse 0, gears 1-7 on 1-7, paddles 8 down and 9 up (PMR).
    {0x0EB7, 0x1A92, AuxRole::kShifter, "Fanatec ClubSport Shifter SQ", 0, -1, -1, -1, 1, 7, 0, 9, 8},  // HWDB, PMR
    {0x0EB7, 0x1A93, AuxRole::kOther, "Fanatec ClubSport Handbrake"},                  // HWDB
    // Thrustmaster. T-LCM: X throttle, Y clutch, Z brake (PMR).
    {0x044F, 0xB371, AuxRole::kPedals, "Thrustmaster T-LCM", 3, 0, 2, 1},     // HWDB, PMR
    // TH8 RS (the T500 RS shifter): gears 1-7 on 0-6, reverse 7, paddles 8 down and 9 up (PMR).
    {0x044F, 0xB660, AuxRole::kShifter, "Thrustmaster TH8 RS", 0, -1, -1, -1, 0, 7, 7, 9, 8},
    {0x044F, 0xB695, AuxRole::kOther, "Thrustmaster TSS handbrake"},          // PMR
    // Logitech. G PRO pedals: Y throttle, Z brake, Rx clutch (PMR).
    {0x046D, 0xC26A, AuxRole::kPedals, "Logitech G PRO Racing Pedals", 3, 0, 1, 2},  // PMR
    {0x046D, 0xC277, AuxRole::kPedals, "Logitech RS Pedals"},                        // HWDB
    {0x046D, 0xC278, AuxRole::kShifter, "Logitech RS Shifter and Handbrake"},        // PMR
    // The Driving Force Shifter on Logitech's USB adapter: gears 1-6 on 0-5, reverse 6 (PMR).
    {0x046D, 0xC273, AuxRole::kShifter, "Logitech Driving Force Shifter (USB adapter)", 0, -1, -1, -1, 0, 6, 6},
    // Heusinkveld. Pedals: Rx throttle, Ry brake, Rz clutch (PMR).
    {0x30B7, 0x1001, AuxRole::kPedals, "Heusinkveld Sprint", 3, 0, 1, 2},        // HWDB, PMR
    {0x30B7, 0x1003, AuxRole::kPedals, "Heusinkveld Ultimate", 3, 0, 1, 2},      // HWDB, PMR
    {0x30B7, 0x1004, AuxRole::kPedals, "Heusinkveld Race Center", 3, 0, 1, 2},   // PMR
    {0x10C4, 0x8B02, AuxRole::kPedals, "Heusinkveld Pedals (DC)", 3, 0, 1, 2},   // PMR
    {0xA020, 0x3142, AuxRole::kShifter, "Heusinkveld Sequential Shifter"},       // HWDB
    {0x30B7, 0x1005, AuxRole::kShifter, "Heusinkveld Shifter"},                  // PMR
    {0x30B7, 0x1006, AuxRole::kShifter, "Heusinkveld Shifter"},                  // PMR
    // Moza. Pedals: Rx throttle, Ry brake, Rz clutch (PMR; boxflat reads them so too).
    {0x346E, 0x0003, AuxRole::kPedals, "Moza SR-P", 3, 0, 1, 2},                 // PMR
    {0x346E, 0x0001, AuxRole::kPedals, "Moza CRP", 3, 0, 1, 2},                  // PMR
    {0x346E, 0x0008, AuxRole::kPedals, "Moza mBooster", 3, 0, 1, 2},             // PMR
    {0x346E, 0x0013, AuxRole::kPedals, "Moza mBooster", 3, 0, 1, 2},             // PMR (compatible mode)
    // HGP: reverse 4, gears 1-7 on 5-11 (PMR).
    {0x346E, 0x001E, AuxRole::kShifter, "Moza HGP", 0, -1, -1, -1, 5, 7, 4},
    {0x346E, 0x0023, AuxRole::kShifter, "Moza SGP"},                             // PMR
};
// clang-format on

inline const AuxModel* FindAuxModel(uint16_t vid, uint16_t pid) {
  for (const auto& m : kAuxModels) {
    if (m.vid == vid && m.pid == pid) {
      return &m;
    }
  }
  return nullptr;
}

inline std::string AuxLower(std::string_view s) {
  std::string out;
  bool space = false;
  for (unsigned char c : s) {  // lower case, runs of white space as one, trimmed
    if (std::isspace(c)) {
      space = !out.empty();
      continue;
    }
    if (space) {
      out += ' ';
      space = false;
    }
    out += static_cast<char>(std::tolower(c));
  }
  return out;
}

// What a device's name says it is, for one without a known id: "pedal" (Moza's "Gudsen MOZA SR-P
// Pedals", Heusinkveld's "Sim Pedals"), "shifter" / "th8a" / "gear shift" (Thrustmaster's TH8A,
// whose id no source confirms), "handbrake". A name that also says "wheel" is a wheel base (Moza's
// "Racing Wheel and Pedals" bundle).
inline AuxRole AuxRoleByName(std::string_view name) {
  const std::string n = AuxLower(name);
  auto has = [&](std::string_view w) { return n.find(w) != std::string::npos; };
  if (has("wheel")) {
    return AuxRole::kNone;
  }
  if (has("pedal")) {
    return AuxRole::kPedals;
  }
  if (has("shifter") || has("th8a") || has("th8 rs") || has("gear shift")) {
    return AuxRole::kShifter;
  }
  if (has("handbrake") || has("hand brake")) {
    return AuxRole::kOther;
  }
  return AuxRole::kNone;
}

inline AuxRole DetectAuxRole(uint16_t vid, uint16_t pid, std::string_view name) {
  if (const AuxModel* m = FindAuxModel(vid, pid)) {
    return m->role;
  }
  return AuxRoleByName(name);
}

// One SDL joystick as the picker sees it.
struct JoyInfo {
  std::string name;
  std::string guid;  // SDL's, 32 hex digits
  uint16_t vid = 0, pid = 0;
  bool sdl_wheel = false;  // SDL says it is a wheel (its list of wheel bases)
};

// wheel_pedal_device / wheel_shifter_device: "" or "auto" finds one, "none" (also "off", "wheel")
// reads them from the wheel, anything else names one.
enum class DeviceChoice { kAuto, kNone, kNamed };
inline DeviceChoice ParseDeviceChoice(const std::string& setting) {
  const std::string s = AuxLower(setting);
  if (s.empty() || s == "auto") {
    return DeviceChoice::kAuto;
  }
  if (s == "none" || s == "off" || s == "wheel") {
    return DeviceChoice::kNone;
  }
  return DeviceChoice::kNamed;
}

// How well a named setting matches a joystick: 2 exactly (its name, its GUID or "vid:pid" in
// hex), 1 its name contains the setting, 0 not at all.
inline int DeviceMatch(const std::string& setting, const JoyInfo& j) {
  const std::string s = AuxLower(setting);
  if (s.empty()) {
    return 0;
  }
  char vidpid[10];
  std::snprintf(vidpid, sizeof(vidpid), "%04x:%04x", j.vid, j.pid);
  if (s == AuxLower(j.name) || s == AuxLower(j.guid) || s == vidpid) {
    return 2;
  }
  return AuxLower(j.name).find(s) != std::string::npos ? 1 : 0;
}

// The joysticks that are the pedal set and the shifter (-1: none), and those that are neither a
// wheel nor used (`other`: a known handbrake). A named setting takes the best match (an exact one
// first, then one SDL does not list as a wheel base); auto takes the first known or named-like
// device of the role. One device may be both.
struct AuxPick {
  int pedals = -1;
  int shifter = -1;
  std::vector<int> other;
  bool IsAux(int i) const {
    return i == pedals || i == shifter || std::find(other.begin(), other.end(), i) != other.end();
  }
};

inline int PickNamed(const std::vector<JoyInfo>& joys, const std::string& setting) {
  int best = -1, best_score = 0;
  for (int i = 0; i < static_cast<int>(joys.size()); i++) {
    const int m = DeviceMatch(setting, joys[i]);
    if (!m) {
      continue;
    }
    const int score = m * 2 + (joys[i].sdl_wheel ? 0 : 1);
    if (score > best_score) {
      best = i;
      best_score = score;
    }
  }
  return best;
}

inline AuxPick PickAuxDevices(const std::vector<JoyInfo>& joys, const std::string& pedal_setting,
                              const std::string& shifter_setting) {
  AuxPick p;
  const DeviceChoice pc = ParseDeviceChoice(pedal_setting);
  const DeviceChoice sc = ParseDeviceChoice(shifter_setting);
  if (pc == DeviceChoice::kNamed) {
    p.pedals = PickNamed(joys, pedal_setting);
  }
  if (sc == DeviceChoice::kNamed) {
    p.shifter = PickNamed(joys, shifter_setting);
  }
  for (int i = 0; i < static_cast<int>(joys.size()); i++) {
    const AuxRole role = DetectAuxRole(joys[i].vid, joys[i].pid, joys[i].name);
    if (role == AuxRole::kPedals && pc == DeviceChoice::kAuto && p.pedals < 0) {
      p.pedals = i;
    } else if (role == AuxRole::kShifter && sc == DeviceChoice::kAuto && p.shifter < 0) {
      p.shifter = i;
    } else if (role != AuxRole::kNone && i != p.pedals && i != p.shifter) {
      p.other.push_back(i);  // a second pedal set, a handbrake: not a wheel of its own
    }
  }
  return p;
}

// Which device each pedal reads. A pedal set in wheel_pedal_axes (0 or more) reads the pedal
// device, one set in wheel_axes the wheel; otherwise the pedal device when one is connected, else
// the wheel (as without a pedal device). 0 throttle, 1 brake, 2 clutch.
inline constexpr const char* kPedalKeys[3] = {"throttle", "brake", "clutch"};
struct PedalSources {
  bool from_aux[3] = {false, false, false};
  bool Any() const { return from_aux[0] || from_aux[1] || from_aux[2]; }
};
inline PedalSources PlanPedals(const std::map<std::string, int>& wheel_axes,
                               const std::map<std::string, int>& pedal_axes, bool aux_connected) {
  PedalSources out;
  for (int p = 0; p < 3; p++) {
    auto set = [&](const std::map<std::string, int>& kv) {
      auto it = kv.find(kPedalKeys[p]);
      return it != kv.end() && it->second >= 0;
    };
    out.from_aux[p] = set(pedal_axes) || (!set(wheel_axes) && aux_connected);
  }
  return out;
}

// Fills the pedals still unknown (-1) among those this device gives (`want`) with the axes that
// rest at an end, in axis order: throttle, brake, clutch (a combined pedal axis also counts at the
// centre, as the first). `steer` and the axes already taken are skipped. The wheel driver's
// detection, also used for a pedal device.
inline void DetectPedalAxes(int pedal[3], const bool want[3], int steer,
                            const std::vector<int16_t>& rest, bool combined) {
  auto filled = [&](int k) { return !want[k] || pedal[k] >= 0; };
  int next = 0;
  for (int a = 0; a < static_cast<int>(rest.size()); a++) {
    if (a == steer || a == pedal[0] || a == pedal[1] || a == pedal[2]) {
      continue;
    }
    if (std::abs(rest[a]) >= 12000 || (combined && next == 0 && !filled(0))) {
      while (next < 3 && filled(next)) {
        next++;
      }
      if (next < 3) {
        pedal[next++] = a;
      }
    }
  }
}

// A pedal device's axes: wheel_pedal_axes, then the model's layout (when the device has the axes
// it assumes), then detection from where each axis rests.
inline void PedalDeviceAxes(int pedal[3], const bool want[3],
                            const std::map<std::string, int>& pedal_axes, const AuxModel* model,
                            const std::vector<int16_t>& rest, bool combined) {
  for (int p = 0; p < 3; p++) {
    pedal[p] = -1;
    if (!want[p]) {
      continue;
    }
    auto it = pedal_axes.find(kPedalKeys[p]);
    if (it != pedal_axes.end() && it->second >= 0) {
      pedal[p] = it->second;
    } else if (model && model->role == AuxRole::kPedals && model->axes > 0 &&
               static_cast<int>(rest.size()) == model->axes) {
      pedal[p] = p == 0 ? model->throttle : p == 1 ? model->brake : model->clutch;
    }
  }
  DetectPedalAxes(pedal, want, -1, rest, combined);
}

// A shifter device's gear buttons: the known model's, then wheel_shifter_device_buttons.
inline ShifterMap AuxShifterMap(const AuxModel* model, const std::map<std::string, int>& overrides) {
  ShifterMap m;
  if (model && model->role == AuxRole::kShifter && model->first_gear >= 0) {
    for (int g = 1; g <= std::min(model->gears, kMaxGear); g++) {
      m.gear[g] = model->first_gear + g - 1;
    }
    m.gear[0] = model->reverse;
    m.up = model->up;
    m.down = model->down;
  }
  return ApplyShifterOverrides(m, overrides);
}

}  // namespace rex::input::wheel
