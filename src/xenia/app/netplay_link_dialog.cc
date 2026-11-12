/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/netplay_link_dialog.h"

#include <string>

#include "third_party/imgui/imgui.h"

#include "xenia/app/emulator_window.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/netplay/net_link.h"
#include "xenia/kernel/netplay/netplay_state.h"

namespace xe {
namespace app {

namespace {

const char* LinkStateName(int state) {
  switch (static_cast<NetState>(state)) {
    case NET_IDLE:
      return "Idle";
    case NET_LISTENING:
      return "Waiting for a player";
    case NET_CONNECTING:
      return "Reaching the host";
    case NET_ESTABLISHED:
      return "Connected";
    case NET_CLOSED:
      return "Other player left";
    case NET_TIMED_OUT:
      return "Timed out";
    case NET_REFUSED:
      return "Handshake refused";
  }
  return "Unknown";
}

const char* LinkRefusalName(int status) {
  switch (static_cast<NetStatus>(status)) {
    case NET_REFUSED_VERSION:
      return "the two programs are different builds";
    case NET_REFUSED_CONTENT:
      return "the two game files differ";
    default:
      return "unknown reason";
  }
}

const char* MappingStateName(kernel::NetplayPortMap::State state) {
  switch (state) {
    case kernel::NetplayPortMap::State::kDisabled:
      return "Disabled";
    case kernel::NetplayPortMap::State::kSearching:
      return "Searching";
    case kernel::NetplayPortMap::State::kMapped:
      return "Mapped";
    case kernel::NetplayPortMap::State::kFailed:
      return "Failed";
  }
  return "Unknown";
}

ImVec4 ColourForLink(int state) {
  switch (static_cast<NetState>(state)) {
    case NET_ESTABLISHED:
      return ImVec4(0.35f, 0.85f, 0.35f, 1.0f);
    case NET_CLOSED:
    case NET_TIMED_OUT:
    case NET_REFUSED:
      return ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
    default:
      return ImVec4(0.95f, 0.80f, 0.30f, 1.0f);
  }
}

}  // namespace

NetplayLinkDialog::NetplayLinkDialog(ui::ImGuiDrawer* imgui_drawer,
                                     EmulatorWindow& emulator_window)
    : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

void NetplayLinkDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(540, 0), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.92f);

  bool dialog_open = true;
  if (!ImGui::Begin(
          "Mega Man 10 netplay", &dialog_open,
          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  if (!dialog_open) {
    ImGui::End();
    Close();
    emulator_window_.ToggleNetplayLinkDialog();
    return;
  }

  kernel::KernelState* kernel_state =
      emulator_window_.emulator()->kernel_state();
  kernel::NetplayState* netplay =
      kernel_state ? kernel_state->netplay() : nullptr;
  if (!netplay) {
    ImGui::TextDisabled("The kernel has not started yet.");
    ImGui::End();
    return;
  }

  const kernel::NetplayState::Status status = netplay->Snapshot();
  const kernel::NetplayPortMap::Status mapping =
      netplay->port_map() ? netplay->port_map()->status()
                          : kernel::NetplayPortMap::Status{};

  auto draw_table = [](const char* name) {
    ImGui::PushID(name);
    ImGui::BeginTable("##values", 2,
                      ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 170.0f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
  };
  auto draw_row = [](const char* label, const std::string& value,
                     const ImVec4* colour = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    if (colour) {
      ImGui::TextColored(*colour, "%s", value.c_str());
    } else {
      ImGui::TextUnformatted(value.c_str());
    }
  };
  auto end_table = []() {
    ImGui::EndTable();
    ImGui::PopID();
  };

  if (!status.enabled) {
    ImGui::TextDisabled(
        "Netplay is off. Launch with --netplay=true --netplay_host=true, or "
        "--netplay_join=<host>:<port>.");
  } else if (!status.active) {
    ImGui::TextDisabled(
        "Netplay is requested but the running title is not Mega Man 10 "
        "(584109EE).");
  }

  {
    const ImVec4 link_colour = ColourForLink(status.link_state);
    draw_table("##link");
    draw_row("Role", status.host ? "Host" : "Joiner");
    if (!status.join_address.empty()) {
      draw_row("Host address", status.join_address);
    }
    draw_row("UDP port", std::to_string(status.port));
    draw_row("Player name", status.player_name);
    draw_row("Link", LinkStateName(status.link_state), &link_colour);
    if (status.link_status != NET_OK) {
      draw_row("Refused", LinkRefusalName(status.link_status));
    }
    if (!status.peer.empty()) {
      draw_row("Peer", status.peer);
    }
    draw_row("Game traffic",
             status.game_alive ? "reaching the game" : "none yet");
    draw_row("RTT", fmt::format("{} ms", status.rtt_ms));
    draw_row("Delay answer",
             fmt::format("{} ms, class {} -> {} frames", status.reported_rtt,
                         status.delay_class, status.delay_frames));
    draw_row("Game input delay", fmt::format("{} frames", status.input_delay));
    draw_row("Lockstep tick", std::to_string(status.lockstep_tick));
    if (status.desync) {
      const ImVec4 desync_colour(0.95f, 0.35f, 0.35f, 1.0f);
      draw_row("Desync check",
               fmt::format("DIFFERENT at tick {}", status.desync_tick),
               &desync_colour);
    } else if (status.desync_tick) {
      draw_row("Desync check",
               fmt::format("in step at tick {}", status.desync_tick));
    } else {
      draw_row("Desync check", "waiting for a stage");
    }
    draw_row("Packets",
             fmt::format("sent {}, got {}, retries {}, bad {}", status.sent,
                         status.received, status.retransmits, status.bad));
    end_table();
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Router port forwarding");

  {
    const ImVec4 mapping_colour =
        mapping.state == kernel::NetplayPortMap::State::kMapped
            ? ImVec4(0.35f, 0.85f, 0.35f, 1.0f)
            : (mapping.state == kernel::NetplayPortMap::State::kFailed
                   ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f)
                   : ImVec4(0.95f, 0.80f, 0.30f, 1.0f));

    draw_table("##mapping");
    draw_row("State", MappingStateName(mapping.state), &mapping_colour);
    if (!mapping.method.empty()) {
      draw_row("Method", mapping.method);
    }
    if (mapping.internal_port) {
      draw_row("UDP port", std::to_string(mapping.internal_port));
    }
    if (!mapping.local_address.empty()) {
      draw_row("This machine", mapping.local_address);
    }
    if (!mapping.external_address.empty()) {
      draw_row("Public address", fmt::format("{}:{}", mapping.external_address,
                                             mapping.external_port));
    }
    if (!mapping.error.empty()) {
      draw_row("Reason", mapping.error);
    }
    end_table();
  }

  ImGui::TextDisabled(
      "Forwarding is optional: start the host with --netplay_upnp=true. The "
      "joiner still needs the host's public IP and port. This does not work "
      "behind carrier-grade NAT.");

  ImGui::Separator();
  ImGui::TextDisabled(
      "Screenshot/exit keys still work. Close this window to return to the "
      "game.");

  ImGui::End();
}

}  // namespace app
}  // namespace xe
