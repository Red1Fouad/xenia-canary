/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_MUSIC_TRACKS_H_
#define XENIA_APU_MUSIC_TRACKS_H_

#include <string_view>

namespace xe {
namespace apu {

// Mega Man 10 BGM banks: bank id (file name without extension) -> track title.
struct MusicTrackInfo {
  std::string_view bank;
  std::string_view title;
};

constexpr MusicTrackInfo kMusicTracks[] = {
    {"BGM00", "Menu"},
    {"BGM01", "Stage Select"},
    {"BGM02", "Game Start"},
    {"BGM03", "Stage Complete"},
    {"BGM04", "You Got a Weapon"},
    {"BGM05", "Game Over"},
    {"BGM06", "Blade Man Stage"},
    {"BGM07", "Pump Man Stage"},
    {"BGM08", "Chill Man Stage"},
    {"BGM09", "Commando Man Stage"},
    {"BGM10", "Sheep Man Stage"},
    {"BGM11", "Strike Man Stage"},
    {"BGM12", "Nitro Man Stage"},
    {"BGM13", "Solar Man Stage"},
    {"BGM14", "Boss"},
    {"BGM15", "Title"},
    {"BGM16", "Wily Stage 1 Intro"},
    {"BGM17", "Wily Stage 1"},
    {"BGM18", "Wily Stage 2"},
    {"BGM19", "Wily Stage 3"},
    {"BGM20", "Wily Stage 4"},
    {"BGM21", "Wily Stage 5"},
    {"BGM22", "Wily Stage Boss"},
    {"BGM23", "Shop -Mega Man-"},
    {"BGM24", "Shop -Proto Man-"},
    {"BGM25", "Shop -Bass-"},
    {"BGM26", "Endless Stage"},
    {"BGM27", "Special Stage 1"},
    {"BGM28", "Special Stage 2"},
    {"BGM29", "Special Stage 3"},
    {"BGM30", "Wily Machine"},
    {"BGM31", "Wily Capsule"},
    {"BGM32", "Usual Day (Opening 1)"},
    {"BGM33", "Future World (Opening 2)"},
    {"BGM34", "Go Together (Opening 4)"},
    {"BGM35", "Suspicion (Opening 3)"},
    {"BGM36", "Crisis"},
    {"BGM37", "Evil Wily"},
    {"BGM38", "For You -Roll's Theme-"},
    {"BGM39", "Wily's Castle"},
    {"BGM40", "Wily's Castle (Final)"},
    {"BGM41", "After All"},
    {"BGM42", "Credits"},
    {"BGM43", "Challenges"},
    {"BGM44", "All Clear"},
};

// Title for a bank id like "BGM15"; empty when unknown.
constexpr std::string_view MusicTrackTitle(std::string_view bank) {
  for (const MusicTrackInfo& track : kMusicTracks) {
    if (track.bank == bank) {
      return track.title;
    }
  }
  return {};
}

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_MUSIC_TRACKS_H_
