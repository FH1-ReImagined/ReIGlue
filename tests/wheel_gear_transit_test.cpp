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
  // Third gear without the clutch: second stays for kLeverSettleMs (a misread keeps the gear,
  // FH1 #952), then the rejected third is neutral.
  assert(poll(3, 10, 0) == 2);
  assert(poll(3, 10 + kLeverSettleMs, 0) == kGearNeutral);
  // Rejected third gear must not become engaged when the lever is neutral.
  assert(poll(kGearNeutral, 70, 1) == kGearNeutral);
  assert(poll(kGearNeutral, 150, 1) == kGearNeutral);
  // The actual slot can still engage after the clutch is pressed.
  assert(poll(3, 160, 1) == 3);
  assert(poll(kGearNeutral, 170, 1) == 3);
  assert(poll(4, 300, 1) == 4);
  // A long neutral dwell eventually requests neutral.
  assert(poll(kGearNeutral, 350, 1) == 4);
  assert(poll(kGearNeutral, 750, 1) == kGearNeutral);
  // Disabling the transit hold preserves immediate neutral.
  assert(EngageLeverGear(hold, 2, gear, 800, 0, true, 1, .5f) == 2);
  assert(EngageLeverGear(hold, kGearNeutral, 2, 810, 0, true, 1, .5f) == kGearNeutral);

  // FH1 #952: in a gear with the clutch up, a read of no gear or of another gear that is over
  // within kLeverSettleMs keeps the gear, whatever wheel_shifter_neutral_ms is.
  for (double hold_ms : {400.0, 0.0}) {
    LeverHold h;
    double t = 0;
    int g = EngageLeverGear(h, 3, 0, t, hold_ms, true, 1, .5f);
    assert(g == 3);
    g = EngageLeverGear(h, 3, g, t += 100, hold_ms, true, 0, .5f);  // clutch out
    for (int misread : {kGearNeutral, 2, 4, kGearReverse}) {
      g = EngageLeverGear(h, misread, g, t += 100, hold_ms, true, 0, .5f);
      assert(g == 3);
      g = EngageLeverGear(h, 3, g, t += 10, hold_ms, true, 0, .5f);
      assert(g == 3);
      g = EngageLeverGear(h, 3, g, t += 800, hold_ms, true, 0, .5f);
      assert(g == 3);
    }
  }
  for (double hold_ms : {400.0, 0.0}) {
    // A lever left in neutral that now and then reads another gear (near a gate) is neutral after
    // the hold and kLeverSettleMs from when it left the slot: the misreads do not restart the hold.
    LeverHold h;
    double t = 0;
    int g = EngageLeverGear(h, 3, 0, t, hold_ms, true, 1, .5f);
    g = EngageLeverGear(h, 3, g, t += 100, hold_ms, true, 0, .5f);
    const double left = t + 5;
    double neutral_at = -1;
    for (t = left; t < left + 3000; t += 5) {
      const int lever = static_cast<int>(t - left) % 100 == 50 ? 4 : kGearNeutral;
      g = EngageLeverGear(h, lever, g, t, hold_ms, true, 0, .5f);
      if (g == kGearNeutral && neutral_at < 0) {
        neutral_at = t - left;
      }
      assert(g == 3 || g == kGearNeutral);
      assert(neutral_at < 0 || g == kGearNeutral);
    }
    assert(neutral_at >= 0 && neutral_at <= hold_ms + kLeverSettleMs + 5);
    // The clutch then down with the lever in neutral engages nothing.
    assert(EngageLeverGear(h, kGearNeutral, g, t, hold_ms, true, 1, .5f) == kGearNeutral);
  }
  {
    // A lever that misread a gear and then reads neutral carries the engaged gear through neutral,
    // not the misread one: the clutch pressed then does not engage the misread gear.
    LeverHold h;
    int g = EngageLeverGear(h, 1, 0, 0, 400, true, 1, .5f);
    g = EngageLeverGear(h, 1, g, 100, 400, true, 0, .5f);
    g = EngageLeverGear(h, 2, g, 200, 400, true, 0, .5f);
    assert(g == 1);
    g = EngageLeverGear(h, kGearNeutral, g, 220, 400, true, 0, .5f);
    assert(g == 1);
    g = EngageLeverGear(h, kGearNeutral, g, 260, 400, true, 1, .5f);
    assert(g == 1);
    g = EngageLeverGear(h, kGearNeutral, g, 700, 400, true, 1, .5f);
    assert(g == kGearNeutral);
  }
  {
    // A real move to neutral with the clutch up still ends in neutral: after the hold through
    // neutral and kLeverSettleMs (at once with neither), and the gear needs the clutch again.
    LeverHold h;
    int g = EngageLeverGear(h, 3, 0, 0, 0, true, 1, .5f);
    g = EngageLeverGear(h, kGearNeutral, g, 100, 0, true, 0, .5f);
    assert(g == 3);
    g = EngageLeverGear(h, kGearNeutral, g, 100 + kLeverSettleMs, 0, true, 0, .5f);
    assert(g == kGearNeutral);
    g = EngageLeverGear(h, 3, g, 300, 0, true, 0, .5f);
    assert(g == kGearNeutral);
    // Without wheel_shifter_clutch neutral is at once, as before.
    LeverHold h2;
    g = EngageLeverGear(h2, 3, 0, 0, 0, false, 0, .5f);
    assert(EngageLeverGear(h2, kGearNeutral, g, 10, 0, false, 0, .5f) == kGearNeutral);
  }
}
