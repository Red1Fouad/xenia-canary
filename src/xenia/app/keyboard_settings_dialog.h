/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_KEYBOARD_SETTINGS_DIALOG_H_
#define XENIA_APP_KEYBOARD_SETTINGS_DIALOG_H_

#include <string>
#include <vector>

#include "xenia/hid/keyboard/keyboard_input_driver.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace app {

class EmulatorWindow;

// Keyboard HID driver settings: mode, slot, key bindings. Applies live.
class KeyboardSettingsDialog final : public ui::ImGuiDialog {
 public:
  KeyboardSettingsDialog(ui::ImGuiDrawer* imgui_drawer,
                         EmulatorWindow& emulator_window);

  // Key capture hook; true if consumed.
  bool OnCaptureKey(ui::KeyEvent& e);

  bool IsCapturing() const { return capture_index_ >= 0; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void RefreshBindings();
  void ApplyBinding(int index, const std::string& value);

  EmulatorWindow& emulator_window_;

  std::vector<hid::keyboard::KeyboardBindingInfo> bindings_;
  int capture_index_ = -1;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_KEYBOARD_SETTINGS_DIALOG_H_
