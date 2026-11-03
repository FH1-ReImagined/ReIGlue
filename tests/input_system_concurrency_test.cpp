#ifdef NDEBUG
#undef NDEBUG
#endif

#include <rex/input/device_assignment.h>
#include <rex/input/input_driver.h>

#define private public
#include <rex/input/input_system.h>
#undef private

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;

namespace {

struct ConcurrentDriver final : public InputDriver {
  ConcurrentDriver() : InputDriver(nullptr, 0) {}

  mutable std::mutex driver_mutex;
  std::vector<DeviceInfo> devices;

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    std::lock_guard<std::mutex> lock(driver_mutex);
    out = devices;
  }

  X_RESULT GetDeviceState(DeviceId, X_INPUT_STATE* state) override {
    *state = {};
    state->gamepad.buttons = X_INPUT_GAMEPAD_A;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId, uint32_t, X_INPUT_CAPABILITIES* caps) override {
    *caps = {};
    caps->gamepad.thumb_lx = 32767;
    caps->gamepad.thumb_ly = 32767;
    caps->gamepad.thumb_rx = 32767;
    caps->gamepad.thumb_ry = 32767;
    return X_ERROR_SUCCESS;
  }

  std::atomic<int> slow_vibration_ms{0};
  X_RESULT SetDeviceVibration(DeviceId, X_INPUT_VIBRATION*) override {
    int delay = slow_vibration_ms.load();
    if (delay > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceKeystroke(DeviceId, uint32_t, X_INPUT_KEYSTROKE* out_keystroke) override {
    *out_keystroke = {};
    return X_ERROR_SUCCESS;
  }

  void SetDevices(const std::vector<DeviceInfo>& list) {
    std::lock_guard<std::mutex> lock(driver_mutex);
    devices = list;
  }
};

DeviceInfo MakeDevice(DeviceId id, const std::string& name, bool synthetic = false) {
  DeviceInfo info;
  info.id = id;
  info.name = name;
  info.guid = "030000005e040000d102000000000000";
  info.synthetic = synthetic;
  return info;
}

}  // namespace

int main() {
  // Test 1: Concurrency stress between main polling thread, telemetry trigger rumble thread,
  // body rumble thread, and device plug/unplug thread.
  {
    InputSystem input(nullptr);
    auto driver = std::make_unique<ConcurrentDriver>();
    auto* raw_driver = driver.get();

    raw_driver->SetDevices({
        MakeDevice(DeviceId(1), "Xbox Wireless Controller"),
        MakeDevice(DeviceId(2), "Keyboard/Mouse Emulation", true),
    });

    input.AddDriver(std::move(driver));
    input.SetDeviceAssignment(std::make_unique<SlotAssignment>());

    std::atomic<bool> running{true};
    constexpr int kIterations = 10000;

    // Thread 1: Emulator main loop polling guest input.
    std::thread poll_thread([&]() {
      for (int i = 0; i < kIterations; ++i) {
        for (uint32_t u = 0; u < kMaxGuestUsers; ++u) {
          X_INPUT_STATE state{};
          input.GetStateForUI(u, &state);
          X_INPUT_CAPABILITIES caps{};
          input.GetCapabilities(u, 0, &caps);
        }
      }
      running.store(false);
    });

    // Thread 2: Physics / telemetry thread calling SetTriggerRumble.
    std::thread rumble_trigger_thread([&]() {
      uint16_t amp = 0;
      while (running.load()) {
        input.SetTriggerRumble(0x1, amp, amp, 150);
        amp = static_cast<uint16_t>(amp + 100);
      }
    });

    // Thread 3: Body rumble thread.
    std::thread rumble_body_thread([&]() {
      while (running.load()) {
        X_INPUT_VIBRATION vib{1000, 2000};
        input.SetState(0, &vib);
      }
    });

    // Thread 4: Hotplug / device churn thread.
    std::thread hotplug_thread([&]() {
      int step = 0;
      while (running.load()) {
        if (step % 2 == 0) {
          raw_driver->SetDevices({
              MakeDevice(DeviceId(1), "Xbox Wireless Controller"),
              MakeDevice(DeviceId(2), "Keyboard/Mouse Emulation", true),
              MakeDevice(DeviceId(3), "Spare Controller"),
          });
        } else {
          raw_driver->SetDevices({
              MakeDevice(DeviceId(1), "Xbox Wireless Controller"),
              MakeDevice(DeviceId(2), "Keyboard/Mouse Emulation", true),
          });
        }
        X_INPUT_KEYSTROKE ks{};
        input.GetKeystroke(0, 0, &ks);
        ++step;
      }
    });

    poll_thread.join();
    rumble_trigger_thread.join();
    rumble_body_thread.join();
    hotplug_thread.join();
  }

  // Test 2: Unchanged device stability verification.
  // When devices have not changed, RefreshDevices preserves device pointers and avoids heap churn.
  {
    InputSystem input(nullptr);
    auto driver = std::make_unique<ConcurrentDriver>();
    auto* raw_driver = driver.get();

    raw_driver->SetDevices({
        MakeDevice(DeviceId(10), "Primary Pad"),
        MakeDevice(DeviceId(20), "Secondary Pad"),
    });

    input.AddDriver(std::move(driver));
    input.SetDeviceAssignment(std::make_unique<SlotAssignment>());

    X_INPUT_STATE state{};
    assert(input.GetStateForUI(0, &state) == X_ERROR_SUCCESS);

    auto dev10_before = input.DeviceInfoFor(DeviceId(10));
    assert(dev10_before.has_value());
    assert(dev10_before->name == "Primary Pad");
    const auto* devices_data_before = input.devices_.data();
    const size_t devices_size_before = input.devices_.size();

    // Call GetStateForUI and RefreshDevices many times without modifying devices.
    for (int i = 0; i < 1000; ++i) {
      assert(input.GetStateForUI(0, &state) == X_ERROR_SUCCESS);
      auto dev10_after = input.DeviceInfoFor(DeviceId(10));
      assert(dev10_after.has_value());
      assert(dev10_after->id == dev10_before->id);
      assert(dev10_after->name == dev10_before->name);
      assert(input.devices_.data() == devices_data_before);
      assert(input.devices_.size() == devices_size_before);
    }
  }

  // Test 3: Lock not held across driver I/O.
  // DirectInput / force feedback vibration I/O in SetState or SetTriggerRumble must not
  // block the main loop's GetState / GetStateForUI input polling.
  {
    InputSystem input(nullptr);
    auto driver = std::make_unique<ConcurrentDriver>();
    auto* raw_driver = driver.get();

    raw_driver->SetDevices({
        MakeDevice(DeviceId(1), "DirectInput Force Feedback Wheel"),
    });

    input.AddDriver(std::move(driver));
    input.SetDeviceAssignment(std::make_unique<SlotAssignment>());

    // Prime device enumeration
    X_INPUT_STATE state{};
    assert(input.GetStateForUI(0, &state) == X_ERROR_SUCCESS);

    // Make vibration simulate a 60ms DirectInput driver call
    raw_driver->slow_vibration_ms.store(60);

    std::atomic<bool> vibration_in_progress{false};
    std::thread rumble_thread([&]() {
      vibration_in_progress.store(true);
      X_INPUT_VIBRATION vib{30000, 30000};
      input.SetState(0, &vib);
      vibration_in_progress.store(false);
    });

    // Wait until rumble thread enters SetState
    while (!vibration_in_progress.load()) {
      std::this_thread::yield();
    }

    // Call GetStateForUI repeatedly while vibration is actively executing its 60ms driver I/O
    int polls_completed = 0;
    while (vibration_in_progress.load()) {
      X_INPUT_STATE s{};
      assert(input.GetStateForUI(0, &s) == X_ERROR_SUCCESS);
      polls_completed++;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    rumble_thread.join();
    // Verify that the main loop was NOT blocked by the vibration call
    assert(polls_completed >= 5);
  }

  std::printf("input_system_concurrency_test: ok\n");
  return 0;
}
