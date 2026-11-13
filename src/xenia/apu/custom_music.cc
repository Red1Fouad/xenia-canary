/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/custom_music.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "xenia/apu/music_tracks.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"

DEFINE_bool(custom_music, true,
            "Replace Mega Man 10 background music with custom_music/BGMnn.wav "
            "files next to the executable.",
            "APU");

DEFINE_double(custom_music_volume, 0.8,
              "Volume of custom music tracks (0.0 - 1.0).", "APU");

DEFINE_int32(custom_music_mute_client, -1,
             "Audio render client that carries the game music; its frames are "
             "silenced while a custom track plays. -1 = do not mute anything.",
             "APU");

namespace xe {
namespace apu {
namespace {

constexpr uint32_t kOutputRate = 48000;
constexpr size_t kFadeFrames = 240;  // ~5 ms, avoids clicks on start/stop.
constexpr size_t kMaxInputBytes = size_t(256) << 20;
constexpr double kMaxTrackSeconds = 8.0 * 60.0;

struct Track {
  std::string file;        // Original file name, for logging.
  std::vector<float> pcm;  // Interleaved stereo at kOutputRate.
  size_t loop_start = 0;   // Frames in pcm; intro plays once before this.
  size_t loop_end = 0;     // 0 = loop the whole track.
};

struct LoopPoints {
  size_t start = 0;
  size_t end = 0;
  bool valid = false;
};

std::mutex state_mutex_;
std::map<std::string, Track> tracks_;
const Track* current_ = nullptr;
std::string current_bank_;
size_t cursor_ = 0;
float gain_ = 0.0f;
bool fading_out_ = false;
bool output_available_ = false;

uint16_t ReadU16(const uint8_t* p) {
  return uint16_t(p[0] | (uint16_t(p[1]) << 8));
}

uint32_t ReadU32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

std::string FileName(std::string_view path) {
  size_t slash = path.find_last_of("/\\");
  std::string_view name =
      slash == std::string_view::npos ? path : path.substr(slash + 1);
  return std::string(name);
}

// "BGM15.xwb" or "BGM15.wav" -> "BGM15"; empty for anything else.
std::string BankId(std::string_view path) {
  std::string name = FileName(path);
  if (name.size() < 5) {
    return {};
  }
  std::string id = name.substr(0, 5);
  for (char& c : id) {
    c = char(std::toupper(static_cast<unsigned char>(c)));
  }
  if (id[0] != 'B' || id[1] != 'G' || id[2] != 'M') {
    return {};
  }
  const std::string_view digits = std::string_view(id).substr(3, 2);
  if (digits.size() != 2 ||
      !std::isdigit(static_cast<unsigned char>(digits[0])) ||
      !std::isdigit(static_cast<unsigned char>(digits[1]))) {
    return {};
  }
  return id;
}

// "Menu.wav" -> "BGM00" by matching the track title, so friendly names work
// as well as BGMnn.wav. Punctuation and case are ignored.
std::string BankIdFromTitle(std::string_view path) {
  std::string name = FileName(path);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos) {
    name.resize(dot);
  }
  std::string key;
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      key += char(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  if (key.empty()) {
    return {};
  }
  for (const MusicTrackInfo& track : kMusicTracks) {
    std::string title;
    for (char c : track.title) {
      if (std::isalnum(static_cast<unsigned char>(c))) {
        title += char(std::tolower(static_cast<unsigned char>(c)));
      }
    }
    if (title == key) {
      return std::string(track.bank);
    }
  }
  return {};
}

bool HasWavExtension(std::string_view name) {
  if (name.size() < 4) {
    return false;
  }
  const size_t dot = name.size() - 4;
  return name[dot] == '.' && (name[dot + 1] | 0x20) == 'w' &&
         (name[dot + 2] | 0x20) == 'a' && (name[dot + 3] | 0x20) == 'v';
}

bool ParseNumber(const std::string& text, size_t* value) {
  if (text.empty()) {
    return false;
  }
  size_t number = 0;
  for (char c : text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
    number = number * 10 + size_t(c - '0');
  }
  *value = number;
  return true;
}

// Optional "<wav>.txt" sidecar with loopstart=/loopend= in source frames.
// BGMnn.txt is accepted as a fallback name for title aliases (Menu.wav).
bool LoadLoopPoints(const std::filesystem::path& wav_path,
                    const std::string& bank_id, LoopPoints* loop) {
  std::filesystem::path path = wav_path;
  path.replace_extension(".txt");
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file && !bank_id.empty()) {
    path = wav_path.parent_path() / (bank_id + ".txt");
    file = xe::filesystem::OpenFile(path, "rb");
  }
  if (!file) {
    return false;
  }
  char buffer[1024];
  const size_t read = fread(buffer, 1, sizeof(buffer) - 1, file);
  fclose(file);

