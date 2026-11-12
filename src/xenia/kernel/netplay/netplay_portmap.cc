/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/netplay/netplay_portmap.h"

#include <chrono>
#include <cstring>

// clang-format off
#include "xenia/base/platform.h"
// clang-format on

#ifdef XE_PLATFORM_WIN32
// clang-format off
// Winsock first.
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
// clang-format on
#endif

#include <third_party/miniupnp/miniupnpc/include/miniupnpc.h>
#include <third_party/miniupnp/miniupnpc/include/upnpcommands.h>

#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

namespace xe {
namespace kernel {

namespace {

#ifdef XE_PLATFORM_WIN32
// NAT-PMP (RFC 6886), UDP 5351, big-endian.
constexpr uint16_t kNatPmpPort = 5351;
constexpr uint8_t kNatPmpVersion = 0;
constexpr uint8_t kNatPmpOpExternalAddress = 0;
constexpr uint8_t kNatPmpOpMapUdp = 2;
constexpr uint8_t kNatPmpReplyBit = 0x80;
constexpr uint32_t kNatPmpLifetime = 7200;

constexpr uint8_t kNatPmpSuccess = 0;

void Put16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value >> 8);
  p[1] = static_cast<uint8_t>(value);
}

void Put32(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value >> 24);
  p[1] = static_cast<uint8_t>(value >> 16);
  p[2] = static_cast<uint8_t>(value >> 8);
  p[3] = static_cast<uint8_t>(value);
}

uint16_t Get16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t Get32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// Default gateway.
uint32_t DefaultGateway() {
  MIB_IPFORWARDROW route = {};
  if (GetBestRoute(0, 0, &route) != NO_ERROR) {
    return 0;
  }
  return route.dwForwardNextHop;
}

bool NatPmpAsk(uint32_t gateway, const uint8_t* request, int request_length,
               uint8_t* reply, int reply_capacity, int* reply_length) {
  *reply_length = 0;
  SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_handle == INVALID_SOCKET) {
    return false;
  }

  sockaddr_in router = {};
  router.sin_family = AF_INET;
  router.sin_port = htons(kNatPmpPort);
  router.sin_addr.s_addr = gateway;

  bool answered = false;
  for (int attempt = 0; attempt < 3 && !answered; attempt++) {
    if (sendto(socket_handle, reinterpret_cast<const char*>(request),
               request_length, 0, reinterpret_cast<const sockaddr*>(&router),
               sizeof router) == SOCKET_ERROR) {
      break;
    }

    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket_handle, &readable);
    timeval timeout = {1, 0};
    if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) {
      continue;
    }

    sockaddr_in from = {};
    int from_length = sizeof from;
    const int received =
        recvfrom(socket_handle, reinterpret_cast<char*>(reply), reply_capacity,
                 0, reinterpret_cast<sockaddr*>(&from), &from_length);
    if (received >= 8 && from.sin_addr.s_addr == gateway) {
      answered = true;
      *reply_length = received;
    }
  }

  closesocket(socket_handle);
  return answered;
}

const char* NatPmpReason(uint8_t code) {
  switch (code) {
    case 1:
      return "unsupported NAT-PMP version";
    case 2:
      return "not authorized or refused by the router";
    case 3:
      return "network failure";
    case 4:
      return "out of resources";
    case 5:
      return "opcode not supported";
    default:
      return "unknown error";
  }
}
#endif  // XE_PLATFORM_WIN32

// Lease renewal.
constexpr auto kRenewAfter = std::chrono::minutes(20);

// Retry delay after failure.
constexpr auto kRetryAfterFailure = std::chrono::seconds(45);

}  // namespace

NetplayPortMap::NetplayPortMap() = default;

NetplayPortMap::~NetplayPortMap() { Stop(); }

void NetplayPortMap::Start(uint16_t port) {
  if (running_.exchange(true)) {
    return;
  }

  stopping_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = Status{};
    status_.state = State::kSearching;
    status_.internal_port = port;
  }

  thread_ = std::thread(&NetplayPortMap::Run, this, port);
}

