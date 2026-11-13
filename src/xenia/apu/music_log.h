/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_MUSIC_LOG_H_
#define XENIA_APU_MUSIC_LOG_H_

#include <cstdint>
#include <string>
#include <string_view>

// Music logging: names the wave bank behind XMA playback so tracks can be
// identified from the log. Groundwork for custom music replacement.
namespace xe {
namespace apu {

// True for paths ending in .xwb (any case).
bool IsWaveBankPath(std::string_view path);

// Registers a loaded wave bank chunk (physical guest range + file offset).
void RegisterWaveBankRange(std::string_view path, uint32_t file_offset,
                           uint32_t physical_address, uint32_t length);

// Zeroes the sample data of a replaced BGM bank in guest memory so the guest
// mixer plays silence (MM10 music is PCM mixed by the game, not XMA).
void SilenceWaveBankData(std::string_view path, uint8_t* destination,
                         uint32_t file_offset, uint32_t length);

// Logs a kicked XMA input buffer, music banks only.
void LogXmaInputBuffer(uint32_t context_id, uint32_t physical_address,
                       uint32_t packet_count, int sample_rate,
                       bool is_stereo);

// Logs a stopped XMA context (bank pointer is dropped).
void LogXmaContextStopped(uint32_t context_id);

// True when this context's output must be silenced because a custom track is
// replacing its bank.
bool ShouldMuteMusicContext(uint32_t context_id);

// Drops all ranges and playback state (title reload).
void ResetMusicLog();

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_MUSIC_LOG_H_
