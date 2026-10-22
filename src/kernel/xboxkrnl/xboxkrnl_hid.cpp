/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/input/input.h>
#include <rex/input/wheel/wheel_ff.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

u32 HidReadKeys_entry(u32 unk1, u32 unk2, u32 unk3) {
  /* TODO(gibbed):
   * Games check for the following errors:
   *   0xC000009D - translated to 0x48F  - ERROR_DEVICE_NOT_CONNECTED
   *   0x103      - translated to 0x10D2 - ERROR_EMPTY
   * Other errors appear to be ignored?
   *
   * unk1 is 0
   * unk2 is a pointer to &unk3[2], possibly a 6-byte buffer
   * unk3 is a pointer to a 20-byte buffer
   */
  return 0xC000009D;
}

u32 HidGetCapabilities_entry(u32 unk1, u32 caps_ptr) {
  // HidGetCapabilities - ordinal 0x01EA
  // Returns capabilities for HID device (keyboard/mouse)
  // Not supported in rexglue - return unsuccessful
  return X_STATUS_UNSUCCESSFUL;
}

u32 HidGetLastInputTime_entry(mapped_u32 time_ptr) {
  // HidGetLastInputTime - ordinal 0x01F1
  // Returns the last time any HID input was received
  if (time_ptr) {
    *time_ptr = 0;
  }
  return X_STATUS_SUCCESS;
}

u32 HidReadMouseChanges_entry(u32 unk1, u32 unk2) {
  // HidReadMouseChanges - ordinal 0x0273
  // Reads mouse input changes - not supported in rexglue
  return X_STATUS_UNSUCCESSFUL;
}

// Force feedback of the Xbox 360 wheel (ordinals 0x282-0x289), played by the wheel driver
// (rex/input/wheel/wheel_ff.h has what Forza Horizon sends). The device handle is the one
// XamUserGetDeviceContext gave; every wheel plays the effects. With an XOVERLAPPED the call
// completes at once and returns STATUS_PENDING, which the title's XInputFF library turns
// into ERROR_IO_PENDING and then reads the result from the overlapped.
namespace {

constexpr X_STATUS kStatusDeviceNotConnected = static_cast<X_STATUS>(0xC000009DL);

u32 FfDone(bool ok, u32 overlapped) {
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(
        overlapped, ok ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED);
    if (ok) {
      return X_STATUS_PENDING;
    }
  }
  return ok ? X_STATUS_SUCCESS : kStatusDeviceNotConnected;
}

const uint8_t* FfGuest(u32 address) {
  return static_cast<const uint8_t*>(REX_KERNEL_MEMORY()->TranslateVirtual(address));
}

}  // namespace

// info: the second word is how many effects the device plays at once (FH1 needs 5).
u32 XInputdFFGetDeviceInfo_entry(u32 handle, mapped_u32 info) {
  if (!info) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (!rex::input::wheel::GameFfDeviceInfo()) {
    return kStatusDeviceNotConnected;
  }
  info[0] = 0;
  info[1] = rex::input::wheel::kFfMaxBlocks;
  return X_STATUS_SUCCESS;
}

// effects: `count` effects of 0x74 bytes, each naming its block.
u32 XInputdFFSetEffect_entry(u32 handle, u32 effects, u32 count, u32 overlapped) {
  if (!effects || !count || count > rex::input::wheel::kFfMaxBlocks) {
    return X_STATUS_INVALID_PARAMETER;
  }
  bool ok = true;
  for (u32 i = 0; i < count; i++) {
    const auto e = rex::input::wheel::DecodeFfEffect(
        FfGuest(effects + i * 4 * static_cast<u32>(rex::input::wheel::kFfEffectWords)));
    ok = rex::input::wheel::GameFfSetEffect(e) && ok;
  }
  return FfDone(ok, overlapped);
}

// flags: what changed (FH1: 0x400 the type's parameters, 0x100 the gain).
u32 XInputdFFUpdateEffect_entry(u32 handle, u32 effect, u32 flags, u32 overlapped) {
  if (!effect) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const auto e = rex::input::wheel::DecodeFfEffect(FfGuest(effect));
  return FfDone(rex::input::wheel::GameFfUpdateEffect(e, flags), overlapped);
}

// operations: `count` pairs of (block, operation); FH1 starts with 2 and a loop count of 1.
u32 XInputdFFEffectOperation_entry(u32 handle, u32 operations, u32 count, u32 loop_count,
                                   u32 overlapped) {
  if (!operations || !count || count > rex::input::wheel::kFfMaxBlocks) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const uint8_t* ops = FfGuest(operations);
  bool ok = true;
  for (u32 i = 0; i < count; i++) {
    ok = rex::input::wheel::GameFfOperate(rex::input::wheel::FfWord(ops, 2 * i),
                                          rex::input::wheel::FfWord(ops, 2 * i + 1)) &&
         ok;
  }
  return FfDone(ok, overlapped);
}

// command: HID PID device control (FH1 sends 3, stop all effects).
u32 XInputdFFDeviceControl_entry(u32 handle, u32 command, u32 overlapped) {
  return FfDone(rex::input::wheel::GameFfControl(command), overlapped);
}

