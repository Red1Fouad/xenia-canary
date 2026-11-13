/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/netplay/netplay_state.h"

#include <cstdlib>
#include <cstring>
#include <random>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/string_util.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/netplay/netplay_game.h"
#include "xenia/kernel/netplay/netplay_sockets.h"
#include "xenia/kernel/netplay/netplay_worker.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/memory.h"

DEFINE_bool(netplay, false,
            "Enable the Mega Man 10 two-player link (title 584109EE).",
            "Netplay");
DEFINE_bool(netplay_host, false, "Wait for a player instead of joining.",
            "Netplay");
DEFINE_string(netplay_join, "", "Address of the host to join, as host:port.",
              "Netplay");
DEFINE_int32(netplay_port, 29710, "UDP port used by the host.", "Netplay");
DEFINE_string(netplay_name, "Player", "Name reported to the game.", "Netplay");
DEFINE_bool(netplay_verbose, false, "Log every netplay message.", "Netplay");
DEFINE_bool(netplay_upnp, false,
            "Host: ask the router to forward the netplay UDP port "
            "automatically with UPnP, then NAT-PMP.",
            "Netplay");
DEFINE_uint32(netplay_test_desync, 0,
              "Plant a one-byte state difference at this lockstep tick, to "
              "prove the desync check catches one (0 = off).",
              "Netplay");
DEFINE_int32(netplay_test_loss, 0,
             "Test network: percent of outgoing packets dropped (0 = off).",
             "Netplay");
DEFINE_int32(netplay_test_lag, 0,
             "Test network: milliseconds to hold every outgoing packet "
             "(0 = off).",
             "Netplay");
DEFINE_int32(netplay_test_jitter, 0,
             "Test network: up to this many extra milliseconds on each "
             "outgoing packet (0 = off).",
             "Netplay");
DEFINE_string(netplay_test_stall, "",
              "Test: stop the game once at this lockstep tick for this long, "
              "as tick,ms (empty = off).",
              "Netplay");
DEFINE_bool(netplay_rtt_report, false,
            "Log the link RTT, the delay class the game is given, and every "
            "change to its input delay.",
            "Netplay");
DEFINE_uint32(netplay_delay, 0,
              "Report at least the RTT the game needs for this much input "
              "delay in frames (2-12; 0 = report the real RTT).",
              "Netplay");
DEFINE_bool(netplay_rename_profile, false,
            "Rename the signed-in profile to --netplay_name, so a name typed "
            "at launch always wins over the one baked into the save data.",
            "Netplay");

