/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/music_log.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xenia/apu/custom_music.h"
#include "xenia/apu/music_tracks.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DEFINE_bool(log_music, true,
            "Log wave bank loads and XMA music playback. Music banks are the "
            "BGMnn.xwb files.",
            "Logging");

namespace xe {
namespace apu {
namespace {

constexpr size_t kMaxBankRanges = 1024;

struct BankRange {
  std::string path;
  uint32_t file_offset;
  uint32_t address;  // Physical guest range.
  uint32_t length;
};

struct PlayState {
  std::string path;
  uint32_t offset;
};

std::mutex state_mutex_;
std::vector<BankRange> bank_ranges_;
std::vector<std::string> logged_banks_;
std::map<uint32_t, PlayState> playing_;
std::string last_music_bank_;
std::map<std::string, std::pair<uint32_t, uint32_t>> bank_data_segments_;
int kick_log_budget_ = 32;

std::string_view FileName(std::string_view path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// "BGM15.xwb" -> "BGM15".
std::string_view BankId(std::string_view path) {
  std::string_view name = FileName(path);
  return name.size() >= 5 ? name.substr(0, 5) : std::string_view();
}

// "BGM15.xwb (Title)" for the log; unknown banks still name the file.
std::string Label(std::string_view path) {
  std::string label(FileName(path));
  std::string_view title = MusicTrackTitle(BankId(path));
  if (title.empty()) {
    label += " (unknown)";
  } else {
    label += " (";
    label += title;
    label += ")";
  }
  return label;
}

bool SamePath(const std::string& a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool IsMusicBank(std::string_view path) {
  std::string_view name = FileName(path);
  if (name.size() < 3) {
    return false;
  }
  return (name[0] | 0x20) == 'b' && (name[1] | 0x20) == 'g' &&
         (name[2] | 0x20) == 'm';
}

// Returns the bank and byte offset for an XMA input buffer, if it is loaded.
bool LookupBank(uint32_t address, std::string* out_path,
                uint32_t* out_offset) {
  for (const BankRange& range : bank_ranges_) {
    if (address >= range.address && address < range.address + range.length) {
      *out_path = range.path;
      *out_offset = range.file_offset + (address - range.address);
      return true;
    }
  }
  return false;
}

}  // namespace

bool IsWaveBankPath(std::string_view path) {
  std::string_view name = FileName(path);
  if (name.size() < 4) {
    return false;
  }
  name = name.substr(name.size() - 4);
  return name[0] == '.' && (name[1] | 0x20) == 'x' && (name[2] | 0x20) == 'w' &&
         (name[3] | 0x20) == 'b';
}

void RegisterWaveBankRange(std::string_view path, uint32_t file_offset,
                           uint32_t address, uint32_t length) {
  if (!length) {
    return;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);

  // Chunked loads: extend the previous chunk when it is contiguous.
  for (BankRange& range : bank_ranges_) {
    if (range.address + range.length == address &&
        range.file_offset + range.length == file_offset &&
        SamePath(range.path, path)) {
      range.length += length;
      return;
    }
  }

  // The memory was reused for something else.
  const uint64_t new_first = address;
  const uint64_t new_last = uint64_t(address) + length;
  for (auto it = bank_ranges_.begin(); it != bank_ranges_.end();) {
    const uint64_t first = it->address;
    const uint64_t last = uint64_t(it->address) + it->length;
    if (first < new_last && new_first < last) {
      it = bank_ranges_.erase(it);
    } else {
      ++it;
    }
  }
  if (bank_ranges_.size() >= kMaxBankRanges) {
    bank_ranges_.erase(bank_ranges_.begin());
  }
  bank_ranges_.push_back({std::string(path), file_offset, address, length});

  // A new read of a music bank is the track start signal for MM10 (the music
  // is mixed by the guest, so there are no XMA kicks to hook). Chunked reads
  // of the same bank do not restart the track.
  if (IsMusicBank(path)) {
    if (!SamePath(last_music_bank_, path)) {
      last_music_bank_ = std::string(path);
      StartCustomMusic(path, true);
    }
  }

  // One line per bank, chunked reads stay quiet.
  for (const std::string& logged : logged_banks_) {
    if (SamePath(logged, path)) {
      return;
    }
  }
  logged_banks_.emplace_back(path);
  if (cvars::log_music && IsMusicBank(path)) {
    XELOGI("music: bank {} at 0x{:08X}-0x{:08X} ({} bytes)", Label(path),
           address, address + length, length);
  }
}

void LogXmaInputBuffer(uint32_t context_id, uint32_t physical_address,
                       uint32_t packet_count, int sample_rate,
                       bool is_stereo) {
  std::lock_guard<std::mutex> lock(state_mutex_);

  std::string path;
  uint32_t offset = 0;
  const bool matched = LookupBank(physical_address, &path, &offset);
  if (cvars::log_music && kick_log_budget_ > 0) {
    // Bounded trace so the first kicks can be compared with the bank ranges.
    --kick_log_budget_;
    XELOGI("music: kick ctx {} buf 0x{:08X} ({} pkts, {} Hz) -> {}", context_id,
           physical_address, packet_count, sample_rate,
           matched ? Label(path) : "no bank");
  }
  if (!packet_count) {
    return;
  }
  if (!matched) {
    if (cvars::log_music) {
      XELOGD("music: ctx {} buffer 0x{:08X} is not a loaded bank ({} pkts)",
             context_id, physical_address, packet_count);
    }
    return;
  }
  if (!IsMusicBank(path)) {
    // Sound effects are not interesting for track identification.
    if (cvars::log_music) {
      XELOGD("music: ctx {} sfx {} +0x{:X}", context_id, FileName(path),
             offset);
    }
    return;
  }

  auto it = playing_.find(context_id);
  const bool same_track =
      it != playing_.end() && SamePath(it->second.path, path);
  if (same_track && offset >= it->second.offset) {
    // Same track, still streaming forward.
    it->second.offset = offset;
    return;
  }
  const bool restarted = same_track;
  playing_[context_id] = PlayState{path, offset};
  if (cvars::log_music) {
    XELOGI("music: {} {} +0x{:X} (ctx {}, {} pkts, {} Hz, {})",
           restarted ? "restart" : "play", Label(path), offset, context_id,
           packet_count, sample_rate, is_stereo ? "stereo" : "mono");
  }
  StartCustomMusic(path);
}

void LogXmaContextStopped(uint32_t context_id) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  auto it = playing_.find(context_id);
  if (it == playing_.end()) {
    return;
  }
  if (cvars::log_music) {
    XELOGI("music: stop {} (ctx {})", Label(it->second.path), context_id);
  }
  StopCustomMusic(it->second.path);
  playing_.erase(it);
}

void SilenceWaveBankData(std::string_view path, uint8_t* destination,
                         uint32_t file_offset, uint32_t length) {
  if (!length || !destination || !IsMusicBank(path) ||
      !HasReplacementForBank(path)) {
    return;
  }

  std::lock_guard<std::mutex> lock(state_mutex_);

  // XWB header: 'WBND', then five big-endian {offset, length} segments. The
  // largest segment is the wave data.
  const std::string key(path);
  auto it = bank_data_segments_.find(key);
  if (it == bank_data_segments_.end()) {
    // Headers are only parsed from the first read of the file; the on-disk
    // magic is 'DNBW' (byte-reversed 'WBND').
    const bool header_present =
        file_offset == 0 && length >= 0x34 &&
        (std::memcmp(destination, "DNBW", 4) == 0 ||
         std::memcmp(destination, "WBND", 4) == 0);
    if (!header_present) {
      return;
    }
    auto read_be32 = [](const uint8_t* p) {
      return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
             (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    };
    uint32_t data_offset = 0;
    uint32_t data_length = 0;
    for (int i = 0; i < 5; ++i) {
      const uint32_t offset = read_be32(destination + 12 + i * 8);
      const uint32_t segment_length = read_be32(destination + 16 + i * 8);
      if (segment_length > data_length) {
        data_offset = offset;
        data_length = segment_length;
      }
    }
    if (data_offset < 0x34 || !data_length) {
      return;
    }
    it = bank_data_segments_.emplace(key, std::make_pair(data_offset,
                                                         data_length))
             .first;
  }

  const uint64_t read_begin = file_offset;
  const uint64_t read_end = read_begin + length;
  const uint64_t data_begin = it->second.first;
  const uint64_t data_end = data_begin + it->second.second;
  const uint64_t begin = std::max(read_begin, data_begin);
  const uint64_t end = std::min(read_end, data_end);
  if (end <= begin) {
    return;
  }
  std::memset(destination + (begin - read_begin), 0, size_t(end - begin));
  if (cvars::log_music) {
    XELOGI("custom music: silenced {} ({} bytes)", Label(path),
           static_cast<uint64_t>(end - begin));
  }
}

bool ShouldMuteMusicContext(uint32_t context_id) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  auto it = playing_.find(context_id);
  if (it == playing_.end()) {
    return false;
  }
  return HasReplacementForBank(it->second.path);
}

void ResetMusicLog() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  bank_ranges_.clear();
  logged_banks_.clear();
  playing_.clear();
  last_music_bank_.clear();
  bank_data_segments_.clear();
  kick_log_budget_ = 32;
}

}  // namespace apu
}  // namespace xe