// FH1 resolves it but never calls it; gain 0-255 assumed.
u32 XInputdFFSetDeviceGain_entry(u32 handle, u32 gain, u32 overlapped) {
  return FfDone(rex::input::wheel::GameFfSetGain(gain), overlapped);
}

u32 XInputdFFCancelIo_entry(u32 handle, u32 unk1, u32 unk2) {
  return X_STATUS_SUCCESS;  // every call completes at once
}

u32 XInputdFFSetRumble_entry(u32 handle, ppc_ptr_t<rex::input::X_INPUT_VIBRATION> vibration,
                             u32 overlapped) {
  if (!vibration) {
    return X_STATUS_INVALID_PARAMETER;
  }
  return FfDone(rex::input::wheel::GameFfSetRumble(vibration->left_motor_speed,
                                                   vibration->right_motor_speed),
                overlapped);
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__HidReadKeys, rex::kernel::xboxkrnl::HidReadKeys_entry)
REX_EXPORT(__imp__HidGetCapabilities, rex::kernel::xboxkrnl::HidGetCapabilities_entry)
REX_EXPORT(__imp__HidGetLastInputTime, rex::kernel::xboxkrnl::HidGetLastInputTime_entry)
REX_EXPORT(__imp__HidReadMouseChanges, rex::kernel::xboxkrnl::HidReadMouseChanges_entry)
REX_EXPORT(__imp__XInputdFFGetDeviceInfo, rex::kernel::xboxkrnl::XInputdFFGetDeviceInfo_entry)
REX_EXPORT(__imp__XInputdFFSetEffect, rex::kernel::xboxkrnl::XInputdFFSetEffect_entry)
REX_EXPORT(__imp__XInputdFFUpdateEffect, rex::kernel::xboxkrnl::XInputdFFUpdateEffect_entry)
REX_EXPORT(__imp__XInputdFFEffectOperation, rex::kernel::xboxkrnl::XInputdFFEffectOperation_entry)
REX_EXPORT(__imp__XInputdFFDeviceControl, rex::kernel::xboxkrnl::XInputdFFDeviceControl_entry)
REX_EXPORT(__imp__XInputdFFSetDeviceGain, rex::kernel::xboxkrnl::XInputdFFSetDeviceGain_entry)
REX_EXPORT(__imp__XInputdFFCancelIo, rex::kernel::xboxkrnl::XInputdFFCancelIo_entry)
REX_EXPORT(__imp__XInputdFFSetRumble, rex::kernel::xboxkrnl::XInputdFFSetRumble_entry)

// XInputd stubs
REX_EXPORT_STUB(__imp__XInputdGetCapabilities);
REX_EXPORT_STUB(__imp__XInputdReadState);
REX_EXPORT_STUB(__imp__XInputdWriteState);
REX_EXPORT_STUB(__imp__XInputdNotify);
REX_EXPORT_STUB(__imp__XInputdRawState);
REX_EXPORT_STUB(__imp__XInputdGetDeviceStats);
REX_EXPORT_STUB(__imp__XInputdResetDevice);
REX_EXPORT_STUB(__imp__XInputdSetRingOfLight);
REX_EXPORT_STUB(__imp__XInputdSetRFPowerMode);
REX_EXPORT_STUB(__imp__XInputdSetRadioFrequency);
REX_EXPORT_STUB(__imp__XInputdPassThroughRFCommand);
REX_EXPORT_STUB(__imp__XInputdPowerDownDevice);
REX_EXPORT_STUB(__imp__XInputdReadTextKeystroke);
REX_EXPORT_STUB(__imp__XInputdSendStayAliveRequest);
REX_EXPORT_STUB(__imp__XInputdGetLastTextInputTime);
REX_EXPORT_STUB(__imp__XInputdSetTextMessengerIndicator);
REX_EXPORT_STUB(__imp__XInputdSetTextDeviceKeyLocks);
REX_EXPORT_STUB(__imp__XInputdGetTextDeviceKeyLocks);
REX_EXPORT_STUB(__imp__XInputdControl);
REX_EXPORT_STUB(__imp__XInputdSetWifiChannel);
REX_EXPORT_STUB(__imp__XInputdGetDevicePid);
REX_EXPORT_STUB(__imp__XInputdGetFailedConnectionOrBind);
REX_EXPORT_STUB(__imp__XInputdSetFailedConnectionOrBindCallback);
REX_EXPORT_STUB(__imp__XInputdSetMinMaxAuthDelay);

// Drv stubs
REX_EXPORT_STUB(__imp__DrvSetSysReqCallback);
REX_EXPORT_STUB(__imp__DrvSetUserBindingCallback);
REX_EXPORT_STUB(__imp__DrvSetContentStorageCallback);
REX_EXPORT_STUB(__imp__DrvSetAutobind);
REX_EXPORT_STUB(__imp__DrvGetContentStorageNotification);
REX_EXPORT_STUB(__imp__DrvXenonButtonPressed);
REX_EXPORT_STUB(__imp__DrvBindToUser);
REX_EXPORT_STUB(__imp__DrvSetDeviceConfigChangeCallback);
REX_EXPORT_STUB(__imp__DrvDeviceConfigChange);
REX_EXPORT_STUB(__imp__DrvSetMicArrayStartCallback);
REX_EXPORT_STUB(__imp__DrvSetAudioLatencyCallback);
