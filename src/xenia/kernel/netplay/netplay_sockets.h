/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NETPLAY_NETPLAY_SOCKETS_H_
#define XENIA_KERNEL_NETPLAY_NETPLAY_SOCKETS_H_

#include <array>
#include <cstdint>
#include <memory>

#include "xenia/kernel/netplay/net_inputs.h"

namespace xe {
namespace kernel {

class NetplayState;

// Game sockets over the link. Three traffic kinds:
// - member "TCP": message-bounded stream reads
// - UDP signalling: reliable channel
// - UDP lockstep (type 7): net_inputs, ordered, nothing skipped
class NetplaySockets {
 public:
  explicit NetplaySockets(NetplayState* state);
  ~NetplaySockets();

  NetplaySockets(const NetplaySockets&) = delete;
  NetplaySockets& operator=(const NetplaySockets&) = delete;

  // NetDll_ exports. Caller holds NetplayState::mutex().
  uint32_t Socket(uint32_t af, uint32_t type, uint32_t protocol);
  uint32_t CloseSocket(uint32_t handle);
  uint32_t IoctlSocket(uint32_t handle, uint32_t cmd, uint32_t arg);
  uint32_t Bind(uint32_t handle, uint32_t addr);
  uint32_t Connect(uint32_t handle, uint32_t addr);
  uint32_t Listen(uint32_t handle, uint32_t backlog);
  uint32_t Accept(uint32_t handle, uint32_t addr, uint32_t addrlen);
  uint32_t Select(uint32_t nfds, uint32_t readfds, uint32_t writefds,
                  uint32_t exceptfds, uint32_t timeout);
  uint32_t Recv(uint32_t handle, uint32_t buf, uint32_t len, uint32_t flags);
  uint32_t RecvFrom(uint32_t handle, uint32_t buf, uint32_t len,
                    uint32_t flags, uint32_t from, uint32_t fromlen);
  uint32_t Send(uint32_t handle, uint32_t buf, uint32_t len, uint32_t flags);
  uint32_t SendTo(uint32_t handle, uint32_t buf, uint32_t len, uint32_t flags,
                  uint32_t to, uint32_t tolen);
  uint32_t WsaFdIsSet(uint32_t handle, uint32_t set);

  // Link delivery. false: wait for room.
  bool Deliver(const uint8_t* b, unsigned n, bool reliable);
  // Move input stream to game socket.
  void Drain();
  // Link closed: streams read as closed.
  void StreamsEnd();
  // New link.
  void Reset();
  // Release table.
  void Shutdown();

  // Per-guest-thread WSA error.
  static uint32_t LastError();
  static void SetLastError(uint32_t error);

 private:
  static constexpr uint32_t kSockBase = 0x4E500000u;
  static constexpr unsigned kSockets = 16;
  static constexpr unsigned kRing = 128;
  static constexpr uint32_t kGameMsg = 0x4F0;
  static constexpr uint8_t kGuestStream = 1;
  static constexpr uint8_t kGuestDgram = 2;
  static constexpr uint16_t kLockstepPort = 1000;
  static constexpr uint16_t kTypeLockstep = 0x7000;

  // Link kinds; session kinds (5, 6, 7) follow.
  enum Kind : uint8_t {
    kDgram = 0,
    kInput = 1,
    kOpen = 2,
    kData = 3,
    kClose = 4,
    kSearchReq = 5,
    kSearchReply = 6,
    kStateHash = 7,
  };

  struct Msg {
    uint16_t len = 0;
    uint16_t off = 0;
    uint16_t from_port = 0;
    uint32_t from_ina = 0;
    uint8_t data[kGameMsg] = {};
  };

  struct Sock {
    bool used = false;
    uint8_t type = 0;
    bool listening = false;
    bool connected = false;
    bool peer_closed = false;
    bool accepted = false;
    bool mid = false;
    uint16_t port = 0;
    uint16_t remote = 0;
    unsigned head = 0;
    unsigned count = 0;
    std::unique_ptr<Msg[]> ring;
  };

  Sock* SockOf(uint32_t handle);
  const Sock* SockOf(uint32_t handle) const;
  uint32_t HandleOf(const Sock* s) const;
  Sock* SockNew(uint8_t type, uint16_t port);
  void SockFree(Sock* s);
  Sock* FindDgram(uint16_t port);
  const Sock* FindDgram(uint16_t port) const;
  Sock* FindStream(uint16_t port, uint16_t remote);
  bool RingPut(Sock* s, const uint8_t* d, unsigned len, uint16_t from_port,
               uint32_t from_ina);
  // Remember stage cursor/slot and answer peer MSG_GO early.
  void RememberStage(const uint8_t* d, unsigned len);
  void EchoStageGo(const uint8_t* d, unsigned len, uint16_t local_port,
                   uint16_t peer_port);
  uint32_t DoSendTo(Sock* s, uint32_t buf, uint32_t len, uint32_t to);
  uint32_t DoRecv(Sock* s, uint32_t buf, uint32_t cap, uint32_t from,
                  uint32_t fromlen);
  bool Readable(const Sock* s) const;
  bool Writable(const Sock* s) const;
  uint32_t SelectSet(uint32_t set, bool write);

  static uint32_t& ErrorRef();
  static uint32_t Fail(uint32_t error);

  NetplayState* state_ = nullptr;
  std::array<Sock, kSockets> socks_;
  NetInputs inputs_;

  // Stage select cursor/slot from last sent message.
  bool stage_valid_ = false;
  uint8_t stage_cursor_hi_ = 0;
  uint8_t stage_cursor_lo_ = 0;
  uint8_t stage_slot_ = 0;
  // MSG_GO sent this ready-up.
  bool stage_go_sent_ = false;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NETPLAY_NETPLAY_SOCKETS_H_