namespace xe {
namespace kernel {

namespace {

constexpr uint32_t kMegaMan10TitleId = 0x584109EE;

// Session link kinds.
constexpr uint8_t kSessionSearchReq = 5;
constexpr uint8_t kSessionSearchReply = 6;

// Reference timings.
constexpr uint32_t kSearchMs = 6000;
constexpr uint32_t kJoinMs = 30000;

// XNQOS layout: count, pending, XNQOSINFO (0x18), data.
constexpr uint32_t kQosBlockSize = 0x10000;
constexpr uint32_t kQosInfoOffset = 8;
constexpr uint32_t kQosDataOffset = kQosInfoOffset + 0x18;
constexpr uint8_t kQosInfoComplete = 0x01;
constexpr uint8_t kQosInfoContacted = 0x02;
constexpr uint8_t kQosInfoDataReceived = 0x08;

bool EnvFlag(const char* name) {
  const char* value = std::getenv(name);
  return value && *value && std::string(value) != "0";
}

std::string EnvString(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return value && *value ? std::string(value) : fallback;
}

uint16_t ParsePort(const char* text, uint16_t fallback) {
  if (!text || !*text) {
    return fallback;
  }
  const int parsed = std::atoi(text);
  return parsed > 0 && parsed <= 65535 ? static_cast<uint16_t>(parsed)
                                       : fallback;
}

}  // namespace

NetplayState::NetplayState(KernelState* kernel_state)
    : kernel_state_(kernel_state) {
  enabled_ = cvars::netplay || EnvFlag("XB_NET");
  host_ = cvars::netplay_host;
  join_ = EnvString("XB_NET_JOIN", cvars::netplay_join);
  port_ = ParsePort(std::getenv("XB_NET_PORT"),
                    static_cast<uint16_t>(cvars::netplay_port));
  name_ = EnvString("XB_NET_NAME", cvars::netplay_name);
  verbose_ = cvars::netplay_verbose;

  // Accept "host:port" or bare host.
  const auto colon = join_.rfind(':');
  if (colon != std::string::npos) {
    port_ = ParsePort(join_.c_str() + colon + 1, port_);
    join_.resize(colon);
  }

  // XB_NET alone means host.
  if (enabled_ && join_.empty()) {
    host_ = true;
  }

  sockets_ = std::make_unique<NetplaySockets>(this);
  game_ = std::make_unique<NetplayGame>(this);
  worker_ = std::make_unique<NetplayWorker>(this);
  port_map_ = std::make_unique<NetplayPortMap>();

  // XNADDR is an identity, not an address: fixed 10.0.0.1 prefix, random rest.
  std::random_device device;
  for (uint32_t i = 0; i < kXnAddrSize; i++) {
    self_xna_[i] = static_cast<uint8_t>(device());
  }
  self_xna_[0] = 0x0A;
  self_xna_[1] = 0x00;
  self_xna_[2] = 0x00;
  self_xna_[3] = 0x01;
}

NetplayState::~NetplayState() { Shutdown(); }

bool NetplayState::IsActive() const {
  return enabled_ && kernel_state_ &&
         kernel_state_->title_id() == kMegaMan10TitleId;
}

void NetplayState::Refresh() {
  const uint32_t title_id = kernel_state_ ? kernel_state_->title_id() : 0;
  const bool active = IsActive();
  if (logged_title_id_ == title_id && logged_active_ == active) {
    return;
  }

  logged_title_id_ = title_id;
  logged_active_ = active;

  if (active) {
    XELOGI("netplay: enabled for title {:08X} ({} port {}, name '{}')",
           title_id, host_ ? "host" : "join", port_, name_);
    EnsureProfile();
    EnsureStarted();
  } else if (enabled_) {
    XELOGI("netplay: requested but title {:08X} is not 584109EE", title_id);
  }
}

void NetplayState::Shutdown() {
  // Stop threads before taking the lock to avoid deadlock.
  if (port_map_) {
    port_map_->Stop();
  }
  if (worker_) {
    worker_->Stop();
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sockets_) {
      sockets_->Shutdown();
    }
    ReleaseQosBlock();
    hosted_ = false;
    qos_len_ = 0;
    join_deadline_ = 0;
    search_ = SearchState{};
  }

