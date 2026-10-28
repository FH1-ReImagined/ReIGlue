/**
 * @file        src/input/wheel/wheel_gear_request.h
 * @brief       The H-pattern shifter's gear as the game's gear request (#303), without SDL: a gear
 *              is asked for for as long as the lever sits in its slot (wheel_shifter_follow_slot,
 *              the default, as on the Xbox), or only when the lever moves.
 *
 * The car's input frame (sub_824B7360) turns the wheel's thumb_rx (sub_82CBC320 returns it as it
 * is) into a gear request every frame: -1 reverse, 0 neutral, 1 to 8 that gear; any other value
 * leaves its "no request" (10), as on a wheel without a shifter. The game shifts when the request
 * changes, and takes it again when it resets the car at a race or event start: a lever left in 3rd
 * put the car in 3rd at every start even after the paddles had shifted, and one left in neutral
 * put it in neutral (#4). The Xbox does the same with a 360 wheel's shifter (#4), so that stays
 * the default. With wheel_shifter_follow_slot off (for paddle users), a change of the engaged
 * gear (EngageGear in wheel_shifter.h: the lever moved into a slot or out to neutral, or the
 * clutch let a gear in) is asked for while the game reads at least kGearRequestReads states over
 * kGearRequestMs, then nothing: the paddles and a sequential shifter keep the gear they chose
 * until the lever moves. The lever's position when the wheel is first read is not a movement.
 *
 * The lever crosses neutral between two gates (#468). The car's gears are numbered R 0, 1 to 8 and
 * neutral 9 (its gear and the request, also car + 2240 in TU4), and the game takes a change for an
 * upshift when the new gear's number is higher (sub_8250B318: the flag the cockpit's paddle
 * animation reads, vehicle + 11717 in TU4; a change into neutral plays no animation). From neutral
 * every gear is lower, so an H-pattern shift 2 > N > 3 played the paddle downshift animation (the
 * paddles and a sequential shifter go 2 > 3). LeverThroughNeutral keeps the gear the lever left
 * for wheel_shifter_neutral_ms in neutral: a lever that reaches another gate in that time is one
 * change, 2 > 3; one that stays in neutral longer is neutral then.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/wheel/wheel_shifter.h>

namespace rex::input::wheel {

constexpr int kGearNoRequest = 10;  // the game's own "no request"; 9 and up read the same
constexpr double kNeutralTransitMs = 400.0;  // wheel_shifter_neutral_ms's default
constexpr double kGearRequestMs = 250.0;
constexpr int kGearRequestReads = 4;

// The car's gear number in the game for a lever gear: R 0, 1 to 8, neutral 9 (#468).
inline int GameGearNumber(int gear) {
  return gear == kGearReverse ? 0 : gear == kGearNeutral ? 9 : gear;
}
// Whether the game takes from > to for an upshift (sub_8250B318 compares the numbers).
inline bool GameUpshift(int from, int to) { return GameGearNumber(to) > GameGearNumber(from); }

// The lever on its way through neutral (#468).
struct LeverHold {
  int gear = kGearNeutral;      // the gear reported for the lever
  double neutral_since = -1.0;  // when the lever left that gear for neutral; -1: it has not
};

// The lever's gear as the game should see it: neutral for less than `hold_ms` after a gear is still
// that gear. `lever` is LeverGear's; now_ms a steady clock in milliseconds. hold_ms 0: as it is.
inline int LeverThroughNeutral(LeverHold& h, int lever, double now_ms, double hold_ms) {
  if (lever != kGearNeutral || hold_ms <= 0.0) {
    h = LeverHold{lever, -1.0};
    return lever;
  }
  if (h.gear == kGearNeutral) {
    return kGearNeutral;
  }
  if (h.neutral_since < 0.0) {
    h.neutral_since = now_ms;
  }
  if (now_ms - h.neutral_since < hold_ms) {
    return h.gear;
  }
  h = LeverHold{};
  return kGearNeutral;
}

// A clutch-rejected slot is not a gear to carry through neutral. Otherwise
// pressing the clutch after leaving that slot would engage it with the lever
// already in neutral.
inline int EngageLeverGear(LeverHold& h, int lever, int engaged, double now_ms,
                           double hold_ms, bool need_clutch, float clutch, float threshold) {
  const int filtered = LeverThroughNeutral(h, lever, now_ms, hold_ms);
  const int result = EngageGear(filtered, engaged, need_clutch, clutch, threshold);
  if (result != filtered) {
    h = LeverHold{result, -1.0};
  }
  return result;
}

struct GearRequest {
  bool started = false;
  int engaged = kGearNeutral;      // the engaged gear last seen
  int request = kGearNoRequest;    // what is asked for now
  double since_ms = 0.0;           // when it was asked for
  int reads = 0;                   // states the game has read since
};

// thumb_rx for this state. engaged: EngageGear's gear now; now_ms: a steady clock in
// milliseconds; game_read: the game reads this state (not a keystroke poll); follow_slot: the
// engaged gear all the time (wheel_shifter_follow_slot).
inline int GearToReport(GearRequest& r, int engaged, double now_ms, bool game_read,
                        bool follow_slot) {
  if (follow_slot || !r.started) {
    // Switching to the edge mode later starts from where the lever is, without a request.
    r = GearRequest{};
    r.started = true;
    r.engaged = engaged;
    return follow_slot ? engaged : kGearNoRequest;
  }
  if (engaged != r.engaged) {
    r.engaged = engaged;
    r.request = engaged;
    r.since_ms = now_ms;
    r.reads = 0;
  }
  if (r.request == kGearNoRequest) {
    return kGearNoRequest;
  }
  const int out = r.request;
  if (game_read && ++r.reads >= kGearRequestReads && now_ms - r.since_ms >= kGearRequestMs) {
    r.request = kGearNoRequest;
  }
  return out;
}

}  // namespace rex::input::wheel
