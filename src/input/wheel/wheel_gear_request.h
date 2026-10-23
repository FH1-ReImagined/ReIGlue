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
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/wheel/wheel_shifter.h>

namespace rex::input::wheel {

constexpr int kGearNoRequest = 10;  // the game's own "no request"; 9 and up read the same
constexpr double kGearRequestMs = 250.0;
constexpr int kGearRequestReads = 4;

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
