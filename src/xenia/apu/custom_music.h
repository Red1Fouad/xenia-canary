/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_CUSTOM_MUSIC_H_
#define XENIA_APU_CUSTOM_MUSIC_H_

#include <cstdint>
#include <string_view>

// Mega Man 10 custom music: replaces a BGM bank with custom_music/BGMnn.wav.
// The game's own music for that bank is silenced; the WAV is mixed into the
// final audio frame, so SFX and the rest of the mix are untouched.
namespace xe {
namespace apu {

// Scans custom_music/ next to the executable and caches the WAV files.
// Called once per title load.
void InitializeCustomMusic();

// True when an output path exists and a WAV is loaded for this bank, meaning
// the game's music for the bank must be silenced.
bool HasReplacementForBank(std::string_view bank_path);

// Called when the game starts / restarts / stops a bank.
void StartCustomMusic(std::string_view bank_path, bool restart = false);
void StopCustomMusic(std::string_view bank_path);

// True when this audio render client carries the game's music and must be
// silenced while a custom track is playing (see --custom_music_mute_client).
bool ShouldMuteAudioClient(int client_index);

// The audio driver that can mix the custom track announces itself here.
void SetCustomMusicOutputAvailable(bool available);

// Mixes the active custom track into an interleaved float frame.
void MixCustomMusic(float* samples, uint32_t channels, uint32_t frames,
                    uint32_t sample_rate);

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_CUSTOM_MUSIC_H_
