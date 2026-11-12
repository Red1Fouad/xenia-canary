/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_NETPLAY_LINK_DIALOG_H_
#define XENIA_APP_NETPLAY_LINK_DIALOG_H_

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace app {

class EmulatorWindow;

// Read-only link status: role, state/peer, RTT, input delay, desync, port map.
class NetplayLinkDialog final : public ui::ImGuiDialog {
 public:
  NetplayLinkDialog(ui::ImGuiDrawer* imgui_drawer,
                    EmulatorWindow& emulator_window);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  EmulatorWindow& emulator_window_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_NETPLAY_LINK_DIALOG_H_