  if (logged_active_) {
    XELOGI("netplay: stopped");
  }
  logged_active_ = false;
  profile_checked_ = false;
  logged_title_id_ = 0;
}

void NetplayState::EnsureProfile() {
  if (profile_checked_ || !IsActive() || !kernel_state_ ||
      !kernel_state_->xam_state()) {
    return;
  }

  auto* profiles = kernel_state_->xam_state()->profile_manager();
  if (!profiles) {
    return;
  }

  // The gamertag is baked in on first launch, so rename on request. Only when
  // there is exactly one account: with several, we can't tell which is ours.
  // Done before sign-in so no logout/login cycle is needed.
  if (cvars::netplay_rename_profile && !name_.empty()) {
    const auto* accounts = profiles->GetAccounts();
    if (accounts && accounts->size() == 1) {
      const uint64_t xuid = accounts->begin()->first;
      const auto* current = profiles->GetAccount(xuid);
      // 15 characters is the gamertag limit; copy_and_swap adds the null.
      std::u16string wanted = xe::to_utf16(name_);
      if (wanted.size() > 15) {
        wanted.resize(15);
      }
      if (current && xe::to_utf16(current->GetGamertagString()) != wanted) {
        xam::X_XAMACCOUNTINFO account = *current;
        const bool renamed = profiles->ModifyAccount(
            xuid, account, [&wanted](xam::X_XAMACCOUNTINFO& info) {
              xe::string_util::copy_and_swap_truncating(
                  info.gamertag, wanted, xe::countof(info.gamertag));
              return true;
            });
        XELOGI("netplay: rename profile to '{}': {}", name_,
               renamed ? "ok" : "failed");
      }
    } else if (accounts) {
      XELOGI("netplay: {} accounts, not renaming any of them",
             accounts->size());
    }
  }

  bool signed_in = profiles->IsAnyProfileSignedIn();

  if (!signed_in) {
    // Retry from disk and reload before creating.
    auto sign_in_existing = [profiles]() -> bool {
      const auto* accounts = profiles->GetAccounts();
      if (!accounts) {
        return false;
      }

      for (const auto& entry : *accounts) {
        const auto* account = profiles->GetAccount(entry.first);
        if (account && !account->IsLiveEnabled()) {
          profiles->ConvertToXboxLiveEnabledProfile(entry.first);
        }

        profiles->Login(entry.first);
        if (profiles->IsAnyProfileSignedIn()) {
          XELOGI("netplay: signed in profile {:016X}", entry.first);
          return true;
        }
      }

      return false;
    };

    signed_in = sign_in_existing();
    if (!signed_in) {
      profiles->ReloadProfiles();
      signed_in = sign_in_existing();
    }
  }

  if (!signed_in) {
    const std::string gamertag = name_.empty() ? "Player" : name_;
    uint64_t xuid = 0;
    if (profiles->CreateProfile(
            gamertag, true, false,
            xam::X_XAMACCOUNTINFO::AccountReservedFlags::kLiveEnabled, &xuid)) {
      XELOGI("netplay: created Live profile '{}' ({:016X})", gamertag, xuid);
    }
    signed_in = profiles->IsAnyProfileSignedIn();
  }

  if (!signed_in) {
    XELOGE("netplay: no Live profile could be signed in");
    return;
  }

  profile_checked_ = true;
  if (const auto* profile = profiles->GetProfile(static_cast<uint8_t>(0))) {
    XELOGI("netplay: profile '{}' sign-in state {} (2 = Live)", profile->name(),
           static_cast<uint32_t>(profile->signin_state()));
  }
}

bool NetplayState::EnsureStarted() {
  if (!IsActive()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!worker_) {
    return false;
  }
  if (worker_->running()) {
    return true;
  }
  if (!worker_->Start()) {
    return false;
  }
  if (host_) {
    worker_->HostStart();
  } else {
    worker_->JoinStart();
  }

  if (host_ && cvars::netplay_upnp && port_map_) {
    port_map_->Start(port_);
  }

  return true;
}

void NetplayState::RefreshPortMap() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!port_map_) {
    return;
  }

  const bool wanted = IsActive() && host_ && worker_ && worker_->running() &&
                      cvars::netplay_upnp;
  if (wanted) {
    port_map_->Start(port_);
  } else {
    port_map_->Stop();
  }
}

NetplayState::Status NetplayState::Snapshot() {
  Status status;
  status.enabled = enabled_;
  status.active = IsActive();
  status.host = host_;
  status.join_address = join_;
  status.port = port_;
  status.player_name = name_;

  std::lock_guard<std::mutex> lock(mutex_);

  if (worker_) {
    status.running = worker_->running();
    status.link_state = static_cast<int>(worker_->LinkState());
    status.link_status = static_cast<int>(worker_->LinkStatus());
    status.listening = worker_->LinkState() == NET_LISTENING;
    status.link_up = worker_->LinkUp();
    status.game_alive = worker_->GameAlive();
    status.peer = worker_->PeerText();

    const NetStats* stats = worker_->LinkStats();
    status.sent = stats->sent;
    status.received = stats->received;
    status.retransmits = stats->retransmits;
    status.bad = stats->bad;
    status.rtt_ms = stats->rtt_ms;

    const uint16_t real_rtt = static_cast<uint16_t>(
        status.rtt_ms > 0xFFFFu ? 0xFFFFu : status.rtt_ms);
    status.reported_rtt = ReportedRtt(real_rtt);
    status.delay_class = DelayClassForRtt(status.reported_rtt);
    status.delay_frames = DelayFramesForClass(status.delay_class);
  }

  if (game_) {
    status.lockstep_tick = game_->LockstepTick();
    status.input_delay = game_->InputDelayFrames();
    status.desync = game_->StateDiffering();
    status.desync_tick = game_->StateTick();
  }

  return status;
}