void NetplayPortMap::Stop() {
  if (!running_.exchange(false)) {
    return;
  }

  stopping_ = true;
  if (thread_.joinable()) {
    thread_.join();
  }

  std::lock_guard<std::mutex> lock(mutex_);
  status_ = Status{};
}

NetplayPortMap::Status NetplayPortMap::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

void NetplayPortMap::SetStatus(const Status& status) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_ = status;
}

void NetplayPortMap::Run(uint16_t port) {
  bool announced_failure = false;

  while (!stopping_) {
    Status attempt;
    attempt.state = State::kSearching;
    attempt.internal_port = port;

    if (TryUpnp(port, &attempt)) {
      attempt.state = State::kMapped;
      SetStatus(attempt);
    } else {
      Status natpmp;
      natpmp.state = State::kSearching;
      natpmp.internal_port = port;
      if (TryNatPmp(port, &natpmp)) {
        natpmp.state = State::kMapped;
        SetStatus(natpmp);
      } else {
        Status failed;
        failed.state = State::kFailed;
        failed.internal_port = port;
        failed.error = attempt.error;
        if (!natpmp.error.empty()) {
          if (!failed.error.empty()) {
            failed.error += "; ";
          }
          failed.error += natpmp.error;
        }
        if (failed.error.empty()) {
          failed.error = "no router answered";
        }
        SetStatus(failed);
        if (!announced_failure) {
          XELOGE("netplay: no automatic port mapping ({})", failed.error);
          announced_failure = true;
        }

        // Retry: router may be booting or discovery lost. Keep last status.
        const auto retry =
            std::chrono::steady_clock::now() + kRetryAfterFailure;
        while (!stopping_ && std::chrono::steady_clock::now() < retry) {
          xe::threading::Sleep(std::chrono::milliseconds(250));
        }
        continue;
      }
    }

    announced_failure = false;

    const Status mapped = status();
    if (!mapped.external_address.empty()) {
      XELOGI("netplay: {} mapped UDP {} (public {}:{})", mapped.method,
             mapped.internal_port, mapped.external_address,
             mapped.external_port);
    } else {
      XELOGI("netplay: {} mapped UDP {}", mapped.method, mapped.internal_port);
    }

    // Renew before lease expiry; short sleeps keep Stop() quick.
    const auto deadline = std::chrono::steady_clock::now() + kRenewAfter;
    while (!stopping_ && std::chrono::steady_clock::now() < deadline) {
      xe::threading::Sleep(std::chrono::milliseconds(250));
    }
  }
}

