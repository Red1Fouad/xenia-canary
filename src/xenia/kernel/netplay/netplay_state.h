/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NETPLAY_NETPLAY_STATE_H_
#define XENIA_KERNEL_NETPLAY_NETPLAY_STATE_H_

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "xenia/kernel/netplay/netplay_portmap.h"

namespace xe {
namespace kernel {

class KernelState;
class NetplayGame;
class NetplaySockets;
class NetplayWorker;

// Mega Man 10 two-player link. Owns the carrier, emulated socket table,
// game session/lockstep state and checked guest memory access.
class NetplayState {
 public:
  explicit NetplayState(KernelState* kernel_state);
  ~NetplayState();

  NetplayState(const NetplayState&) = delete;
  NetplayState& operator=(const NetplayState&) = delete;

  // Requested and title is Mega Man 10.
  bool IsActive() const;

  bool enabled() const { return enabled_; }
  bool IsHost() const { return host_; }
  const std::string& join_address() const { return join_; }
  uint16_t port() const { return port_; }
  const std::string& player_name() const { return name_; }
  bool verbose() const { return verbose_; }

  // Called on module change.
  void Refresh();
  void Shutdown();

  static uint64_t now_ms();

  // Always non-null.
  NetplayWorker* worker() const { return worker_.get(); }
  NetplaySockets* sockets() const { return sockets_.get(); }
  NetplayGame* game() const { return game_.get(); }

  // Host router mapping.
  NetplayPortMap* port_map() const { return port_map_.get(); }

  // Status window copy. Safe from UI thread.
  struct Status {
    bool enabled = false;
    bool active = false;   // title is Mega Man 10
    bool running = false;  // carrier open
    bool host = false;
    bool listening = false;  // host waiting
    std::string join_address;
    uint16_t port = 0;
    std::string player_name;

    int link_state = 0;   // NetState
    int link_status = 0;  // refusal reason
    bool link_up = false;
    bool game_alive = false;
    std::string peer;  // "address:port"

    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t retransmits = 0;
    uint32_t bad = 0;
    uint32_t rtt_ms = 0;

    uint32_t lockstep_tick = 0;  // 0 outside a stage
    uint8_t input_delay = 0;     // frames in use
    uint16_t reported_rtt = 0;   // after netplay_delay
    uint32_t delay_class = 0;
    uint32_t delay_frames = 0;

    bool desync = false;
    uint32_t desync_tick = 0;
  };
  Status Snapshot();

  // Start/stop mapping to match cvar and role.
  void RefreshPortMap();

  // Guards link, socket table and guest memory.
  std::mutex& mutex() { return mutex_; }

  // Checked guest memory. Unmapped reads return 0, writes ignored.
  uint8_t Read8(uint32_t address) const;
  uint16_t Read16(uint32_t address) const;
  uint32_t Read32(uint32_t address) const;
  void Write8(uint32_t address, uint8_t value);
  void Write16(uint32_t address, uint16_t value);
  void Write32(uint32_t address, uint32_t value);
  bool GuestRead(uint32_t address, void* out, uint32_t length) const;
  bool GuestWrite(uint32_t address, const void* in, uint32_t length);

  // XNet addresses handed out by XNetXnAddrToInAddr.
  static constexpr uint32_t kSelfInAddr = 0x7F000001u;
  static constexpr uint32_t kPeerInAddr = 0x0A000002u;

  // Local XNADDR; first 4 bytes fixed (10.0.0.1), rest random per process.
  static constexpr uint32_t kXnAddrSize = 0x24;
  void FillSelfXnAddr(uint8_t out[kXnAddrSize]) const;
  bool IsSelfXnAddr(const uint8_t in[kXnAddrSize]) const;

  // Handshake digest.
  static constexpr uint32_t kDigestSize = 32;
  void FillDigest(uint8_t out[kDigestSize]) const;

  // XSESSION_INFO, XSESSION_SEARCHRESULT and max QoS blob.
  static constexpr uint32_t kSessionInfoSize = 0x3C;
  static constexpr uint32_t kSearchResultSize = 0x5C;
  static constexpr uint32_t kQosMax = 1024;

  // Open carrier and listen/connect if not running.
  bool EnsureStarted();

  // Session service (app 0xFB). Takes mutex() internally.
  void SessionCreate(uint32_t flags, uint32_t info_ptr, uint32_t nonce_ptr);
  void SessionDelete();
  void SessionSearch(uint32_t overlapped_ptr, uint32_t results_ptr,
                     uint32_t results_size);
  // XNetQosListen SET_DATA.
  void SessionSetHostQos(uint32_t data_ptr, uint32_t data_size);
  // XNetQosLookup: fills one persistent XNQOS block.
  bool SessionQosLookup(uint32_t num_remote_consoles, uint32_t out_ptr,
                        uint32_t event_handle);

  // QoS RTT -> input delay: 821b1968 class, table 824B085C -> frames.
  // netplay_delay raises the answer instead of writing delay byte.
  static uint32_t DelayClassForRtt(uint32_t rtt);
  static uint32_t DelayFramesForClass(uint32_t delay_class);
  static uint16_t ReportedRtt(uint16_t link_rtt);

  // Session messages routed from socket layer.
  void OnSessionMessage(uint8_t kind, const uint8_t* data, unsigned length);
  void OnStateHash(const uint8_t* data, unsigned length);

  // Link reset: drop per-link observations.
  void CheckReset();
  // Worker pass, mutex() held.
  void SessionTick(uint32_t now);

 private:
  bool RangeMapped(uint32_t address, uint32_t length) const;

  // Answer pending search. Lock held.
  void SearchFinish(const uint8_t* info, uint32_t open_slots);
  void ReleaseQosBlock();

  // Sign in one Live profile (created on first run).
  void EnsureProfile();

  KernelState* kernel_state_ = nullptr;

  bool enabled_ = false;
  bool host_ = false;
  std::string join_;
  uint16_t port_ = 29710;
  std::string name_ = "Player";
  bool verbose_ = false;

  // Destroyed in reverse order: port map, worker, game, sockets.
  std::unique_ptr<NetplaySockets> sockets_;
  std::unique_ptr<NetplayGame> game_;
  std::unique_ptr<NetplayWorker> worker_;
  std::unique_ptr<NetplayPortMap> port_map_;

  std::mutex mutex_;
  uint8_t self_xna_[kXnAddrSize] = {};

  // Host session and QoS blob; joiner takes host's blob from search reply.
  bool hosted_ = false;
  uint8_t session_info_[kSessionInfoSize] = {};
  uint8_t qos_[kQosMax] = {};
  uint32_t qos_len_ = 0;
  // One XNQOS block per title, like the console; release does not free it.
  uint32_t qos_block_ = 0;

  // Search against netplay_join host; answer goes to game results buffer.
  struct SearchState {
    bool active = false;
    bool asked = false;
    uint32_t overlapped = 0;
    uint32_t results = 0;
    uint32_t size = 0;
    uint32_t deadline = 0;
  } search_;
  uint32_t join_deadline_ = 0;

  // Log-once flags.
  bool logged_active_ = false;
  bool profile_checked_ = false;
  uint32_t logged_title_id_ = 0;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NETPLAY_NETPLAY_STATE_H_