  const std::string text(buffer, read);
  bool have_start = false;
  bool have_end = false;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) {
      eol = text.size();
    }
    const std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;

    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string key;
    for (char c : line.substr(0, eq)) {
      const unsigned char u = static_cast<unsigned char>(c);
      if (!std::isspace(u) && c != '_' && c != '-') {
        key += char(std::tolower(u));
      }
    }
    std::string value = line.substr(eq + 1);
    const size_t comment = value.find_first_of("#;");
    if (comment != std::string::npos) {
      value.resize(comment);
    }
    std::string digits;
    for (char c : value) {
      if (!std::isspace(static_cast<unsigned char>(c))) {
        digits += c;
      }
    }
    size_t number = 0;
    if (!ParseNumber(digits, &number)) {
      continue;
    }
    if (key == "loopstart") {
      loop->start = number;
      have_start = true;
    } else if (key == "loopend") {
      loop->end = number;
      have_end = true;
    }
  }

  loop->valid = have_start && have_end && loop->end > loop->start;
  return loop->valid;
}

float ReadSample(const uint8_t* p, uint16_t format, uint16_t bits) {
  if (format == 3) {
    float value = 0.0f;
    std::memcpy(&value, p, sizeof(value));
    return value;
  }
  switch (bits) {
    case 8:
      return (int(p[0]) - 128) * (1.0f / 128.0f);
    case 16:
      return int16_t(p[0] | (p[1] << 8)) * (1.0f / 32768.0f);
    case 24: {
      int32_t value = int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                              (uint32_t(p[2]) << 16));
      if (value & 0x800000) {
        value -= 0x1000000;
      }
      return float(value) * (1.0f / 8388608.0f);
    }
    case 32: {
      int32_t value;
      std::memcpy(&value, p, sizeof(value));
      return float(value) * (1.0f / 2147483648.0f);
    }
    default:
      return 0.0f;
  }
}

