/**
 * @file        rex/input/wheel/wheel_input_driver.cpp
 * @brief       Racing wheel input driver (see the header).
 *
 * A wheel is an SDL joystick that has no gamepad mapping (or whose type SDL reports as a
 * wheel). The game gets it as the Xbox 360 Wireless Racing Wheel: SubType 2 with the force
 * feedback and wireless flags, steering on the left stick X, the pedals on the triggers, the
 * clutch on the right stick Y and an H-pattern shifter's gear on the right stick X, which is how
 * the game reads the 360 wheel's clutch and shifter (wheel_shifter.h). A game that knows the wheel
 * sends its force feedback through the kernel's XInputdFF* calls (wheel_ff.h), played here on SDL
 * haptics; one that takes it for a gamepad only sets the vibration motors, which give an
 * approximate spring and a vibration. The rumble plays as a fine or classic vibration
 * (wheel_rumble.h), updated about 30 times a second whatever the frame rate (wheel_rumble_pace.h).
 * Pedals and a shifter on their own USB devices are read with the first wheel
 * (wheel_devices.h); steering, buttons and force feedback stay on the wheel.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <string_view>

#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/wheel/wheel_buttons.h>
#include <rex/input/wheel/wheel_input_driver.h>
#include <rex/input/wheel/wheel_shifter.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

#include "wheel_devices.h"
#include "wheel_ff_keep_playing.h"
#include "wheel_ff_log.h"
#include "wheel_ff_reject.h"
#include "wheel_gear_request.h"
#include "wheel_handbrake.h"
#include "wheel_min_force.h"
#include "wheel_primary.h"
#include "wheel_rumble.h"
#include "wheel_rumble_pace.h"
#include "wheel_thrustmaster.h"

REXCVAR_DEFINE_STRING(wheel_mode, "auto", "Input",
                      "Racing wheel support: auto (SDL says it is a wheel), on (every joystick "
                      "without a gamepad mapping is a wheel), off")
    .allowed({"auto", "on", "off"});
REXCVAR_DEFINE_INT32(wheel_rotation, 900, "Input",
                     "Degrees the wheel turns lock to lock, as set in its driver")
    .range(90, 2520);
REXCVAR_DEFINE_INT32(wheel_lock, 900, "Input",
                     "Degrees of wheel travel (lock to lock) that give the game's full steering; "
                     "lower than wheel_rotation makes the steering quicker")
    .range(90, 2520);
REXCVAR_DEFINE_DOUBLE(wheel_deadzone, 0.0, "Input", "Steering deadzone, 0 to 0.5")
    .range(0.0, 0.5);
REXCVAR_DEFINE_DOUBLE(wheel_steer_offset, 0.05, "Input",
                      "Steering pre-compensation, 0 to 0.3: the part of the game's full steering that "
                      "any wheel movement beyond a hair already gives, to cancel an inner steering "
                      "deadzone (0: off). Only with wheel_900_steering off: full-range steering "
                      "follows the wheel from the centre")
    .range(0.0, 0.3);
REXCVAR_DEFINE_BOOL(wheel_invert_steering, false, "Input", "Swap left and right steering");
REXCVAR_DEFINE_BOOL(wheel_900_steering, true, "Input",
                    "Report a 900-degree wheel, so the game uses its full-range wheel steering; "
                    "false: the Xbox 360 wheel's 270-degree mode with its steering filter (#4)");
REXCVAR_DEFINE_STRING(wheel_pedals, "auto", "Input",
                      "Pedals: auto/separate (throttle, brake and clutch each on an axis), "
                      "combined (one axis, throttle one way and brake the other)")
    .allowed({"auto", "separate", "combined"});
REXCVAR_DEFINE_BOOL(wheel_invert_throttle, false, "Input",
                    "Flip the throttle axis detected as released/pressed");
REXCVAR_DEFINE_BOOL(wheel_invert_brake, false, "Input",
                    "Flip the brake axis detected as released/pressed");
REXCVAR_DEFINE_DOUBLE(wheel_throttle_curve, 1.0, "Input",
                      "Throttle response, a gamma: 1 linear, above 1 softer at the start")
    .range(0.2, 4.0);
REXCVAR_DEFINE_DOUBLE(wheel_brake_curve, 1.0, "Input",
                      "Brake response, a gamma: 1 linear, above 1 softer at the start")
    .range(0.2, 4.0);
REXCVAR_DEFINE_STRING(wheel_axes, "", "Input",
                      "Axis overrides, e.g. steer=0,throttle=2,brake=5,clutch=1 (empty: the "
                      "built-in profile of a known wheel, else detected from where each axis "
                      "rests when the wheel is plugged in); handbrake=3 reads an analog handbrake "
                      "plugged into the wheel");
REXCVAR_DEFINE_STRING(wheel_buttons, "", "Input",
                      "Button overrides, e.g. a=0,b=1,x=2,y=3,lb=5,rb=4,back=8,start=9,ls=11,"
                      "rs=10,up=12,down=13,left=14,right=15 (SDL button numbers; -1 unmaps)");
REXCVAR_DEFINE_BOOL(wheel_clutch, true, "Input",
                    "Clutch pedal to the game, as the 360 wheel's analog clutch (the game's manual "
                    "with clutch transmission)");
REXCVAR_DEFINE_BOOL(wheel_invert_clutch, false, "Input",
                    "Flip the clutch axis detected as released/pressed");
REXCVAR_DEFINE_STRING(wheel_shifter, "off", "Input",
                      "Shifter: off; h (H-pattern: the gear buttons select the gear, through the "
                      "game's own 360 wheel shifter input); sequential (its up and down buttons shift "
                      "like the bumpers)")
    .allowed({"off", "h", "sequential"});
REXCVAR_DEFINE_STRING(wheel_shifter_buttons, "", "Input",
                      "Shifter buttons, e.g. 1=12,2=13,3=14,4=15,5=16,6=17,r=18 (gears 1 to 7 and "
                      "reverse) or up=..,down=.. (sequential); SDL button numbers. Empty: the Logitech "
                      "Driving Force Shifter on a G29, G920 or G923 (gears 1-6 on 12-17; reverse 18 on "
                      "a G29 or PlayStation G923, 11 on a G920 or Xbox G923). The shifter's buttons "
                      "press no pad button");
REXCVAR_DEFINE_BOOL(wheel_shifter_clutch, false, "Input",
                    "H-pattern: a gear engages only with the clutch pressed past "
                    "wheel_clutch_threshold; without it the car stays in neutral");
REXCVAR_DEFINE_DOUBLE(wheel_clutch_threshold, 0.5, "Input",
                      "How far the clutch must be pressed to engage a gear with wheel_shifter_clutch, "
                      "0.1 to 1")
    .range(0.1, 1.0);
REXCVAR_DEFINE_BOOL(wheel_ffb, true, "Input",
                    "Force feedback on the wheel: the game's own wheel forces (steering force, "
                    "centring spring, damper) and its rumble as a vibration");
REXCVAR_DEFINE_INT32(wheel_ffb_strength, 60, "Input", "Force feedback strength, 0 to 100 percent")
    .range(0, 100);
REXCVAR_DEFINE_INT32(wheel_ffb_spring_strength, 100, "Input",
                     "Centring spring and damper strength, 0 to 100 percent of wheel_ffb_strength")
    .range(0, 100);
REXCVAR_DEFINE_INT32(wheel_rumble_engine_strength, 50, "Input",
                     "Rumble from the game's right motor (mostly the engine), 0 to 100 percent of "
                     "wheel_ffb_strength (#273: 50, it runs at half or more whenever the car "
                     "accelerates)")
    .range(0, 100);
REXCVAR_DEFINE_INT32(wheel_rumble_road_strength, 80, "Input",
                     "Rumble from the game's left motor (road surface, kerbs, impacts), 0 to 100 "
                     "percent of wheel_ffb_strength (#273: 80)")
    .range(0, 100);
REXCVAR_DEFINE_BOOL(wheel_ffb_reverse, false, "Input",
                    "Reverse the game's steering force: for a wheel that pulls away from the centre "
                    "in corners instead of towards it");
REXCVAR_DEFINE_STRING(wheel_rumble_style, "fine", "Input",
                      "How the game's rumble plays on the wheel's motor: fine (a light, quick "
                      "vibration for each rumble motor) or classic (one coarse vibration, for "
                      "wheels with rim motors); fine falls back to classic on a wheel without room "
                      "for its effects")
    .allowed({"fine", "classic"});
REXCVAR_DEFINE_STRING(wheel_pedal_device, "", "Input",
                      "Pedals on their own USB device: empty finds a known pedal set (or one named "
                      "like pedals), a device name, SDL GUID or vid:pid picks one, none reads the "
                      "pedals from the wheel");
REXCVAR_DEFINE_STRING(wheel_pedal_axes, "", "Input",
                      "Axis numbers on the pedal device, e.g. throttle=0,brake=1,clutch=2 (empty: "
                      "a known pedal set's layout, else detected from where each axis rests). A "
                      "pedal set here reads the pedal device, one set in wheel_axes the wheel");
REXCVAR_DEFINE_STRING(wheel_shifter_device, "", "Input",
                      "Shifter on its own USB device: empty finds a known shifter (or one named "
                      "like a shifter), a device name, SDL GUID or vid:pid picks one, none reads "
                      "it from the wheel's buttons");
REXCVAR_DEFINE_STRING(wheel_shifter_device_buttons, "", "Input",
                      "Shifter buttons on the shifter device, e.g. 1=0,2=1,3=2,4=3,5=4,6=5,r=6 or "
                      "up=..,down=.. (empty: a known shifter's layout)");
REXCVAR_DEFINE_INT32(wheel_ffb_min_strength, 0, "Input",
                     "Minimum steering force, 0 to 100 percent of the full force: the game's "
                     "weaker steering forces are scaled up (smoothly, up to 4 times near the "
                     "centre), stronger ones play as they are, so a light car still has weight "
                     "(0: off; before wheel_ffb_strength; not the spring, damper or rumble)")
    .range(0, 100);
REXCVAR_DEFINE_INT32(wheel_shifter_neutral_ms, 400, "Input",
                     "H-pattern: milliseconds the lever may cross neutral between two gates and "
                     "still go from one gear to the other, so the game sees an upshift as one "
                     "(its paddle animation), 0 to 2000; 0: neutral at once")
    .range(0, 2000);
REXCVAR_DEFINE_BOOL(wheel_shifter_follow_slot, true, "Input",
                    "H-pattern: the game takes the lever's slot whenever it sets the gear itself "
                    "(race and event starts and finishes), as on the Xbox; false: a gear engages "
                    "only when the lever moves, so the paddles keep the gear they chose");
REXCVAR_DEFINE_STRING(wheel_handbrake_device, "", "Input",
                      "Analog handbrake on its own USB device: empty finds a known handbrake (or "
                      "one named like one), a device name, SDL GUID or vid:pid picks one (also the "
                      "pedal or shifter device), none reads none. wheel_axes handbrake=N reads one "
                      "plugged into the wheel instead");
REXCVAR_DEFINE_INT32(wheel_handbrake_axis, 0, "Input",
                     "Axis number of the analog handbrake on the handbrake device")
    .range(0, 99);
REXCVAR_DEFINE_BOOL(wheel_invert_handbrake, false, "Input",
                    "Flip the handbrake axis detected as released/pulled");
REXCVAR_DEFINE_INT32(wheel_ffb_keep_playing, 0, "Input",
                     "For a wheel that ends force feedback effects by itself: start every effect "
                     "that plays again every this many seconds (0: never; an effect the wheel says "
                     "has stopped is still started again; -1: not even that)")
    .range(-1, 60);
REXCVAR_DEFINE_STRING(wheel_device, "", "Input",
                      "The wheel the first player drives with and the pedal, shifter and handbrake "
                      "devices are read with: empty picks one with force feedback first; a device "
                      "name, SDL GUID or vid:pid picks that one (and makes it a wheel)");

namespace rex::input::wheel {

namespace {

constexpr uint8_t kSubTypeWheel = 2;  // XINPUT_DEVSUBTYPE_WHEEL
// The driver the kernel's XInputdFF* calls go to (GameFf* below).
std::atomic<WheelInputDriver*> g_ff_driver{nullptr};
// Whether the player drives, from the game (ForzaWheelPlayerDriving): -1 never said, 0, 1.
std::atomic<int> g_player_driving{-1};
// What the game sends and the wheels get, for the log (wheel_ff_log.h, #304); under the driver's
// mutex.
FfLog g_ff_log;

double FfNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// FH1's constant force pushes the wheel right when positive (in a right-hand corner it is negative:
// the tyres pull the wheel back to the centre). An SDL level on the steering axis comes from the
// east (SDL_haptic.h: where the force comes from; Linux direction 0x4000, which pulls left), so it
// pulls left when positive. wheel_ffb_reverse flips it for a driver that disagrees.
constexpr float kConstantToSdl = -1.0f;
constexpr auto kScanEvery = std::chrono::milliseconds(500);
constexpr auto kPumpEvery = std::chrono::milliseconds(2);

bool ContainsCI(const char* name, std::string_view needle) {
  if (!name) {
    return false;
  }
  const std::string_view h(name);
  return std::search(h.begin(), h.end(), needle.begin(), needle.end(), [](char a, char b) {
           return std::tolower(static_cast<unsigned char>(a)) == b;
         }) != h.end();
}

bool NameLooksLikeWheel(const char* name) {
  static constexpr std::string_view kWords[] = {
      "wheel", "racing", "driving force", "g25", "g27", "g29", "g920", "g923", "t300", "t150",
      "t248", "t-gt", "tx racing", "fanatec", "moza", "simagic", "csl", "podium", "ts-xw"};
  for (auto w : kWords) {
    if (ContainsCI(name, w)) {
      return true;
    }
  }
  return false;
}

// "a=0,b=1" to name -> number.
std::map<std::string, int> ParseKv(const std::string& s) {
  std::map<std::string, int> out;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    if (end == std::string::npos) {
      end = s.size();
    }
    const std::string item = s.substr(pos, end - pos);
    const size_t eq = item.find('=');
    if (eq != std::string::npos) {
      std::string key = item.substr(0, eq);
      key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return isspace(c); }),
                key.end());
      for (auto& c : key) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      out[key] = std::atoi(item.c_str() + eq + 1);
    }
    pos = end + 1;
  }
  return out;
}

// Known wheels, by USB vendor:product. Their axes and buttons are fixed and their pedals rest at the
// maximum and read the minimum when pressed (the Logitech wheels), so none of it is guessed from the
// position the axes happen to have when the wheel is opened: right after opening, an axis can still
// read 0 (nothing reported yet) or a pedal can be held down, and a guess then turns a pedal around
// ("phantom throttle"). Axis numbers are SDL's: on Windows (Windows.Gaming.Input racing wheel) they are
// steering, throttle, brake, clutch; on Linux (evdev) steering, clutch, throttle, brake.
// Their buttons and shifter go by vendor:product too (ButtonLayoutOf, DefaultShifter).
struct Profile {
  uint16_t vid, pid;
  const char* name;
};
constexpr Profile kProfiles[] = {
    {0x046D, 0xC24F, "Logitech G29"},
    {0x046D, 0xC262, "Logitech G920"},
    {0x046D, 0xC266, "Logitech G923"},
    {0x046D, 0xC26E, "Logitech G923 (Xbox)"},
    {0x046D, 0xC29A, "Logitech Driving Force GT"},
    {0x046D, 0xC29B, "Logitech G27"},
    {0x046D, 0xC299, "Logitech G25"},
};
#ifdef _WIN32
constexpr int kProfSteer = 0, kProfThrottle = 1, kProfBrake = 2, kProfClutch = 3;
#else
constexpr int kProfSteer = 0, kProfClutch = 1, kProfThrottle = 2, kProfBrake = 3;
#endif
const Profile* FindProfile(uint16_t vid, uint16_t pid) {
  for (const auto& p : kProfiles) {
    if (p.vid == vid && p.pid == pid) {
      return &p;
    }
  }
  return nullptr;
}

// The wheel's shifter buttons: a known wheel's shifter port (by model), then wheel_shifter_buttons.
ShifterMap ShifterFor(uint16_t vid, uint16_t pid) {
  return ApplyShifterOverrides(DefaultShifter(vid, pid),
                               ParseKv(REXCVAR_GET(wheel_shifter_buttons)));
}

FfStrengths Strengths() {
  auto percent = [](int v) { return std::clamp(v, 0, 100) / 100.0f; };
  FfStrengths s;
  s.overall = percent(REXCVAR_GET(wheel_ffb_strength));
  s.spring = percent(REXCVAR_GET(wheel_ffb_spring_strength));
  s.engine = percent(REXCVAR_GET(wheel_rumble_engine_strength));
  s.road = percent(REXCVAR_GET(wheel_rumble_road_strength));
  return s;
}

// The H-pattern gear to report on thumb_rx (wheel_gear_request.h): on lever moves only, or the
// engaged gear all the time with wheel_shifter_follow_slot. A new request is logged.
int GearRequestOf(GearRequest& r, int engaged, bool game_read, double now_ms) {
  const int before = r.request;
  const int out =
      GearToReport(r, engaged, now_ms, game_read, REXCVAR_GET(wheel_shifter_follow_slot));
  if (r.request != before && r.request != kGearNoRequest) {
    REXLOG_INFO("Wheel: the lever moved: gear {} asked of the game",
                engaged == kGearReverse   ? std::string("R")
                : engaged == kGearNeutral ? std::string("N")
                                          : std::to_string(engaged));
  }
  return out;
}

// thumb_rx while the game window is out of focus (no input): with the H-pattern on lever moves,
// no request, so the gear stays; otherwise 0 (neutral), as before.
int IdleGearRequest() {
  return REXCVAR_GET(wheel_shifter) == "h" && !REXCVAR_GET(wheel_shifter_follow_slot)
             ? kGearNoRequest
             : kGearNeutral;
}

// The game's steering force with wheel_ffb_min_strength (wheel_min_force.h).
float MinForceLevelOf(const FfEffect& e, uint32_t device_gain, float level) {
  const float min = std::clamp(REXCVAR_GET(wheel_ffb_min_strength), 0, 100) / 100.0f;
  return FfMinForceLevel(e, device_gain, level, min);
}

// The direction of an SDL effect (each type keeps its own in SDL's union), or none.
SDL_HapticDirection* EffectDirection(SDL_HapticEffect& e) {
  switch (e.type) {
    case SDL_HAPTIC_CONSTANT: return &e.constant.direction;
    case SDL_HAPTIC_SPRING:
    case SDL_HAPTIC_DAMPER:
    case SDL_HAPTIC_INERTIA:
    case SDL_HAPTIC_FRICTION: return &e.condition.direction;
    case SDL_HAPTIC_SINE:
    case SDL_HAPTIC_SQUARE:
    case SDL_HAPTIC_TRIANGLE:
    case SDL_HAPTIC_SAWTOOTHUP:
    case SDL_HAPTIC_SAWTOOTHDOWN: return &e.periodic.direction;
    case SDL_HAPTIC_RAMP: return &e.ramp.direction;
    default: return nullptr;
  }
}

// One of wheel_ff_reject.h's directions: SDL's steering axis, Cartesian X (a force from the east)
// or polar east (9000 hundredths of a degree from north), all pulling the wheel left when positive.
void SetEffectDirection(SDL_HapticEffect& e, int direction) {
  SDL_HapticDirection* d = EffectDirection(e);
  if (!d) {
    return;
  }
  *d = SDL_HapticDirection{};
  d->type = direction == 1   ? SDL_HAPTIC_CARTESIAN
            : direction == 2 ? SDL_HAPTIC_POLAR
                             : SDL_HAPTIC_STEERING_AXIS;
  if (direction == 1) {
    d->dir[0] = 1;
  } else if (direction == 2) {
    d->dir[0] = 9000;
  }
}

// Creates the effect in the direction that worked last, else in the others (FfDirectionOrder);
// `direction` becomes the one that worked. -1 when the wheel refuses all of them.
int CreateEffectAnyDirection(SDL_Haptic* haptic, SDL_HapticEffect& e, int& direction) {
  for (int d : FfDirectionOrder(direction)) {
    SetEffectDirection(e, d);
    const int id = SDL_CreateHapticEffect(haptic, &e);
    if (id >= 0) {
      if (d != direction) {
        REXLOG_INFO("Wheel: effects are created {} from now on",
                    d == 1 ? "in Cartesian X" : d == 2 ? "in polar east" : "on the steering axis");
      }
      direction = d;
      return id;
    }
  }
  SetEffectDirection(e, direction);
  return -1;
}

const char* FfTypeName(uint32_t type) {
  switch (type) {
    case SDL_HAPTIC_CONSTANT: return "steering force (constant)";
    case SDL_HAPTIC_SPRING: return "centring spring";
    case SDL_HAPTIC_DAMPER: return "damper";
    case SDL_HAPTIC_INERTIA: return "inertia";
    case SDL_HAPTIC_FRICTION: return "friction";
    default: return "effect";
  }
}

float Norm(int16_t v) {
  return v < 0 ? v / 32768.0f : v / 32767.0f;
}

// A pedal set or shifter on its own USB device, read with the first wheel (wheel_devices.h).
struct AuxDevice {
  explicit AuxDevice(AuxRole r = AuxRole::kNone) : role(r) {}
  AuxRole role;
  SDL_JoystickID jid = 0;
  SDL_Joystick* joy = nullptr;
  std::string name;
  int num_axes = 0, num_buttons = 0;
  const AuxModel* model = nullptr;
  std::vector<int16_t> rest;   // where each axis sat when the device was opened
  std::vector<uint8_t> armed;  // pedal axes seen at their resting end since then
};

const char* AuxWhat(AuxRole role) {
  return role == AuxRole::kPedals    ? "pedal device"
         : role == AuxRole::kShifter ? "shifter device"
                                     : "handbrake device";
}

void CloseAux(AuxDevice& d, const char* why) {
  if (!d.joy) {
    return;
  }
  REXLOG_INFO("Wheel: {} \"{}\" {}", AuxWhat(d.role), d.name, why);
  SDL_CloseJoystick(d.joy);
  d = AuxDevice(d.role);
}

bool OpenAux(AuxDevice& d, SDL_JoystickID jid) {
  SDL_Joystick* joy = SDL_OpenJoystick(jid);
  if (!joy) {
    REXLOG_WARN("Wheel: could not open the {} {}: {}", AuxWhat(d.role), jid, SDL_GetError());
    return false;
  }
  d.jid = jid;
  d.joy = joy;
  const char* name = SDL_GetJoystickName(joy);
  d.name = name ? name : "Joystick";
  d.num_axes = std::max(SDL_GetNumJoystickAxes(joy), 0);
  d.num_buttons = std::max(SDL_GetNumJoystickButtons(joy), 0);
  const uint16_t vid = SDL_GetJoystickVendor(joy), pid = SDL_GetJoystickProduct(joy);
  d.model = FindAuxModel(vid, pid);
  d.rest.assign(static_cast<size_t>(d.num_axes), 0);
  d.armed.assign(static_cast<size_t>(d.num_axes), 0);
  // Where the axes are now, read once SDL has polled the device: SDL takes a device with two axes
  // (a two-pedal set) for a stick and reports 0 as its initial state, wherever the pedals rest.
  SDL_UpdateJoysticks();
  for (int a = 0; a < d.num_axes; a++) {
    d.rest[a] = SDL_GetJoystickAxis(joy, a);
  }
  char guid[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUID(joy), guid, sizeof(guid));
  REXLOG_INFO("Wheel: {} \"{}\" GUID({}) VID(0x{:04X}) PID(0x{:04X}) axes({}) buttons({}){}{}",
              AuxWhat(d.role), d.name, guid, vid, pid, d.num_axes, d.num_buttons,
              d.model ? ", known: " : "", d.model ? d.model->name : "");
  for (int a = 0; a < d.num_axes; a++) {
    REXLOG_INFO("Wheel:   axis {} rests at {}", a, d.rest[a]);
  }
  return true;
}

// The SDL joysticks as wheel_devices.h sees them, in SDL's order.
std::vector<JoyInfo> JoyInfos(const SDL_JoystickID* ids, int count) {
  std::vector<JoyInfo> out;
  for (int i = 0; ids && i < count; i++) {
    JoyInfo j;
    const char* name = SDL_GetJoystickNameForID(ids[i]);
    j.name = name ? name : "";
    char guid[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), guid, sizeof(guid));
    j.guid = guid;
    j.vid = SDL_GetJoystickVendorForID(ids[i]);
    j.pid = SDL_GetJoystickProductForID(ids[i]);
    j.sdl_wheel = SDL_GetJoystickTypeForID(ids[i]) == SDL_JOYSTICK_TYPE_WHEEL;
    out.push_back(std::move(j));
  }
  return out;
}

// The shifter's gear buttons: on a shifter device its own (known model, then
// wheel_shifter_device_buttons), else the wheel's.
ShifterMap EffectiveShifter(uint16_t vid, uint16_t pid, const AuxDevice& shifter_device) {
  if (shifter_device.joy) {
    return AuxShifterMap(shifter_device.model, ParseKv(REXCVAR_GET(wheel_shifter_device_buttons)));
  }
  return ShifterFor(vid, pid);
}

void LogShifter(const ShifterMap& m, const std::string& mode, const char* where) {
  if (mode == "h" && TopGear(m) == 0) {
    REXLOG_WARN("Wheel: wheel_shifter = h, but no gear buttons are known for the {}: set {}", where,
                std::string_view(where) == "wheel"
                    ? "wheel_shifter_buttons (e.g. 1=12,2=13,3=14,4=15,5=16,6=17,r=18)"
                    : "wheel_shifter_device_buttons (e.g. 1=0,2=1,3=2,4=3,5=4,6=5,r=6)");
  } else if (mode == "h") {
    REXLOG_INFO("Wheel: H-pattern shifter on the {}, buttons r={} 1={} 2={} 3={} 4={} 5={} 6={} "
                "7={} (they press no pad button)",
                where, m.gear[0], m.gear[1], m.gear[2], m.gear[3], m.gear[4], m.gear[5], m.gear[6],
                m.gear[7]);
    REXLOG_INFO("Wheel: {}", REXCVAR_GET(wheel_shifter_follow_slot)
                                 ? "the game takes the lever's slot whenever it sets the gear"
                                 : "a gear engages when the lever moves");
  } else if (mode == "sequential") {
    REXLOG_INFO("Wheel: sequential shifter on the {}, buttons up={} down={}", where, m.up, m.down);
  }
}

// A periodic rumble effect brought to `wave`: updated when its magnitude changes (the game repeats
// its rumble every frame), started while it has one, stopped at 0.
// `restart` starts the wave again even when it is believed running (the forces back at full
// strength, #304).
void PlayWave(SDL_Haptic* haptic, int fx, const RumbleWave& wave, int& sent, bool& running,
              bool restart) {
  if (!haptic || fx < 0) {
    return;
  }
  SDL_HapticEffect e{};
  e.type = SDL_HAPTIC_SINE;
  e.periodic.direction.type = SDL_HAPTIC_STEERING_AXIS;
  e.periodic.length = SDL_HAPTIC_INFINITY;
  e.periodic.period = wave.period_ms;
  e.periodic.magnitude = RumbleMagnitude(wave.level);
  if (e.periodic.magnitude != sent && SDL_UpdateHapticEffect(haptic, fx, &e)) {
    sent = e.periodic.magnitude;  // a failed update is sent again
    g_ff_log.Vibrated(wave.level);
  }
  if (e.periodic.magnitude > 0 && (!running || restart)) {
    running = SDL_RunHapticEffect(haptic, fx, SDL_HAPTIC_INFINITY);
  } else if (e.periodic.magnitude <= 0 && running) {
    SDL_StopHapticEffect(haptic, fx);
    running = false;
  }
}

// The game's level for one vibration effect, played at the fixed pace of wheel_rumble_pace.h
// (#467): when an update is due, or at once with `now_too` (the forces back at full strength, or
// faded out: the vibration stops with them) and when a vibration starts from silence, which plays
// the game's level as it is now and starts the next interval. `restart` as for PlayWave.
void PaceWave(SDL_Haptic* haptic, int fx, RumbleWave wave, RumblePace& pace, int& sent,
              bool& running, bool restart, bool now_too, double now) {
  if (!haptic || fx < 0) {
    return;
  }
  pace.Set(now, wave.level);
  now_too = now_too || (!running && RumbleMagnitude(wave.level) > 0);
  if (!now_too && !pace.Due(now)) {
    return;
  }
  const float paced = pace.Take(now);
  if (!now_too) {
    wave.level = paced;
  }
  PlayWave(haptic, fx, wave, sent, running, restart);
}

// The vibration updates that came due since the game's last rumble (it sets its rumble once a
// frame, and may stop setting it, as in a menu, after its last change), each wave at the level the
// game asked for last. Called on every state poll (several a frame, also in menus).
template <typename W>
void PaceRumble(W& w, double now) {
  if (!w.haptic) {
    return;
  }
  const struct {
    int fx;
    uint16_t period_ms;
    RumblePace& pace;
    int& sent;
    bool& running;
  } waves[] = {{w.road_fx, kFineRoadPeriodMs, w.road_pace, w.road_magnitude, w.road_running},
               {w.engine_fx, kFineEnginePeriodMs, w.engine_pace, w.engine_magnitude,
                w.engine_running},
               {w.sine_fx, kClassicPeriodMs, w.sine_pace, w.sine_magnitude, w.sine_running}};
  for (const auto& v : waves) {
    if (v.fx >= 0 && v.pace.Due(now)) {
      PaceWave(w.haptic, v.fx, RumbleWave{v.period_ms, v.pace.level()}, v.pace, v.sent, v.running,
               false, false, now);
    }
  }
}

// A failed SDL call on the game's effects, logged the 1st, 10th, 100th... time (#304: a wheel that
// stopped taking updates kept its last, faded-out forces, and the log said nothing).
void NoteFfFailure(int& failures, const char* what, uint32_t block) {
  const int n = ++failures;
  for (int k = 1; k > 0 && k <= n; k *= 10) {
    if (k == n) {
      REXLOG_WARN("Wheel: {} of the game's effect block {} failed ({} so far): {}", what, block, n,
                  SDL_GetError());
      return;
    }
  }
}

// Starts every effect the wheel plays again every wheel_ffb_keep_playing seconds, for a wheel that
// ends effects by itself (wheel_ff_keep_playing.h, #304). Starting a playing effect restarts it as
// it is.
template <typename W>
void KeepPlaying(W& w, bool game_ff, double now) {
  const int seconds = FfKeepPlayingSeconds(REXCVAR_GET(wheel_ffb_keep_playing));
  if (!w.haptic || seconds <= 0) {
    return;
  }
  if (w.kept_playing_at < 0) {
    w.kept_playing_at = now;
  }
  if (!FfKeepPlayingDue(w.kept_playing_at, now, seconds)) {
    return;
  }
  w.kept_playing_at = now;
  for (uint32_t b = 1; b <= kFfMaxBlocks; b++) {
    const int id = w.game_fx[b - 1];
    if (id < 0 || !w.game_fx_running[b - 1]) {
      continue;
    }
    if (SDL_RunHapticEffect(w.haptic, id, SDL_HAPTIC_INFINITY)) {
      g_ff_log.Sdl(b, FfSdlCall::kKeepPlaying);
    } else {
      g_ff_log.Sdl(b, FfSdlCall::kFailure);
      NoteFfFailure(w.ff_failures, "keeping playing", b);
    }
  }
  const std::pair<int, bool> waves[] = {
      {w.road_fx, w.road_running}, {w.engine_fx, w.engine_running}, {w.sine_fx, w.sine_running}};
  for (const auto& [fx, running] : waves) {
    if (fx >= 0 && running && SDL_RunHapticEffect(w.haptic, fx, SDL_HAPTIC_INFINITY)) {
      g_ff_log.RumbleKept();
    }
  }
  if (!game_ff && w.spring_fx >= 0) {  // the approximate spring, started when the wheel opened
    SDL_RunHapticEffect(w.haptic, w.spring_fx, SDL_HAPTIC_INFINITY);
  }
}

int CreateSine(SDL_Haptic* haptic, uint16_t period_ms) {
  SDL_HapticEffect e{};
  e.type = SDL_HAPTIC_SINE;
  e.periodic.direction.type = SDL_HAPTIC_STEERING_AXIS;
  e.periodic.length = SDL_HAPTIC_INFINITY;
  e.periodic.period = period_ms;
  e.periodic.magnitude = 0;
  return SDL_CreateHapticEffect(haptic, &e);
}

const rex::ui::VirtualKey kVk[16] = {
    rex::ui::VirtualKey::kXInputPadDpadUp,     rex::ui::VirtualKey::kXInputPadDpadDown,
    rex::ui::VirtualKey::kXInputPadDpadLeft,   rex::ui::VirtualKey::kXInputPadDpadRight,
    rex::ui::VirtualKey::kXInputPadStart,      rex::ui::VirtualKey::kXInputPadBack,
    rex::ui::VirtualKey::kXInputPadLThumbPress, rex::ui::VirtualKey::kXInputPadRThumbPress,
    rex::ui::VirtualKey::kXInputPadLShoulder,  rex::ui::VirtualKey::kXInputPadRShoulder,
    rex::ui::VirtualKey::kNone,                rex::ui::VirtualKey::kNone,
    rex::ui::VirtualKey::kXInputPadA,          rex::ui::VirtualKey::kXInputPadB,
    rex::ui::VirtualKey::kXInputPadX,          rex::ui::VirtualKey::kXInputPadY};

}  // namespace

bool IsWheelDevice(SDL_JoystickID jid) {
  const std::string mode = REXCVAR_GET(wheel_mode);
  if (mode == "off" || SDL_GetJoystickVendorForID(jid) == 0x045E) {
    return false;
  }
  if (SDL_GetJoystickTypeForID(jid) == SDL_JOYSTICK_TYPE_WHEEL ||
      NameLooksLikeWheel(SDL_GetJoystickNameForID(jid)) || (mode == "on" && !SDL_IsGamepad(jid)) ||
      FindThrustmaster(SDL_GetJoystickVendorForID(jid), SDL_GetJoystickProductForID(jid))) {
    return true;
  }
  // A pedal set, shifter or handbrake read with the wheel (#272): not a gamepad either.
  JoyInfo j = JoyInfos(&jid, 1)[0];
  return DetectAuxRole(j.vid, j.pid, j.name) != AuxRole::kNone ||
         (ParseDeviceChoice(REXCVAR_GET(wheel_pedal_device)) == DeviceChoice::kNamed &&
          DeviceMatch(REXCVAR_GET(wheel_pedal_device), j)) ||
         (ParseDeviceChoice(REXCVAR_GET(wheel_shifter_device)) == DeviceChoice::kNamed &&
          DeviceMatch(REXCVAR_GET(wheel_shifter_device), j)) ||
         (ParseDeviceChoice(REXCVAR_GET(wheel_handbrake_device)) == DeviceChoice::kNamed &&
          DeviceMatch(REXCVAR_GET(wheel_handbrake_device), j)) ||
         WheelDeviceMatch(REXCVAR_GET(wheel_device), j) > 0;
}

struct WheelInputDriver::Wheel {
  DeviceId id{};
  SDL_JoystickID jid = 0;
  SDL_Joystick* joy = nullptr;
  SDL_Haptic* haptic = nullptr;
  int spring_fx = -1;
  int sine_fx = -1;  // the classic rumble (wheel_rumble.h)
  bool sine_running = false;
  // The fine rumble: a sine for the road motor and one for the engine motor.
  int road_fx = -1, engine_fx = -1;
  bool road_running = false, engine_running = false;
  int road_magnitude = -1, engine_magnitude = -1;
  // Each vibration effect's level, sent about 30 times a second (wheel_rumble_pace.h, #467).
  RumblePace road_pace, engine_pace, sine_pace;
  // Pedals and shifter on their own USB devices (the first wheel only, wheel_devices.h).
  AuxDevice pedals{AuxRole::kPedals};
  AuxDevice shifter_device{AuxRole::kShifter};
  AuxDevice handbrake_device{AuxRole::kOther};  // an analog handbrake (wheel_handbrake.h)
  // The game's effect blocks (wheel_ff.h) as SDL effects, and the self-centring used for its
  // spring on a wheel without a spring effect.
  int game_fx[kFfMaxBlocks];
  SDL_HapticEffect game_fx_sent[kFfMaxBlocks];  // what each SDL effect was last given
  bool game_fx_running[kFfMaxBlocks] = {};
  // Set while the game's forces come back to full strength: every effect that should play is
  // started again, running or not (#304).
  bool restart_effects = false;
  double kept_playing_at = -1;  // when its effects were last started again (KeepPlaying), seconds
  int ff_failures = 0;  // failed SDL calls on the game's effects (NoteFfFailure)
  int ff_ended = 0;     // the game's effects the wheel says have stopped by themselves (#304)
  int autocenter = -1;
  int sine_magnitude = -1;
  std::string name;
  std::string guid;
  int num_axes = 0, num_buttons = 0, num_hats = 0;
  uint16_t vid = 0, pid = 0;
  const Profile* prof = nullptr;  // known wheel: fixed axes and pedal direction
  const ThrustmasterModel* tm = nullptr;  // a Thrustmaster by id: axes and buttons (#308)
  bool rest_pending = false;  // every axis read 0 when opened: rest read at the first report
  FfRejections ff_rejected;  // the game's effect types this wheel refused (wheel_ff_reject.h)
  int ff_direction = 0;      // the direction its effects are created in (wheel_ff_reject.h)
  std::vector<int16_t> rest;  // where each axis sat when the wheel was opened
  std::vector<uint8_t> armed;  // pedal axes seen at their resting end since the wheel was opened
  bool ffb = false;  // force feedback (wheel_primary.h: such a wheel comes first)
  int gear = kGearNeutral;  // the H-pattern gear engaged (EngageGear)
  GearRequest gear_request;  // what thumb_rx asks of the game (wheel_gear_request.h)
  LeverHold lever_hold;      // the lever crossing neutral between two gates (#468)
  int gears_logged = 0;      // H-pattern gear changes logged so far
  bool keystroke_poll = false;  // ComputeStateLocked for a keystroke: not a state the game reads
  bool shifter_logged = false;  // where the shifter is read was logged
  bool title_caps = false;  // the game read the capabilities, so it takes this for a wheel
  X_INPUT_STATE last_state{};
  uint32_t packet = 0;
  uint16_t keystroke_buttons = 0;
};

WheelInputDriver::WheelInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

WheelInputDriver::~WheelInputDriver() {
  WheelInputDriver* self = this;
  g_ff_driver.compare_exchange_strong(self, nullptr);
  std::lock_guard lock(mutex_);
  for (auto& w : wheels_) {
    CloseLocked(*w);
  }
  wheels_.clear();
}

X_STATUS WheelInputDriver::Setup() {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC)) {
    // Haptics may be missing on its own; a wheel without force feedback still drives.
    if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK)) {
      REXLOG_WARN("Wheel: SDL joystick init failed: {}", SDL_GetError());
      return X_STATUS_UNSUCCESSFUL;
    }
  }
  sdl_ready_ = true;
  g_ff_driver = this;
  REXLOG_INFO("Wheel: driver ready (wheel_mode={})", REXCVAR_GET(wheel_mode));
  return X_STATUS_SUCCESS;
}

void WheelInputDriver::PumpLocked() {
  const auto now = std::chrono::steady_clock::now();
  if (now - last_pump_ < kPumpEvery) {
    return;
  }
  last_pump_ = now;
  SDL_UpdateJoysticks();
}

void WheelInputDriver::RescanLocked() {
  const auto now = std::chrono::steady_clock::now();
  if (last_scan_.time_since_epoch().count() != 0 && now - last_scan_ < kScanEvery) {
    return;
  }
  last_scan_ = now;
  const std::string mode = REXCVAR_GET(wheel_mode);
  PumpLocked();
  int count = 0;
  SDL_JoystickID* ids = mode == "off" ? nullptr : SDL_GetJoysticks(&count);
  // Pedal sets, shifters and handbrakes on their own USB devices are read with the first wheel,
  // never as wheels of their own (#272).
  const AuxPick pick = PickAuxDevices(JoyInfos(ids, ids ? count : 0),
                                      REXCVAR_GET(wheel_pedal_device),
                                      REXCVAR_GET(wheel_shifter_device));
  const SDL_JoystickID pedal_jid = pick.pedals >= 0 ? ids[pick.pedals] : 0;
  const SDL_JoystickID shifter_jid = pick.shifter >= 0 ? ids[pick.shifter] : 0;
  // An analog handbrake on its own device (#303), which may also be the pedal or shifter device.
  const int handbrake = PickHandbrakeDevice(JoyInfos(ids, ids ? count : 0),
                                            REXCVAR_GET(wheel_handbrake_device));
  const SDL_JoystickID handbrake_jid = handbrake >= 0 ? ids[handbrake] : 0;
  // The wheels (wheel_primary.h): wheel-like devices that are not read as another's pedals,
  // shifter or handbrake, and the one wheel_device names. Ordered once they are open.
  const std::vector<JoyInfo> joys = JoyInfos(ids, ids ? count : 0);
  const std::vector<SDL_JoystickID> jids(ids, ids + (ids ? count : 0));
  std::vector<bool> wheel_like(joys.size()), aux(joys.size());
  for (size_t i = 0; i < joys.size(); i++) {
    wheel_like[i] = IsWheelDevice(jids[i]);
    aux[i] = pick.IsAux(static_cast<int>(i)) || static_cast<int>(i) == handbrake;
  }
  const std::string wheel_device = REXCVAR_GET(wheel_device);
  std::vector<SDL_JoystickID> wanted;
  for (int i : WheelOrder(joys, wheel_like, std::vector<bool>(joys.size()), aux, wheel_device)) {
    wanted.push_back(jids[i]);
  }
  SDL_free(ids);
  // A device named in the settings that is not there: said once until it comes.
  static std::string logged_missing[3];
  auto note_missing = [](int k, const char* key, const std::string& setting, bool found) {
    if (ParseDeviceChoice(setting) == DeviceChoice::kNamed && !found &&
        logged_missing[k] != setting) {
      logged_missing[k] = setting;
      REXLOG_INFO("Wheel: {} \"{}\" is not connected", key, setting);
    } else if (found) {
      logged_missing[k].clear();
    }
  };
  note_missing(0, "wheel_pedal_device", REXCVAR_GET(wheel_pedal_device), pedal_jid != 0);
  note_missing(1, "wheel_shifter_device", REXCVAR_GET(wheel_shifter_device), shifter_jid != 0);
  note_missing(2, "wheel_handbrake_device", REXCVAR_GET(wheel_handbrake_device),
               handbrake_jid != 0);
  if (wheels_.empty() && wanted.empty() && !logged_none_) {
    logged_none_ = true;
    REXLOG_INFO("Wheel: no wheel among {} SDL joystick(s)", count);
  }
  for (size_t i = 0; i < wheels_.size();) {
    if (std::find(wanted.begin(), wanted.end(), wheels_[i]->jid) == wanted.end()) {
      REXLOG_INFO("Wheel: \"{}\" removed", wheels_[i]->name);
      CloseLocked(*wheels_[i]);
      wheels_.erase(wheels_.begin() + i);
    } else {
      i++;
    }
  }
  for (SDL_JoystickID jid : wanted) {
    const bool have = std::any_of(wheels_.begin(), wheels_.end(),
                                  [&](const auto& w) { return w->jid == jid; });
    if (!have) {
      OpenLocked(jid);
    }
  }
  // The wheel first (#306): the one wheel_device names, then force feedback, then SDL's wheels.
  // The first player drives with it and the other devices are read with it.
  std::vector<bool> ffb(joys.size());
  for (size_t i = 0; i < jids.size(); i++) {
    for (const auto& w : wheels_) {
      ffb[i] = ffb[i] || (w->jid == jids[i] && w->ffb);
    }
  }
  const std::vector<int> order = WheelOrder(joys, wheel_like, ffb, aux, wheel_device);
  auto rank = [&](SDL_JoystickID jid) {
    for (size_t k = 0; k < order.size(); k++) {
      if (jids[order[k]] == jid) {
        return k;
      }
    }
    return order.size();
  };
  std::stable_sort(wheels_.begin(), wheels_.end(),
                   [&](const auto& a, const auto& b) { return rank(a->jid) < rank(b->jid); });
  static SDL_JoystickID logged_primary = 0;
  if (!wheels_.empty() && wheels_[0]->jid != logged_primary) {
    logged_primary = wheels_[0]->jid;
    const bool named = WheelDeviceMatch(wheel_device, JoyInfos(&logged_primary, 1)[0]) > 0;
    if (named || wheels_.size() > 1) {
      REXLOG_INFO("Wheel: \"{}\" is the wheel (the first player's; {})", wheels_[0]->name,
                  named              ? "wheel_device"
                  : wheels_[0]->ffb ? "it has force feedback"
                                    : "the first of them");
    }
  }
  if (ParseDeviceChoice(wheel_device) == DeviceChoice::kNamed &&
      std::none_of(joys.begin(), joys.end(),
                   [&](const JoyInfo& j) { return WheelDeviceMatch(wheel_device, j) > 0; })) {
    static std::string logged_wheel_missing;
    if (logged_wheel_missing != wheel_device) {
      logged_wheel_missing = wheel_device;
      REXLOG_INFO("Wheel: wheel_device \"{}\" is not connected", wheel_device);
    }
  }
  // The pedal device and the shifter device go with the first wheel; unplugged, the wheel's own
  // pedals and shifter buttons count again until they are back.
  for (size_t i = 0; i < wheels_.size(); i++) {
    Wheel& w = *wheels_[i];
    for (AuxDevice* d : {&w.pedals, &w.shifter_device, &w.handbrake_device}) {
      const SDL_JoystickID want = i != 0                         ? 0
                                  : d->role == AuxRole::kPedals  ? pedal_jid
                                  : d->role == AuxRole::kShifter ? shifter_jid
                                                                 : handbrake_jid;
      if (d->joy && d->jid != want) {
        CloseAux(*d, want ? "replaced" : "removed: read from the wheel until it is back");
      }
      if (want && !d->joy && OpenAux(*d, want)) {
        REXLOG_INFO("Wheel: {} \"{}\" is read with \"{}\"", AuxWhat(d->role), d->name, w.name);
        if (d->role == AuxRole::kShifter) {
          w.shifter_logged = false;
          if (REXCVAR_GET(wheel_shifter) == "off") {
            REXLOG_INFO("Wheel: wheel_shifter is off: set it to h or sequential to use the shifter");
          }
        }
      }
    }
    // Where the shifter is read, once its device (if any) is attached.
    const std::string mode = REXCVAR_GET(wheel_shifter);
    if (!w.shifter_logged && mode != "off") {
      LogShifter(EffectiveShifter(w.vid, w.pid, w.shifter_device), mode,
                 w.shifter_device.joy ? "shifter device" : "wheel");
    }
    w.shifter_logged = true;
  }
}

void WheelInputDriver::OpenLocked(SDL_JoystickID jid) {
  SDL_Joystick* joy = SDL_OpenJoystick(jid);
  if (!joy) {
    REXLOG_WARN("Wheel: could not open joystick {}: {}", jid, SDL_GetError());
    return;
  }
  auto w = std::make_unique<Wheel>();
  std::fill(std::begin(w->game_fx), std::end(w->game_fx), -1);
  std::memset(w->game_fx_sent, 0, sizeof(w->game_fx_sent));
  w->jid = jid;
  w->joy = joy;
  w->id = static_cast<DeviceId>(next_id_++ | (uint64_t(0x57) << 56));
  const char* name = SDL_GetJoystickName(joy);
  w->name = name ? name : "Wheel";
  char guid[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUID(joy), guid, sizeof(guid));
  w->guid = guid;
  w->num_axes = SDL_GetNumJoystickAxes(joy);
  w->num_buttons = SDL_GetNumJoystickButtons(joy);
  w->num_hats = SDL_GetNumJoystickHats(joy);
  w->vid = SDL_GetJoystickVendor(joy);
  w->pid = SDL_GetJoystickProduct(joy);
  w->prof = FindProfile(w->vid, w->pid);
  if (w->prof && w->num_axes <= std::max({kProfSteer, kProfThrottle, kProfBrake})) {
    w->prof = nullptr;  // exposed with fewer axes than the profile expects: detect instead
  }
  w->rest.assign(static_cast<size_t>(std::max(w->num_axes, 0)), 0);
  w->armed.assign(static_cast<size_t>(std::max(w->num_axes, 0)), 0);
  for (int a = 0; a < w->num_axes; a++) {
    Sint16 v = 0;
    if (!SDL_GetJoystickAxisInitialState(joy, a, &v)) {
      v = SDL_GetJoystickAxis(joy, a);
    }
    w->rest[a] = v;
  }
  w->tm = FindThrustmaster(w->vid, w->pid);
  if (w->tm) {
    REXLOG_INFO("Wheel: known wheel {}: {}, its buttons built in (the paddles shift)", w->tm->name,
                ThrustmasterAxes(w->tm, w->num_axes)
                    ? fmt::format("axes steer {}, throttle {}, brake {}, clutch {}", w->tm->steer,
                                  w->tm->throttle, w->tm->brake, w->tm->clutch)
                    : fmt::format("{} axes, not the {} its map has: detected", w->num_axes,
                                  w->tm->axes));
  }
  // An axis that has not reported yet reads 0; a pedal rests at an end (#308).
  w->rest_pending = !w->prof && RestPending(w->rest);
  if (w->rest_pending) {
    REXLOG_INFO("Wheel: every axis of \"{}\" reads 0 (no report yet): where they rest is read "
                "when the first report comes in",
                w->name);
  }
  if (w->prof) {
    REXLOG_INFO("Wheel: known wheel {}: fixed axes (steer {}, throttle {}, brake {}, clutch {}), "
                "pedals rest at the maximum{}",
                w->prof->name, kProfSteer, kProfThrottle, kProfBrake, kProfClutch,
                ButtonLayoutOf(w->vid, w->pid) == ButtonLayout::kPlayStation ? ", G29 buttons"
                : ButtonLayoutOf(w->vid, w->pid) == ButtonLayout::kXbox  ? ", Xbox wheel buttons"
                                                                         : "");
  }
  REXLOG_INFO(
      "Wheel: \"{}\" GUID({}) VID(0x{:04X}) PID(0x{:04X}) type({}) axes({}) buttons({}) hats({}); "
      "presented as an Xbox 360 Wireless Racing Wheel",
      w->name, w->guid, SDL_GetJoystickVendor(joy), SDL_GetJoystickProduct(joy),
      static_cast<int>(SDL_GetJoystickType(joy)), w->num_axes, w->num_buttons, w->num_hats);
  for (int a = 0; a < w->num_axes; a++) {
    REXLOG_INFO("Wheel:   axis {} rests at {}", a, w->rest[a]);
  }
  SetupForceFeedbackLocked(*w);
  w->ffb = w->haptic != nullptr || (!REXCVAR_GET(wheel_ffb) && SDL_WasInit(SDL_INIT_HAPTIC) &&
                                    SDL_IsJoystickHaptic(joy));
  wheels_.push_back(std::move(w));
}

void WheelInputDriver::CloseLocked(Wheel& w) {
  CloseAux(w.pedals, "closed with the wheel");
  CloseAux(w.shifter_device, "closed with the wheel");
  CloseAux(w.handbrake_device, "closed with the wheel");
  if (w.haptic) {
    SDL_StopHapticEffects(w.haptic);
    SDL_CloseHaptic(w.haptic);
    w.haptic = nullptr;
  }
  if (w.joy) {
    SDL_CloseJoystick(w.joy);
    w.joy = nullptr;
  }
}

void WheelInputDriver::SetupForceFeedbackLocked(Wheel& w) {
  if (!REXCVAR_GET(wheel_ffb) || !SDL_WasInit(SDL_INIT_HAPTIC) || !SDL_IsJoystickHaptic(w.joy)) {
    REXLOG_INFO("Wheel: no force feedback for \"{}\" (disabled or not supported)", w.name);
    return;
  }
  w.haptic = SDL_OpenHapticFromJoystick(w.joy);
  if (!w.haptic) {
    REXLOG_WARN("Wheel: haptic open failed: {}", SDL_GetError());
    return;
  }
  const Uint32 f = SDL_GetHapticFeatures(w.haptic);
  REXLOG_INFO("Wheel: haptic features 0x{:X} (spring={} sine={} constant={})", f,
              !!(f & SDL_HAPTIC_SPRING), !!(f & SDL_HAPTIC_SINE), !!(f & SDL_HAPTIC_CONSTANT));
  if (f & SDL_HAPTIC_GAIN) {
    SDL_SetHapticGain(w.haptic, 100);
  }
  if (f & SDL_HAPTIC_AUTOCENTER) {
    SDL_SetHapticAutocenter(w.haptic, 0);  // the spring below centres instead
    w.autocenter = 0;
  }
  if ((f & SDL_HAPTIC_SPRING) && !game_ff_active_) {
    SDL_HapticEffect e{};
    e.type = SDL_HAPTIC_SPRING;
    e.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
    e.condition.length = SDL_HAPTIC_INFINITY;
    for (int i = 0; i < 1; i++) {
      e.condition.right_sat[i] = e.condition.left_sat[i] = 0xFFFF;
    }
    w.spring_fx = SDL_CreateHapticEffect(w.haptic, &e);
    if (w.spring_fx >= 0) {
      if (!SDL_RunHapticEffect(w.haptic, w.spring_fx, SDL_HAPTIC_INFINITY)) {
        REXLOG_WARN("Wheel: spring effect would not start: {}", SDL_GetError());
      }
      REXLOG_INFO("Wheel: spring effect uploaded (id {})", w.spring_fx);
    } else {
      REXLOG_WARN("Wheel: spring effect rejected: {}", SDL_GetError());
    }
  }
  if ((f & SDL_HAPTIC_SINE) &&
      ParseRumbleStyle(REXCVAR_GET(wheel_rumble_style)) == RumbleStyle::kFine) {
    // Fine rumble (#273): a sine for each of the game's motors. Without room for both, classic.
    w.road_fx = CreateSine(w.haptic, kFineRoadPeriodMs);
    w.engine_fx = w.road_fx >= 0 ? CreateSine(w.haptic, kFineEnginePeriodMs) : -1;
    if (w.road_fx >= 0 && w.engine_fx >= 0) {
      REXLOG_INFO("Wheel: fine vibration uploaded: road {} Hz (id {}), engine {} Hz (id {})",
                  1000 / kFineRoadPeriodMs, w.road_fx, 1000 / kFineEnginePeriodMs, w.engine_fx);
    } else {
      REXLOG_WARN("Wheel: fine vibration rejected ({}): classic vibration instead",
                  SDL_GetError());
      if (w.road_fx >= 0) {
        SDL_DestroyHapticEffect(w.haptic, w.road_fx);
      }
      w.road_fx = w.engine_fx = -1;
    }
  }
  if ((f & SDL_HAPTIC_SINE) && w.road_fx < 0) {
    w.sine_fx = CreateSine(w.haptic, kClassicPeriodMs);
    if (w.sine_fx >= 0) {
      REXLOG_INFO("Wheel: vibration effect uploaded (id {})", w.sine_fx);
    } else {
      REXLOG_WARN("Wheel: vibration effect rejected: {}", SDL_GetError());
    }
  } else if (!(f & SDL_HAPTIC_SINE)) {
    REXLOG_INFO("Wheel: no periodic (sine) effect: the game's rumble is not played");
  }
  if (game_ff_active_) {  // a wheel plugged in while the game drives the force feedback
    for (uint32_t b = 1; b <= kFfMaxBlocks; b++) {
      ApplyGameBlockLocked(w, b);
    }
  }
}

void WheelInputDriver::ApplyForceLocked(Wheel& w, uint16_t left_motor, uint16_t right_motor) {
  if (!w.haptic) {
    return;
  }
  const float motor = std::max(left_motor, right_motor) / 65535.0f;
  if (w.spring_fx >= 0 && !game_ff_active_) {
    // Light centring always, firmer while the game drives the motors hard.
    const float c = FfTypeStrength(kFfSpring, Strengths()) * (0.2f + 0.6f * motor);
    SDL_HapticEffect e{};
    e.type = SDL_HAPTIC_SPRING;
    e.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
    e.condition.length = SDL_HAPTIC_INFINITY;
    e.condition.right_sat[0] = e.condition.left_sat[0] = 0xFFFF;
    e.condition.right_coeff[0] = e.condition.left_coeff[0] = static_cast<Sint16>(c * 32767.0f);
    SDL_UpdateHapticEffect(w.haptic, w.spring_fx, &e);
  }
  ApplyRumbleLocked(w, left_motor, right_motor, false);
}

void WheelInputDriver::ApplyRumbleLocked(Wheel& w, uint16_t left_motor, uint16_t right_motor,
                                         bool game) {
  if (!w.haptic) {
    return;
  }
  const float fade = game ? fade_.level : 1.0f;
  // Paced (wheel_rumble_pace.h, #467), except when the forces come back whole or are faded out.
  const bool now_too = w.restart_effects || fade == 0.0f;
  const double now = FfNow();
  if (w.road_fx >= 0 && w.engine_fx >= 0) {  // fine (wheel_rumble.h)
    const FineRumble fine = FineRumbleOf(left_motor, right_motor, Strengths(), fade);
    PaceWave(w.haptic, w.road_fx, fine.road, w.road_pace, w.road_magnitude, w.road_running,
             w.restart_effects, now_too, now);
    PaceWave(w.haptic, w.engine_fx, fine.engine, w.engine_pace, w.engine_magnitude,
             w.engine_running, w.restart_effects, now_too, now);
  } else {
    PaceWave(w.haptic, w.sine_fx, ClassicRumble(left_motor, right_motor, Strengths(), fade),
             w.sine_pace, w.sine_magnitude, w.sine_running, w.restart_effects, now_too, now);
  }
}

// The game's springs as the self-centring of a wheel without a spring effect (FfAutocenter). Taken
// from all the blocks each time, so it drops to 0 when the spring stops, is reset or is redefined
// as another type, not only when its own block is played again.
void WheelInputDriver::ApplyAutocenterLocked(Wheel& w) {
  const Uint32 f = SDL_GetHapticFeatures(w.haptic);
  // A spring the wheel refused becomes its autocentre too (#308).
  if (((f & SDL_HAPTIC_SPRING) && !w.ff_rejected.Rejected(SDL_HAPTIC_SPRING)) ||
      !(f & SDL_HAPTIC_AUTOCENTER)) {
    return;
  }
  const float strength = FfTypeStrength(kFfSpring, Strengths()) * fade_.level;
  const int level = static_cast<int>(std::lround(FfAutocenter(game_ff_, strength) * 100.0f));
  if (level != w.autocenter && SDL_SetHapticAutocenter(w.haptic, level)) {
    w.autocenter = level;
  }
}

// One of the game's effect blocks on one wheel: created, updated, started or stopped to match
// game_ff_. A spring on a wheel without a spring effect becomes its self-centring.
void WheelInputDriver::ApplyGameBlockLocked(Wheel& w, uint32_t block) {
  if (!w.haptic || block < 1 || block > kFfMaxBlocks) {
    return;
  }
  ApplyAutocenterLocked(w);
  const size_t i = block - 1;
  const Uint32 f = SDL_GetHapticFeatures(w.haptic);
  SDL_HapticEffect fx;
  std::memset(&fx, 0, sizeof(fx));  // compared whole with the last one sent
  bool play = false;
  if (game_ff_.defined(block)) {
    const FfEffect& e = game_ff_.effect(block);
    const float scale =
        FfScale(e, game_ff_.device_gain(), FfTypeStrength(e.type, Strengths()) * fade_.level);
    Uint16 condition = 0;
    switch (e.type) {
      case kFfSpring: condition = SDL_HAPTIC_SPRING; break;
      case kFfDamper: condition = SDL_HAPTIC_DAMPER; break;
      case kFfInertia: condition = SDL_HAPTIC_INERTIA; break;
      case kFfFriction: condition = SDL_HAPTIC_FRICTION; break;
      default: break;
    }
    if (e.type == kFfConstant && (f & SDL_HAPTIC_CONSTANT)) {
      fx.type = SDL_HAPTIC_CONSTANT;
      fx.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
      fx.constant.length = SDL_HAPTIC_INFINITY;
      const float sign = REXCVAR_GET(wheel_ffb_reverse) ? -kConstantToSdl : kConstantToSdl;
      fx.constant.level = static_cast<Sint16>(
          std::lround(sign * MinForceLevelOf(e, game_ff_.device_gain(), FfConstantLevel(e, scale)) *
                      32767.0f));
      play = game_ff_.Active(block);
    } else if (condition && (f & condition)) {
      const FfCondition c = FfConditionOf(e, scale);
      fx.type = condition;
      fx.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
      fx.condition.length = SDL_HAPTIC_INFINITY;
      fx.condition.right_sat[0] = static_cast<Uint16>(std::lround(c.right_sat * 65535.0f));
      fx.condition.left_sat[0] = static_cast<Uint16>(std::lround(c.left_sat * 65535.0f));
      fx.condition.right_coeff[0] = static_cast<Sint16>(std::lround(c.right_coeff * 32767.0f));
      fx.condition.left_coeff[0] = static_cast<Sint16>(std::lround(c.left_coeff * 32767.0f));
      fx.condition.deadband[0] = static_cast<Uint16>(std::lround(c.deadband * 65535.0f));
      fx.condition.center[0] = static_cast<Sint16>(std::lround(c.center * 32767.0f));
      play = game_ff_.Active(block) && FfConditionFelt(c);
    }
  }
  SetEffectDirection(fx, w.ff_direction);  // the direction its effects work in (#308)
  int& id = w.game_fx[i];
  // What the log's summary shows of it: the constant force's level, a condition's coefficient.
  const int32_t sent_value =
      fx.type == SDL_HAPTIC_CONSTANT ? fx.constant.level : fx.condition.right_coeff[0];
  if (!play) {
    if (id >= 0 && w.game_fx_running[i]) {
      SDL_StopHapticEffect(w.haptic, id);
      w.game_fx_running[i] = false;
      g_ff_log.Sdl(block, FfSdlCall::kStop);
    }
    return;
  }
  if (id >= 0 && w.game_fx_sent[i].type != fx.type) {
    SDL_DestroyHapticEffect(w.haptic, id);
    id = -1;
    w.game_fx_running[i] = false;
  }
  if (id < 0) {
    // A type the wheel refused is not tried again until the game resets its force feedback or the
    // wheel is plugged in again (#308: it was tried, and logged, every frame).
    if (w.ff_rejected.Rejected(fx.type)) {
      return;
    }
    id = CreateEffectAnyDirection(w.haptic, fx, w.ff_direction);
    g_ff_log.Sdl(block, id < 0 ? FfSdlCall::kFailure : FfSdlCall::kCreate, sent_value);
    if (id < 0) {
      if (w.ff_rejected.Reject(fx.type)) {
        REXLOG_WARN("Wheel: \"{}\" refuses the game's {} (effect block {}, SDL type {}) in every "
                    "direction: {}. Not tried again until the game resets its force feedback or "
                    "the wheel is plugged in again{}",
                    w.name, FfTypeName(fx.type), block, fx.type, SDL_GetError(),
                    fx.type == SDL_HAPTIC_SPRING &&
                            (SDL_GetHapticFeatures(w.haptic) & SDL_HAPTIC_AUTOCENTER)
                        ? "; the wheel's autocentre stands in for it"
                        : "");
        ApplyAutocenterLocked(w);
      }
      return;
    }
    w.game_fx_sent[i] = fx;
  } else if (std::memcmp(&fx, &w.game_fx_sent[i], sizeof(fx)) != 0) {
    // FH1 updates its constant force every frame, mostly with the same value.
    if (SDL_UpdateHapticEffect(w.haptic, id, &fx)) {
      w.game_fx_sent[i] = fx;
      g_ff_log.Sdl(block, FfSdlCall::kUpdate, sent_value);
    } else {
      g_ff_log.Sdl(block, FfSdlCall::kFailure);
      NoteFfFailure(w.ff_failures, "an update", block);
    }
  }
  if (!w.game_fx_running[i] || w.restart_effects) {
    w.game_fx_running[i] = SDL_RunHapticEffect(w.haptic, id, SDL_HAPTIC_INFINITY);
    g_ff_log.Sdl(block, w.game_fx_running[i] ? FfSdlCall::kStart : FfSdlCall::kFailure);
    if (!w.game_fx_running[i]) {
      NoteFfFailure(w.ff_failures, "starting", block);
    }
  }
}

bool WheelInputDriver::GameForceFeedback(const std::function<bool(FfState&)>& change,
                                         uint32_t block) {
  std::lock_guard lock(mutex_);
  if (wheels_.empty()) {
    return false;
  }
  if (!change(game_ff_)) {
    return true;
  }
  if (FfNothingDefined(game_ff_)) {
    for (auto& w : wheels_) {
      w->ff_rejected.Reset();  // a reset: effects a wheel refused are tried again (#308)
    }
  }
  UpdateFadeLocked();
  for (uint32_t b = 1; b <= kFfMaxBlocks; b++) {
    if (!game_ff_.defined(b) || game_ff_logged_[b - 1]) {
      continue;
    }
    game_ff_logged_[b - 1] = true;
    const FfEffect& e = game_ff_.effect(b);
    REXLOG_INFO("Wheel: the game's effect block {}: type {} gain {}, word 11 {}, coefficients "
                "{}/{}, saturations {}/{}, dead band {}",
                b, e.type, e.gain, e.magnitude, e.pos_coeff, e.neg_coeff, e.pos_sat, e.neg_sat,
                e.deadband);
  }
  if (!game_ff_active_) {
    game_ff_active_ = true;
    REXLOG_INFO("Wheel: the game drives the force feedback itself (XInputdFF); the approximate "
                "spring is off");
  }
  for (auto& w : wheels_) {
    if (w->haptic && w->spring_fx >= 0) {  // the game's own spring replaces it
      SDL_DestroyHapticEffect(w->haptic, w->spring_fx);
      w->spring_fx = -1;
    }
    for (uint32_t b = 1; b <= kFfMaxBlocks; b++) {
      if (block == 0 || b == block) {
        ApplyGameBlockLocked(*w, b);
      }
    }
  }
  return true;
}

bool WheelInputDriver::GameRumble(uint16_t left_motor, uint16_t right_motor) {
  std::lock_guard lock(mutex_);
  if (wheels_.empty()) {
    return false;
  }
  game_rumble_[0] = left_motor;
  game_rumble_[1] = right_motor;
  g_ff_log.Rumbled();
  UpdateFadeLocked();
  for (auto& w : wheels_) {
    ApplyRumbleLocked(*w, left_motor, right_motor, true);
  }
  return true;
}

void WheelInputDriver::SetPlayerDriving(bool driving) {
  if (g_player_driving.exchange(driving ? 1 : 0) == -1) {
    REXLOG_INFO("Wheel: the game says when the player drives; its forces fade out in the pause "
                "menu, rewinds, replays and menus");
  }
}

void WheelInputDriver::UpdateFadeLocked() {
  const auto now = std::chrono::steady_clock::now();
  const float dt = fade_time_.time_since_epoch().count() == 0
                       ? 0.0f
                       : std::chrono::duration<float>(now - fade_time_).count();
  fade_time_ = now;
  if (!fade_.Step(g_player_driving.load(std::memory_order_relaxed) != 0, dt)) {
    return;
  }
  // Played again in steps of 1/32 (each step uploads every effect), and at the ends.
  const float level = fade_.level;
  if (std::fabs(level - fade_played_) < 1.0f / 32.0f && level != 0.0f && level != 1.0f) {
    return;
  }
  // The ends are logged (the game logs why it changes, forza-recomp wheel_bridge.cpp), so a log
  // shows the forces came back whole (#304).
  if (level == 0.0f) {
    REXLOG_INFO("Wheel: the game's forces are faded out");
  } else if (level == 1.0f && fade_played_ < 1.0f) {
    REXLOG_INFO("Wheel: the game's forces are back at full strength");
  }
  fade_played_ = level;
  if (!game_ff_active_) {
    return;
  }
  for (auto& w : wheels_) {
    // Back at full strength, every effect that should play is started again, even one this driver
    // believes is running: a wheel that dropped its effects meanwhile plays them again (DirectInput
    // stops a device's effects when it has to acquire it again) instead of going on without its
    // steering force (#304). Every level is sent whole, so a quick toggle leaves nothing behind.
    w->restart_effects = level == 1.0f;
    for (uint32_t b = 1; b <= kFfMaxBlocks; b++) {
      ApplyGameBlockLocked(*w, b);
    }
    ApplyRumbleLocked(*w, game_rumble_[0], game_rumble_[1], true);
    w->restart_effects = false;
  }
}

bool GameFfDeviceInfo() {
  WheelInputDriver* d = g_ff_driver;
  return d && d->GameForceFeedback([](FfState&) { return false; }, 0);
}

bool GameFfSetEffect(const FfEffect& effect) {
  WheelInputDriver* d = g_ff_driver;
  bool ok = true;
  const bool wheel = d && d->GameForceFeedback(
                              [&](FfState& s) {
                                ok = s.Define(effect);
                                // A change of the block after its first definition (logged once,
                                // below): its type, coefficients, gain to or from 0 (#304).
                                if (ok && g_ff_log.Defined(effect, FfNow())) {
                                  REXLOG_INFO("Wheel: the game's effect block {} is now {} (type "
                                              "{}) gain {}, word 11 {}, coefficients {}/{}, "
                                              "saturations {}/{}, dead band {}",
                                              effect.block, FfBlockTypeName(effect.type),
                                              effect.type, effect.gain, effect.magnitude,
                                              effect.pos_coeff, effect.neg_coeff, effect.pos_sat,
                                              effect.neg_sat, effect.deadband);
                                }
                                return ok;
                              },
                              effect.block);
  return wheel && ok;
}

bool GameFfUpdateEffect(const FfEffect& effect, uint32_t flags) {
  // FH1 always sends the whole effect, with flags 0x400 (its parameters) or 0x100 (its gain).
  (void)flags;
  return GameFfSetEffect(effect);
}

bool GameFfOperate(uint32_t block, uint32_t op) {
  WheelInputDriver* d = g_ff_driver;
  bool ok = true;
  const bool wheel = d && d->GameForceFeedback(
                              [&](FfState& s) {
                                ok = s.Operate(block, op);
                                if (g_ff_log.Operated(block, op)) {
                                  REXLOG_INFO("Wheel: the game {} effect block {} (operation {}){}",
                                              op == kFfOpStop ? "stops" : "starts", block, op,
                                              ok ? "" : ", which is not defined");
                                }
                                return ok;
                              },
                              block);
  return wheel && ok;
}

bool GameFfControl(uint32_t command) {
  WheelInputDriver* d = g_ff_driver;
  return d && d->GameForceFeedback(
                  [&](FfState& s) {
                    s.Control(command);
                    if (g_ff_log.Controlled()) {
                      REXLOG_INFO("Wheel: the game's device control {} ({})", command,
                                  FfControlName(command));
                    }
                    return true;
                  },
                  0);
}

bool GameFfSetGain(uint32_t gain) {
  WheelInputDriver* d = g_ff_driver;
  return d && d->GameForceFeedback(
                  [&](FfState& s) {
                    if (std::min<uint32_t>(gain, 255) != s.device_gain() && g_ff_log.Controlled()) {
                      REXLOG_INFO("Wheel: the game sets the device gain to {}", gain);
                    }
                    s.SetDeviceGain(gain);
                    return true;
                  },
                  0);
}

bool GameFfSetRumble(uint16_t left_motor, uint16_t right_motor) {
  WheelInputDriver* d = g_ff_driver;
  return d && d->GameRumble(left_motor, right_motor);
}

WheelInputDriver::Wheel* WheelInputDriver::FindLocked(DeviceId id) {
  for (auto& w : wheels_) {
    if (w->id == id) {
      return w.get();
    }
  }
  return nullptr;
}

void WheelInputDriver::ComputeStateLocked(Wheel& w, X_INPUT_GAMEPAD* out) {
  if (w.rest_pending) {
    // The first report: where each axis is now is its rest (wheel_thrustmaster.h).
    std::vector<int16_t> now(w.rest.size());
    for (size_t a = 0; a < now.size(); a++) {
      now[a] = SDL_GetJoystickAxis(w.joy, static_cast<int>(a));
    }
    if (RestArrived(now)) {
      w.rest = now;
      w.armed.assign(w.armed.size(), 0);
      w.rest_pending = false;
      for (size_t a = 0; a < now.size(); a++) {
        REXLOG_INFO("Wheel:   axis {} rests at {} (first report)", a, now[a]);
      }
    }
  }
  const auto axes = ParseKv(REXCVAR_GET(wheel_axes));
  auto axis_override = [&](const char* key) {
    auto it = axes.find(key);
    return it == axes.end() ? -1 : it->second;
  };

  // Which pedals a pedal device on its own USB device gives (wheel_devices.h, #272); the others
  // come from the wheel as without one.
  const auto pedal_axes = ParseKv(REXCVAR_GET(wheel_pedal_axes));
  const PedalSources src = PlanPedals(axes, pedal_axes, w.pedals.joy != nullptr);
  const bool from_wheel[3] = {!src.from_aux[0], !src.from_aux[1], !src.from_aux[2]};

  // Detect: the steering axis rests near the middle, the pedals at an end.
  int steer = axis_override("steer");
  int pedal[3] = {from_wheel[0] ? axis_override("throttle") : -1,
                  from_wheel[1] ? axis_override("brake") : -1,
                  from_wheel[2] ? axis_override("clutch") : -1};
  if (w.prof) {
    if (steer < 0) steer = kProfSteer;
    if (pedal[0] < 0 && from_wheel[0]) pedal[0] = kProfThrottle;
    if (pedal[1] < 0 && from_wheel[1]) pedal[1] = kProfBrake;
    if (pedal[2] < 0 && from_wheel[2] && kProfClutch < w.num_axes) pedal[2] = kProfClutch;
  } else if (const ThrustmasterModel* tm = ThrustmasterAxes(w.tm, w.num_axes)) {
    if (steer < 0) steer = tm->steer;
    if (pedal[0] < 0 && from_wheel[0]) pedal[0] = tm->throttle;
    if (pedal[1] < 0 && from_wheel[1]) pedal[1] = tm->brake;
    if (pedal[2] < 0 && from_wheel[2]) pedal[2] = tm->clutch;
  }
  if (steer < 0) {
    for (int a = 0; a < w.num_axes; a++) {
      if (std::abs(w.rest[a]) < 12000 && a != pedal[0] && a != pedal[1] && a != pedal[2]) {
        steer = a;
        break;
      }
    }
    if (steer < 0 && w.num_axes > 0) {
      steer = 0;
    }
  }
  const bool combined = REXCVAR_GET(wheel_pedals) == "combined";
  // The analog handbrake's axis is no pedal (wheel_handbrake.h).
  const HandbrakePlan hb = PlanHandbrake(axes, w.handbrake_device.joy != nullptr,
                                         REXCVAR_GET(wheel_handbrake_axis));
  const bool hb_on_pedals = hb.source == HandbrakeSource::kDevice && w.pedals.joy &&
                            w.handbrake_device.jid == w.pedals.jid;
  if (!w.prof) {
    DetectPedalAxes(pedal, from_wheel, steer,
                    hb.source == HandbrakeSource::kWheel ? RestWithout(w.rest, hb.axis) : w.rest,
                    combined);
  }
  int aux_pedal[3] = {-1, -1, -1};
  if (src.Any() && w.pedals.joy) {
    PedalDeviceAxes(aux_pedal, src.from_aux, pedal_axes, w.pedals.model,
                    hb_on_pedals ? RestWithout(w.pedals.rest, hb.axis) : w.pedals.rest, combined);
  }

  // A pedal's axis on the device it reads: the wheel (whose known profile's pedals rest at the
  // maximum) or the pedal device.
  struct Axis {
    SDL_Joystick* joy;
    int axis, count;
    const std::vector<int16_t>& rest;
    std::vector<uint8_t>& armed;
    bool known;
  };
  auto pedal_axis = [&](int p) -> Axis {
    if (src.from_aux[p]) {
      return {w.pedals.joy, aux_pedal[p], w.pedals.num_axes, w.pedals.rest, w.pedals.armed, false};
    }
    return {w.joy, pedal[p], w.num_axes, w.rest, w.armed, w.prof != nullptr};
  };
  auto pressed = [&](const Axis& d, bool invert) -> float {
    const int a = d.axis;
    if (!d.joy || a < 0 || a >= d.count) {
      return 0.0f;
    }
    const float v = Norm(SDL_GetJoystickAxis(d.joy, a));
    // A known wheel's pedal rests at the maximum; otherwise where the axis sat when it was opened.
    const float r = d.known ? 1.0f : Norm(d.rest[a]);
    bool high_rest = r > 0.0f;
    if (invert) {
      high_rest = !high_rest;
    }
    const float rest = (invert || d.known) ? (high_rest ? 1.0f : -1.0f) : r;
    // Nothing from a pedal until it has been seen at rest: a first reading of 0 (not reported yet)
    // or a pedal held down while the game starts must not count as a press.
    if (!d.armed[a]) {
      if (std::fabs(v - rest) < 0.2f) {
        d.armed[a] = 1;
      } else {
        return 0.0f;
      }
    }
    const float p = high_rest ? (rest - v) / (rest + 1.0f) : (v - rest) / (1.0f - rest);
    const float d0 = 0.02f;
    return std::clamp((p - d0) / (1.0f - d0), 0.0f, 1.0f);
  };

  float thr, brk;
  if (combined) {
    const Axis t = pedal_axis(0);
    const float v =
        t.joy && t.axis >= 0 && t.axis < t.count ? Norm(SDL_GetJoystickAxis(t.joy, t.axis)) : 0.0f;
    const float flip = REXCVAR_GET(wheel_invert_throttle) ? -1.0f : 1.0f;
    thr = std::clamp(-v * flip, 0.0f, 1.0f);
    brk = std::clamp(v * flip, 0.0f, 1.0f);
  } else {
    thr = pressed(pedal_axis(0), REXCVAR_GET(wheel_invert_throttle));
    brk = pressed(pedal_axis(1), REXCVAR_GET(wheel_invert_brake));
  }
  thr = std::pow(thr, static_cast<float>(REXCVAR_GET(wheel_throttle_curve)));
  brk = std::pow(brk, static_cast<float>(REXCVAR_GET(wheel_brake_curve)));
  // The 360 wheel's analog clutch is the right stick Y: the wheel layouts bind both of its
  // directions to Clutch, and the game applies its own clutch deadzones.
  const float clutch =
      REXCVAR_GET(wheel_clutch) ? pressed(pedal_axis(2), REXCVAR_GET(wheel_invert_clutch)) : 0.0f;
  // The analog handbrake: the 360 wheel's, on the left stick Y (wheel_handbrake.h).
  float handbrake = 0.0f;
  if (hb.source == HandbrakeSource::kWheel) {
    handbrake = pressed({w.joy, hb.axis, w.num_axes, w.rest, w.armed, false},
                        REXCVAR_GET(wheel_invert_handbrake));
  } else if (hb.source == HandbrakeSource::kDevice) {
    AuxDevice& d = w.handbrake_device;
    handbrake = pressed({d.joy, hb.axis, d.num_axes, d.rest, d.armed, false},
                        REXCVAR_GET(wheel_invert_handbrake));
  }

  float s = 0.0f;
  if (steer >= 0 && steer < w.num_axes) {
    s = Norm(SDL_GetJoystickAxis(w.joy, steer));
  }
  const float gain = static_cast<float>(REXCVAR_GET(wheel_rotation)) /
                     static_cast<float>(std::max(1, REXCVAR_GET(wheel_lock)));
  s *= gain * (REXCVAR_GET(wheel_invert_steering) ? -1.0f : 1.0f);
  const float dz = static_cast<float>(REXCVAR_GET(wheel_deadzone));
  const float as = std::fabs(s);
  s = as <= dz ? 0.0f : std::copysign(std::min(1.0f, (as - dz) / (1.0f - dz)), s);
  // An inner steering deadzone (and a 900 degree wheel turned 50 degrees is only 5.5 percent of its
  // range): everything beyond a hair already gives `off` of the full steering. Not with full-range
  // steering: the game then follows the wheel's angle from the centre (its wheel deadzones are its
  // own options), and the step of `off` at the centre only made the car twitch left and right
  // there (#219).
  const float off =
      REXCVAR_GET(wheel_900_steering) ? 0.0f : static_cast<float>(REXCVAR_GET(wheel_steer_offset));
  if (off > 0.0f && std::fabs(s) > 0.001f) {
    s = std::copysign(off + (1.0f - off) * std::fabs(s), s);
  }

  // Buttons: SDL numbers to the pad (wheel_buttons.h); the shifter's own press none.
  // A shifter on its own USB device reads its own buttons, and the wheel's are all free.
  const std::string shifter = REXCVAR_GET(wheel_shifter);
  const bool aux_shifter = shifter != "off" && w.shifter_device.joy != nullptr;
  const ShifterMap shifter_map =
      shifter != "off" ? EffectiveShifter(w.vid, w.pid, w.shifter_device) : ShifterMap{};
  const std::map<std::string, int> button_overrides =
      ThrustmasterButtons(w.tm, ParseKv(REXCVAR_GET(wheel_buttons)));
  std::map<std::string, int> map =
      WheelButtons(ButtonLayoutOf(w.vid, w.pid), button_overrides,
                   aux_shifter ? std::set<int>{} : ShifterButtons(shifter_map, shifter));
  static const std::pair<const char*, uint16_t> kBits[] = {
      {"a", 0x1000},  {"b", 0x2000},   {"x", 0x4000},    {"y", 0x8000},
      {"lb", 0x0100}, {"rb", 0x0200},  {"back", 0x0020}, {"start", 0x0010},
      {"ls", 0x0040}, {"rs", 0x0080},  {"up", 0x0001},   {"down", 0x0002},
      {"left", 0x0004}, {"right", 0x0008}};
  uint16_t buttons = 0;
  for (auto& [key, bit] : kBits) {
    const int n = map[key];
    if (n >= 0 && n < w.num_buttons && SDL_GetJoystickButton(w.joy, n)) {
      buttons |= bit;
    }
  }
  if (w.num_hats > 0) {
    // The hat is the d-pad, except for directions wheel_buttons sets (HatDpad).
    buttons |= HatDpad(SDL_GetJoystickHat(w.joy, 0), button_overrides);
  }

  // Shifter. H-pattern: the lever's gear on the right stick X (-1 reverse, 0 neutral), which
  // the game takes as the gear to be in. Sequential: up and down shift like the bumpers (the
  // wheel layouts' ShiftUp and ShiftDown), next to the paddles.
  int gear = kGearNeutral;
  if (shifter != "off") {
    const ShifterMap& m = shifter_map;
    SDL_Joystick* sj = aux_shifter ? w.shifter_device.joy : w.joy;
    const int buttons_n = aux_shifter ? w.shifter_device.num_buttons : w.num_buttons;
    auto held = [&](int n) { return n >= 0 && n < buttons_n && SDL_GetJoystickButton(sj, n); };
    if (shifter == "h") {
      const double now_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
      // A lever crossing neutral between two gates goes from one gear to the other (#468).
      const double crossing_since = w.lever_hold.neutral_since;
      const int before = w.gear;
      w.gear = EngageLeverGear(w.lever_hold, LeverGear(m, held), w.gear, now_ms,
                               REXCVAR_GET(wheel_shifter_neutral_ms),
                               REXCVAR_GET(wheel_shifter_clutch), clutch,
                               static_cast<float>(REXCVAR_GET(wheel_clutch_threshold)));
      if (w.gear != before && !w.keystroke_poll && w.gears_logged < 16) {
        // How the game takes the change, which its paddle animation shows (#468). A change into
        // neutral plays none; one out of it is a downshift for the game whatever the gear.
        auto name = [](int g) {
          return g == kGearReverse ? std::string("R")
                 : g == kGearNeutral ? std::string("N")
                                     : std::to_string(g);
        };
        std::string how;
        if (w.gear == kGearNeutral) {
          how = "";
        } else if (before == kGearNeutral) {
          how = ", a downshift for the game (its neutral is above every gear)";
        } else {
          how = GameUpshift(before, w.gear) ? ", an upshift for the game"
                                            : ", a downshift for the game";
          if (crossing_since >= 0.0) {
            how += fmt::format(" (the lever crossed neutral in {:.0f} ms)",
                               now_ms - crossing_since);
          }
        }
        REXLOG_INFO("Wheel: H-pattern {} > {}{}{}", name(before), name(w.gear), how,
                    ++w.gears_logged == 16 ? "; the next changes are not logged" : "");
      }
      gear = GearRequestOf(w.gear_request, w.gear, !w.keystroke_poll, now_ms);
    } else {
      if (held(m.up)) buttons |= 0x0200;
      if (held(m.down)) buttons |= 0x0100;
    }
  }

  out->buttons = buttons;
  out->left_trigger = static_cast<uint8_t>(std::lround(brk * 255.0f));
  out->right_trigger = static_cast<uint8_t>(std::lround(thr * 255.0f));
  out->thumb_lx = static_cast<int16_t>(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
  // Only for a game that took this for a wheel: on a gamepad the right stick looks around.
  if (w.title_caps) {
    out->thumb_rx = static_cast<int16_t>(gear);
    out->thumb_ry = static_cast<int16_t>(std::lround(clutch * 32767.0f));
    out->thumb_ly = HandbrakeThumb(handbrake);
  }
}

void WheelInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  if (!sdl_ready_) {
    return;
  }
  std::lock_guard lock(mutex_);
  RescanLocked();
  for (const auto& w : wheels_) {
    DeviceInfo info;
    info.id = w->id;
    info.name = w->name;
    info.guid = w->guid;
    info.subtype = kSubTypeWheel;
    info.synthetic = false;
    out.push_back(info);
  }
}

X_RESULT WheelInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  std::lock_guard lock(mutex_);
  Wheel* w = FindLocked(id);
  if (!w) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  PumpLocked();
  UpdateFadeLocked();
  for (auto& wheel : wheels_) {
    KeepPlaying(*wheel, game_ff_active_, FfNow());
    PaceRumble(*wheel, FfNow());
  }
  if (game_ff_active_ && g_ff_log.SummaryDue(FfNow())) {
    // Every 10 s, what the game sent and what the wheel got (wheel_ff_log.h, #304), and whether the
    // wheel still plays what it was told to (where SDL can ask: DirectInput, macOS, SDL's HIDAPI
    // Logitech driver).
    Wheel& first = *wheels_[0];
    const bool status = first.haptic && (SDL_GetHapticFeatures(first.haptic) & SDL_HAPTIC_STATUS);
    uint32_t running = 0, ended = 0;
    for (uint32_t i = 0; i < kFfMaxBlocks; i++) {
      if (!first.game_fx_running[i] || first.game_fx[i] < 0) {
        continue;
      }
      running |= 1u << i;
      if (!status) {
        continue;
      }
      // 1 playing, 0 stopped by itself, -1 the wheel could not tell (SDL sets an error).
      SDL_ClearError();
      const int playing = SDL_GetHapticEffectStatus(first.haptic, first.game_fx[i]) ? 1
                          : *SDL_GetError()                                          ? -1
                                                                                     : 0;
      // A stopped effect plays again unless wheel_ffb_keep_playing is -1 (wheel_ff_keep_playing.h).
      const FfStoppedCheck check =
          FfCheckStopped(playing, REXCVAR_GET(wheel_ffb_keep_playing), [&] {
            return SDL_RunHapticEffect(first.haptic, first.game_fx[i], SDL_HAPTIC_INFINITY);
          });
      FfNoteCheck(g_ff_log, i + 1, check);
      if (check.failed) {
        NoteFfFailure(first.ff_failures, "starting again", i + 1);
      }
      if (check.stopped) {
        ended |= 1u << i;
        // The 1st, 10th, 100th... time: a wheel that ends effects by itself (#304).
        if (FfLogThisCount(++first.ff_ended)) {
          REXLOG_WARN("Wheel: \"{}\" says the game's effect block {} ({}) has stopped, though it "
                      "was started and not stopped ({} so far){}",
                      first.name, i + 1, FfBlockTypeName(game_ff_.effect(i + 1).type),
                      first.ff_ended,
                      check.restarted ? "; started again"
                      : check.failed  ? "; starting it again failed"
                                      : "");
        }
      }
    }
    const std::string summary = g_ff_log.Summary(game_ff_, running, ended, fade_.level, FfNow());
    if (!summary.empty()) {
      REXLOG_INFO("Wheel: {}", summary);
    }
  }
  X_INPUT_STATE s{};
  if (is_active()) {
    ComputeStateLocked(*w, &s.gamepad);
  } else if (w->title_caps) {
    // Out of focus: nothing pressed, and an H-pattern shifter asks for no gear (not neutral).
    s.gamepad.thumb_rx = static_cast<int16_t>(IdleGearRequest());
  }
  if (std::memcmp(&s.gamepad, &w->last_state.gamepad, sizeof(s.gamepad)) != 0) {
    w->packet++;
    w->last_state = s;
  }
  s.packet_number = w->packet;
  *out_state = s;
  return X_ERROR_SUCCESS;
}

X_RESULT WheelInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                                X_INPUT_CAPABILITIES* caps) {
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  std::lock_guard lock(mutex_);
  Wheel* w = FindLocked(id);
  if (!w) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // The game asks with its flags (FH1: XINPUT_FLAG_GAMEPAD), the SDK's own deadzone lookup
  // with none.
  if (flags != 0 && !w->title_caps) {
    w->title_caps = true;
    REXLOG_INFO("Wheel: the game asked for capabilities; answering SubType WHEEL{}",
                REXCVAR_GET(wheel_shifter) == "h" ? " with an H-pattern shifter" : "");
  }
  // What the Xbox 360 Wireless Racing Wheel reports. FH1 takes a pad for a wheel when
  // SubType is 2, read once when the pad slot is created (sub_82CBAFE0). The car's input
  // frame (sub_824B7360) then uses the full-range wheel steering only when the wheel
  // reports a thumb_lx of 900 (sub_82CB84A0), the 270-degree Xbox 360 wheel mode otherwise.
  std::memset(caps, 0, sizeof(*caps));
  caps->type = XINPUT_DEVTYPE_GAMEPAD;
  caps->sub_type = kSubTypeWheel;
  caps->flags = X_INPUT_CAPS_FFB_SUPPORTED | X_INPUT_CAPS_WIRELESS;
  caps->gamepad.buttons = 0xF3FF;
  caps->gamepad.left_trigger = 0xFF;
  caps->gamepad.right_trigger = 0xFF;
  caps->gamepad.thumb_lx = REXCVAR_GET(wheel_900_steering) ? int16_t(900) : static_cast<int16_t>(0x7FFF);
  // An H-pattern shifter: thumb_rx other than 0 and 1, its high byte the number of forward gears.
  // The game then reads the gear from the state's thumb_rx (wheel_shifter.h).
  const int top_gear = REXCVAR_GET(wheel_shifter) == "h"
                           ? TopGear(EffectiveShifter(w->vid, w->pid, w->shifter_device))
                           : 0;
  caps->gamepad.thumb_rx = static_cast<int16_t>(top_gear << 8);
  caps->vibration.left_motor_speed = 0xFFFFu;
  caps->vibration.right_motor_speed = 0xFFFFu;
  return X_ERROR_SUCCESS;
}

X_RESULT WheelInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vib) {
  std::lock_guard lock(mutex_);
  Wheel* w = FindLocked(id);
  if (!w) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  ApplyForceLocked(*w, vib->left_motor_speed, vib->right_motor_speed);
  return X_ERROR_SUCCESS;
}

X_RESULT WheelInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE* ks) {
  std::lock_guard lock(mutex_);
  Wheel* w = FindLocked(id);
  if (!w || !ks) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  PumpLocked();
  uint16_t cur = 0;
  if (is_active()) {
    X_INPUT_GAMEPAD pad{};
    w->keystroke_poll = true;
    ComputeStateLocked(*w, &pad);
    w->keystroke_poll = false;
    cur = pad.buttons;
  }
  const uint16_t changed = cur ^ w->keystroke_buttons;
  if (!changed) {
    return X_ERROR_EMPTY;
  }
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < 16; i++) {
      const uint16_t bit = uint16_t(1u << i);
      if (!(changed & bit) || kVk[i] == rex::ui::VirtualKey::kNone) {
        continue;
      }
      const bool down = cur & bit;
      if ((pass == 0) == down) {
        continue;  // releases first
      }
      ks->virtual_key = uint16_t(kVk[i]);
      ks->unicode = 0;
      ks->user_index = 0;
      ks->hid_code = 0;
      ks->flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
      w->keystroke_buttons = down ? (w->keystroke_buttons | bit) : (w->keystroke_buttons & ~bit);
      return X_ERROR_SUCCESS;
    }
  }
  w->keystroke_buttons = cur;  // only unmapped bits changed
  return X_ERROR_EMPTY;
}

}  // namespace rex::input::wheel

// Whether the player is driving (1) or not (0: the pause menu, rewinds, replays, menus), from the
// game (#256; Forza Horizon: forza-recomp/src/platform/wheel_bridge.cpp). Looked up at run time,
// so a game built against the stock SDK headers can call it and an older runtime still loads.
// Until the first call the game's forces never fade.
#if defined(_WIN32)
#define REX_WHEEL_EXPORT __declspec(dllexport)
#else
#define REX_WHEEL_EXPORT __attribute__((visibility("default")))
#endif
extern "C" REX_WHEEL_EXPORT void ForzaWheelPlayerDriving(int driving) {
  rex::input::wheel::WheelInputDriver::SetPlayerDriving(driving != 0);
}
