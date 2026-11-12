/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NETPLAY_NETPLAY_GAME_H_
#define XENIA_KERNEL_NETPLAY_NETPLAY_GAME_H_

#include <cstddef>
#include <cstdint>

namespace xe {
namespace kernel {

class NetplayState;

// Mega Man 10 session/lockstep state. All reads/writes go through checked
// NetplayState memory helpers.
class NetplayGame {
 public:
  explicit NetplayGame(NetplayState* state);

  // Session block (821af320), 0 if none.
  uint32_t SessionBlock() const;
  // Message receiver (821b2c88 table entry).
  uint32_t Receiver() const;

  // Guest-visible message check.
  bool MessageOk(const uint8_t* d, unsigned len) const;

  // Queue lockstep packet instead of dropping on full ring (821b6ea8).
  bool Hold(const uint8_t* d, unsigned len) const;

  // Log session state/flags changes.
  void Watch();

  // Refresh member timestamps.
  void MembersHeard();

  // Age out members immediately on link close.
  void MembersGone();

  // Desync check every 60th lockstep tick; reports only.
  void StateCheckTick();  // guest thread, end of frame
  // Peer sample arrived.
  void StateCheckMessage(const uint8_t* data, unsigned length);
  void StateCheckReset();  // new link

  // Status window values.
  uint32_t LockstepTick() const;
  uint8_t InputDelayFrames() const;
  bool StateDiffering() const { return state_differing_; }
  uint32_t StateTick() const { return state_last_tick_; }

  // netplay_rtt_report: log on class/delay change only.
  void RttReport();  // guest thread

 private:
  // One hash per range.
  static constexpr unsigned kStateRanges = 4;
  struct StateSample {
    uint32_t tick = 0;
    uint64_t hash[kStateRanges] = {};
    bool used = false;
  };

  bool LockstepFits(const uint8_t* p, unsigned n) const;
  bool StateHash(uint64_t* out) const;
  void StateCompare(const StateSample& mine, const StateSample& theirs);
  // Format differing ranges for the log.
  static void DescribeDifference(char* out, size_t size, const char* what,
                                 const StateSample& mine,
                                 const StateSample& theirs, unsigned differ);

  // Member table lookup; false if no session.
  bool MemberTable(uint32_t* members, uint32_t* mask, uint8_t* local_slot,
                   uint8_t* count) const;
  // Game clock (821b8b48).
  uint32_t GameClock() const;

  // netplay_test_stall.
  void StallTest(uint32_t tick);

  NetplayState* state_ = nullptr;
  uint32_t last_state_ = 0xFFFFFFFF;
  uint32_t last_flags_ = 0;
  uint32_t heard_block_ = 0;
  uint64_t heard_log_ms_ = 0;

  // State hash history. Guarded by NetplayState::mutex().
  static constexpr unsigned kStateCheckKept = 8;
  StateSample state_mine_[kStateCheckKept];
  StateSample state_theirs_[kStateCheckKept];
  unsigned state_mine_at_ = 0;
  unsigned state_theirs_at_ = 0;
  uint32_t state_last_tick_ = 0;
  bool state_differing_ = false;
  uint32_t state_report_tick_ = 0;
  bool test_desync_done_ = false;

  // netplay_test_stall state.
  bool stall_parsed_ = false;
  int stall_tick_ = -1;
  int stall_ms_ = 0;
  bool stall_done_ = false;

  // Last netplay_rtt_report values.
  uint32_t rtt_class_ = 0xFFFFFFFFu;
  uint32_t rtt_delay_ = 0xFFFFFFFFu;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NETPLAY_NETPLAY_GAME_H_