void NetplayState::SessionCreate(uint32_t flags, uint32_t info_ptr,
                                 uint32_t nonce_ptr) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!worker_) {
    return;
  }

  // Nonce differs per session.
  uint8_t nonce[8] = {};
  worker_->Random(nonce, sizeof nonce);
  if (nonce_ptr) {
    GuestWrite(nonce_ptr, nonce, sizeof nonce);
  }

  if (flags & 1u) {  // XSESSION_CREATE_HOST
    uint8_t info[kSessionInfoSize] = {};
    worker_->Random(info, 8);                       // session id
    FillSelfXnAddr(info + 8);                       // host XNADDR
    worker_->Random(info + 8 + kXnAddrSize, 0x10);  // key exchange key
    if (info_ptr) {
      GuestWrite(info_ptr, info, sizeof info);
    }
    std::memcpy(session_info_, info, sizeof info);
    hosted_ = true;
    join_deadline_ = 0;
    worker_->HostStart();
  } else {
    join_deadline_ = 0;
  }

  if (verbose_) {
    XELOGI("netplay: session created ({}), info {:08X}, nonce {:08X}",
           (flags & 1u) ? "host" : "client", info_ptr, nonce_ptr);
  }
}

void NetplayState::SessionDelete() {
  std::lock_guard<std::mutex> lock(mutex_);
  hosted_ = false;
  qos_len_ = 0;
  join_deadline_ = 0;
  if (worker_) {
    worker_->LinkStop();
  }
}

void NetplayState::SessionSetHostQos(uint32_t data_ptr, uint32_t data_size) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!data_ptr || !data_size) {
    return;
  }
  if (data_size > kQosMax) {
    XELOGI("netplay: more QoS data than the table holds ({}), keeping {}",
           data_size, kQosMax);
    data_size = kQosMax;
  }
  if (!GuestRead(data_ptr, qos_, data_size)) {
    XELOGE("netplay: the QoS data at {:08X} is not mapped", data_ptr);
    return;
  }
  qos_len_ = data_size;
  if (verbose_) {
    XELOGI("netplay: host QoS data {} bytes", qos_len_);
  }
}

void NetplayState::SessionSearch(uint32_t overlapped_ptr, uint32_t results_ptr,
                                 uint32_t results_size) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!worker_) {
    return;
  }

  search_.results = results_ptr;
  search_.size = results_size;
  search_.overlapped = overlapped_ptr;
  search_.asked = false;
  search_.deadline = worker_->NowMs() + kSearchMs;
  search_.active = true;
  join_deadline_ = 0;

  if (verbose_) {
    XELOGI("netplay: search started (results {:08X}, size {}, host '{}')",
           results_ptr, results_size, join_address());
  }

  if (!worker_->JoinNamed()) {
    // No host named: empty search.
    XELOGI("netplay: nothing to search: no host was named");
    SearchFinish(nullptr, 0);
  } else {
    worker_->JoinStart();
  }
}

uint32_t NetplayState::DelayClassForRtt(uint32_t rtt) {
  // 821b1968: five thresholds.
  if (rtt < 0x10) {
    return 5;
  }
  if (rtt < 0x30) {
    return 4;
  }
  if (rtt < 0x50) {
    return 3;
  }
  if (rtt < 0x70) {
    return 2;
  }
  if (rtt < 0x90) {
    return 1;
  }
  return 0;
}

uint32_t NetplayState::DelayFramesForClass(uint32_t delay_class) {
  // Table 824B085C.
  static constexpr uint32_t kFrames[] = {12, 10, 8, 6, 4, 2};
  return kFrames[delay_class < 6 ? delay_class : 5];
}

