/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/netplay/netplay_sockets.h"

#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/kernel/netplay/net_link.h"
#include "xenia/kernel/netplay/netplay_game.h"
#include "xenia/kernel/netplay/netplay_state.h"
#include "xenia/kernel/netplay/netplay_worker.h"

namespace xe {
namespace kernel {

namespace {

constexpr uint32_t kWsaWouldBlock = 0x2733u;
constexpr uint32_t kWsaNotConn = 0x2749u;
constexpr uint32_t kWsaBadF = 0x2736u;
constexpr uint32_t kWsaConnReset = 0x2746u;
constexpr uint32_t kWsaMsgSize = 0x2738u;
constexpr uint32_t kWsaAddrNotAvail = 0x2741u;
constexpr uint32_t kWsaSocktNoSupport = 0x273Cu;

constexpr uint32_t kFionBio = 0x8004667Eu;

void PutPorts(uint8_t* b, uint8_t kind, uint16_t dst, uint16_t src) {
  b[0] = kind;
  b[1] = static_cast<uint8_t>(dst >> 8);
  b[2] = static_cast<uint8_t>(dst);
  b[3] = static_cast<uint8_t>(src >> 8);
  b[4] = static_cast<uint8_t>(src);
}

// Game message: u32 dest, u16 channel, u16 size, payload. Stage select is
// channel 3, 8-byte payload, first byte type (7 = MSG_GO).
constexpr uint16_t kStageChannel = 3;
constexpr uint16_t kStagePayload = 8;

bool IsStageMessage(const uint8_t* d, unsigned len) {
  if (len < 8 + kStagePayload) {
    return false;
  }
  return static_cast<uint16_t>(d[4] | d[5] << 8) == kStageChannel &&
         static_cast<uint16_t>(d[6] | d[7] << 8) == kStagePayload;
}

}  // namespace

NetplaySockets::NetplaySockets(NetplayState* state) : state_(state) {
  net_inputs_init(&inputs_);
}

NetplaySockets::~NetplaySockets() = default;

uint32_t& NetplaySockets::ErrorRef() {
  static thread_local uint32_t error = 0;
  return error;
}

uint32_t NetplaySockets::LastError() { return ErrorRef(); }

void NetplaySockets::SetLastError(uint32_t error) { ErrorRef() = error; }

uint32_t NetplaySockets::Fail(uint32_t error) {
  SetLastError(error);
  return 0xFFFFFFFFu;
}

NetplaySockets::Sock* NetplaySockets::SockOf(uint32_t handle) {
  const uint32_t index = handle - kSockBase;
  return index < kSockets && socks_[index].used ? &socks_[index] : nullptr;
}

const NetplaySockets::Sock* NetplaySockets::SockOf(uint32_t handle) const {
  const uint32_t index = handle - kSockBase;
  return index < kSockets && socks_[index].used ? &socks_[index] : nullptr;
}

uint32_t NetplaySockets::HandleOf(const Sock* s) const {
  return kSockBase + static_cast<uint32_t>(s - socks_.data());
}

NetplaySockets::Sock* NetplaySockets::SockNew(uint8_t type, uint16_t port) {
  for (unsigned i = 0; i < kSockets; i++) {
    Sock& s = socks_[i];
    if (s.used) {
      continue;
    }
    s.used = true;
    s.type = type;
    s.port = port;
    s.listening = s.connected = s.peer_closed = s.accepted = s.mid = false;
    s.remote = 0;
    s.head = s.count = 0;
    s.ring = std::unique_ptr<Msg[]>(new Msg[kRing]);
    if (!s.ring) {
      s.used = false;
      XELOGE("netplay: no memory for a socket's queue");
      return nullptr;
    }
    return &s;
  }
  XELOGE("netplay: more sockets than the table holds ({})", kSockets);
  return nullptr;
}

void NetplaySockets::SockFree(Sock* s) {
  s->ring.reset();
  *s = Sock();
}

NetplaySockets::Sock* NetplaySockets::FindDgram(uint16_t port) {
  for (Sock& s : socks_) {
    if (s.used && s.type == kGuestDgram && s.port == port) {
      return &s;
    }
  }
  return nullptr;
}

const NetplaySockets::Sock* NetplaySockets::FindDgram(uint16_t port) const {
  for (const Sock& s : socks_) {
    if (s.used && s.type == kGuestDgram && s.port == port) {
      return &s;
    }
  }
  return nullptr;
}

NetplaySockets::Sock* NetplaySockets::FindStream(uint16_t port,
                                                 uint16_t remote) {
  for (Sock& s : socks_) {
    if (s.used && s.type == kGuestStream && !s.listening && s.port == port &&
        s.remote == remote && !s.peer_closed) {
      return &s;
    }
  }
  return nullptr;
}

bool NetplaySockets::RingPut(Sock* s, const uint8_t* d, unsigned len,
                             uint16_t from_port, uint32_t from_ina) {
  if (s->count == kRing) {
    return false;
  }
  Msg& m = s->ring[(s->head + s->count) % kRing];
  m.len = static_cast<uint16_t>(len);
  m.off = 0;
  m.from_port = from_port;
  m.from_ina = from_ina;
  std::memcpy(m.data, d, len);
  s->count++;
  return true;
}

void NetplaySockets::RememberStage(const uint8_t* d, unsigned len) {
  if (!IsStageMessage(d, len)) {
    return;
  }
  stage_valid_ = true;
  stage_cursor_hi_ = d[9];
  stage_cursor_lo_ = d[10];
  stage_slot_ = d[11];
  if (d[8] == 7) {
    stage_go_sent_ = true;
  } else if (d[8] == 1) {
    // Fresh ready-up.
    stage_go_sent_ = false;
  }
}

void NetplaySockets::EchoStageGo(const uint8_t* d, unsigned len,
                                 uint16_t local_port, uint16_t peer_port) {
  // 82140b10: simultaneous ready can make both sides leave without sending
  // MSG_GO. Echo the first one; duplicates are ignored by the game.
  if (!IsStageMessage(d, len) || d[8] != 7 || stage_go_sent_) {
    return;
  }
  uint8_t echo[5 + 8 + kStagePayload] = {};
  PutPorts(echo, kDgram, peer_port, local_port);
  uint8_t* m = echo + 5;
  std::memcpy(m, d, 8 + kStagePayload);
  m[8] = 7;
  // Real cursor/slot if known, else sibling slot.
  m[9] = stage_valid_ ? stage_cursor_hi_ : d[9];
  m[10] = stage_valid_ ? stage_cursor_lo_ : d[10];
  m[11] = stage_valid_ ? stage_slot_ : static_cast<uint8_t>(1 - d[11]);
  m[12] = m[13] = m[14] = m[15] = 0;
  if (state_->worker()->LinkWrite(true, echo, sizeof echo) == 0) {
    stage_go_sent_ = true;
    XELOGI("netplay: stage select: the other player is going, answering");
  }
}

bool NetplaySockets::Deliver(const uint8_t* b, unsigned n, bool reliable) {
  if (n == 0) {
    return true;
  }
  if (b[0] <= kClose) {
    state_->worker()->GameHeard();
  }
  if (b[0] == kSearchReq || b[0] == kSearchReply) {
    state_->OnSessionMessage(b[0], b + 1, n - 1);
    return true;
  }
  if (b[0] == kStateHash) {
    state_->OnStateHash(b + 1, n - 1);
    return true;
  }
  if (b[0] == kInput) {
    if (reliable && !net_inputs_fits(&inputs_, b + 1, static_cast<int>(n) - 1)) {
      return false;
    }
    net_inputs_on_packet(&inputs_, b + 1, static_cast<int>(n) - 1);
    return true;
  }
  if (n < 5) {
    return true;
  }
  const uint16_t dst = static_cast<uint16_t>(b[1] << 8 | b[2]);
  const uint16_t src = static_cast<uint16_t>(b[3] << 8 | b[4]);
  switch (b[0]) {
    case kDgram: {
      Sock* s = FindDgram(dst);
      if (!s) {
        return true;
      }
      if (!state_->game()->MessageOk(b + 5, n - 5)) {
        XELOGI("netplay: dropped a bad message, {} bytes", n - 5);
        return true;
      }
      EchoStageGo(b + 5, n - 5, dst, src);
      return RingPut(s, b + 5, n - 5, src,
                     NetplayState::kPeerInAddr);
    }
    case kOpen: {
      unsigned room = 0;
      for (const Sock& s : socks_) {
        room += !s.used;
      }
      for (Sock& s : socks_) {
        if (room && s.used && s.listening && s.port == dst) {
          Sock* c = SockNew(kGuestStream, dst);
          if (c) {
            c->remote = src;
            c->connected = true;
          }
          return true;
        }
      }
      uint8_t no[5];
      PutPorts(no, kClose, src, dst);
      state_->worker()->LinkWrite(true, no, 5);
      return true;
    }
    case kData: {
      Sock* s = FindStream(dst, src);
      if (!s) {
        return true;
      }
      if (!state_->game()->MessageOk(b + 5, n - 5)) {
        XELOGI("netplay: dropped a bad message, {} bytes", n - 5);
        return true;
      }
      return RingPut(s, b + 5, n - 5, src,
                     NetplayState::kPeerInAddr);
    }
    case kClose: {
      Sock* s = FindStream(dst, src);
      if (s && !s->accepted) {
        SockFree(s);
      } else if (s) {
        s->peer_closed = true;
      }
      return true;
    }
    default:
      return true;
  }
}

void NetplaySockets::Drain() {
  Sock* s = FindDgram(kLockstepPort);
  uint8_t b[NET_INPUT_MSG];
  while (s && s->count < kRing) {
    // Peek: keep ordered until the game's ring has room.
    int n = net_inputs_peek(&inputs_, b, sizeof b);
    if (n <= 0) {
      return;
    }
    if (!state_->game()->MessageOk(b, static_cast<unsigned>(n))) {
      XELOGI("netplay: dropped a bad lockstep message, {} bytes", n);
      net_inputs_next(&inputs_, b, sizeof b);
      continue;
    }
    if (state_->game()->Hold(b, static_cast<unsigned>(n))) {
      return;
    }
    n = net_inputs_next(&inputs_, b, sizeof b);
    if (n <= 0) {
      return;
    }
    RingPut(s, b, static_cast<unsigned>(n), kLockstepPort,
            NetplayState::kPeerInAddr);
  }
}

void NetplaySockets::StreamsEnd() {
  for (Sock& s : socks_) {
    if (!s.used || s.type != kGuestStream || s.listening) {
      continue;
    }
    if (s.accepted) {
      s.peer_closed = true;
    } else {
      SockFree(&s);
    }
  }
}

void NetplaySockets::Reset() {
  net_inputs_init(&inputs_);
  stage_valid_ = false;
  stage_go_sent_ = false;
  stage_cursor_hi_ = stage_cursor_lo_ = stage_slot_ = 0;
  for (Sock& s : socks_) {
    if (s.used && s.type == kGuestDgram) {
      s.head = s.count = 0;
      s.mid = false;
    }
  }
}

void NetplaySockets::Shutdown() {
  net_inputs_init(&inputs_);
  stage_valid_ = false;
  stage_go_sent_ = false;
  stage_cursor_hi_ = stage_cursor_lo_ = stage_slot_ = 0;
  for (Sock& s : socks_) {
    if (s.used) {
      SockFree(&s);
    }
  }
}

uint32_t NetplaySockets::Socket(uint32_t af, uint32_t type, uint32_t protocol) {
  (void)af;
  (void)protocol;
  if (type != kGuestStream && type != kGuestDgram) {
    XELOGE("netplay: a socket of another type {}", type);
    return Fail(kWsaSocktNoSupport);
  }
  Sock* s = SockNew(static_cast<uint8_t>(type), 0);
  if (!s) {
    return Fail(kWsaMsgSize);
  }
  return HandleOf(s);
}

uint32_t NetplaySockets::CloseSocket(uint32_t handle) {
  Sock* s = SockOf(handle);
  if (!s) {
    return Fail(kWsaBadF);
  }
  if (s->type == kGuestStream && s->remote && !s->listening &&
      !s->peer_closed) {
    uint8_t b[5];
    PutPorts(b, kClose, s->remote, s->port);
    state_->worker()->LinkWrite(true, b, 5);
  }
  if (s->listening) {
    for (Sock& c : socks_) {
      if (&c != s && c.used && c.type == kGuestStream && !c.accepted &&
          !c.listening && c.port == s->port) {
        SockFree(&c);
      }
    }
  }
  SockFree(s);
  return 0;
}

uint32_t NetplaySockets::IoctlSocket(uint32_t handle, uint32_t cmd,
                                     uint32_t arg) {
  Sock* s = SockOf(handle);
  if (!s) {
    return Fail(kWsaBadF);
  }
  (void)arg;
  if (cmd != kFionBio) {
    XELOGW("netplay: an ioctl other than non-blocking: {:08X}", cmd);
  }
  return 0;
}

uint32_t NetplaySockets::Bind(uint32_t handle, uint32_t addr) {
  Sock* s = SockOf(handle);
  if (!s) {
    return Fail(kWsaBadF);
  }
  if (!addr) {
    return Fail(0x2714u);  // WSAEFAULT
  }
  s->port = state_->Read16(addr + 2);
  return 0;
}

uint32_t NetplaySockets::Connect(uint32_t handle, uint32_t addr) {
  Sock* s = SockOf(handle);
  if (!s) {
    return Fail(kWsaBadF);
  }
  if (!addr) {
    return Fail(0x2714u);  // WSAEFAULT
  }
  uint8_t b[5];
  s->remote = state_->Read16(addr + 2);
  s->connected = true;
  s->accepted = true;
  PutPorts(b, kOpen, s->remote, s->port);
  if (state_->worker()->LinkWrite(true, b, 5)) {
    s->peer_closed = true;
  }
  return Fail(kWsaWouldBlock);
}

uint32_t NetplaySockets::Listen(uint32_t handle, uint32_t backlog) {
  Sock* s = SockOf(handle);
  if (!s) {
    return Fail(kWsaBadF);
  }
  (void)backlog;
  s->listening = true;
  return 0;
}

uint32_t NetplaySockets::Accept(uint32_t handle, uint32_t addr,
                                uint32_t addrlen) {
  Sock* s = SockOf(handle);
  if (!s) {
    return Fail(kWsaBadF);
  }
  (void)addrlen;
  for (Sock& c : socks_) {
    if (c.used && c.type == kGuestStream && !c.listening && !c.accepted &&
        !c.peer_closed && c.connected && c.port == s->port && c.remote) {
      c.accepted = true;
      if (addr) {
        state_->Write16(addr, 2);
        state_->Write16(addr + 2, c.remote);
        state_->Write32(addr + 4, NetplayState::kPeerInAddr);
      }
      return HandleOf(&c);
    }
  }
  return Fail(kWsaWouldBlock);
}

bool NetplaySockets::Readable(const Sock* s) const {
  if (s->listening) {
    for (const Sock& c : socks_) {
      if (c.used && c.type == kGuestStream && !c.listening && !c.accepted &&
          !c.peer_closed && c.connected && c.port == s->port && c.remote) {
        return true;
      }
    }
    return false;
  }
  if (s->count && s->type == kGuestDgram &&
      state_->game()->Hold(s->ring[s->head].data, s->ring[s->head].len)) {
    return false;
  }
  return s->count != 0 || s->mid ||
         (s->type == kGuestStream && s->peer_closed);
}

bool NetplaySockets::Writable(const Sock* s) const {
  return s->type == kGuestDgram ||
         (s->connected && state_->worker()->LinkUp());
}

uint32_t NetplaySockets::SelectSet(uint32_t set, bool write) {
  uint32_t n = state_->Read32(set);
  uint32_t out = 0;
  if (n > 64) {
    n = 64;
  }
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t h = state_->Read32(set + 4 + 4 * i);
    const Sock* s = SockOf(h);
    if (s && (write ? Writable(s) : Readable(s))) {
      state_->Write32(set + 4 + 4 * out++, h);
    }
  }
  state_->Write32(set, out);
  return out;
}

uint32_t NetplaySockets::Select(uint32_t nfds, uint32_t readfds,
                                uint32_t writefds, uint32_t exceptfds,
                                uint32_t timeout) {
  (void)nfds;
  (void)timeout;
  uint32_t n = 0;
  if (readfds) {
    n += SelectSet(readfds, false);
  }
  if (writefds) {
    n += SelectSet(writefds, true);
  }
  if (exceptfds) {
    state_->Write32(exceptfds, 0);
  }
  return n;
}

uint32_t NetplaySockets::DoRecv(Sock* s, uint32_t buf, uint32_t cap,
                                uint32_t from, uint32_t fromlen) {
  if (!s->count) {
    if (s->mid) {
      s->mid = false;
      return 0;
    }
    return s->type == kGuestStream && s->peer_closed ? 0
                                                     : Fail(kWsaWouldBlock);
  }
  Msg& m = s->ring[s->head];
  if (s->type == kGuestDgram &&
      state_->game()->Hold(m.data, m.len)) {
    return Fail(kWsaWouldBlock);
  }
  if (s->mid && m.off == 0) {
    s->mid = false;
    return 0;
  }
  uint32_t n = static_cast<uint32_t>(m.len) - m.off;
  if (n > cap) {
    n = cap;
  }
  state_->GuestWrite(buf, m.data + m.off, n);
  m.off = static_cast<uint16_t>(m.off + n);
  if (from) {
    state_->Write16(from, 2);
    state_->Write16(from + 2, m.from_port);
    state_->Write32(from + 4, m.from_ina);
    if (fromlen) {
      state_->Write32(fromlen, 16);
    }
  }
  if (s->type == kGuestDgram || m.off == m.len) {
    s->mid = s->type == kGuestStream && n == cap && m.off == cap;
    s->head = (s->head + 1) % kRing;
    s->count--;
  } else {
    s->mid = false;
  }
  return n;
}

uint32_t NetplaySockets::Recv(uint32_t handle, uint32_t buf, uint32_t len,
                              uint32_t flags) {
  Sock* s = SockOf(handle);
  if (!s) {
    return Fail(kWsaBadF);
  }
  (void)flags;
  return DoRecv(s, buf, len, 0, 0);
}

uint32_t NetplaySockets::RecvFrom(uint32_t handle, uint32_t buf, uint32_t len,
                                  uint32_t flags, uint32_t from,
                                  uint32_t fromlen) {
  Sock* s = SockOf(handle);
  if (!s) {
    return Fail(kWsaBadF);
  }
  (void)flags;
  return DoRecv(s, buf, len, from, fromlen);
}

uint32_t NetplaySockets::DoSendTo(Sock* s, uint32_t buf, uint32_t len,
                                  uint32_t to) {
  uint8_t b[NET_MAX_MSG];
  if (!len || len > kGameMsg || !buf || !to) {
    return Fail(kWsaMsgSize);
  }
  const uint32_t ina = state_->Read32(to + 4);
  const uint16_t dport = state_->Read16(to + 2);
  if (ina == NetplayState::kSelfInAddr || ina == 0) {
    Sock* d = FindDgram(dport);
    if (d) {
      uint8_t packet[kGameMsg];
      if (state_->GuestRead(buf, packet, len)) {
        RingPut(d, packet, len, s->port, NetplayState::kSelfInAddr);
      }
    }
    return len;
  }
  if (ina != NetplayState::kPeerInAddr) {
    XELOGE("netplay: a send to an address nobody gave out: {:08X}", ina);
    return Fail(kWsaAddrNotAvail);
  }
  if ((state_->Read16(buf + 4) & 0xF000u) == kTypeLockstep) {
    uint8_t fast[NET_MAX_MSG];
    uint8_t sure[NET_INPUT_MSG + 16];
    uint8_t message[NET_INPUT_MSG];
    if (!state_->worker()->LinkUp()) {
      return len;  // no peer; must not consume a sequence number
    }
    if (!state_->GuestRead(buf, message, len)) {
      return Fail(kWsaMsgSize);
    }
    fast[0] = sure[0] = kInput;
    int fast_len = 0;
    int sure_len = 0;
    if (net_inputs_pack(&inputs_, message, static_cast<int>(len), fast + 1,
                        sizeof fast - 1, &fast_len, sure + 1, sizeof sure - 1,
                        &sure_len)) {
      return Fail(kWsaMsgSize);
    }
    state_->worker()->LinkWrite(false, fast,
                                static_cast<unsigned>(fast_len) + 1);
    state_->worker()->LinkWrite(true, sure,
                                static_cast<unsigned>(sure_len) + 1);
  } else {
    PutPorts(b, kDgram, dport, s->port);
    if (!state_->GuestRead(buf, b + 5, len)) {
      return Fail(kWsaMsgSize);
    }
    RememberStage(b + 5, len);
    state_->worker()->LinkWrite(true, b, len + 5);
  }
  return len;
}

uint32_t NetplaySockets::Send(uint32_t handle, uint32_t buf, uint32_t len,
                              uint32_t flags) {
  Sock* s = SockOf(handle);
  if (!s) {
    return Fail(kWsaBadF);
  }
  (void)flags;
  if (!s->connected) {
    return Fail(kWsaNotConn);
  }
  if (s->peer_closed) {
    return Fail(kWsaConnReset);
  }
  if (!len || len > kGameMsg) {
    return Fail(kWsaMsgSize);
  }
  uint8_t b[NET_MAX_MSG];
  PutPorts(b, kData, s->remote, s->port);
  if (!state_->GuestRead(buf, b + 5, len)) {
    return Fail(kWsaMsgSize);
  }
  return state_->worker()->LinkWrite(true, b, len + 5) ? Fail(kWsaConnReset)
                                                       : len;
}

uint32_t NetplaySockets::SendTo(uint32_t handle, uint32_t buf, uint32_t len,
                                uint32_t flags, uint32_t to, uint32_t tolen) {
  Sock* s = SockOf(handle);
  if (!s) {
    return Fail(kWsaBadF);
  }
  (void)flags;
  (void)tolen;
  return DoSendTo(s, buf, len, to);
}

uint32_t NetplaySockets::WsaFdIsSet(uint32_t handle, uint32_t set) {
  uint32_t n = state_->Read32(set);
  if (n > 64) {
    n = 64;
  }
  for (uint32_t i = 0; i < n; i++) {
    if (state_->Read32(set + 4 + 4 * i) == handle) {
      return 1;
    }
  }
  return 0;
}

}  // namespace kernel
}  // namespace xe
