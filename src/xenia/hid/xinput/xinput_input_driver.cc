/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/xinput/xinput_input_driver.h"

// Must be included before xinput.h to avoid windows.h conflicts:
#include "xenia/base/platform_win.h"

#include <xinput.h>  // NOLINT(build/include_order)

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/hid/hid_flags.h"

DEFINE_int32(xinput_user_index, -1,
             "Physical XInput controller used for guest slot 0. -1 uses the "
             "guest slot as the physical index. Set to 1 on a second instance "
             "so two instances on one PC read different controllers.",
             "HID");

DEFINE_bool(xinput_focus_only, false,
            "Only read the XInput pad while the emulator window is focused. "
            "XInput itself is process-wide, so without this two instances on "
            "one PC both react to the same controller.",
            "HID");

DECLARE_bool(netplay);

namespace xe {
namespace hid {
namespace xinput {

// Maps a guest slot to the physical XInput index. With no override this is the
// slot itself, as before. With an override only guest slot 0 is connected, and
// it reads the chosen controller; that is what lets two emulator instances on
// one PC split two pads.
//
// Netplay is one local player per instance. Keeping guest slots 1-3
// disconnected matters for Mega Man 10: it picks the first controller that
// reports activity and mounts that user's saves, so a pad that enumerates as
// XInput pad 1 would send the game looking for a profile at user 1, which is
// signed out, and the game sits on the sign-in prompt. Slot 0 therefore
// follows the selected pad, or the first connected pad when none is selected.
int32_t XInputInputDriver::FirstConnectedPhysicalPad() const {
  auto get_state = cvars::guide_button
                       ? (decltype(&XInputGetState))XInputGetStateEx_
                       : (decltype(&XInputGetState))XInputGetState_;
  if (!get_state) {
    return 0;
  }
  for (uint32_t i = 0; i < XUserMaxUserCount; i++) {
    XINPUT_STATE state;
    if (get_state(i, &state) == ERROR_SUCCESS) {
      return static_cast<int32_t>(i);
    }
  }
  return 0;
}

int32_t XInputInputDriver::PhysicalUserIndex(uint32_t user_index) const {
  const int32_t selected = cvars::xinput_user_index;
  const bool has_override = selected >= 0 && selected < XUserMaxUserCount;

  if (cvars::netplay) {
    if (user_index != XUserIndexAny && user_index != 0) {
      return -1;
    }
    return has_override ? selected : FirstConnectedPhysicalPad();
  }

  if (!has_override) {
    return static_cast<int32_t>(user_index);
  }
  if (user_index == XUserIndexAny) {
    return selected;
  }
  return user_index == 0 ? selected : -1;
}

XInputInputDriver::XInputInputDriver(xe::ui::Window* window,
                                     size_t window_z_order)
    : InputDriver(window, window_z_order),
      module_(nullptr),
      XInputGetCapabilities_(nullptr),
      XInputGetState_(nullptr),
      XInputGetStateEx_(nullptr),
      XInputGetKeystroke_(nullptr),
      XInputSetState_(nullptr),
      XInputEnable_(nullptr) {}

XInputInputDriver::~XInputInputDriver() {
  if (module_) {
    FreeLibrary((HMODULE)module_);
    module_ = nullptr;
    XInputGetCapabilities_ = nullptr;
    XInputGetState_ = nullptr;
    XInputGetStateEx_ = nullptr;
    XInputGetKeystroke_ = nullptr;
    XInputSetState_ = nullptr;
    XInputEnable_ = nullptr;
  }
}

X_STATUS XInputInputDriver::Setup() {
  HMODULE module = LoadLibraryW(L"xinput1_4.dll");
  if (!module) {
    return X_STATUS_DLL_NOT_FOUND;
  }

  // Support guide button with XInput using XInputGetStateEx
  // https://source.winehq.org/git/wine.git/?a=commit;h=de3591ca9803add117fbacb8abe9b335e2e44977
  auto const XInputGetStateEx = (LPCSTR)100;

  // Required.
  auto xigc = GetProcAddress(module, "XInputGetCapabilities");
  auto xigs = GetProcAddress(module, "XInputGetState");
  auto xigsEx = GetProcAddress(module, XInputGetStateEx);
  auto xigk = GetProcAddress(module, "XInputGetKeystroke");
  auto xiss = GetProcAddress(module, "XInputSetState");

  // Not required.
  auto xie = GetProcAddress(module, "XInputEnable");

  // Only fail when we don't have the bare essentials;
  if (!xigc || !xigs || !xigk || !xiss) {
    FreeLibrary(module);
    return X_STATUS_PROCEDURE_NOT_FOUND;
  }

  module_ = module;
  XInputGetCapabilities_ = xigc;
  XInputGetState_ = xigs;
  XInputGetStateEx_ = xigsEx;
  XInputGetKeystroke_ = xigk;
  XInputSetState_ = xiss;
  XInputEnable_ = xie;

  return X_STATUS_SUCCESS;
}

constexpr uint64_t SKIP_INVALID_CONTROLLER_TIME = 1100;
static uint64_t last_invalid_time[4];

static DWORD should_skip(uint32_t user_index) {
  // XUserIndexAny (0xFF) and any other out-of-range value must not index the
  // per-slot cache.
  if (user_index >= XUserMaxUserCount) {
    return 0;
  }
  uint64_t time = last_invalid_time[user_index];
  if (time) {
    uint64_t deltatime = xe::Clock::QueryHostUptimeMillis() - time;

    if (deltatime < SKIP_INVALID_CONTROLLER_TIME) {
      return ERROR_DEVICE_NOT_CONNECTED;
    }
    last_invalid_time[user_index] = 0;
  }
  return 0;
}

static void set_skip(uint32_t user_index) {
  if (user_index >= XUserMaxUserCount) {
    return;
  }
  last_invalid_time[user_index] = xe::Clock::QueryHostUptimeMillis();
}

X_RESULT XInputInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                            X_INPUT_CAPABILITIES* out_caps) {
  const int32_t physical_index = PhysicalUserIndex(user_index);
  if (physical_index < 0) {
    return ERROR_DEVICE_NOT_CONNECTED;
  }
  const uint32_t physical = static_cast<uint32_t>(physical_index);

  DWORD skipper = should_skip(physical);
  if (skipper) {
    return skipper;
  }
  XINPUT_CAPABILITIES native_caps;
  auto xigc = (decltype(&XInputGetCapabilities))XInputGetCapabilities_;
  DWORD result =
      xigc(physical, flags & ~X_INPUT_DEVTYPE::XINPUT_DEVTYPE_KEYBOARD,
           &native_caps);
  if (result) {
    if (result == ERROR_DEVICE_NOT_CONNECTED) {
      set_skip(physical);
    }
    return result;
  }

  out_caps->type = native_caps.Type;
  out_caps->sub_type = native_caps.SubType;
  out_caps->flags = native_caps.Flags;
  out_caps->gamepad.buttons = native_caps.Gamepad.wButtons;
  out_caps->gamepad.left_trigger = native_caps.Gamepad.bLeftTrigger;
  out_caps->gamepad.right_trigger = native_caps.Gamepad.bRightTrigger;
  out_caps->gamepad.thumb_lx = native_caps.Gamepad.sThumbLX;
  out_caps->gamepad.thumb_ly = native_caps.Gamepad.sThumbLY;
  out_caps->gamepad.thumb_rx = native_caps.Gamepad.sThumbRX;
  out_caps->gamepad.thumb_ry = native_caps.Gamepad.sThumbRY;
  out_caps->vibration.left_motor_speed = native_caps.Vibration.wLeftMotorSpeed;
  out_caps->vibration.right_motor_speed =
      native_caps.Vibration.wRightMotorSpeed;

  return result;
}

X_RESULT XInputInputDriver::GetState(uint32_t user_index,
                                     X_INPUT_STATE* out_state) {
  const int32_t physical_index = PhysicalUserIndex(user_index);
  if (physical_index < 0) {
    return ERROR_DEVICE_NOT_CONNECTED;
  }
  const uint32_t physical = static_cast<uint32_t>(physical_index);

  // XInput is process-wide and ignores which window has focus, so two
  // instances on one PC would both react to the same controller. When asked,
  // report the pad as connected but idle while unfocused, so the other
  // instance can drive it.
  if (cvars::xinput_focus_only && window() && !window()->HasFocus()) {
    *out_state = X_INPUT_STATE{};
    return ERROR_SUCCESS;
  }

  DWORD skipper = should_skip(physical);
  if (skipper) {
    return skipper;
  }

  // Added padding in case we are using XInputGetStateEx
  struct {
    XINPUT_STATE state;
    unsigned int dwPaddingReserved;
  } native_state;

  // If the guide button is enabled use XInputGetStateEx, otherwise use the
  // default XInputGetState.
  auto xigs = cvars::guide_button ? (decltype(&XInputGetState))XInputGetStateEx_
                                  : (decltype(&XInputGetState))XInputGetState_;

  DWORD result = xigs(physical, &native_state.state);
  if (result) {
    if (result == ERROR_DEVICE_NOT_CONNECTED) {
      set_skip(physical);
    }
    return result;
  }

  out_state->packet_number = native_state.state.dwPacketNumber;
  out_state->gamepad.buttons = native_state.state.Gamepad.wButtons;
  out_state->gamepad.left_trigger = native_state.state.Gamepad.bLeftTrigger;
  out_state->gamepad.right_trigger = native_state.state.Gamepad.bRightTrigger;
  out_state->gamepad.thumb_lx = native_state.state.Gamepad.sThumbLX;
  out_state->gamepad.thumb_ly = native_state.state.Gamepad.sThumbLY;
  out_state->gamepad.thumb_rx = native_state.state.Gamepad.sThumbRX;
  out_state->gamepad.thumb_ry = native_state.state.Gamepad.sThumbRY;

  return result;
}

X_RESULT XInputInputDriver::SetState(uint32_t user_index,
                                     X_INPUT_VIBRATION* vibration) {
  const int32_t physical_index = PhysicalUserIndex(user_index);
  if (physical_index < 0) {
    return ERROR_DEVICE_NOT_CONNECTED;
  }
  const uint32_t physical = static_cast<uint32_t>(physical_index);

  // With focus-only input, an unfocused instance must not drive the shared
  // pad's motors either.
  if (cvars::xinput_focus_only && window() && !window()->HasFocus()) {
    return ERROR_SUCCESS;
  }

  DWORD skipper = should_skip(physical);
  if (skipper) {
    return skipper;
  }
  XINPUT_VIBRATION native_vibration;
  native_vibration.wLeftMotorSpeed = vibration->left_motor_speed;
  native_vibration.wRightMotorSpeed = vibration->right_motor_speed;
  auto xiss = (decltype(&XInputSetState))XInputSetState_;
  DWORD result = xiss(physical, &native_vibration);
  if (result == ERROR_DEVICE_NOT_CONNECTED) {
    set_skip(physical);
  }
  return result;
}

X_RESULT XInputInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                         X_INPUT_KEYSTROKE* out_keystroke) {
  const int32_t physical_index = PhysicalUserIndex(user_index);
  if (physical_index < 0) {
    return ERROR_DEVICE_NOT_CONNECTED;
  }
  const uint32_t physical = static_cast<uint32_t>(physical_index);

  // We may want to filter flags/user_index before sending to native.
  // flags is reserved on desktop.
  DWORD result;

  // XInputGetKeystroke on Windows has a bug where it will return
  // ERROR_SUCCESS (0) even if the device is not connected:
  // https://stackoverflow.com/questions/23669238/xinputgetkeystroke-returning-error-success-while-controller-is-unplugged
  //
  // So we first check if the device is connected via XInputGetCapabilities, so
  // we are not passing back an uninitialized X_INPUT_KEYSTROKE structure.
  // If any user (0xFF) is polled this bug does not occur but GetCapabilities
  // would fail so we need to skip it.
  if (user_index != XUserIndexAny) {
    XINPUT_CAPABILITIES caps;
    auto xigc = (decltype(&XInputGetCapabilities))XInputGetCapabilities_;
    result = xigc(physical, 0, &caps);
    if (result) {
      return result;
    }
  }

  XINPUT_KEYSTROKE native_keystroke;
  auto xigk = (decltype(&XInputGetKeystroke))XInputGetKeystroke_;
  result = xigk(physical, 0, &native_keystroke);
  if (result) {
    return result;
  }

  out_keystroke->virtual_key = native_keystroke.VirtualKey;
  out_keystroke->unicode = native_keystroke.Unicode;
  out_keystroke->flags = native_keystroke.Flags;
  // Report the guest slot the game asked about, not the physical pad that
  // answered, so a remapped second instance still looks like player 1.
  out_keystroke->user_index =
      user_index == XUserIndexAny ? native_keystroke.UserIndex : user_index;
  out_keystroke->hid_code = native_keystroke.HidCode;
  // X_ERROR_EMPTY if no new keys
  // X_ERROR_DEVICE_NOT_CONNECTED if no device
  // X_ERROR_SUCCESS if key
  return result;
}

InputType XInputInputDriver::GetInputType() const {
  return InputType::Controller;
}

}  // namespace xinput
}  // namespace hid
}  // namespace xe
