/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NETPLAY_NETPLAY_WORKER_H_
#define XENIA_KERNEL_NETPLAY_NETPLAY_WORKER_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "xenia/kernel/netplay/net_link.h"

namespace xe {
namespace kernel {

class NetplayState;

struct OutMsg;

// UDP carrier, link and worker. Session layer calls link helpers with
// NetplayState::mutex() held.
class NetplayWorker {
 public:
  explicit NetplayWorker(NetplayState* state);
  ~NetplayWorker();

  NetplayWorker(const NetplayWorker&) = delete;
  NetplayWorker& operator=(const NetplayWorker&) = delete;

  // Open port. Lock held.
  bool Start();
  // Notify peer, stop thread, close port.
  void Stop();
  bool running() const { return running_.load(); }

  // Lock held.
  void HostStart();
  void JoinStart();
  bool JoinNamed() const;
  void LinkStop();
  bool LinkUp() const;
  void GameHeard();
  bool GameAlive() const;
  uint16_t LinkRtt() const;
  bool LinkFailed() const;
  NetState LinkState() const { return net_link_state(&link_); }
  NetStatus LinkStatus() const { return net_link_status(&link_); }
  const NetStats* LinkStats() const { return net_link_stats(&link_); }
  // Peer "address:port", empty until known.
  std::string PeerText() const;
  int LinkWrite(bool reliable, const uint8_t* d, unsigned len);
  int SendSession(int kind, const void* data, unsigned len);
  void Random(void* out, unsigned len);

  static uint32_t NowMs();

 private:
  static constexpr unsigned kOutQueue = 3600;
  static constexpr uint32_t kGameSilenceMs = 30000;

  // Held-back test packet.
  static constexpr unsigned kTestHeld = 1024;
  struct TestPacket {
    uint32_t due;
    uint16_t len;
    uint8_t data[NET_MAX_PACKET];
  };

  void WorkerMain();
  void ResetLink();
  void StateWatch(uint32_t now);
  void OutFlush();
  void LinkDrain();
  void Carrier(const uint8_t* packet, int len);
  static void CarrierThunk(void* ctx, const uint8_t* packet, int len);

  // netplay_test_loss/lag/jitter.
  void TestNetworkStart();
  void TestNetworkSend(const uint8_t* packet, int len);
  void TestNetworkTick(uint32_t now);
  uint32_t TestRandom(uint32_t below);

  NetplayState* state_ = nullptr;
  NetLink link_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  uintptr_t udp_ = ~static_cast<uintptr_t>(0);
  alignas(16) uint8_t peer_[16] = {};
  bool peer_set_ = false;
  bool hosting_ = false;
  uint8_t digest_[NET_DIGEST] = {};
  std::unique_ptr<OutMsg[]> out_;
  unsigned out_head_ = 0;
  unsigned out_count_ = 0;
  uint8_t held_[NET_MAX_MSG] = {};
  int held_len_ = 0;
  NetState last_state_ = NET_IDLE;
  uint32_t game_at_ = 0;

  std::unique_ptr<TestPacket[]> test_;
  std::minstd_rand test_rng_;
  int test_loss_ = 0;
  int test_lag_ = 0;
  int test_jitter_ = 0;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NETPLAY_NETPLAY_WORKER_H_
