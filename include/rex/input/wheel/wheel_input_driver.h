/**
 * @file        rex/input/wheel/wheel_input_driver.h
 * @brief       Racing wheel input driver - presents any SDL joystick that is a wheel as an
 *              Xbox 360 Wireless Racing Wheel (XINPUT_DEVSUBTYPE_WHEEL), with the clutch
 *              pedal, an H-pattern shifter (wheel_shifter.h) and force feedback: the game's
 *              own wheel effects (wheel_ff.h), or an approximation from the vibration motors
 *              for a game that drives the wheel like a gamepad.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/input/input_driver.h>
#include <rex/input/wheel/wheel_ff.h>

#include <SDL3/SDL.h>

namespace rex::input::wheel {

/// True when the wheel driver (not the gamepad driver) takes this SDL joystick: SDL says it is
/// a wheel, its name looks like one, or wheel_mode is "on" and it has no gamepad mapping.
/// Microsoft devices (the 360 wheel itself, via XInput) stay with the gamepad driver.
bool IsWheelDevice(SDL_JoystickID id);

class WheelInputDriver final : public InputDriver {
 public:
  explicit WheelInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~WheelInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

  /// The game's own force feedback (the kernel's XInputdFF* calls, wheel_ff.h): `change` updates
  /// the effect state; when it returns true, `block` (0: every block) is played again on each
  /// wheel. False when no wheel is open.
  bool GameForceFeedback(const std::function<bool(FfState&)>& change, uint32_t block);
  /// The game's rumble on a wheel whose force feedback the game drives (XInputdFFSetRumble).
  bool GameRumble(uint16_t left_motor, uint16_t right_motor);
  /// Whether the player drives (from the game, ForzaWheelPlayerDriving): the game's forces fade out
  /// while they do not (wheel_ff.h FfFade). Never called: never faded.
  static void SetPlayerDriving(bool driving);

 private:
  struct Wheel;
  void RescanLocked();
  void PumpLocked();
  Wheel* FindLocked(DeviceId id);
  void OpenLocked(SDL_JoystickID jid);
  void CloseLocked(Wheel& w);
  // Steering, pedals, clutch (thumb_ry), shifter gear (thumb_rx) and buttons.
  void ComputeStateLocked(Wheel& w, X_INPUT_GAMEPAD* out);
  void SetupForceFeedbackLocked(Wheel& w);
  void ApplyForceLocked(Wheel& w, uint16_t left_motor, uint16_t right_motor);
  // `game`: the game's own rumble (XInputdFFSetRumble), which fades with its forces.
  void ApplyRumbleLocked(Wheel& w, uint16_t left_motor, uint16_t right_motor, bool game);
  void ApplyGameBlockLocked(Wheel& w, uint32_t block);
  void ApplyAutocenterLocked(Wheel& w);
  // Steps the fade of the game's forces (FfFade) and plays them again when it moved.
  void UpdateFadeLocked();

  std::mutex mutex_;
  std::vector<std::unique_ptr<Wheel>> wheels_;
  bool sdl_ready_ = false;
  bool logged_none_ = false;
  FfState game_ff_;
  bool game_ff_active_ = false;  // the game has defined effects: its forces, not the approximation
  bool game_ff_logged_[kFfMaxBlocks] = {};
  uint16_t game_rumble_[2] = {};  // the game's last XInputdFFSetRumble (left, right)
  FfFade fade_;
  float fade_played_ = 1.0f;  // the fade level the effects were last played at
  std::chrono::steady_clock::time_point fade_time_{};
  uint64_t next_id_ = 1;
  std::chrono::steady_clock::time_point last_scan_{};
  std::chrono::steady_clock::time_point last_pump_{};
};

}  // namespace rex::input::wheel