uint16_t NetplayState::ReportedRtt(uint16_t link_rtt) {
  const uint32_t want = cvars::netplay_delay;
  if (!want) {
    return link_rtt;
  }

  // Smallest class meeting `want` frames.
  const uint32_t frames = want < 2 ? 2 : (want > 12 ? 12 : want);
  const uint32_t delay_class = 5 - (frames - 1) / 2;

  // Lowest RTT per class.
  static constexpr uint16_t kLowestRtt[] = {0x90, 0x70, 0x50, 0x30, 0x10, 0x00};
  const uint16_t lowest = kLowestRtt[delay_class];
  return link_rtt > lowest ? link_rtt : lowest;
}

bool NetplayState::SessionQosLookup(uint32_t num_remote_consoles,
                                    uint32_t out_ptr, uint32_t event_handle) {
  if (!out_ptr || !kernel_state_ || !kernel_state_->memory()) {
    return false;
  }

  bool answered = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!worker_) {
      return false;
    }

    if (!qos_block_) {
      qos_block_ = kernel_state_->memory()->SystemHeapAlloc(kQosBlockSize);
      if (!qos_block_) {
        XELOGE("netplay: no memory for the QoS block");
        return false;
      }
    }

    if (num_remote_consoles != 1) {
      XELOGI("netplay: a QoS lookup of {} results, answering one",
             num_remote_consoles);
    }

    const uint32_t data_ptr = qos_block_ + kQosDataOffset;
    const uint32_t data_len = qos_len_ > kQosMax ? kQosMax : qos_len_;
    if (data_len) {
      GuestWrite(data_ptr, qos_, data_len);
    }

    // count, pending, one result.
    Write32(qos_block_, 1);
    Write32(qos_block_ + 4, 0);
    Write8(qos_block_ + kQosInfoOffset,
           kQosInfoComplete | kQosInfoContacted | kQosInfoDataReceived);
    Write16(qos_block_ + kQosInfoOffset + 0x02, 8);  // probes sent
    Write16(qos_block_ + kQosInfoOffset + 0x04, 8);  // probes answered
    Write16(qos_block_ + kQosInfoOffset + 0x06,
            static_cast<uint16_t>(data_len));
    Write32(qos_block_ + kQosInfoOffset + 0x08, data_ptr);
    const uint16_t real_rtt = worker_->LinkRtt();
    const uint16_t rtt = ReportedRtt(real_rtt);
    Write16(qos_block_ + kQosInfoOffset + 0x0C, rtt);
    Write16(qos_block_ + kQosInfoOffset + 0x0E, rtt);
    Write32(qos_block_ + kQosInfoOffset + 0x10, 1000000);
    Write32(qos_block_ + kQosInfoOffset + 0x14, 1000000);

    Write32(out_ptr, qos_block_);
    answered = true;
    if (verbose_) {
      XELOGI("netplay: QoS lookup -> rtt {} ms, {} data bytes", rtt, data_len);
    }
    if (cvars::netplay_delay && rtt != real_rtt) {
      XELOGI(
          "netplay: QoS rtt {} ms raised to {} ms for {} frames of input delay",
          real_rtt, rtt, DelayFramesForClass(DelayClassForRtt(rtt)));
    }
  }

  // Signal event outside link lock.
  if (answered && event_handle) {
    xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  }
  return answered;
}