// Reads a RIFF/WAVE file and stores it as interleaved stereo at kOutputRate.
bool LoadWaveFile(const std::filesystem::path& path,
                  const std::string& bank_id, Track* track) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }
  xe::filesystem::Seek(file, 0, SEEK_END);
  const int64_t file_size = xe::filesystem::Tell(file);
  xe::filesystem::Seek(file, 0, SEEK_SET);
  if (file_size < 44 || uint64_t(file_size) > kMaxInputBytes) {
    fclose(file);
    return false;
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(file_size));
  const size_t read = fread(bytes.data(), 1, bytes.size(), file);
  fclose(file);
  if (read != bytes.size()) {
    return false;
  }

  if (std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
      std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
    return false;
  }

  uint16_t format = 0;
  uint16_t channels = 0;
  uint16_t bits = 0;
  uint32_t sample_rate = 0;
  const uint8_t* samples = nullptr;
  size_t samples_size = 0;

  size_t offset = 12;
  while (offset + 8 <= bytes.size()) {
    const uint8_t* chunk = bytes.data() + offset;
    const uint32_t chunk_size = ReadU32(chunk + 4);
    const size_t body = offset + 8;
    if (body > bytes.size()) {
      break;
    }
    size_t available = bytes.size() - body;
    if (available > chunk_size) {
      available = chunk_size;
    }
    if (std::memcmp(chunk, "fmt ", 4) == 0 && available >= 16) {
      format = ReadU16(bytes.data() + body);
      channels = ReadU16(bytes.data() + body + 2);
      sample_rate = ReadU32(bytes.data() + body + 4);
      bits = ReadU16(bytes.data() + body + 14);
      // WAVE_FORMAT_EXTENSIBLE: real tag is the first word of the subformat.
      if (format == 0xFFFE && available >= 40) {
        format = ReadU16(bytes.data() + body + 24);
      }
    } else if (std::memcmp(chunk, "data", 4) == 0) {
      samples = bytes.data() + body;
      samples_size = available;
    }
    offset = body + chunk_size + (chunk_size & 1);
  }

  if (!samples || !channels || !sample_rate || !bits) {
    return false;
  }
  if (format != 1 && format != 3) {
    return false;
  }
  if ((format == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
      (format == 3 && bits != 32)) {
    return false;
  }

  const size_t bytes_per_sample = bits / 8;
  const size_t block = size_t(channels) * bytes_per_sample;
  const size_t frames = samples_size / block;
  if (!frames) {
    return false;
  }

  LoopPoints loop;
  LoadLoopPoints(path, bank_id, &loop);

  // Convert to interleaved stereo float at the source rate first.
  std::vector<float> stereo(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    const uint8_t* frame = samples + i * block;
    const float left = ReadSample(frame, format, bits);
    const float right =
        channels > 1 ? ReadSample(frame + bytes_per_sample, format, bits)
                     : left;
    stereo[i * 2] = left;
    stereo[i * 2 + 1] = right;
  }

  size_t out_frames = size_t(double(frames) * kOutputRate / sample_rate);
  const size_t max_frames = size_t(kMaxTrackSeconds * kOutputRate);
  if (out_frames > max_frames) {
    XELOGW("custom music: {} is longer than {} minutes, truncating",
           xe::path_to_utf8(path.filename()), int(kMaxTrackSeconds / 60.0));
    out_frames = max_frames;
  }
  if (!out_frames) {
    return false;
  }

  // Loop points are given in source sample frames; rescale to output frames.
  if (loop.valid) {
    const double scale = double(kOutputRate) / double(sample_rate);
    size_t start = size_t(std::llround(double(loop.start) * scale));
    size_t end = size_t(std::llround(double(loop.end) * scale));
    if (end > out_frames) {
      end = out_frames;
    }
    if (start < end) {
      track->loop_start = start;
      track->loop_end = end;
    } else {
      XELOGW("custom music: ignoring loop points in {} (start {} >= end {})",
             xe::path_to_utf8(path.filename()), loop.start, loop.end);
    }
  }

  track->file = xe::path_to_utf8(path.filename());
  if (sample_rate == kOutputRate && out_frames == frames) {
    track->pcm = std::move(stereo);
  } else {
    track->pcm.resize(out_frames * 2);
    if (sample_rate == kOutputRate) {
      std::memcpy(track->pcm.data(), stereo.data(),
                  out_frames * 2 * sizeof(float));
    } else {
      const double step = double(sample_rate) / double(kOutputRate);
      for (size_t i = 0; i < out_frames; ++i) {
        const double position = double(i) * step;
        size_t index = size_t(position);
        if (index >= frames) {
          index = frames - 1;
        }
        const size_t next = std::min(index + 1, frames - 1);
        const float frac = float(position - double(index));
        for (size_t channel = 0; channel < 2; ++channel) {
          const float a = stereo[index * 2 + channel];
          const float b = stereo[next * 2 + channel];
          track->pcm[i * 2 + channel] = a + (b - a) * frac;
        }
      }
    }
  }

  XELOGI("custom music: loaded {} ({} Hz, {} ch, {} bits -> {} frames)",
         track->file, sample_rate, channels, bits, out_frames);
  if (track->loop_end) {
    XELOGI("custom music: {} loops {}-{} of {} frames", track->file,
           track->loop_start, track->loop_end, out_frames);
  }
  return true;
}

