/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/netplay/netplay_worker.h"

#include <cstring>
#include <mutex>
#include <random>
#include <string>

// clang-format off
#include "xenia/base/platform.h"
// clang-format on

#ifdef XE_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/netplay/netplay_game.h"
#include "xenia/kernel/netplay/netplay_sockets.h"
#include "xenia/kernel/netplay/netplay_state.h"

DECLARE_int32(netplay_test_loss);
DECLARE_int32(netplay_test_lag);
DECLARE_int32(netplay_test_jitter);

namespace xe {
namespace kernel {

struct OutMsg {
  uint16_t len;
  uint8_t data[NET_MAX_MSG];
};

namespace {

#ifdef XE_PLATFORM_WIN32
constexpr SOCKET kInvalidSocket = INVALID_SOCKET;
#else
constexpr uintptr_t kInvalidSocket = ~static_cast<uintptr_t>(0);
#endif

}  // namespace

NetplayWorker::NetplayWorker(NetplayState* state) : state_(state) {}

NetplayWorker::~NetplayWorker() { Stop(); }

bool NetplayWorker::Start() {
#ifdef XE_PLATFORM_WIN32
  if (running_) {
    return true;
  }

  WSADATA wsa = {};
  if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
    XELOGE("netplay: Winsock did not start");
    return false;
  }

  state_->FillDigest(digest_);
  out_.reset(new OutMsg[kOutQueue]);
  TestNetworkStart();

  SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(state_->join_address().empty() ? state_->port() : 0);
  if (udp == kInvalidSocket ||
      bind(udp, reinterpret_cast<sockaddr*>(&address), sizeof address)) {
    XELOGE("netplay: can't open UDP port {}", ntohs(address.sin_port));
    if (udp != kInvalidSocket) {
      closesocket(udp);
    }
    return false;
  }

  u_long non_blocking = 1;
  ioctlsocket(udp, FIONBIO, &non_blocking);

  udp_ = static_cast<uintptr_t>(udp);
  running_ = true;
  ResetLink();
  thread_ = std::thread(&NetplayWorker::WorkerMain, this);

  XELOGI("netplay: worker on UDP port {}", ntohs(address.sin_port));
  return true;
#else
  XELOGE("netplay: this build has no network carrier");
  return false;
#endif
}

void NetplayWorker::Stop() {
  running_ = false;
  // Say goodbye inside loop while port is open.
  if (thread_.joinable()) {
    thread_.join();
  }
#ifdef XE_PLATFORM_WIN32
  if (udp_ != static_cast<uintptr_t>(kInvalidSocket)) {
    closesocket(static_cast<SOCKET>(udp_));
    udp_ = static_cast<uintptr_t>(kInvalidSocket);
  }
#else
  udp_ = kInvalidSocket;
#endif
  out_.reset();
  test_.reset();
}

bool NetplayWorker::JoinNamed() const {
  return !state_->join_address().empty();
}

void NetplayWorker::HostStart() {
  hosting_ = true;
  const NetState st = net_link_state(&link_);
  if (st == NET_ESTABLISHED || st == NET_LISTENING) {
    return;
  }
  ResetLink();
  net_link_listen(&link_, NowMs());
}

void NetplayWorker::JoinStart() {
  const NetState st = net_link_state(&link_);
  if (st == NET_ESTABLISHED || st == NET_CONNECTING) {
    return;
  }
  ResetLink();

  const std::string& host = state_->join_address();
  if (host.empty()) {
    return;
  }

  char port[16];
  std::snprintf(port, sizeof port, "%u", static_cast<unsigned>(state_->port()));

  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), port, &hints, &result) || !result) {
    XELOGE("netplay: can't find the host '{}'", host);
    return;
  }
  std::memcpy(peer_, result->ai_addr, sizeof(sockaddr_in));
  freeaddrinfo(result);
  peer_set_ = true;

  XELOGI("netplay: reaching {} port {}", host, port);
  net_link_connect(&link_, NowMs());
}

void NetplayWorker::LinkStop() {
  hosting_ = false;
  if (net_link_state(&link_) == NET_ESTABLISHED) {
    net_link_close(&link_);
  }
  state_->sockets()->StreamsEnd();
  ResetLink();
  last_state_ = NET_IDLE;
}

bool NetplayWorker::LinkUp() const {
  return net_link_state(&link_) == NET_ESTABLISHED;
}

void NetplayWorker::GameHeard() { game_at_ = NowMs(); }

bool NetplayWorker::GameAlive() const {
  return LinkUp() && NowMs() - game_at_ < kGameSilenceMs;
}

uint16_t NetplayWorker::LinkRtt() const {
  const uint32_t ms = net_link_stats(&link_)->rtt_ms;
  return static_cast<uint16_t>(ms > 0xFFFFu ? 0xFFFFu : ms);
}

bool NetplayWorker::LinkFailed() const {
  const NetState st = net_link_state(&link_);
  return st == NET_CLOSED || st == NET_TIMED_OUT || st == NET_REFUSED ||
         st == NET_IDLE;
}