void NetplayState::SearchFinish(const uint8_t* info, uint32_t open_slots) {
  const uint32_t results = search_.results;
  const uint32_t size = search_.size;
  const uint32_t count = (info && size >= 8 + kSearchResultSize) ? 1u : 0u;

  if (results) {
    Write32(results, count);
    Write32(results + 4, results + 8);
  }
  if (count && results) {
    uint8_t zero[kSearchResultSize] = {};
    GuestWrite(results + 8, zero, sizeof zero);
    GuestWrite(results + 8, info, kSessionInfoSize);
    Write32(results + 8 + kSessionInfoSize, open_slots);  // open public
    Write32(results + 8 + kSessionInfoSize + 8, 1);       // filled public
  }

  const uint32_t overlapped = search_.overlapped;
  search_.active = false;
  search_.asked = false;
  search_.overlapped = 0;

  if (verbose_) {
    XELOGI("netplay: search found {}, session data {} bytes", count, qos_len_);
  }

  if (count) {
    join_deadline_ = worker_ ? worker_->NowMs() + kJoinMs : 0;
  } else {
    join_deadline_ = 0;
    if (worker_) {
      worker_->LinkStop();
    }
  }

  if (overlapped && kernel_state_) {
    kernel_state_->CompleteDeferredOverlapped(overlapped, X_ERROR_SUCCESS);
  }
}

void NetplayState::ReleaseQosBlock() {
  if (qos_block_ && kernel_state_ && kernel_state_->memory()) {
    kernel_state_->memory()->SystemHeapFree(qos_block_);
  }
  qos_block_ = 0;
}

bool NetplayState::RangeMapped(uint32_t address, uint32_t length) const {
  if (!kernel_state_ || !kernel_state_->memory() || !length) {
    return false;
  }
  Memory* memory = kernel_state_->memory();
  if (!memory->LookupHeap(address)) {
    return false;
  }
  return memory->LookupHeap(address + length - 1) != nullptr;
}

uint8_t NetplayState::Read8(uint32_t address) const {
  if (!RangeMapped(address, sizeof(uint8_t))) {
    return 0;
  }
  return xe::load_and_swap<uint8_t>(
      kernel_state_->memory()->TranslateVirtual<const uint8_t*>(address));
}

uint16_t NetplayState::Read16(uint32_t address) const {
  if (!RangeMapped(address, sizeof(uint16_t))) {
    return 0;
  }
  return xe::load_and_swap<uint16_t>(
      kernel_state_->memory()->TranslateVirtual<const uint8_t*>(address));
}

uint32_t NetplayState::Read32(uint32_t address) const {
  if (!RangeMapped(address, sizeof(uint32_t))) {
    return 0;
  }
  return xe::load_and_swap<uint32_t>(
      kernel_state_->memory()->TranslateVirtual<const uint8_t*>(address));
}

void NetplayState::Write8(uint32_t address, uint8_t value) {
  if (!RangeMapped(address, sizeof(uint8_t))) {
    return;
  }
  xe::store_and_swap<uint8_t>(
      kernel_state_->memory()->TranslateVirtual<uint8_t*>(address), value);
}

void NetplayState::Write16(uint32_t address, uint16_t value) {
  if (!RangeMapped(address, sizeof(uint16_t))) {
    return;
  }
  xe::store_and_swap<uint16_t>(
      kernel_state_->memory()->TranslateVirtual<uint8_t*>(address), value);
}

void NetplayState::Write32(uint32_t address, uint32_t value) {
  if (!RangeMapped(address, sizeof(uint32_t))) {
    return;
  }
  xe::store_and_swap<uint32_t>(
      kernel_state_->memory()->TranslateVirtual<uint8_t*>(address), value);
}

bool NetplayState::GuestRead(uint32_t address, void* out,
                             uint32_t length) const {
  if (!length) {
    return true;
  }
  if (!out || !RangeMapped(address, length)) {
    return false;
  }
  std::memcpy(
      out, kernel_state_->memory()->TranslateVirtual<const uint8_t*>(address),
      length);
  return true;
}

bool NetplayState::GuestWrite(uint32_t address, const void* in,
                              uint32_t length) {
  if (!length) {
    return true;
  }
  if (!in || !RangeMapped(address, length)) {
    return false;
  }
  std::memcpy(kernel_state_->memory()->TranslateVirtual<uint8_t*>(address), in,
              length);
  return true;
}

void NetplayState::FillSelfXnAddr(uint8_t out[kXnAddrSize]) const {
  std::memcpy(out, self_xna_, kXnAddrSize);
}

bool NetplayState::IsSelfXnAddr(const uint8_t in[kXnAddrSize]) const {
  return std::memcmp(in, self_xna_, kXnAddrSize) == 0;
}