void WriteReadme(const std::filesystem::path& path) {
  std::string text;
  text += "Mega Man 10 - custom music\r\n";
  text += "==========================\r\n\r\n";
  text += "Put WAV files in this folder named after the track they replace.\r\n";
  text += "Only the tracks you provide are replaced; everything else keeps\r\n";
  text += "the original game music. Example: BGM15.wav replaces the title\r\n";
  text += "screen music.\r\n\r\n";
  text += "Format: mono or stereo WAV, 8/16/24/32-bit PCM or 32-bit float.\r\n";
  text += "Any sample rate is converted to 48 kHz. Tracks loop automatically.\r\n";
  text += "You can also name a file after its title, e.g. Menu.wav or\r\n";
  text += "Boss.wav - BGMnn.wav wins if both exist.\r\n\r\n";
  text += "Loop points (optional): put a text file next to the WAV with the\r\n";
  text += "same name, e.g. Menu.txt for Menu.wav:\r\n\r\n";
  text += "  loopstart=48000\r\n";
  text += "  loopend=1440000\r\n\r\n";
  text += "Values are sample frames in the WAV (1 second at 48 kHz = 48000).\r\n";
  text += "Everything before loopstart plays once, then the track repeats\r\n";
  text += "between loopstart and loopend. Without the file the whole track\r\n";
  text += "loops. BGM00.txt works too if the WAV uses a title name.\r\n\r\n";
  text += "Track list:\r\n";
  for (const MusicTrackInfo& track : kMusicTracks) {
    text += "  ";
    text += track.bank;
    text += ".wav  ";
    text += track.title;
    text += "\r\n";
  }
  text += "\r\n";
  text += "Files are read when the game starts. Restart after adding or\r\n";
  text += "removing files.\r\n";
  text += "Volume: --custom_music_volume=0.8   Off: --custom_music=false\r\n";

  FILE* file = xe::filesystem::OpenFile(path, "wb");
  if (!file) {
    return;
  }
  fwrite(text.data(), 1, text.size(), file);
  fclose(file);
}

}  // namespace

