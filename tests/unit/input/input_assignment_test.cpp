#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/input_system.h>

// The X_STATUS_* / X_ERROR_* macros cast through these unqualified, so
// FakeDriver cannot match the base class signatures without them.
using rex::X_RESULT;
using rex::X_STATUS;

using rex::input::DeviceAssignment;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::InputDriver;
using rex::input::InputSystem;
using rex::input::SharedAssignment;
using rex::input::SlotAssignment;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_B;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

namespace {

DeviceInfo Pad(uint64_t id, uint32_t ordinal) {
  DeviceInfo info;
  info.id = static_cast<DeviceId>(id);
  info.ordinal = ordinal;
  return info;
}

std::vector<DeviceId> For(const DeviceAssignment& a, uint32_t user) {
  std::vector<DeviceId> out;
  a.DevicesForUser(user, out);
  return out;
}

/// Exercises assignment with no SDL, window, or OS behind it.
class FakeDriver final : public InputDriver {
 public:
  FakeDriver() : InputDriver(nullptr, 0) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void Add(uint64_t id, bool synthetic = false,
           uint8_t subtype = rex::input::XINPUT_DEVSUBTYPE_GAMEPAD) {
    DeviceInfo info;
    info.id = static_cast<DeviceId>(id);
    info.synthetic = synthetic;
    info.subtype = subtype;
    devices_.push_back(info);
    states_[static_cast<DeviceId>(id)] = {};
  }

  void SetSticks(uint64_t id, int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    auto& g = states_[static_cast<DeviceId>(id)].gamepad;
    g.thumb_lx = lx;
    g.thumb_ly = ly;
    g.thumb_rx = rx;
    g.thumb_ry = ry;
  }

  // The stick ranges the capabilities report (the deadzones scale against them).
  void SetCapsSticks(uint64_t id, int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    caps_sticks_[static_cast<DeviceId>(id)] = {lx, ly, rx, ry};
  }

  void Remove(uint64_t id) {
    std::erase_if(devices_, [&](const DeviceInfo& d) { return d.id == static_cast<DeviceId>(id); });
    states_.erase(static_cast<DeviceId>(id));
  }

  void SetButtons(uint64_t id, uint16_t buttons) {
    states_[static_cast<DeviceId>(id)].gamepad.buttons = buttons;
  }

  void QueueKeystroke(uint64_t id, uint16_t vk) {
    X_INPUT_KEYSTROKE ks = {};
    ks.virtual_key = vk;
    keystrokes_[static_cast<DeviceId>(id)] = ks;
  }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override { out = devices_; }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    auto it = states_.find(id);
    if (it == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out_state = it->second;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out) override {
    if (states_.find(id) == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out = {};
    // Stamp the id so the test can tell which device answered.
    out->sub_type = static_cast<uint8_t>(id);
    if (auto it = caps_sticks_.find(id); it != caps_sticks_.end()) {
      out->gamepad.thumb_lx = it->second[0];
      out->gamepad.thumb_ly = it->second[1];
      out->gamepad.thumb_rx = it->second[2];
      out->gamepad.thumb_ry = it->second[3];
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return states_.count(id) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE* out) override {
    if (states_.find(id) == states_.end()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    auto it = keystrokes_.find(id);
    if (it == keystrokes_.end()) {
      return X_ERROR_EMPTY;
    }
    *out = it->second;
    return X_ERROR_SUCCESS;
  }

 private:
  std::vector<DeviceInfo> devices_;
  std::map<DeviceId, X_INPUT_STATE> states_;
  std::map<DeviceId, X_INPUT_KEYSTROKE> keystrokes_;
  std::map<DeviceId, std::array<int16_t, 4>> caps_sticks_;
};

std::unique_ptr<InputSystem> SharedSystem(FakeDriver** out_driver) {
  auto driver = std::make_unique<FakeDriver>();
  *out_driver = driver.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(driver));
  system->SetDeviceAssignment(std::make_unique<SharedAssignment>());
  return system;
}

}  // namespace

TEST_CASE("SharedAssignment puts every device on user 0", "[input]") {
  SharedAssignment shared;
  shared.OnDevicesChanged({Pad(10, 0), Pad(11, 1)});

  REQUIRE(For(shared, 0) ==
          std::vector<DeviceId>{static_cast<DeviceId>(10), static_cast<DeviceId>(11)});
  REQUIRE(For(shared, 1).empty());
}

TEST_CASE("SlotAssignment does not renumber survivors when a pad is unplugged", "[input]") {
  SlotAssignment slots;
  slots.OnDevicesChanged({Pad(10, 0), Pad(11, 1)});
  slots.OnDevicesChanged({Pad(11, 1)});

  REQUIRE(For(slots, 0).empty());
  REQUIRE(For(slots, 1) == std::vector<DeviceId>{static_cast<DeviceId>(11)});
}

TEST_CASE("Both controllers reach one guest user", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->SetButtons(1, X_INPUT_GAMEPAD_A);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B));
}

