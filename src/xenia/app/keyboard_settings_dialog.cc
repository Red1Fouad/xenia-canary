/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/keyboard_settings_dialog.h"

#include "third_party/imgui/imgui.h"

#include "xenia/app/emulator_window.h"
#include "xenia/config.h"

namespace xe {
namespace app {

KeyboardSettingsDialog::KeyboardSettingsDialog(ui::ImGuiDrawer* imgui_drawer,
                                               EmulatorWindow& emulator_window)
    : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {
  RefreshBindings();
}

void KeyboardSettingsDialog::RefreshBindings() {
  bindings_ = hid::keyboard::KeyboardBindings();
}

void KeyboardSettingsDialog::ApplyBinding(int index, const std::string& value) {
  if (index < 0 || index >= static_cast<int>(bindings_.size())) {
    return;
  }
  hid::keyboard::SetKeyboardBindingValue(bindings_[index].name, value);
  RefreshBindings();
  config::SaveConfig();
}

bool KeyboardSettingsDialog::OnCaptureKey(ui::KeyEvent& e) {
  if (capture_index_ < 0) {
    return false;
  }

  // Escape cancels capture.
  if (e.virtual_key() == ui::VirtualKey::kEscape) {
    capture_index_ = -1;
    return true;
  }

  const std::string token =
      hid::keyboard::KeyboardBindingToken(e.virtual_key());
  if (!token.empty()) {
    ApplyBinding(capture_index_, token);
  }
  capture_index_ = -1;
  return true;
}

void KeyboardSettingsDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.90f);
  bool dialog_open = true;
  if (!ImGui::Begin(
          "Keyboard bindings", &dialog_open,
          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  if (!dialog_open) {
    Close();
    ImGui::End();
    emulator_window_.ToggleKeyboardSettingsDialog();
    return;
  }

  int mode = hid::keyboard::KeyboardModeValue();
  if (ImGui::Combo("Mode", &mode, "Disabled\0Enabled\0Passthrough\0")) {
    hid::keyboard::SetKeyboardModeValue(mode);
    config::SaveConfig();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Enabled: the keyboard acts as a controller.\n"
        "Passthrough: raw keystrokes are forwarded to the game and require a "
        "controller to be connected.");
  }

  int slot = hid::keyboard::KeyboardUserIndexValue();
  if (ImGui::SliderInt("Slot", &slot, 0, 3)) {
    hid::keyboard::SetKeyboardUserIndexValue(slot);
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    config::SaveConfig();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(forced to 0 during netplay)");

  ImGui::Separator();
  ImGui::TextUnformatted("Click Set, then press the key you want to use.");
  ImGui::TextDisabled("Escape cancels. Clear removes the binding.");

  if (ImGui::BeginTable("##bindings", 3,
                        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthFixed, 180.0f);
    ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthFixed,
                            110.0f);
    ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (int i = 0; i < static_cast<int>(bindings_.size()); ++i) {
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(bindings_[i].description.c_str());

      ImGui::TableSetColumnIndex(1);
      if (i == capture_index_) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Press a key...");
      } else if (bindings_[i].value.empty()) {
        ImGui::TextDisabled("(none)");
      } else {
        ImGui::TextUnformatted(bindings_[i].value.c_str());
      }

      ImGui::TableSetColumnIndex(2);
      ImGui::PushID(i);
      if (i == capture_index_) {
        if (ImGui::Button("Cancel")) {
          capture_index_ = -1;
        }
      } else {
        if (ImGui::Button("Set")) {
          capture_index_ = i;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
          ApplyBinding(i, "");
        }
        ImGui::SameLine();
        if (ImGui::Button("Default")) {
          hid::keyboard::ResetKeyboardBindingValue(bindings_[i].name);
          RefreshBindings();
          config::SaveConfig();
        }
      }
      ImGui::PopID();
    }

    ImGui::EndTable();
  }

  ImGui::Separator();
  ImGui::TextDisabled("Changes are saved to the config file automatically.");

  ImGui::End();
}

}  // namespace app
}  // namespace xe
