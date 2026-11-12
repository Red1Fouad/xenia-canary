/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NETPLAY_NETPLAY_PORTMAP_H_
#define XENIA_KERNEL_NETPLAY_NETPLAY_PORTMAP_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace xe {
namespace kernel {

// Optional host port forwarding (UPnP, then NAT-PMP). Peers still exchange
// address/port by hand.
class NetplayPortMap {
 public:
  enum class State {
    kDisabled,   // off or not host
    kSearching,  // request in flight
    kMapped,     // accepted
    kFailed,     // UPnP and NAT-PMP failed
  };

  struct Status {
    State state = State::kDisabled;
    std::string method;         // "UPnP" / "NAT-PMP"
    std::string local_address;  // LAN address
    uint16_t internal_port = 0;
    uint16_t external_port = 0;
    std::string external_address;  // public address
    std::string error;             // last failure
  };

  NetplayPortMap();
  ~NetplayPortMap();

  NetplayPortMap(const NetplayPortMap&) = delete;
  NetplayPortMap& operator=(const NetplayPortMap&) = delete;

  void Start(uint16_t port);
  void Stop();

  bool running() const { return running_.load(); }

  // Thread-safe copy.
  Status status() const;

 private:
  void Run(uint16_t port);
  bool TryUpnp(uint16_t port, Status* status);
  bool TryNatPmp(uint16_t port, Status* status);
  void SetStatus(const Status& status);

  mutable std::mutex mutex_;
  Status status_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NETPLAY_NETPLAY_PORTMAP_H_
