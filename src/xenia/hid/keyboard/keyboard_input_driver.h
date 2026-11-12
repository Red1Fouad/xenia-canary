/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_KEYBOARD_KEYBOARD_INPUT_DRIVER_H_
#define XENIA_HID_KEYBOARD_KEYBOARD_INPUT_DRIVER_H_

#include <queue>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/hid/input_driver.h"
#include "xenia/ui/virtual_key.h"

namespace xe {
namespace hid {
namespace keyboard {

enum class KeyboardMode { Disabled, Enabled, Passthrough };

// Keybinding row for host UI.
struct KeyboardBindingInfo {
  // Cvar name, e.g. "keybind_a".
  std::string name;
  // Controller input name, e.g. "A".
  std::string description;
  // Config default, e.g. "0xBA".
  std::string default_value;
  // Current value, e.g. "0xBA" or "^A".
  std::string value;
};

// Full binding table in button order.
std::vector<KeyboardBindingInfo> KeyboardBindings();

// Current binding value ("" if missing).
std::string KeyboardBindingValue(std::string_view name);

// Set binding and apply live; false if cvar missing. Not saved.
bool SetKeyboardBindingValue(std::string_view name, std::string_view value);

// Restore built-in default.
bool ResetKeyboardBindingValue(std::string_view name);

// Virtual key -> cvar token ("A".."Z"/"0".."9" or "0xHH"; "" for 0).
std::string KeyboardBindingToken(ui::VirtualKey key);

// Mode/slot via config vars.
int KeyboardModeValue();
void SetKeyboardModeValue(int mode);
int KeyboardUserIndexValue();
void SetKeyboardUserIndexValue(int index);

// Re-read bindings into live driver.
void ReloadKeyboardBindings();

class KeyboardInputDriver final : public InputDriver {
 public:
  explicit KeyboardInputDriver(xe::ui::Window* window, size_t window_z_order);
  ~KeyboardInputDriver() override;

  uint8_t VirtualKeyToHIDUsage(uint16_t vk) const;

  bool IsPassthroughEnabled();

  bool IsKeyboardForUserEnabled(uint32_t user_index);

  // Rebuild binding table; safe during polling.
  void ReloadBindings();

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;

  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  virtual InputType GetInputType() const override;

 protected:
  class KeyboardWindowInputListener final : public ui::WindowInputListener {
   public:
    explicit KeyboardWindowInputListener(KeyboardInputDriver& driver)
        : driver_(driver) {}

    void OnKeyDown(ui::KeyEvent& e) override;
    void OnKeyUp(ui::KeyEvent& e) override;
    void OnKeyChar(ui::KeyEvent& e) override;

   private:
    KeyboardInputDriver& driver_;
  };

  struct KeyEvent {
    ui::VirtualKey virtual_key = ui::VirtualKey::kNone;
    int repeat_count = 0;
    bool transition = false;  // going up(false) or going down(true)
    bool prev_state = false;  // down(true) or up(false)
    bool shift_pressed = false;
    bool ctrl_pressed = false;
    bool alt_pressed = false;
    bool capital_pressed = false;
    uint32_t unicode = 0;
  };

  struct KeyBinding {
    ui::VirtualKey input_key = ui::VirtualKey::kNone;
    ui::VirtualKey output_key = ui::VirtualKey::kNone;
    bool uppercase = false;
    bool lowercase = false;
    bool is_pressed = false;
  };

  void ParseKeyBinding(ui::VirtualKey virtual_key,
                       const std::string_view description,
                       const std::string_view binding);

  void OnKey(ui::KeyEvent& e, bool is_down);

  void OnChar(ui::KeyEvent& e);

  xe::global_critical_region global_critical_region_;

  std::deque<KeyEvent> key_events_;
  std::vector<KeyBinding> key_bindings_;

  KeyboardWindowInputListener window_input_listener_;

  uint32_t packet_number_ = 1;
};

}  // namespace keyboard
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_WINKEY_WINKEY_INPUT_DRIVER_H_