TEST_CASE("Unplugging one controller leaves the other working", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);

  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  driver->Remove(1);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
}

TEST_CASE("Capabilities follow the controller in hand", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);

  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  X_INPUT_CAPABILITIES caps = {};
  REQUIRE(system->GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  REQUIRE(caps.sub_type == 2);
}

TEST_CASE("A keystroke from the second controller reaches the guest", "[input]") {
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->Add(2);
  driver->QueueKeystroke(2, 0x5800);

  X_INPUT_KEYSTROKE ks = {};
  REQUIRE(system->GetKeystroke(0, 0, &ks) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(ks.virtual_key) == 0x5800);
}

TEST_CASE("A reconnected pad comes back to guest user 0", "[input]") {
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* driver = owned.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(owned));
  system->SetDeviceAssignment(std::make_unique<SlotAssignment>());

  driver->Add(1);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  // Replugging arrives as a new device id, not as the old one coming back.
  driver->Remove(1);
  driver->Add(2);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
}

TEST_CASE("A synthetic device does not push the first pad off guest user 0", "[input]") {
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* driver = owned.get();
  auto system = std::make_unique<InputSystem>(nullptr);
  system->AddDriver(std::move(owned));
  system->SetDeviceAssignment(std::make_unique<SlotAssignment>());

  // The NOP stand-in is always present and enumerates before any pad connects.
  driver->Add(1, true);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);

  driver->Add(2);
  driver->SetButtons(2, X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(0, &state) == X_ERROR_SUCCESS);
  REQUIRE(static_cast<uint16_t>(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
  REQUIRE(system->GetState(1, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
}

TEST_CASE("The stick deadzones leave a racing wheel's gear and clutch alone (FH1 #952)",
          "[input]") {
  // The wheel driver's state: steering on the left stick X, the H-pattern gear on the right stick
  // X (-1 reverse, 1 to 7) and the clutch on the right stick Y; its capabilities report the
  // number of forward gears in thumb_rx's high byte and no right stick Y range.
  struct Deadzones {
    Deadzones(double left, double right) {
      REXCVAR_SET(left_stick_deadzone_percentage, left);
      REXCVAR_SET(right_stick_deadzone_percentage, right);
    }
    ~Deadzones() {
      REXCVAR_SET(left_stick_deadzone_percentage, 0.0);
      REXCVAR_SET(right_stick_deadzone_percentage, 0.0);
    }
  } deadzones(0.1, 0.1);

  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1, false, rex::input::XINPUT_DEVSUBTYPE_WHEEL);
  driver->SetCapsSticks(1, 0x7FFF, 0, 6 << 8, 0);

  X_INPUT_STATE state = {};
  for (int16_t gear : {int16_t(1), int16_t(3), int16_t(6), int16_t(-1)}) {
    driver->SetSticks(1, 200, 0, gear, 0);  // clutch up, steering a hair off centre
    REQUIRE(system->GetStateForUI(0, &state) == X_ERROR_SUCCESS);
    CHECK(state.gamepad.thumb_rx == gear);
    CHECK(state.gamepad.thumb_ry == 0);
    CHECK(state.gamepad.thumb_lx == 200);
  }
  driver->SetSticks(1, 0, 0, 3, 300);  // the clutch just touched
  REQUIRE(system->GetStateForUI(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_rx == 3);
  CHECK(state.gamepad.thumb_ry == 300);

  // A pad next to the wheel still has its deadzone, and the wheel's gear survives the merge.
  driver->Add(2);
  driver->SetCapsSticks(2, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF);
  driver->SetSticks(2, 0, 0, 100, 0);  // a drifting right stick, inside the deadzone
  driver->SetSticks(1, 0, 0, 3, 0);
  REQUIRE(system->GetStateForUI(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_rx == 3);
  driver->SetSticks(1, 0, 0, 0, 0);
  REQUIRE(system->GetStateForUI(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_rx == 0);
}

TEST_CASE("The stick deadzones still apply to a pad", "[input]") {
  REXCVAR_SET(right_stick_deadzone_percentage, 0.1);
  FakeDriver* driver = nullptr;
  auto system = SharedSystem(&driver);
  driver->Add(1);
  driver->SetCapsSticks(1, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF);
  driver->SetSticks(1, 0, 0, 1000, 0);
  X_INPUT_STATE state = {};
  REQUIRE(system->GetStateForUI(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_rx == 0);
  driver->SetSticks(1, 0, 0, 20000, 0);
  REQUIRE(system->GetStateForUI(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.thumb_rx == 20000);
  REXCVAR_SET(right_stick_deadzone_percentage, 0.0);
}
