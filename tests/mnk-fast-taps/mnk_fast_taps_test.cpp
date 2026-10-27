// Calls the real driver with ordinary window-listener edges, without a window,
// SendInput, guest-memory writes, or a synthetic pad-state provider.
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/cvar.h>
#include <rex/input/mnk/digital_taps.h>
#include <cassert>
#include <cstdio>
#include <thread>
#include <chrono>
using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::mnk::MnkInputDriver;
using rex::ui::VirtualKey;
REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(bool, mnk_passthrough);
REXCVAR_DECLARE(bool, mnk_mouse);
static void Edge(MnkInputDriver& d, VirtualKey key, bool down) {
  rex::ui::KeyEvent e(nullptr, key, 1, false, false, false, false, false);
  if (down)
    d.OnKeyDown(e);
  else
    d.OnKeyUp(e);
}
static X_INPUT_STATE Poll(MnkInputDriver& d) {
  X_INPUT_STATE s{};
  assert(d.GetDeviceState(DeviceId(0x4D4E4B00), &s) == X_ERROR_SUCCESS);
  return s;
}
int main() {
  // Explicit time examples cover a title that polls many times per frame,
  // a late first observation, stale taps, and a burst without a sample.
  using rex::input::mnk::DigitalTaps;
  constexpr uint32_t start = 1u << 11;
  DigitalTaps taps;
  taps.Observe(start, 0);
  taps.Observe(0, 1);
  assert(taps.Sample(0, 99000000) == start);
  assert(taps.Sample(0, 132999999) == start);
  assert(taps.Sample(0, 133000000) == 0);
  taps.Reset();
  taps.Observe(start, 0);
  taps.Observe(0, 1);
  taps.Observe(start, 90000000);
  taps.Observe(0, 90000001);
  assert(taps.Sample(0, 100000000) == 0);  // Retaps did not refresh the stale edge.
  taps.Reset();
  taps.Observe(1u << 6, 0);
  taps.Observe(0, 1);
  assert(taps.Sample(0, 2) == 0);  // No trigger retention.
  REXCVAR_SET(mnk_mode, true);
  REXCVAR_SET(mnk_passthrough, false);
  REXCVAR_SET(mnk_mouse, false);
  MnkInputDriver d(nullptr, 0);
  assert(d.Setup() == X_STATUS_SUCCESS);
  Edge(d, VirtualKey::kReturn, true);
  Edge(d, VirtualKey::kReturn, false);
  const auto first = Poll(d);
  assert((first.gamepad.buttons & X_INPUT_GAMEPAD_START) != 0 &&
         "a complete tap between polls must reach the sampled controller");
  // Multiple polls within the same simulated frame must see the same tap.
  for (int i = 0; i < 8; ++i)
    assert(Poll(d).gamepad.buttons & X_INPUT_GAMEPAD_START);
  std::this_thread::sleep_for(std::chrono::milliseconds(45));
  assert(!(Poll(d).gamepad.buttons & X_INPUT_GAMEPAD_START));
  Edge(d, VirtualKey::kReturn, true);
  assert(Poll(d).gamepad.buttons & X_INPUT_GAMEPAD_START);
  std::this_thread::sleep_for(std::chrono::milliseconds(45));
  assert(Poll(d).gamepad.buttons & X_INPUT_GAMEPAD_START);  // Real hold persists.
  Edge(d, VirtualKey::kReturn, false);
  assert(!(Poll(d).gamepad.buttons & X_INPUT_GAMEPAD_START));  // Long hold releases immediately.

  Edge(d, VirtualKey::kReturn, true);
  Edge(d, VirtualKey::kReturn, false);
  rex::ui::UISetupEvent focus(nullptr);
  d.OnLostFocus(focus);
  assert(Poll(d).gamepad.buttons == 0);
  d.OnGotFocus(focus);
  assert(Poll(d).gamepad.buttons == 0);
  Edge(d, VirtualKey::kReturn, true);
  Edge(d, VirtualKey::kReturn, false);
  d.OnLostFocus(focus);
  d.OnGotFocus(focus);
  assert(Poll(d).gamepad.buttons == 0);  // No intervening unfocused poll.

  Edge(d, VirtualKey::kReturn, true);
  Edge(d, VirtualKey::kReturn, false);
  REXCVAR_SET(mnk_mode, false);
  std::vector<DeviceInfo> devices;
  d.EnumerateDevices(devices);
  assert(devices.empty());
  REXCVAR_SET(mnk_mode, true);
  assert(Poll(d).gamepad.buttons == 0);

  Edge(d, VirtualKey::kReturn, true);
  Edge(d, VirtualKey::kReturn, false);
  REXCVAR_SET(mnk_passthrough, true);
  X_INPUT_STATE state{};
  assert(d.GetDeviceState(DeviceId(0x4D4E4B00), &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  REXCVAR_SET(mnk_passthrough, false);
  assert(Poll(d).gamepad.buttons == 0);

  bool active = true;
  d.set_is_active_callback([&] { return active; });
  Edge(d, VirtualKey::kReturn, true);
  Edge(d, VirtualKey::kReturn, false);
  active = false;
  assert(Poll(d).gamepad.buttons == 0);
  active = true;
  assert(Poll(d).gamepad.buttons == 0);
  active = false;
  Edge(d, VirtualKey::kReturn, true);
  Edge(d, VirtualKey::kReturn, false);
  active = true;
  assert(Poll(d).gamepad.buttons == 0);  // Overlay tap, with no intervening inactive poll.

  Edge(d, VirtualKey::kShift, true);
  Edge(d, VirtualKey::kUp, true);
  Edge(d, VirtualKey::kUp, false);
  Edge(d, VirtualKey::kShift, false);
  assert(Poll(d).gamepad.buttons & X_INPUT_GAMEPAD_DPAD_UP);
  std::this_thread::sleep_for(std::chrono::milliseconds(45));
  assert(Poll(d).gamepad.buttons == 0);

  Edge(d, VirtualKey::kE, true);
  Edge(d, VirtualKey::kE, false);
  Edge(d, VirtualKey::kW, true);
  Edge(d, VirtualKey::kW, false);
  state = Poll(d);
  assert(state.gamepad.right_trigger == 0 && state.gamepad.thumb_ly == 0);

  Edge(d, VirtualKey::kReturn, true);
  Edge(d, VirtualKey::kReturn, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  assert(Poll(d).gamepad.buttons == 0);  // Loading does not replay a stale action.

  Edge(d, VirtualKey::kReturn, true);
  Edge(d, VirtualKey::kReturn, false);
  assert(Poll(d).gamepad.buttons & X_INPUT_GAMEPAD_START);
  Edge(d, VirtualKey::kReturn, true);
  Edge(d, VirtualKey::kReturn, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(45));
  assert(Poll(d).gamepad.buttons == 0);  // No delayed second automatic action.
  Edge(d, VirtualKey::kReturn, true);
  Edge(d, VirtualKey::kReturn, false);
  assert(Poll(d).gamepad.buttons & X_INPUT_GAMEPAD_START);  // Later independent tap works.

  X_INPUT_KEYSTROKE key{};
  auto stroke = [&] { return d.GetDeviceKeystroke(DeviceId(0x4D4E4B00), 0, &key); };
  while (stroke() == X_ERROR_SUCCESS) {}
  REXCVAR_SET(mnk_passthrough, true);
  Edge(d, VirtualKey::kX, true);
  Edge(d, VirtualKey::kX, false);
  assert(stroke() == X_ERROR_SUCCESS && key.virtual_key == unsigned(VirtualKey::kX));
  assert(key.flags & X_INPUT_KEYSTROKE_KEYDOWN);
  assert(stroke() == X_ERROR_SUCCESS && key.flags & X_INPUT_KEYSTROKE_KEYUP);
  assert(stroke() == X_ERROR_EMPTY);
  REXCVAR_SET(mnk_passthrough, false);
  assert(Poll(d).gamepad.buttons == 0);

  Edge(d, VirtualKey::kReturn, true);
  Edge(d, VirtualKey::kReturn, false);
  assert(stroke() == X_ERROR_SUCCESS && key.flags & X_INPUT_KEYSTROKE_KEYDOWN);
  assert(stroke() == X_ERROR_SUCCESS && key.flags & X_INPUT_KEYSTROKE_KEYUP);
  assert(Poll(d).gamepad.buttons & X_INPUT_GAMEPAD_START);
  assert(stroke() == X_ERROR_EMPTY);  // Sampling adds no extra keystrokes.
  std::puts("actual MnK rapid tap: OK");
}
