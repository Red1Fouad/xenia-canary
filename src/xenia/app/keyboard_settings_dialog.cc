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

namespace {

// Pad canvas, ImGui pixels. Everything below is placed inside this box.
constexpr float kPadWidth = 560.0f;
constexpr float kPadHeight = 352.0f;

constexpr float kCapWidth = 48.0f;
constexpr float kCapHeight = 22.0f;
constexpr float kCapGapX = 54.0f;
constexpr float kCapGapY = 26.0f;

ImVec4 Shade(const ImVec4& color, float factor) {
  auto clamp = [](float v) { return v > 1.0f ? 1.0f : v; };
  return ImVec4(clamp(color.x * factor), clamp(color.y * factor),
                clamp(color.z * factor), color.w);
}

}  // namespace

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
  ImGui::SetNextWindowBgAlpha(0.94f);
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
  ImGui::SetNextItemWidth(150.0f);
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
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120.0f);
  if (ImGui::SliderInt("Slot", &slot, 0, 3)) {
    hid::keyboard::SetKeyboardUserIndexValue(slot);
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    config::SaveConfig();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Controller port the keyboard emulates. Forced to 0\n"
                      "while a netplay link is active.");
  }

  ImGui::Separator();

  // Binding lookup by cvar name, so the layout below doesn't depend on order.
  auto index_of = [this](const char* name) {
    for (int i = 0; i < static_cast<int>(bindings_.size()); ++i) {
      if (bindings_[i].name == name) {
        return i;
      }
    }
    return -1;
  };

  const ImVec4 kNeutral(0.25f, 0.26f, 0.29f, 1.0f);
  const ImVec4 kDimText(0.62f, 0.63f, 0.66f, 1.0f);
  const ImVec4 kCapture(0.86f, 0.66f, 0.14f, 1.0f);

  ImGui::BeginChild("##pad", ImVec2(kPadWidth, kPadHeight), false,
                    ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
  const ImVec2 base = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();

  auto at = [&base](float x, float y) {
    return ImVec2(base.x + x, base.y + y);
  };

  // Controller silhouette: body plus two grips, filled edge first so the
  // darker shape on top leaves a border.
  const ImU32 edge = IM_COL32(82, 86, 94, 255);
  const ImU32 shell = IM_COL32(30, 32, 36, 255);
  draw->AddRectFilled(at(24, 12), at(536, 232), edge, 96.0f);
  draw->AddCircleFilled(at(130, 272), 78.0f, edge);
  draw->AddCircleFilled(at(430, 272), 78.0f, edge);
  draw->AddRectFilled(at(28, 16), at(532, 228), shell, 92.0f);
  draw->AddCircleFilled(at(130, 272), 74.0f, shell);
  draw->AddCircleFilled(at(430, 272), 74.0f, shell);

  // Centred over the control it names.
  auto caption = [&](float cx, float y, const char* text) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::SetCursorScreenPos(at(cx - size.x * 0.5f, y));
    ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
  };

  // One key cap: current key inside, tooltip for what it drives.
  auto cap = [&](int index, float x, float y, float width, float height,
                 const ImVec4& tint) {
    if (index < 0) {
      return;
    }
    const bool capturing = index == capture_index_;
    const std::string& value = bindings_[index].value;
    const std::string text = capturing ? "..." : (value.empty() ? "-" : value);
    const ImVec4 fill = capturing ? kCapture : tint;

    ImGui::SetCursorScreenPos(at(x, y));
    ImGui::PushID(index);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, height * 0.42f);
    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Shade(fill, 1.45f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Shade(fill, 0.78f));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          value.empty() && !capturing ? kDimText
                                                      : ImVec4(1, 1, 1, 1));
    if (ImGui::Button(text.c_str(), ImVec2(width, height))) {
      capture_index_ = capturing ? -1 : index;
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      ApplyBinding(index, "");
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s  (%s)\n%s",
                        bindings_[index].description.c_str(),
                        bindings_[index].name.c_str(),
                        "Left-click to set, right-click to clear.");
    }
    ImGui::PopID();
  };

  // Cells laid out around a centre, like the pad they belong to.
  auto cell = [&](float cx, float cy, int col, int row) {
    return ImVec2(cx + col * kCapGapX - kCapWidth * 0.5f,
                  cy + row * kCapGapY - kCapHeight * 0.5f);
  };

  // Shoulders, guide and menu buttons along the top.
  const ImVec4 kTop = ImVec4(0.31f, 0.32f, 0.36f, 1.0f);
  caption(72.0f, 18.0f, "LT");
  caption(142.0f, 18.0f, "LB");
  caption(280.0f, 18.0f, "GUIDE");
  caption(418.0f, 18.0f, "RB");
  caption(488.0f, 18.0f, "RT");
  cap(index_of("keybind_left_trigger"), 40.0f, 32.0f, 64.0f, 22.0f, kTop);
  cap(index_of("keybind_left_shoulder"), 110.0f, 32.0f, 64.0f, 22.0f, kTop);
  cap(index_of("keybind_guide"), 252.0f, 32.0f, 56.0f, 22.0f,
      ImVec4(0.44f, 0.46f, 0.50f, 1.0f));
  cap(index_of("keybind_right_shoulder"), 386.0f, 32.0f, 64.0f, 22.0f, kTop);
  cap(index_of("keybind_right_trigger"), 456.0f, 32.0f, 64.0f, 22.0f, kTop);

  caption(220.0f, 58.0f, "BACK");
  caption(340.0f, 58.0f, "START");
  cap(index_of("keybind_back"), 196.0f, 72.0f, 48.0f, 22.0f, kTop);
  cap(index_of("keybind_start"), 316.0f, 72.0f, 48.0f, 22.0f, kTop);

  // Left stick and d-pad on the left half, face buttons and right stick on
  // the right half.
  const ImVec4 kKey = ImVec4(0.27f, 0.29f, 0.33f, 1.0f);
  caption(130.0f, 97.0f, "LEFT STICK");
  caption(130.0f, 223.0f, "D-PAD");
  caption(430.0f, 223.0f, "RIGHT STICK");

  const float lsx = 130.0f, lsy = 150.0f;
  ImVec2 ls;
  ls = cell(lsx, lsy, 0, -1);
  cap(index_of("keybind_left_thumb_up"), ls.x, ls.y, kCapWidth, kCapHeight,
      kKey);
  ls = cell(lsx, lsy, -1, 0);
  cap(index_of("keybind_left_thumb_left"), ls.x, ls.y, kCapWidth, kCapHeight,
      kKey);
  ls = cell(lsx, lsy, 0, 0);
  cap(index_of("keybind_left_thumb"), ls.x, ls.y, kCapWidth, kCapHeight, kKey);
  ls = cell(lsx, lsy, 1, 0);
  cap(index_of("keybind_left_thumb_right"), ls.x, ls.y, kCapWidth, kCapHeight,
      kKey);
  ls = cell(lsx, lsy, 0, 1);
  cap(index_of("keybind_left_thumb_down"), ls.x, ls.y, kCapWidth, kCapHeight,
      kKey);

  const float dpx = 130.0f, dpy = 276.0f;
  ImVec2 dp = cell(dpx, dpy, 0, -1);
  cap(index_of("keybind_dpad_up"), dp.x, dp.y, kCapWidth, kCapHeight, kKey);
  dp = cell(dpx, dpy, -1, 0);
  cap(index_of("keybind_dpad_left"), dp.x, dp.y, kCapWidth, kCapHeight, kKey);
  dp = cell(dpx, dpy, 1, 0);
  cap(index_of("keybind_dpad_right"), dp.x, dp.y, kCapWidth, kCapHeight, kKey);
  dp = cell(dpx, dpy, 0, 1);
  cap(index_of("keybind_dpad_down"), dp.x, dp.y, kCapWidth, kCapHeight, kKey);

  const ImVec4 kA(0.16f, 0.50f, 0.22f, 1.0f);
  const ImVec4 kB(0.60f, 0.16f, 0.15f, 1.0f);
  const ImVec4 kX(0.16f, 0.31f, 0.60f, 1.0f);
  const ImVec4 kY(0.70f, 0.58f, 0.10f, 1.0f);

  const float fbx = 430.0f, fby = 150.0f;
  ImVec2 fb = cell(fbx, fby, 0, -1);
  cap(index_of("keybind_y"), fb.x, fb.y, kCapWidth, kCapHeight, kY);
  fb = cell(fbx, fby, -1, 0);
  cap(index_of("keybind_x"), fb.x, fb.y, kCapWidth, kCapHeight, kX);
  fb = cell(fbx, fby, 1, 0);
  cap(index_of("keybind_b"), fb.x, fb.y, kCapWidth, kCapHeight, kB);
  fb = cell(fbx, fby, 0, 1);
  cap(index_of("keybind_a"), fb.x, fb.y, kCapWidth, kCapHeight, kA);

  const float rsx = 430.0f, rsy = 276.0f;
  ImVec2 rs = cell(rsx, rsy, 0, -1);
  cap(index_of("keybind_right_thumb_up"), rs.x, rs.y, kCapWidth, kCapHeight,
      kKey);
  rs = cell(rsx, rsy, -1, 0);
  cap(index_of("keybind_right_thumb_left"), rs.x, rs.y, kCapWidth, kCapHeight,
      kKey);
  rs = cell(rsx, rsy, 0, 0);
  cap(index_of("keybind_right_thumb"), rs.x, rs.y, kCapWidth, kCapHeight,
      kKey);
  rs = cell(rsx, rsy, 1, 0);
  cap(index_of("keybind_right_thumb_right"), rs.x, rs.y, kCapWidth, kCapHeight,
      kKey);
  rs = cell(rsx, rsy, 0, 1);
  cap(index_of("keybind_right_thumb_down"), rs.x, rs.y, kCapWidth, kCapHeight,
      kKey);

  ImGui::EndChild();

  ImGui::TextDisabled(
      "Click a cap, then press a key. Right-click a cap to clear it. "
      "Escape cancels.");

  if (ImGui::Button("Reset all to defaults")) {
    for (const auto& binding : bindings_) {
      hid::keyboard::ResetKeyboardBindingValue(binding.name);
    }
    RefreshBindings();
    config::SaveConfig();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("Changes are saved automatically.");

  ImGui::End();
}

}  // namespace app
}  // namespace xe