std::string NetplayWorker::PeerText() const {
#ifdef XE_PLATFORM_WIN32
  if (!peer_set_) {
    return {};
  }
  const sockaddr_in* peer = reinterpret_cast<const sockaddr_in*>(peer_);
  char address[INET_ADDRSTRLEN] = {};
  if (!inet_ntop(AF_INET, &peer->sin_addr, address, sizeof address)) {
    return {};
  }
  return fmt::format("{}:{}", address, ntohs(peer->sin_port));
#else
  return {};
#endif
}

int NetplayWorker::LinkWrite(bool reliable, const uint8_t* d, unsigned len) {
  if (net_link_state(&link_) != NET_ESTABLISHED) {
    return -1;
  }
  if (!reliable) {
    return net_link_send(&link_, 0, d, static_cast<int>(len), NowMs());
  }
  if (!out_count_ &&
      net_link_send(&link_, 1, d, static_cast<int>(len), NowMs()) == 0) {
    return 0;
  }
  if (out_count_ == kOutQueue) {
    XELOGI(
        "netplay: the other player stopped taking messages, closing the link");
    net_link_close(&link_);
    return -1;
  }
  OutMsg& slot = out_[(out_head_ + out_count_) % kOutQueue];
  slot.len = static_cast<uint16_t>(len);
  std::memcpy(slot.data, d, len);
  out_count_++;
  return 0;
}

int NetplayWorker::SendSession(int kind, const void* data, unsigned len) {
  uint8_t b[NET_MAX_MSG];
  if (len + 1 > sizeof b) {
    return -1;
  }
  b[0] = static_cast<uint8_t>(kind);
  if (len) {
    std::memcpy(b + 1, data, len);
  }
  return LinkWrite(true, b, len + 1);
}

void NetplayWorker::Random(void* out, unsigned len) {
  std::random_device device;
  uint8_t* p = static_cast<uint8_t*>(out);
  for (unsigned i = 0; i < len; i++) {
    p[i] = static_cast<uint8_t>(device());
  }
}

uint32_t NetplayWorker::NowMs() {
  return static_cast<uint32_t>(NetplayState::now_ms());
}

uint32_t NetplayWorker::TestRandom(uint32_t below) {
  if (!below) {
    return 0;
  }
  return static_cast<uint32_t>(test_rng_() % below);
}

void NetplayWorker::TestNetworkStart() {
  const int loss = cvars::netplay_test_loss;
  const int lag = cvars::netplay_test_lag;
  const int jitter = cvars::netplay_test_jitter;
  if (loss <= 0 && lag <= 0 && jitter <= 0) {
    return;
  }

  test_loss_ = loss < 0 ? 0 : (loss > 100 ? 100 : loss);
  test_lag_ = lag < 0 ? 0 : lag;
  test_jitter_ = jitter < 0 ? 0 : jitter;
  std::random_device device;
  test_rng_.seed(device());
  test_.reset(new TestPacket[kTestHeld]);
  XELOGI("netplay: TEST NETWORK: {}% lost, {} ms late, up to {} ms more",
         test_loss_, test_lag_, test_jitter_);
}

void NetplayWorker::TestNetworkSend(const uint8_t* packet, int len) {
  if (test_loss_ && static_cast<int>(TestRandom(100)) < test_loss_) {
    return;
  }
  for (unsigned i = 0; i < kTestHeld; i++) {
    TestPacket& slot = test_[i];
    if (slot.len) {
      continue;
    }
    slot.due = NowMs() + static_cast<uint32_t>(test_lag_) +
               TestRandom(static_cast<uint32_t>(test_jitter_) + 1);
    slot.len = static_cast<uint16_t>(len);
    std::memcpy(slot.data, packet, static_cast<size_t>(len));
    return;
  }
  // Lost when all slots busy.
}

void NetplayWorker::TestNetworkTick(uint32_t now) {
  if (!test_) {
    return;
  }
#ifdef XE_PLATFORM_WIN32
  for (unsigned i = 0; i < kTestHeld; i++) {
    TestPacket& slot = test_[i];
    if (!slot.len || static_cast<int32_t>(now - slot.due) < 0) {
      continue;
    }
    if (peer_set_ && udp_ != static_cast<uintptr_t>(kInvalidSocket)) {
      sendto(static_cast<SOCKET>(udp_),
             reinterpret_cast<const char*>(slot.data), slot.len, 0,
             reinterpret_cast<const sockaddr*>(peer_), sizeof(sockaddr_in));
    }
    slot.len = 0;
  }
#else
  (void)now;
#endif
}

void NetplayWorker::CarrierThunk(void* ctx, const uint8_t* packet, int len) {
  static_cast<NetplayWorker*>(ctx)->Carrier(packet, len);
}

