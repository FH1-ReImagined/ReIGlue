#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/input/wheel/wheel_rumble_pace.h"
#include <cassert>
#include <cmath>
using namespace rex::input::wheel;

namespace {
bool Near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

// The updates a steady game level gets in `seconds` when the driver polls every `poll` seconds.
int Updates(int rate, double poll, double seconds) {
  const double pace = RumblePaceSeconds(rate);
  RumblePace p;
  int n = 0;
  for (double t = 0; t <= seconds; t += poll) {
    p.Set(t, 0.5f, pace);
    if (p.Due(t, pace)) {
      p.Take(t, pace);
      n++;
    }
  }
  return n;
}
}  // namespace

int main() {
  // The setting is a rate in updates a second, 15 to 120.
  assert(kRumbleRateDefault == 60);
  assert(Near(RumblePaceSeconds(60), 1.0 / 60));
  assert(Near(RumblePaceSeconds(30), 1.0 / 30));
  assert(Near(RumblePaceSeconds(0), 1.0 / 15));
  assert(Near(RumblePaceSeconds(-5), 1.0 / 15));
  assert(Near(RumblePaceSeconds(1000), 1.0 / 120));

  // The schedule is a pace long, with an early tolerance of 6% of it, at every rate.
  for (int rate : {15, 30, 60, 120}) {
    const double pace = RumblePaceSeconds(rate);
    RumblePace p;
    p.Set(10.0, 1.0f, pace);
    assert(!p.Due(10.0, pace));
    assert(!p.Due(10.0 + pace * 0.9, pace));
    assert(p.Due(10.0 + pace * 0.95, pace));
    assert(p.Due(10.0 + pace, pace));
    p.Take(10.0 + pace, pace);
    assert(!p.Due(10.0 + pace * 1.5, pace));
    assert(p.Due(10.0 + pace * 2, pace));
    // A hitch: behind by many paces, the next update is half a pace on, not a burst of updates.
    p.Take(10.0 + pace * 10, pace);
    assert(!p.Due(10.0 + pace * 10.4, pace));
    assert(p.Due(10.0 + pace * 10.5, pace));
  }

  // The level sent is the root mean square of the game's over the interval.
  {
    const double pace = RumblePaceSeconds(60);
    RumblePace p;
    p.Set(0.0, 1.0f, pace);
    p.Set(pace / 2, 0.0f, pace);
    assert(Near(p.Take(pace, pace), std::sqrt(0.5), 1e-6));
    assert(Near(p.Take(2 * pace, pace), 0.0, 1e-6));
  }

  // About `rate` updates a second at any frame rate (polled every 1 ms, as 240 fps and more).
  for (int rate : {15, 30, 60, 90, 120}) {
    assert(std::abs(Updates(rate, 0.001, 10.0) - rate * 10) <= 2);
  }
  // A 60 fps frame cap at 60 updates a second: one per frame; at 30 fps, one per frame too.
  assert(std::abs(Updates(60, 1.0 / 60, 10.0) - 600) <= 2);
  assert(std::abs(Updates(60, 1.0 / 30, 10.0) - 300) <= 2);
  // Reset forgets the interval.
  {
    const double pace = RumblePaceSeconds(60);
    RumblePace p;
    p.Set(0.0, 1.0f, pace);
    p.Reset();
    assert(!p.Due(1.0, pace));
  }
}
