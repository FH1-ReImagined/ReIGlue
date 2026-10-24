#pragma once

#include <atomic>
#include <cstdlib>

namespace rex::ui::fh1 {

// FORZA_TEST_MINIMIZED_SURFACE_EMPTY=1 (#221): while the window is minimized, its surface reports
// no area on Linux too, as a minimized Win32 window's client area (0x0) does, and minimizing drops
// the presenter's connection the way a present that fails as outdated does on Windows. So
// minimizing and restoring can be tested on Linux (for example with xdotool and an X11 window).
inline bool MinimizedSurfaceEmptyTest() {
  static const bool enabled = [] {
    const char* value = std::getenv("FORZA_TEST_MINIMIZED_SURFACE_EMPTY");
    return value && *value && *value != '0';
  }();
  return enabled;
}

// Whether the test above currently treats the window as minimized.
inline std::atomic<bool>& MinimizedSurfaceEmptyNow() {
  static std::atomic<bool> minimized{false};
  return minimized;
}

}  // namespace rex::ui::fh1
