/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/netplay/netplay_game.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/netplay/netplay_state.h"
#include "xenia/kernel/netplay/netplay_worker.h"

DECLARE_uint32(netplay_test_desync);
DECLARE_string(netplay_test_stall);
DECLARE_bool(netplay_rtt_report);

namespace xe {
namespace kernel {

namespace {

constexpr uint32_t kGameMsg = 0x4F0;
constexpr uint32_t kSessionSlots = 0x8752E984u;
constexpr uint32_t kSessionSlotsEnd = 0x8752E994u;
constexpr uint32_t kLockstepRoom = 0x8752E910u;
constexpr uint32_t kLockstepActive = 0x8752E8E8u;
constexpr uint32_t kLockstepHead = 0x8752E902u;
constexpr uint32_t kLockstepTail = 0x8752E904u;
constexpr uint32_t kLockstepSize = 0x8752E900u;
constexpr uint32_t kLockstepReceiver = 0x821B6EA8u;

// Input delay frames (820c51a0).
constexpr uint32_t kInputDelayByte = 0x875BEC26u;

// Game clock (821b8b48), reads [820006B0]+0x10.
constexpr uint32_t kGameClockRoot = 0x820006B0u;

// Lockstep cursor (821b79a0).
constexpr uint32_t kLockstepCursor = 0x8752E8D8u;

// Desync pacing (xb_net_check.c).
constexpr uint32_t kStateCheckEvery = 60;   // ticks between samples
constexpr uint32_t kStateCheckNote = 3600;  // "in step" log interval
constexpr uint8_t kLinkStateHash = 7;       // NetplaySockets::Kind::kStateHash

// Compared ranges: game state, not camera/sprites/pool/audio.
struct StateRange {
  uint32_t lo, hi;
  const char* name;
};
constexpr StateRange kStateRangeTable[] = {
    {0x874C0000u, 0x874C0EB0u, "state A"},
    {0x874C0EC8u, 0x874C3000u, "state B"},
    {0x87543000u, 0x87544000u, "score"},
    {0x875A9000u, 0x875AA100u, "hit boxes"},
};
static_assert(sizeof(kStateRangeTable) / sizeof(kStateRangeTable[0]) == 4,
              "one hash per range");

// Same words on both sides.
struct StateWire {
  uint32_t tick;
  uint32_t pad;
  uint64_t hash[4];
};
static_assert(sizeof(StateWire) == 40, "state hash wire format");

}  // namespace

NetplayGame::NetplayGame(NetplayState* state) : state_(state) {}

uint32_t NetplayGame::SessionBlock() const {
  for (uint32_t slot = kSessionSlots; slot < kSessionSlotsEnd; slot += 4) {
    const uint32_t block = state_->Read32(slot);
    if (block && state_->Read8(block) != 3 &&
        (state_->Read32(block + 8) & 0xF) == 1) {
      return block;
    }
  }
  return 0;
}

uint32_t NetplayGame::Receiver() const {
  const uint32_t block = SessionBlock();
  return block ? state_->Read32(state_->Read32(block + 0xC) + 0x28) : 0;
}

bool NetplayGame::LockstepFits(const uint8_t* p, unsigned n) const {
  unsigned at = 4, out = 4;
  const unsigned room = state_->Read16(kLockstepRoom);
  if (n < 4) {
    return false;
  }
  if (!(p[3] & 1)) {
    return n <= room;
  }
  while (at < n) {
    const unsigned code = p[at++];
    const unsigned count = code & 0x7F;
    if (code & 0x80) {
      if (at >= n) {
        return false;
      }
      at++;
    } else {
      if (count > n - at) {
        return false;
      }
      at += count;
    }
    out += count;
    if (out > room) {
      return false;
    }
  }
  return true;
}

bool NetplayGame::MessageOk(const uint8_t* d, unsigned len) const {
  if (len < 8 || len > kGameMsg) {
    return false;
  }
  const unsigned type = (unsigned)d[4] >> 4;
  if (type == 7) {
    return (d[3] & 0x1F) < 2 &&
           (Receiver() != kLockstepReceiver || LockstepFits(d + 8, len - 8));
  }
  return type != 10 && type != 11;
}

bool NetplayGame::Hold(const uint8_t* d, unsigned len) const {
  if (len < 8 || (unsigned)d[4] >> 4 != 7 || Receiver() != kLockstepReceiver ||
      state_->Read32(kLockstepActive) != 1) {
    return false;
  }
  const uint8_t used =
      (uint8_t)(state_->Read8(kLockstepTail) - state_->Read8(kLockstepHead));
  return used >= state_->Read8(kLockstepSize);
}

void NetplayGame::Watch() {
  const uint32_t block = SessionBlock();
  const uint32_t session_state = block ? state_->Read8(block) : 0xFFu;
  const uint32_t flags = block ? state_->Read32(block + 8) : 0;
  if (session_state == last_state_ && flags == last_flags_) {
    return;
  }
  last_state_ = session_state;
  last_flags_ = flags;
  XELOGI("netplay: session state {} flags {:08X}", session_state, flags);
}

bool NetplayGame::MemberTable(uint32_t* members, uint32_t* mask,
                              uint8_t* local_slot, uint8_t* count) const {
  const uint32_t block = SessionBlock();
  if (!block || !(state_->Read32(block + 8) & 0x200)) {
    return false;
  }
  const uint32_t inner = state_->Read32(block + 0xC);
  if (!inner) {
    return false;
  }
  *members = state_->Read32(inner + 0xDC);
  *mask = state_->Read32(inner + 0xC); /* low half: at most 4 */
  *local_slot = state_->Read8(inner + 3);
  *count = state_->Read8(inner + 1);
  return *members != 0 && *local_slot < 32;
}

uint32_t NetplayGame::GameClock() const {
  const uint32_t root = state_->Read32(kGameClockRoot);
  return root ? state_->Read32(root + 0x10) : 0;
}

void NetplayGame::MembersHeard() {
  /* Refresh peer timestamps using the game clock. Sampling a member's own
   * stamp can freeze and let the peer age out. */
  if (!state_->worker()->GameAlive()) {
    return;
  }
  uint32_t members = 0, mask = 0;
  uint8_t local_slot = 0, member_count = 0;
  if (!MemberTable(&members, &mask, &local_slot, &member_count)) {
    return;
  }

  uint32_t now = GameClock();
  if (!now) {
    /* No clock: local slot stamp. */
    now = state_->Read32(members + local_slot * 0x18 + 0xC);
  }
  if (!now) {
    now = static_cast<uint32_t>(NetplayState::now_ms());
  }

  for (uint32_t slot = 0; slot < member_count && slot < 32; slot++) {
    if (!(mask >> slot & 1) || slot == local_slot) {
      continue;
    }
    state_->Write32(members + slot * 0x18 + 0xC, now);
  }
  if (heard_block_ != members) {
    heard_block_ = members;
    XELOGI("netplay: keeping members heard (local slot {}, game clock {})",
           local_slot, now);
  }
  if (state_->verbose()) {
    const uint64_t real = NetplayState::now_ms();
    if (real - heard_log_ms_ >= 2000) {
      heard_log_ms_ = real;
      char line[192];
      int at = std::snprintf(line, sizeof line,
                             "netplay: members heard clock %u mask %X slots",
                             now, mask);
      for (uint32_t slot = 0; slot < member_count && slot < 32 && at > 0 &&
                              at < static_cast<int>(sizeof line);
           slot++) {
        if (!(mask >> slot & 1)) {
          continue;
        }
        at += std::snprintf(line + at, sizeof line - at, " %u:%u", slot,
                            state_->Read32(members + slot * 0x18 + 0xC));
      }
      XELOGI("{}", line);
    }
  }
}

void NetplayGame::MembersGone() {
  uint32_t members = 0, mask = 0;
  uint8_t local_slot = 0, member_count = 0;
  if (!MemberTable(&members, &mask, &local_slot, &member_count)) {
    return;
  }
  uint32_t now = GameClock();
  if (!now) {
    now = state_->Read32(members + local_slot * 0x18 + 0xC);
  }
  if (!now) {
    /* No clock to age members with. */
    return;
  }
  /* Age over 0x1388 so 821b58c8 removes the member. */
  const uint32_t gone = now - 0x1389u;
  for (uint32_t slot = 0; slot < member_count && slot < 32; slot++) {
    if (!(mask >> slot & 1) || slot == local_slot) {
      continue;
    }
    state_->Write32(members + slot * 0x18 + 0xC, gone);
  }
  XELOGI("netplay: link over, ageing member timestamps so the game lets go");
}

bool NetplayGame::StateHash(uint64_t* out) const {
  uint8_t buffer[0x2200];  // largest range 0x2138
  for (unsigned r = 0; r < kStateRanges; r++) {
    const StateRange& range = kStateRangeTable[r];
    const uint32_t length = range.hi - range.lo;
    if (!state_->GuestRead(range.lo, buffer, length)) {
      return false;
    }
    uint64_t h = 0xCBF29CE484222325ull;  // FNV-1a
    for (uint32_t i = 0; i < length; i++) {
      h = (h ^ buffer[i]) * 0x100000001B3ull;
    }
    out[r] = h;
  }
  return true;
}

void NetplayGame::DescribeDifference(char* out, size_t size, const char* what,
                                     const StateSample& mine,
                                     const StateSample& theirs,
                                     unsigned differ) {
  int at =
      std::snprintf(out, size, "netplay: %s at tick %u (", what, mine.tick);
  const char* sep = "";
  for (unsigned r = 0;
       r < kStateRanges && at > 0 && at < static_cast<int>(size); r++) {
    if (!(differ >> r & 1)) {
      continue;
    }
    at += std::snprintf(out + at, size - static_cast<size_t>(at),
                        "%s%s: ours %016llX, theirs %016llX", sep,
                        kStateRangeTable[r].name,
                        static_cast<unsigned long long>(mine.hash[r]),
                        static_cast<unsigned long long>(theirs.hash[r]));
    sep = "; ";
  }
  if (at > 0 && at < static_cast<int>(size)) {
    std::snprintf(out + at, size - static_cast<size_t>(at), ")");
  }
}

void NetplayGame::StateCompare(const StateSample& mine,
                               const StateSample& theirs) {
  unsigned differ = 0;
  for (unsigned r = 0; r < kStateRanges; r++) {
    if (mine.hash[r] != theirs.hash[r]) {
      differ |= 1u << r;
    }
  }

  if (!differ) {
    if (state_differing_) {
      state_differing_ = false;
      XELOGI("netplay: back in step at tick {}", mine.tick);
    } else if (mine.tick % kStateCheckNote == 0) {
      XELOGI("netplay: in step at tick {}", mine.tick);
    }
    return;
  }

  /* Report a new difference at once; repeat an ongoing one once a minute. */
  const bool first = !state_differing_;
  state_differing_ = true;
  if (!first && mine.tick < state_report_tick_ + kStateCheckNote) {
    return;
  }
  state_report_tick_ = mine.tick;

  char detail[320];
  DescribeDifference(detail, sizeof detail,
                     first ? "DESYNC" : "still differing", mine, theirs,
                     differ);
  XELOGE("{}", detail);
}

void NetplayGame::StallTest(uint32_t tick) {
  if (!stall_parsed_) {
    stall_parsed_ = true;
    const std::string& text = cvars::netplay_test_stall;
    int at = -1;
    int ms = 0;
    if (!text.empty() && std::sscanf(text.c_str(), "%d,%d", &at, &ms) == 2 &&
        at >= 0 && ms > 0) {
      stall_tick_ = at;
      stall_ms_ = ms;
    }
  }
  if (stall_tick_ < 0 || stall_done_ ||
      tick < static_cast<uint32_t>(stall_tick_)) {
    return;
  }
  stall_done_ = true;
  XELOGI("netplay: TEST: stopping the game at tick {} for {} ms", tick,
         stall_ms_);
  xe::threading::Sleep(std::chrono::milliseconds(stall_ms_));
}

void NetplayGame::RttReport() {
  if (!cvars::netplay_rtt_report) {
    return;
  }
  const uint32_t rtt = state_->worker()->LinkRtt();
  const uint16_t reported =
      NetplayState::ReportedRtt(static_cast<uint16_t>(rtt));
  const uint32_t delay_class = NetplayState::DelayClassForRtt(reported);
  const uint32_t delay = state_->Read8(kInputDelayByte);
  if (delay_class == rtt_class_ && delay == rtt_delay_) {
    return;
  }
  rtt_class_ = delay_class;
  rtt_delay_ = delay;
  XELOGI(
      "netplay: rtt {} ms, reporting {} ms, class {} ({} frames); game input "
      "delay {} frames",
      rtt, reported, delay_class,
      NetplayState::DelayFramesForClass(delay_class), delay);
}

void NetplayGame::StateCheckTick() {
  const uint32_t tick = state_->Read32(kLockstepCursor);
  if (tick) {
    // Stall before taking the link lock.
    StallTest(tick);
  }
  if (!tick) {
    return;
  }

  std::lock_guard<std::mutex> lock(state_->mutex());
  if (!state_->worker()->LinkUp()) {
    return;  // no peer
  }
  RttReport();
  if (tick == state_last_tick_) {
    return;
  }
  if (tick < state_last_tick_) {
    /* Stage restarted. */
    std::memset(state_mine_, 0, sizeof state_mine_);
    std::memset(state_theirs_, 0, sizeof state_theirs_);
    state_mine_at_ = state_theirs_at_ = 0;
    state_differing_ = false;
    state_report_tick_ = 0;
  }
  state_last_tick_ = tick;

  /* Planted difference for testing. */
  const uint32_t plant = cvars::netplay_test_desync;
  if (plant && !test_desync_done_ && tick >= plant) {
    state_->Write8(0x874C0000u, state_->Read8(0x874C0000u) ^ 0x10);
    test_desync_done_ = true;
    XELOGI("netplay: planted a state difference at tick {}", tick);
  }

  if (tick % kStateCheckEvery) {
    return;
  }

  uint64_t hash[NetplayGame::kStateRanges];
  if (!StateHash(hash)) {
    return;
  }
  StateSample* mine = &state_mine_[state_mine_at_++ % kStateCheckKept];
  mine->tick = tick;
  std::memcpy(mine->hash, hash, sizeof mine->hash);
  mine->used = true;

  StateWire wire = {};
  wire.tick = tick;
  std::memcpy(wire.hash, hash, sizeof wire.hash);
  state_->worker()->SendSession(kLinkStateHash, &wire, sizeof wire);

  for (const StateSample& theirs : state_theirs_) {
    if (theirs.used && theirs.tick == tick) {
      StateCompare(*mine, theirs);
      break;
    }
  }
}

void NetplayGame::StateCheckMessage(const uint8_t* data, unsigned length) {
  if (length != sizeof(StateWire)) {
    return;
  }
  StateWire wire;
  std::memcpy(&wire, data, sizeof wire);
  if (!wire.tick) {
    return;
  }
  StateSample* theirs = &state_theirs_[state_theirs_at_++ % kStateCheckKept];
  theirs->tick = wire.tick;
  std::memcpy(theirs->hash, wire.hash, sizeof theirs->hash);
  theirs->used = true;
  for (const StateSample& mine : state_mine_) {
    if (mine.used && mine.tick == wire.tick) {
      StateCompare(mine, *theirs);
      break;
    }
  }
}

uint32_t NetplayGame::LockstepTick() const {
  return state_->Read32(kLockstepCursor);
}

uint8_t NetplayGame::InputDelayFrames() const {
  return state_->Read8(kInputDelayByte);
}

void NetplayGame::StateCheckReset() {
  std::memset(state_mine_, 0, sizeof state_mine_);
  std::memset(state_theirs_, 0, sizeof state_theirs_);
  state_mine_at_ = state_theirs_at_ = 0;
  state_last_tick_ = 0;
  state_differing_ = false;
  state_report_tick_ = 0;
  test_desync_done_ = false;
  stall_done_ = false;
  rtt_class_ = 0xFFFFFFFFu;
  rtt_delay_ = 0xFFFFFFFFu;
}

}  // namespace kernel
}  // namespace xe