void NetplayState::FillDigest(uint8_t out[kDigestSize]) const {
  // Fixed tag + title id: same fork, same title.
  const char tag[] = "xenia-canary-mm10-netplay-v2";
  uint64_t state = 1469598103934665603ull;
  auto mix = [&state](uint8_t byte) {
    state ^= byte;
    state *= 1099511628211ull;
  };
  for (const char* p = tag; *p; p++) {
    mix(static_cast<uint8_t>(*p));
  }
  for (int i = 0; i < 4; i++) {
    mix(static_cast<uint8_t>(kMegaMan10TitleId >> (i * 8)));
  }

  // splitmix64.
  uint64_t x = state;
  for (uint32_t i = 0; i < kDigestSize; i++) {
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    out[i] = static_cast<uint8_t>(z);
  }
}

void NetplayState::OnSessionMessage(uint8_t kind, const uint8_t* data,
                                    unsigned length) {
  if (!worker_) {
    return;
  }

  if (kind == kSessionSearchReq) {
    // Host session + QoS; length is big-endian.
    uint8_t reply[2 + kSessionInfoSize + 2 + kQosMax];
    reply[0] = hosted_ ? 1 : 0;
    reply[1] = 1;  // open public slots
    std::memcpy(reply + 2, session_info_, kSessionInfoSize);
    reply[2 + kSessionInfoSize] = static_cast<uint8_t>(qos_len_ >> 8);
    reply[3 + kSessionInfoSize] = static_cast<uint8_t>(qos_len_);
    if (qos_len_) {
      std::memcpy(reply + 4 + kSessionInfoSize, qos_, qos_len_);
    }
    worker_->SendSession(kSessionSearchReply, reply,
                         4 + kSessionInfoSize + qos_len_);
    if (verbose_) {
      XELOGI("netplay: answered a search (hosted {}, QoS {} bytes)", hosted_,
             qos_len_);
    }
    return;
  }

  if (kind == kSessionSearchReply && search_.active) {
    if (!data || length < 4 + kSessionInfoSize) {
      if (verbose_) {
        XELOGI("netplay: a short search reply");
      }
      return;
    }
    const unsigned qos_len =
        (static_cast<unsigned>(data[2 + kSessionInfoSize]) << 8) |
        data[3 + kSessionInfoSize];
    if (qos_len > kQosMax || length != 4 + kSessionInfoSize + qos_len) {
      if (verbose_) {
        XELOGI("netplay: a bad search reply");
      }
      return;
    }
    if (qos_len) {
      std::memcpy(qos_, data + 4 + kSessionInfoSize, qos_len);
    }
    qos_len_ = qos_len;
    SearchFinish(data[0] ? data + 2 : nullptr, data[1]);
    return;
  }

  if (verbose_) {
    XELOGI("netplay: session message {} of {} bytes", kind, length);
  }
}

void NetplayState::OnStateHash(const uint8_t* data, unsigned length) {
  // Lock held.
  if (game_) {
    game_->StateCheckMessage(data, length);
  }
}

void NetplayState::CheckReset() {
  // New link: ask again.
  search_.asked = false;
  if (game_) {
    game_->StateCheckReset();
  }
}

void NetplayState::SessionTick(uint32_t now) {
  if (!worker_) {
    return;
  }

  if (search_.active) {
    if (worker_->LinkUp() && !search_.asked) {
      search_.asked = worker_->SendSession(kSessionSearchReq, nullptr, 0) == 0;
    }
    if (worker_->LinkFailed() ||
        static_cast<int32_t>(now - search_.deadline) >= 0) {
      if (verbose_) {
        XELOGI("netplay: no host answered");
      }
      SearchFinish(nullptr, 0);
    }
  }

  if (join_deadline_ && static_cast<int32_t>(now - join_deadline_) >= 0) {
    join_deadline_ = 0;
    XELOGI("netplay: the session found was not joined, letting it go");
    worker_->LinkStop();
  }
}

uint64_t NetplayState::now_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace kernel
}  // namespace xe