bool HasReplacementForBank(std::string_view bank_path) {
  if (!cvars::custom_music) {
    return false;
  }
  const std::string id = BankId(bank_path);
  if (id.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  return output_available_ && tracks_.find(id) != tracks_.end();
}

void InitializeCustomMusic() {
  std::lock_guard<std::mutex> lock(state_mutex_);

  tracks_.clear();
  current_ = nullptr;
  current_bank_.clear();
  cursor_ = 0;
  gain_ = 0.0f;
  fading_out_ = false;

  const std::filesystem::path folder =
      xe::filesystem::GetExecutableFolder() / "custom_music";
  xe::filesystem::CreateFolder(folder);
  const std::filesystem::path readme = folder / "README.txt";
  // Regenerate the generated help so it always documents current features.
  WriteReadme(readme);

  size_t loaded = 0;
  const std::vector<xe::filesystem::FileInfo> files =
      xe::filesystem::ListFiles(folder);
  // Pass 0: explicit BGMnn.wav names. Pass 1: title aliases (Menu.wav).
  for (int pass = 0; pass < 2; ++pass) {
    for (const xe::filesystem::FileInfo& file : files) {
      if (file.type != xe::filesystem::FileInfo::Type::kFile) {
        continue;
      }
      const std::string name = xe::path_to_utf8(file.name);
      if (!HasWavExtension(name)) {
        continue;
      }
      std::string id = BankId(name);
      if (pass == 0) {
        if (id.empty()) {
          continue;
        }
      } else {
        if (!id.empty() || tracks_.find(id) != tracks_.end()) {
          continue;
        }
        id = BankIdFromTitle(name);
        if (id.empty() || tracks_.find(id) != tracks_.end()) {
          continue;
        }
      }
      Track track;
      if (LoadWaveFile(file.path / file.name, id, &track)) {
        if (pass == 1) {
          XELOGI("custom music: {} -> {}", name, id);
        }
        tracks_.emplace(id, std::move(track));
        ++loaded;
      } else {
        XELOGW("custom music: could not read {}", name);
      }
    }
  }

  XELOGI("custom music: {} track(s) in {}", loaded,
         xe::path_to_utf8(folder));
}

void StartCustomMusic(std::string_view bank_path, bool restart) {
  if (!cvars::custom_music) {
    return;
  }
  const std::string id = BankId(bank_path);
  if (id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);

  auto it = tracks_.find(id);
  if (it == tracks_.end()) {
    // No replacement for this bank: fade out whatever we were playing.
    if (current_) {
      fading_out_ = true;
    }
    return;
  }
  if (current_ == &it->second && !fading_out_ && !restart) {
    // Same track looping or being retriggered: keep it running.
    return;
  }

  current_ = &it->second;
  current_bank_ = id;
  cursor_ = 0;
  gain_ = 0.0f;
  fading_out_ = false;
  XELOGI("custom music: playing {} ({})", it->second.file,
         MusicTrackTitle(id));
}

void StopCustomMusic(std::string_view bank_path) {
  if (!cvars::custom_music) {
    return;
  }
  const std::string id = BankId(bank_path);
  if (id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (current_ && current_bank_ == id) {
    fading_out_ = true;
  }
}

void SetCustomMusicOutputAvailable(bool available) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  output_available_ = available;
}

bool ShouldMuteAudioClient(int client_index) {
  if (!cvars::custom_music || cvars::custom_music_mute_client < 0 ||
      client_index != cvars::custom_music_mute_client) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  return current_ != nullptr && !fading_out_;
}

void MixCustomMusic(float* samples, uint32_t channels, uint32_t frames,
                    uint32_t sample_rate) {
  if (!cvars::custom_music || !samples || !frames || channels < 1 ||
      sample_rate != kOutputRate) {
    return;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!output_available_ || !current_) {
    return;
  }

  const size_t track_frames = current_->pcm.size() / 2;
  if (!track_frames) {
    current_ = nullptr;
    return;
  }
  const size_t loop_end =
      current_->loop_end ? std::min(current_->loop_end, track_frames)
                         : track_frames;
  const size_t loop_start =
      current_->loop_start < loop_end ? current_->loop_start : 0;
  if (cursor_ >= loop_end) {
    cursor_ = loop_start;
  }
  const float volume = float(cvars::custom_music_volume);
  const float fade_step = 1.0f / float(kFadeFrames);

  for (uint32_t i = 0; i < frames; ++i) {
    if (fading_out_) {
      gain_ = std::max(0.0f, gain_ - fade_step);
      if (gain_ <= 0.0f) {
        current_ = nullptr;
        current_bank_.clear();
        fading_out_ = false;
        break;
      }
    } else if (gain_ < 1.0f) {
      gain_ = std::min(1.0f, gain_ + fade_step);
    }

    const float scale = volume * gain_;
    const float left = current_->pcm[cursor_ * 2] * scale;
    const float right = current_->pcm[cursor_ * 2 + 1] * scale;
    samples[i * channels] =
        std::clamp(samples[i * channels] + left, -1.0f, 1.0f);
    if (channels > 1) {
      samples[i * channels + 1] =
          std::clamp(samples[i * channels + 1] + right, -1.0f, 1.0f);
    }

    if (++cursor_ >= loop_end) {
      cursor_ = loop_start;
    }
  }
}

}  // namespace apu
}  // namespace xe