bool NetplayPortMap::TryUpnp(uint16_t port, Status* status) {
  status->method = "UPnP";

  int error = 0;
  UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
  if (!devices) {
    status->error = fmt::format("UPnP: no devices found ({})", error);
    return false;
  }

  UPNPUrls urls = {};
  IGDdatas data = {};
  char lan_address[NI_MAXHOST] = {};
  char wan_address[NI_MAXHOST] = {};
  const int valid =
      UPNP_GetValidIGD(devices, &urls, &data, lan_address, sizeof lan_address,
                       wan_address, sizeof wan_address);
  freeUPNPDevlist(devices);

  if (valid != UPNP_CONNECTED_IGD && valid != UPNP_PRIVATEIP_IGD) {
    FreeUPNPUrls(&urls);
    status->error = fmt::format("UPnP: no internet gateway (status {})", valid);
    return false;
  }

  if (!urls.controlURL || !urls.controlURL[0]) {
    FreeUPNPUrls(&urls);
    status->error = "UPnP: the gateway has no control address";
    return false;
  }

  status->local_address = lan_address;

  const std::string port_text = fmt::format("{}", port);
  int result =
      UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                          port_text.c_str(), port_text.c_str(), lan_address,
                          "Xenia Canary MM10 Netplay", "UDP", nullptr, "7200");

  // Some routers only allow permanent leases.
  if (result == 725) {
    result =
        UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                            port_text.c_str(), port_text.c_str(), lan_address,
                            "Xenia Canary MM10 Netplay", "UDP", nullptr, "0");
  }

  // 718: mapping exists. Keep only if it matches this machine/port.
  if (result == 718) {
    char mapped_client[64] = {};
    char mapped_port[8] = {};
    char mapped_desc[96] = {};  // miniupnpc writes up to 80
    char mapped_enabled[8] = {};
    char mapped_lease[32] = {};
    const int listing = UPNP_GetSpecificPortMappingEntry(
        urls.controlURL, data.first.servicetype, port_text.c_str(), "UDP",
        nullptr, mapped_client, mapped_port, mapped_desc, mapped_enabled,
        mapped_lease);
    if (listing == UPNPCOMMAND_SUCCESS &&
        std::strcmp(mapped_client, lan_address) == 0 &&
        std::strcmp(mapped_port, port_text.c_str()) == 0) {
      result = UPNPCOMMAND_SUCCESS;
    } else {
      UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype,
                             port_text.c_str(), "UDP", nullptr);
      result = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                   port_text.c_str(), port_text.c_str(),
                                   lan_address, "Xenia Canary MM10 Netplay",
                                   "UDP", nullptr, "7200");
      if (result == 725) {
        result = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                     port_text.c_str(), port_text.c_str(),
                                     lan_address, "Xenia Canary MM10 Netplay",
                                     "UDP", nullptr, "0");
      }
    }
  }

  if (result != UPNPCOMMAND_SUCCESS) {
    FreeUPNPUrls(&urls);
    status->error =
        fmt::format("UPnP: the router refused the mapping ({})", result);
    return false;
  }

  char external_address[64] = {};
  if (UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype,
                                external_address) == UPNPCOMMAND_SUCCESS) {
    status->external_address = external_address;
  }

  FreeUPNPUrls(&urls);
  status->external_port = port;
  return true;
}

bool NetplayPortMap::TryNatPmp(uint16_t port, Status* status) {
  status->method = "NAT-PMP";

#ifdef XE_PLATFORM_WIN32
  const uint32_t gateway = DefaultGateway();
  if (!gateway) {
    status->error = "NAT-PMP: no default gateway";
    return false;
  }

  uint8_t request[12] = {};
  uint8_t reply[32] = {};
  int reply_length = 0;

  // Public address first for status.
  request[0] = kNatPmpVersion;
  request[1] = kNatPmpOpExternalAddress;
  if (NatPmpAsk(gateway, request, 2, reply, sizeof reply, &reply_length) &&
      reply_length >= 12 &&
      reply[1] == (kNatPmpOpExternalAddress | kNatPmpReplyBit) &&
      reply[2] == 0 && reply[3] == kNatPmpSuccess) {
    const uint32_t address = Get32(reply + 8);
    if (address) {
      status->external_address =
          fmt::format("{}.{}.{}.{}", address >> 24, (address >> 16) & 0xFF,
                      (address >> 8) & 0xFF, address & 0xFF);
    }
  }

  std::memset(request, 0, sizeof request);
  request[0] = kNatPmpVersion;
  request[1] = kNatPmpOpMapUdp;
  Put16(request + 4, port);
  Put16(request + 6, port);
  Put32(request + 8, kNatPmpLifetime);

  std::memset(reply, 0, sizeof reply);
  if (!NatPmpAsk(gateway, request, sizeof request, reply, sizeof reply,
                 &reply_length)) {
    status->error = "NAT-PMP: the router did not answer";
    return false;
  }

  if (reply_length < 16 || reply[1] != (kNatPmpOpMapUdp | kNatPmpReplyBit)) {
    status->error = "NAT-PMP: an unexpected reply";
    return false;
  }

  const uint8_t result = reply[3];
  if (result != kNatPmpSuccess) {
    status->error = fmt::format("NAT-PMP: {}", NatPmpReason(result));
    return false;
  }

  status->external_port = Get16(reply + 10);
  if (!status->external_port) {
    status->external_port = port;
  }
  return true;
#else
  (void)port;
  status->error = "NAT-PMP is not implemented on this platform";
  return false;
#endif
}

}  // namespace kernel
}  // namespace xe
