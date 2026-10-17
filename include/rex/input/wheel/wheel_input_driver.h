/**
 * @file        rex/input/wheel/wheel_input_driver.h
 * @brief       Racing wheel input driver - presents any SDL joystick that is a wheel as an
 *              Xbox 360 Wireless Racing Wheel (XINPUT_DEVSUBTYPE_WHEEL), with approximate
 *              force feedback from the vibration motors.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/input/input_driver.h>

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

 private:
  struct Wheel;
  void RescanLocked();
  void PumpLocked();
  Wheel* FindLocked(DeviceId id);
  void OpenLocked(SDL_JoystickID jid);
  void CloseLocked(Wheel& w);
  void ComputeStateLocked(Wheel& w, uint16_t* buttons, uint8_t* lt, uint8_t* rt, int16_t* lx);
  void SetupForceFeedbackLocked(Wheel& w);
  void ApplyForceLocked(Wheel& w, float motor);

  std::mutex mutex_;
  std::vector<std::unique_ptr<Wheel>> wheels_;
  bool sdl_ready_ = false;
  bool logged_none_ = false;
  bool logged_caps_ = false;
  uint64_t next_id_ = 1;
  std::chrono::steady_clock::time_point last_scan_{};
  std::chrono::steady_clock::time_point last_pump_{};
};

}  // namespace rex::input::wheel
