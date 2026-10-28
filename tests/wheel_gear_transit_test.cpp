#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/input/wheel/wheel_gear_request.h"
#include <cassert>
using namespace rex::input::wheel;

int main() {
  LeverHold hold;
  int gear = 2;
  auto poll = [&](int lever, double ms, float clutch) {
    gear = EngageLeverGear(hold, lever, gear, ms, 400, true, clutch, .5f);
    return gear;
  };
  assert(poll(2, 0, 0) == 2);
  // Rejected third gear must not become engaged when the lever is neutral.
  assert(poll(3, 10, 0) == kGearNeutral);
  assert(poll(kGearNeutral, 20, 1) == kGearNeutral);
  assert(poll(kGearNeutral, 100, 1) == kGearNeutral);
  // The actual slot can still engage after the clutch is pressed.
  assert(poll(3, 110, 1) == 3);
  assert(poll(kGearNeutral, 120, 1) == 3);
  assert(poll(4, 250, 1) == 4);
  // A long neutral dwell eventually requests neutral.
  assert(poll(kGearNeutral, 300, 1) == 4);
  assert(poll(kGearNeutral, 700, 1) == kGearNeutral);
  // Disabling the transit hold preserves immediate neutral.
  assert(EngageLeverGear(hold, 2, gear, 800, 0, true, 1, .5f) == 2);
  assert(EngageLeverGear(hold, kGearNeutral, 2, 810, 0, true, 1, .5f) == kGearNeutral);
}