void NetplayWorker::Carrier(const uint8_t* packet, int len) {
#ifdef XE_PLATFORM_WIN32
  if (test_) {
    TestNetworkSend(packet, len);
    return;
  }
  if (peer_set_ && udp_ != static_cast<uintptr_t>(kInvalidSocket)) {
    sendto(static_cast<SOCKET>(udp_), reinterpret_cast<const char*>(packet),
           len, 0, reinterpret_cast<const sockaddr*>(peer_),
           sizeof(sockaddr_in));
  }
#else
  (void)packet;
  (void)len;
#endif
}

void NetplayWorker::ResetLink() {
  uint32_t nonce = 0;
  Random(&nonce, sizeof nonce);
  net_link_init(&link_, &NetplayWorker::CarrierThunk, this, digest_, nonce);
  state_->sockets()->Reset();
  state_->CheckReset();
  out_head_ = out_count_ = 0;
  held_len_ = 0;
  peer_set_ = false;
  game_at_ = NowMs();
  if (test_) {
    for (unsigned i = 0; i < kTestHeld; i++) {
      test_[i].len = 0;
    }
  }
}

void NetplayWorker::OutFlush() {
  while (out_count_ && net_link_send(&link_, 1, out_[out_head_].data,
                                     out_[out_head_].len, NowMs()) == 0) {
    out_head_ = (out_head_ + 1) % kOutQueue;
    out_count_--;
  }
}

void NetplayWorker::LinkDrain() {
  uint8_t b[NET_MAX_MSG];
  int reliable = 0;
  if (held_len_ && state_->sockets()->Deliver(
                       held_, static_cast<unsigned>(held_len_), true)) {
    held_len_ = 0;
  }
  while (!held_len_) {
    const int n = net_link_recv(&link_, &reliable, b, sizeof b);
    if (n <= 0) {
      break;
    }
    if (!state_->sockets()->Deliver(b, static_cast<unsigned>(n),
                                    reliable != 0)) {
      std::memcpy(held_, b, static_cast<size_t>(n));
      held_len_ = n;
    }
  }
  state_->sockets()->Drain();
}

void NetplayWorker::StateWatch(uint32_t now) {
  const NetState st = net_link_state(&link_);
  if (st == last_state_) {
    return;
  }
  const NetStats* stats = net_link_stats(&link_);
  XELOGI("netplay: link {} -> {}, sent {} got {} retries {} bad {} rtt {} ms",
         static_cast<int>(last_state_), static_cast<int>(st), stats->sent,
         stats->received, stats->retransmits, stats->bad, stats->rtt_ms);
  if (st == NET_REFUSED) {
    XELOGI("netplay: the two copies differ ({})",
           net_link_status(&link_) == NET_REFUSED_CONTENT ? "game data"
                                                          : "program version");
  }
  last_state_ = st;
  if (st == NET_CLOSED || st == NET_TIMED_OUT || st == NET_REFUSED) {
    // Peer gone: age members now rather than wait out the 5s rule.
    state_->game()->MembersGone();
    state_->sockets()->StreamsEnd();
    if (hosting_) {
      // Keep hosting for the next player.
      ResetLink();
      net_link_listen(&link_, now);
      last_state_ = NET_LISTENING;
    }
  }
}

void NetplayWorker::WorkerMain() {
#ifdef XE_PLATFORM_WIN32
  uint8_t packet[NET_MAX_PACKET + 64];
  while (running_) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(static_cast<SOCKET>(udp_), &readable);
    timeval timeout = {0, 5000};
    select(0, &readable, nullptr, nullptr, &timeout);

    std::lock_guard<std::mutex> lock(state_->mutex());
    if (!running_) {
      // Goodbye once while port is open.
      if (net_link_state(&link_) == NET_ESTABLISHED) {
        XELOGI("netplay: saying goodbye to the other player");
      }
      net_link_close(&link_);
      break;
    }

    const uint32_t now = NowMs();
    for (;;) {
      sockaddr_in from = {};
      int from_len = sizeof from;
      const int n = recvfrom(static_cast<SOCKET>(udp_),
                             reinterpret_cast<char*>(packet), sizeof packet, 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
      const NetState st = net_link_state(&link_);
      if (n == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAECONNRESET) {
          // ICMP reset from a refused port.
          continue;
        }
        break;
      }
      const sockaddr_in* peer = reinterpret_cast<const sockaddr_in*>(peer_);
      if (st == NET_LISTENING) {
        std::memcpy(peer_, &from, sizeof from);
        peer_set_ = true;
      } else if (!peer_set_ || from.sin_addr.s_addr != peer->sin_addr.s_addr ||
                 from.sin_port != peer->sin_port) {
        continue;
      }
      net_link_on_packet(&link_, packet, n, now);
    }

    net_link_tick(&link_, now);
    TestNetworkTick(now);
    OutFlush();
    LinkDrain();
    StateWatch(now);
    state_->SessionTick(now);
    state_->game()->Watch();
    state_->game()->MembersHeard();
  }
#endif
}

}  // namespace kernel
}  // namespace xe
