// Optional music for the menus and races ([Music] section of mcm2fix.ini).
//
// Mcm2.exe has no music player of its own (only an unused CD-audio class at
// 0x432080..0x432268 that opens "cdaudio" but never plays), so mcm2fix plays
// music files itself: WAV, FLAC or MP3 (dr_libs, music_decoders.cpp), in
// stereo through waveOut on a thread of its own. waveOut is outside
// DirectSound, so neither the game's 3D positioning nor EAX reverb (DSOAL)
// can reach the music, and it works the same on Windows XP.
//
// What plays:
// - Menus: [Music] Menu=.
// - Races: the track's own key (its file name in Teraform without .env:
//   BAJA01, SX05, NAT12, urbanjunglebaja, ...), else the event's key (Baja,
//   Enduro, Nationals, Quarry, Supercross, Tag), else Race=. Empty = silence.
//   The track is the last Teraform .env file the game opened before the
//   race's countdown was set (CreateFileA import, chained after the per-user
//   file hooks); the event number (game+0x2d78) is the fallback.
// - Each value: files and/or folders (every .wav/.flac/.mp3 in it),
//   separated by ';', relative to the game folder (or the per-user folder
//   %LOCALAPPDATA%\Motocross Madness 2) or full paths. A list plays in turn
//   (Shuffle=1: random order) and repeats; one file loops without a gap.
//
// When: race lifetime comes from the race title hooks (countdown set
// 0x421f90 = start and Restart Race, BikeRace destructor 0x41dd30 = end).
// Race music pauses while the race is paused (the fixed-rate simulation
// stops, SimRate>0) when PauseInRace=1, and all music pauses while the game
// is in the background when BackgroundMute=1. Nothing plays while one of
// the game's videos runs, nor for 1.5 s after one.
#include "music.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "framerate.h"
#include "music_decoder.h"
#include "patch.h"
#include "xp_compat.h"

namespace music {
namespace {

constexpr uintptr_t kIatCreateFileA = 0x5551EC;
constexpr uintptr_t kGame = 0x5743AC;
constexpr uintptr_t kEvent78 = 0x2D78, kEvent74 = 0x2D74;
constexpr const char* kSection = "Music";
// Event number (game+0x2d78) -> ini key, and Teraform folder -> ini key.
const char* const kEventKey[6] = {"Quarry", "Baja", "Nationals", "Supercross", "Tag", "Enduro"};
struct FolderEvent {
  const char* folder;
  const char* key;
};
const FolderEvent kFolders[] = {{"baja", "Baja"},         {"enduro", "Enduro"}, {"national", "Nationals"},
                                {"quarries", "Quarry"},    {"sx", "Supercross"}, {"tag", "Tag"}};
// [Music] keys that are settings, not music.
const char* const kSettingKeys[] = {"MusicVolume", "RaceMusicVolume", "Shuffle", "PauseInRace", "FadeTime", "Log"};

constexpr DWORD kVideoGapMs = 1500;  // after a video, before menu music
constexpr DWORD kRacePauseMs = 250;   // simulation stopped this long = race paused
constexpr int kQuickFadeMs = 300;     // loading a track, race finished
constexpr uintptr_t kPodium = 0x3430;  // game: podium cutscene running (see dd/racetitle.cpp)

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '"')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '"')) --b;
  return s.substr(a, b - a);
}

// ---- Settings ----------------------------------------------------------------

struct Settings {
  bool loaded = false;
  bool any = false;  // some music is set
  std::string ini, game_dir, user_dir;
  int volume = 80, race_volume = 80;
  bool shuffle = true, pause_in_race = true, log = false;
  int fade_ms = 1000;
} S;

std::string ReadKey(const char* key) {
  char buf[4096] = {};
  GetPrivateProfileStringA(kSection, key, "", buf, sizeof(buf), S.ini.c_str());
  // ';' starts a comment only at the start of a line in an ini file, so a
  // trailing "; comment" stays in the value: values are lists split by ';'.
  return Trim(buf);
}

int ReadInt(const char* key, int def, int lo, int hi) {
  const std::string v = ReadKey(key);
  if (v.empty()) return def;
  char* end = nullptr;
  const long n = std::strtol(v.c_str(), &end, 10);
  if (end == v.c_str()) return def;
  return n < lo ? lo : n > hi ? hi : static_cast<int>(n);
}

bool IsSettingKey(const std::string& key) {
  for (const char* k : kSettingKeys)
    if (Lower(key) == Lower(k)) return true;
  return false;
}

void LoadSettings() {
  if (S.loaded) return;
  S.loaded = true;
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (char* slash = std::strrchr(path, '\\')) *slash = '\0';
  S.game_dir = path;
  S.ini = S.game_dir + "\\mcm2fix.ini";
  const std::string local = xp::LocalAppData();
  if (!local.empty()) S.user_dir = local + "\\Motocross Madness 2";

  // Any non-setting key with a value = music is on.
  std::vector<char> sec(32768);
  const DWORD n = GetPrivateProfileSectionA(kSection, sec.data(), static_cast<DWORD>(sec.size()), S.ini.c_str());
  for (const char* p = sec.data(); p < sec.data() + n && *p; p += std::strlen(p) + 1) {
    const char* eq = std::strchr(p, '=');
    if (!eq) continue;
    const std::string key = Trim(std::string(p, eq));
    const std::string value = Trim(eq + 1);
    if (!key.empty() && key[0] != ';' && !value.empty() && !IsSettingKey(key)) S.any = true;
  }
  S.volume = ReadInt("MusicVolume", 80, 0, 100);
  S.race_volume = ReadInt("RaceMusicVolume", S.volume, 0, 100);
  S.shuffle = ReadInt("Shuffle", 1, 0, 1) != 0;
  S.pause_in_race = ReadInt("PauseInRace", 1, 0, 1) != 0;
  S.fade_ms = ReadInt("FadeTime", 1000, 0, 10000);
  S.log = ReadInt("Log", 0, 0, 1) != 0;
}

// One ini value -> the music files it names, in order. Logs what it found.
std::vector<std::string> Resolve(const char* key, const std::string& value) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= value.size()) {
    size_t end = value.find(';', start);
    if (end == std::string::npos) end = value.size();
    std::string entry = Trim(value.substr(start, end - start));
    start = end + 1;
    if (entry.empty()) continue;
    for (char& c : entry)
      if (c == '/') c = '\\';
    while (entry.size() > 3 && entry.back() == '\\') entry.pop_back();
    const bool absolute = entry.size() > 1 && (entry[1] == ':' || (entry[0] == '\\' && entry[1] == '\\'));
    std::vector<std::string> tries;
    if (absolute) {
      tries.push_back(entry);
    } else {
      tries.push_back(S.game_dir + "\\" + entry);
      if (!S.user_dir.empty()) tries.push_back(S.user_dir + "\\" + entry);
    }
    bool found = false;
    for (const std::string& t : tries) {
      const DWORD attr = GetFileAttributesA(t.c_str());
      if (attr == INVALID_FILE_ATTRIBUTES) continue;
      found = true;
      if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        std::vector<std::string> files;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((t + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
          do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::string f = t + "\\" + fd.cFileName;
            if (IsMusicFile(f.c_str())) files.push_back(f);
          } while (FindNextFileA(h, &fd));
          FindClose(h);
        }
        std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
        patch::Log("music: [%s] \"%s\" is a folder: %u music file(s) in %s", key, entry.c_str(),
                   static_cast<unsigned>(files.size()), t.c_str());
        out.insert(out.end(), files.begin(), files.end());
      } else if (IsMusicFile(t.c_str())) {
        out.push_back(t);
        if (S.log) patch::Log("music: [%s] \"%s\" -> %s", key, entry.c_str(), t.c_str());
      } else {
        patch::Log("music: [%s] \"%s\" skipped: not a .wav, .flac or .mp3 file", key, entry.c_str());
      }
      break;
    }
    if (!found)
      patch::Log("music: [%s] \"%s\" NOT FOUND (looked in %s%s%s)", key, entry.c_str(), tries[0].c_str(),
                 tries.size() > 1 ? " and " : "", tries.size() > 1 ? tries[1].c_str() : "");
  }
  return out;
}

// ---- Shared state: game thread -> music thread ---------------------------------

struct Want {
  std::string key;  // "" = silence
  std::vector<std::string> files;
  int volume = 0;
  bool paused = false;
  int fade_ms = 1000;  // fade out the old music over this long when the key changes
  std::string why;
};

CRITICAL_SECTION g_cs;
bool g_cs_ready = false;
HANDLE g_wake = nullptr;
Want g_want;
LONG g_want_gen = 0;

CRITICAL_SECTION g_env_cs;
std::string g_last_env;  // last Teraform .env the game opened
DWORD g_last_env_tick = 0;
unsigned g_env_opens = 0;

// ---- Music thread -----------------------------------------------------------------

constexpr int kBufs = 6;  // 6 x 40 ms queued

class Player {
 public:
  void Run();

 private:
  bool OpenDevice(unsigned rate);
  void CloseDevice(const char* why);
  int Queued() const;
  int FreeBuffer() const;
  bool StartList(const Want& w);
  bool OpenNext(bool first);
  void FillAndSubmit(int i);
  void Stop(const char* why);
  void Summary(bool force);

  // waveOut
  HWAVEOUT out_ = nullptr;
  HANDLE done_ev_ = nullptr;
  unsigned rate_ = 0;
  WAVEHDR hdr_[kBufs] = {};
  std::vector<int16_t> mem_[kBufs];
  bool submitted_[kBufs] = {};
  bool dev_paused_ = false;
  DWORD frames_per_buf_ = 0;
  DWORD retry_open_at_ = 0;
  int retries_ = 0;

  // what plays
  Want want_;
  LONG gen_ = -1;
  std::string key_;  // list playing ("" = none)
  std::vector<std::string> list_;
  std::vector<size_t> order_;
  size_t pos_ = 0;
  std::unique_ptr<Decoder> dec_;
  std::string file_;
  std::vector<int16_t> tmp_;
  bool reopen_ = false;  // next file needs another sample rate: drain, reopen
  int fails_in_row_ = 0;

  // level
  float amp_ = 0, amp_target_ = 0, amp_step_ = 0;
  enum Mode { kIdle, kPlaying, kFadeSwitch, kFadePause, kPaused } mode_ = kIdle;
  bool tail_ = false;  // faded to zero: only waiting for the queue to drain
  bool expect_queue_ = false;  // blocks were sent and should still be queued (underrun check)

  // stats
  unsigned blocks_ = 0, underruns_ = 0, loops_ = 0, files_opened_ = 0, decode_fails_ = 0;
  double decode_us_ = 0, decode_max_us_ = 0;
  unsigned decode_n_ = 0;
  DWORD last_summary_ = 0;
  LARGE_INTEGER freq_ = {};
};

float Amp(int volume) {
  const float v = volume / 100.0f;
  return v * v;  // closer to how loud it sounds than a straight line
}

void Player::Summary(bool force) {
  const DWORD now = GetTickCount();
  if (!force && (!S.log || now - last_summary_ < 30000)) return;
  last_summary_ = now;
  patch::Log("music: summary: \"%s\" mode %d, %s, device %s %u Hz, %u blocks, %u underruns, %u loops, %u files opened, "
             "%u decode failures, decode %.0f us avg / %.0f us max per 40 ms block, level %.3f -> %.3f",
             key_.c_str(), static_cast<int>(mode_), file_.empty() ? "no file" : file_.c_str(), out_ ? "open" : "closed",
             rate_, blocks_, underruns_, loops_, files_opened_, decode_fails_, decode_n_ ? decode_us_ / decode_n_ : 0.0,
             decode_max_us_, amp_, amp_target_);
  decode_us_ = 0;
  decode_n_ = 0;
  decode_max_us_ = 0;
}

bool Player::OpenDevice(unsigned rate) {
  CloseDevice(nullptr);
  WAVEFORMATEX f{};
  f.wFormatTag = WAVE_FORMAT_PCM;
  f.nChannels = 2;
  f.nSamplesPerSec = rate;
  f.wBitsPerSample = 16;
  f.nBlockAlign = 4;
  f.nAvgBytesPerSec = rate * 4;
  ResetEvent(done_ev_);
  const MMRESULT r = waveOutOpen(&out_, WAVE_MAPPER, &f, reinterpret_cast<DWORD_PTR>(done_ev_), 0, CALLBACK_EVENT);
  if (r != MMSYSERR_NOERROR) {
    out_ = nullptr;
    patch::Log("music: waveOutOpen %u Hz stereo FAILED (%u); %u output device(s)", rate, r, waveOutGetNumDevs());
    return false;
  }
  rate_ = rate;
  frames_per_buf_ = rate / 25;
  for (int i = 0; i < kBufs; ++i) {
    mem_[i].assign(frames_per_buf_ * 2, 0);
    hdr_[i] = WAVEHDR{};
    hdr_[i].lpData = reinterpret_cast<LPSTR>(mem_[i].data());
    hdr_[i].dwBufferLength = frames_per_buf_ * 4;
    waveOutPrepareHeader(out_, &hdr_[i], sizeof(WAVEHDR));
    hdr_[i].dwFlags |= WHDR_DONE;  // free
    submitted_[i] = false;
  }
  dev_paused_ = false;
  WAVEOUTCAPSA caps{};
  waveOutGetDevCapsA(reinterpret_cast<UINT_PTR>(out_), &caps, sizeof(caps));
  patch::Log("music: output opened: \"%s\", %u Hz 16-bit stereo, %d x %lu frames queued (plain stereo, not DirectSound: "
             "no 3D or EAX)",
             caps.szPname, rate, kBufs, frames_per_buf_);
  return true;
}

void Player::CloseDevice(const char* why) {
  if (!out_) return;
  waveOutReset(out_);
  for (int i = 0; i < kBufs; ++i) waveOutUnprepareHeader(out_, &hdr_[i], sizeof(WAVEHDR));
  waveOutClose(out_);
  out_ = nullptr;
  dev_paused_ = false;
  if (why) patch::Log("music: output closed (%s)", why);
}

int Player::Queued() const {
  int n = 0;
  for (int i = 0; i < kBufs; ++i)
    if (submitted_[i] && !(hdr_[i].dwFlags & WHDR_DONE)) ++n;
  return n;
}

int Player::FreeBuffer() const {
  for (int i = 0; i < kBufs; ++i)
    if (!submitted_[i] || (hdr_[i].dwFlags & WHDR_DONE)) return i;
  return -1;
}

void Player::Stop(const char* why) {
  CloseDevice(why);
  dec_.reset();
  file_.clear();
  key_.clear();
  list_.clear();
  mode_ = kIdle;
  tail_ = false;
  reopen_ = false;
  amp_ = 0;
}

// Opens the next playable file of the list (first: the list's start).
// Returns false if none of them opens.
bool Player::OpenNext(bool first) {
  if (list_.empty()) return false;
  for (size_t tries = 0; tries < list_.size(); ++tries) {
    if (!first || tries) {
      pos_ = (pos_ + 1) % order_.size();
      if (pos_ == 0 && S.shuffle && order_.size() > 2) {
        // New round in a new order, not starting with the one just played.
        const size_t last = order_.back();
        for (size_t i = order_.size() - 1; i > 0; --i) std::swap(order_[i], order_[GetTickCount() % (i + 1)]);
        if (order_[0] == last) std::swap(order_[0], order_[1]);
      }
    }
    const std::string& f = list_[order_[pos_]];
    std::string why;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    auto d = OpenDecoder(f.c_str(), &why);
    QueryPerformanceCounter(&b);
    const double ms = 1000.0 * (b.QuadPart - a.QuadPart) / freq_.QuadPart;
    if (!d || d->channels <= 0 || d->rate < 8000 || d->rate > 192000) {
      ++decode_fails_;
      patch::Log("music: CAN'T PLAY %s: %s", f.c_str(),
                 d ? "unsupported channels or sample rate" : why.c_str());
      continue;
    }
    ++files_opened_;
    char len[48] = "length unknown";
    if (d->frames) std::snprintf(len, sizeof(len), "%.1f s", static_cast<double>(d->frames) / d->rate);
    patch::Log("music: playing %s (%s, %u Hz, %d channel%s%s, %s; opened in %.1f ms; %u of %u in \"%s\")", f.c_str(),
               d->format, d->rate, d->channels, d->channels == 1 ? "" : "s",
               d->channels > 2 ? ", first two used" : "", len, ms, static_cast<unsigned>(pos_ + 1),
               static_cast<unsigned>(list_.size()), key_.c_str());
    dec_ = std::move(d);
    file_ = f;
    if (out_ && rate_ != dec_->rate) reopen_ = true;  // drain at the old rate first
    return true;
  }
  patch::Log("music: none of the %u file(s) in \"%s\" can be played; staying silent", static_cast<unsigned>(list_.size()),
             key_.c_str());
  dec_.reset();
  file_.clear();
  return false;
}

bool Player::StartList(const Want& w) {
  dec_.reset();
  file_.clear();
  key_ = w.key;
  list_ = w.files;
  tail_ = false;
  reopen_ = false;
  expect_queue_ = false;
  fails_in_row_ = 0;
  if (out_ && Queued()) waveOutReset(out_);  // switching without a fade: drop what the old list queued
  if (key_.empty() || list_.empty()) {
    CloseDevice(key_.empty() ? "no music for this part of the game" : "nothing to play");
    mode_ = kIdle;
    return false;
  }
  order_.resize(list_.size());
  for (size_t i = 0; i < order_.size(); ++i) order_[i] = i;
  if (S.shuffle)
    for (size_t i = order_.size() - 1; i > 0; --i) std::swap(order_[i], order_[(GetTickCount() * 2654435761u >> 7) % (i + 1)]);
  pos_ = 0;
  if (!OpenNext(true)) {
    CloseDevice("nothing playable");
    mode_ = kIdle;
    return false;
  }
  if (!out_ || rate_ != dec_->rate) {
    if (!OpenDevice(dec_->rate)) {
      retry_open_at_ = GetTickCount() + 3000;
      mode_ = kIdle;
      return false;
    }
  }
  reopen_ = false;
  if (dev_paused_) {
    waveOutRestart(out_);
    dev_paused_ = false;
  }
  amp_ = 0;
  amp_target_ = w.paused ? 0 : Amp(w.volume);
  amp_step_ = amp_target_ / (0.04f * rate_);  // 40 ms fade in (no click)
  mode_ = w.paused ? kPaused : kPlaying;
  if (w.paused) {
    waveOutPause(out_);
    dev_paused_ = true;
  }
  patch::Log("music: \"%s\" started (%u file(s), volume %d%s)", key_.c_str(), static_cast<unsigned>(list_.size()), w.volume,
             w.paused ? ", paused" : "");
  return true;
}

void Player::FillAndSubmit(int i) {
  int16_t* dst = mem_[i].data();
  DWORD done = 0;
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  while (done < frames_per_buf_ && dec_ && !reopen_) {
    const size_t want = frames_per_buf_ - done;
    tmp_.resize(want * dec_->channels);
    const size_t got = dec_->Read(tmp_.data(), want);
    const int ch = dec_->channels;
    for (size_t k = 0; k < got; ++k) {
      const int16_t l = tmp_[k * ch], r = ch > 1 ? tmp_[k * ch + 1] : l;
      if (amp_ != amp_target_) {
        if (amp_ < amp_target_) amp_ = std::min(amp_target_, amp_ + amp_step_);
        else amp_ = std::max(amp_target_, amp_ - amp_step_);
      }
      dst[(done + k) * 2] = static_cast<int16_t>(l * amp_);
      dst[(done + k) * 2 + 1] = static_cast<int16_t>(r * amp_);
    }
    done += static_cast<DWORD>(got);
    if (got == 0) {
      // Files that give no sound at all: don't spin on them.
      if (++fails_in_row_ > static_cast<int>(list_.size()) + 3) {
        patch::Log("music: \"%s\" gives no sound (last %s); stopped", key_.c_str(), file_.c_str());
        dec_.reset();
        file_.clear();
        break;
      }
    } else {
      fails_in_row_ = 0;
    }
    if (got < want) {
      // End of the file: loop it, or on to the next one.
      if (list_.size() == 1 && dec_->Rewind()) {
        ++loops_;
        if (S.log) patch::Log("music: %s looped", file_.c_str());
        continue;
      }
      if (S.log) patch::Log("music: %s finished", file_.c_str());
      if (!OpenNext(false)) break;
    }
  }
  QueryPerformanceCounter(&b);
  const double us = 1e6 * (b.QuadPart - a.QuadPart) / freq_.QuadPart;
  decode_us_ += us;
  decode_max_us_ = std::max(decode_max_us_, us);
  ++decode_n_;
  if (done == 0) return;  // nothing to send
  std::memset(dst + done * 2, 0, (frames_per_buf_ - done) * 4);
  hdr_[i].dwBufferLength = done * 4;
  hdr_[i].dwFlags &= ~WHDR_DONE;
  const MMRESULT r = waveOutWrite(out_, &hdr_[i], sizeof(WAVEHDR));
  if (r != MMSYSERR_NOERROR) {
    hdr_[i].dwFlags |= WHDR_DONE;
    patch::Log("music: waveOutWrite FAILED (%u); output reopened in 3 s", r);
    CloseDevice("write failed");
    retry_open_at_ = GetTickCount() + 3000;
    return;
  }
  submitted_[i] = true;
  expect_queue_ = true;
  ++blocks_;
}

void Player::Run() {
  QueryPerformanceFrequency(&freq_);
  done_ev_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  HANDLE waits[2] = {g_wake, done_ev_};
  last_summary_ = GetTickCount();
  for (;;) {
    WaitForMultipleObjects(2, waits, FALSE, 20);
    Want w;
    LONG gen;
    EnterCriticalSection(&g_cs);
    gen = g_want_gen;
    if (gen != gen_) w = g_want;
    LeaveCriticalSection(&g_cs);
    if (gen != gen_) {
      gen_ = gen;
      retries_ = 0;
      if (S.log)
        patch::Log("music: thread got \"%s\" (%u file(s)) volume %d %s (%s); playing \"%s\" mode %d", w.key.c_str(),
                   static_cast<unsigned>(w.files.size()), w.volume, w.paused ? "paused" : "playing", w.why.c_str(),
                   key_.c_str(), static_cast<int>(mode_));
      want_ = w;
      if (want_.key != key_) {
        if (out_ && (mode_ == kPlaying || mode_ == kFadePause || mode_ == kFadeSwitch) && amp_ > 0 && want_.fade_ms > 0) {
          patch::Log("music: fading out \"%s\" over %d ms for \"%s\"", key_.c_str(), want_.fade_ms,
                     want_.key.empty() ? "silence" : want_.key.c_str());
          mode_ = kFadeSwitch;
          amp_target_ = 0;
          amp_step_ = std::max(amp_, 0.001f) / (want_.fade_ms / 1000.0f * rate_);
        } else {
          StartList(want_);
        }
      } else if (mode_ == kPlaying || mode_ == kFadePause || mode_ == kPaused || mode_ == kFadeSwitch) {
        const float target = want_.paused ? 0 : Amp(want_.volume);
        if (want_.paused && (mode_ == kPlaying || mode_ == kFadeSwitch)) {
          mode_ = kFadePause;
          amp_target_ = 0;
          amp_step_ = std::max(amp_, 0.001f) / (0.15f * rate_);
          patch::Log("music: pausing \"%s\" (%s)", key_.c_str(), want_.why.c_str());
        } else if (!want_.paused && (mode_ == kPaused || mode_ == kFadePause || mode_ == kFadeSwitch)) {
          if (dev_paused_ && out_) waveOutRestart(out_);
          dev_paused_ = false;
          tail_ = false;
          expect_queue_ = false;
          mode_ = kPlaying;
          amp_target_ = target;
          amp_step_ = std::max(target, 0.001f) / (0.15f * rate_);
          patch::Log("music: resuming \"%s\" (%s)", key_.c_str(), want_.why.c_str());
        } else if (mode_ == kPlaying && amp_target_ != target) {
          amp_target_ = target;
          amp_step_ = 0.5f / rate_;
          patch::Log("music: \"%s\" volume %d", key_.c_str(), want_.volume);
        }
      }
    }

    if (!out_) {
      if (retry_open_at_ && GetTickCount() >= retry_open_at_) {
        retry_open_at_ = 0;
        if (++retries_ > 5) {
          patch::Log("music: output still failing after 5 tries; silent until the music changes");
          Summary(false);
          continue;
        }
        patch::Log("music: trying the output again (try %d of 5)", retries_);
        const Want again = want_;
        Stop(nullptr);
        StartList(again);
      }
      Summary(false);
      continue;
    }

    // Faded down: once what was queued has played, switch or pause.
    if ((mode_ == kFadeSwitch || mode_ == kFadePause) && (amp_ <= 0.0f || !dec_)) tail_ = true;
    if (tail_ && Queued() == 0) {
      tail_ = false;
      if (mode_ == kFadeSwitch) {
        patch::Log("music: \"%s\" faded out", key_.c_str());
        StartList(want_);
      } else if (mode_ == kFadePause) {
        waveOutPause(out_);
        dev_paused_ = true;
        mode_ = kPaused;
        patch::Log("music: \"%s\" paused", key_.c_str());
      }
      Summary(false);
      continue;
    }
    if (reopen_ && Queued() == 0) {
      patch::Log("music: next file is %u Hz, output was %u Hz: reopening", dec_ ? dec_->rate : 0, rate_);
      reopen_ = false;
      expect_queue_ = false;
      if (!dec_ || !OpenDevice(dec_->rate)) {
        retry_open_at_ = GetTickCount() + 3000;
        continue;
      }
    }
    if (mode_ == kPlaying || ((mode_ == kFadeSwitch || mode_ == kFadePause) && !tail_)) {
      if (!dec_) {
        if (Queued() == 0 && mode_ == kPlaying) {
          patch::Log("music: \"%s\" has nothing left to play", key_.c_str());
          CloseDevice("list ended");
          mode_ = kIdle;
        }
      } else {
        if (Queued() == 0 && expect_queue_ && !reopen_) {
          ++underruns_;
          if (underruns_ <= 10 || S.log)
            patch::Log("music: UNDERRUN #%u (queue ran dry in \"%s\")", underruns_, key_.c_str());
        }
        for (int i; !reopen_ && dec_ && out_ && (i = FreeBuffer()) >= 0;) {
          FillAndSubmit(i);
          if ((mode_ == kFadeSwitch || mode_ == kFadePause) && amp_ <= 0.0f) break;
        }
      }
    }
    Summary(false);
  }
}

Player* g_player = nullptr;

DWORD WINAPI ThreadMain(void*) {
  g_player->Run();
  return 0;
}

// ---- Game thread ----------------------------------------------------------------

bool g_applied = false;
bool g_thread_started = false;
uintptr_t g_race = 0;
std::string g_race_key;  // ini key that gave the race music ("" = none)
std::vector<std::string> g_race_files;
std::string g_race_desc;
bool g_race_sim_seen = false;
DWORD g_sim_stopped_at = 0;
bool g_active = true;
LONG g_videos = 0;
DWORD g_video_end = 0;
DWORD g_first_frame = 0;
bool g_menus_reached = false;  // the menus' first video (ui\events.avi) opened: the intro slides are over
bool g_loading = false;        // a track is loading (display mode left 640x480 for the race's)
bool g_finished = false;       // the player crossed the finish line (finish text, 0x497b11)
bool g_podium_seen = false;
int g_last_podium = 0;
std::vector<std::string> g_menu_files;
bool g_menu_resolved = false;
// Last state posted.
std::string g_posted_key = "?";
bool g_posted_paused = false;
std::string g_last_why;

void StartThread() {
  if (g_thread_started) return;
  g_thread_started = true;
  g_player = new Player();
  HANDLE t = CreateThread(nullptr, 0, &ThreadMain, nullptr, 0, nullptr);
  if (!t) {
    patch::Log("music: COULD NOT START the music thread (%lu)", GetLastError());
    return;
  }
  SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
  CloseHandle(t);
  patch::Log("music: thread started");
}

void Post(const std::string& key, const std::vector<std::string>& files, int volume, bool paused, int fade_ms,
          const std::string& why) {
  if (key == g_posted_key && paused == g_posted_paused) return;
  patch::Log("music: now \"%s\"%s (%s; fade %d ms)", key.empty() ? "silence" : key.c_str(), paused ? " paused" : "",
             why.c_str(), fade_ms);
  g_posted_key = key;
  g_posted_paused = paused;
  StartThread();
  EnterCriticalSection(&g_cs);
  g_want.key = key;
  g_want.files = files;
  g_want.volume = volume;
  g_want.paused = paused;
  g_want.fade_ms = fade_ms;
  g_want.why = why;
  ++g_want_gen;
  LeaveCriticalSection(&g_cs);
  SetEvent(g_wake);
}

// The game opens Teraform\<event>\<track>.env when it loads a track.
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileAFn g_create_file = nullptr;

HANDLE WINAPI MusicCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                               DWORD flags, HANDLE templ) {
  HANDLE h = g_create_file(name, access, share, sa, disposition, flags, templ);
  if (name && h != INVALID_HANDLE_VALUE) {
    const size_t n = std::strlen(name);
    if (n > 4 && Lower(name + n - 4) == ".env") {
      EnterCriticalSection(&g_env_cs);
      g_last_env = name;
      g_last_env_tick = GetTickCount();
      const unsigned count = ++g_env_opens;
      LeaveCriticalSection(&g_env_cs);
      if (S.log || count <= 30) patch::Log("music: track file opened: %s (#%u)", name, count);
    }
  }
  return h;
}

// "Teraform\Baja\BAJA01.ENV" -> stem "BAJA01", event key "Baja" (or null).
void SplitEnv(const std::string& path, std::string* stem, const char** event) {
  std::string p = path;
  for (char& c : p)
    if (c == '/') c = '\\';
  const size_t slash = p.rfind('\\');
  std::string file = slash == std::string::npos ? p : p.substr(slash + 1);
  *stem = file.size() > 4 ? file.substr(0, file.size() - 4) : file;
  *event = nullptr;
  if (slash == std::string::npos) return;
  const size_t prev = p.rfind('\\', slash - 1);
  const std::string folder = Lower(p.substr(prev == std::string::npos ? 0 : prev + 1,
                                            slash - (prev == std::string::npos ? 0 : prev + 1)));
  for (const FolderEvent& f : kFolders)
    if (folder == f.folder) *event = f.key;
}

int GameEvent(uintptr_t off) {
  const uintptr_t g = *reinterpret_cast<uintptr_t*>(kGame);
  return g ? *reinterpret_cast<int*>(g + off) : -1;
}

}  // namespace

bool Enabled() {
  LoadSettings();
  return S.any;
}

void Apply() {
  LoadSettings();
  if (!S.any) {
    patch::Log("music: off (no music set in the [Music] section of mcm2fix.ini)");
    return;
  }
  InitializeCriticalSection(&g_cs);
  InitializeCriticalSection(&g_env_cs);
  g_cs_ready = true;
  g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  g_create_file = reinterpret_cast<CreateFileAFn>(patch::HookIat(kIatCreateFileA, reinterpret_cast<void*>(&MusicCreateFileA)));
  g_applied = g_create_file != nullptr;
  patch::Log("music: on: MusicVolume %d, RaceMusicVolume %d, shuffle %d, PauseInRace %d, BackgroundMute %d, fade %d ms, "
             "Log %d; track file watch %s",
             S.volume, S.race_volume, S.shuffle ? 1 : 0, S.pause_in_race ? 1 : 0, GetConfig().background_mute ? 1 : 0,
             S.fade_ms, S.log ? 1 : 0, g_applied ? "on" : "FAILED (race music by event only)");
  if (!g_applied) {
    g_create_file = &CreateFileA;
    g_applied = true;
  }
  // Startup check of every entry, so a typo shows in the log at once.
  std::vector<char> sec(32768);
  const DWORD n = GetPrivateProfileSectionA(kSection, sec.data(), static_cast<DWORD>(sec.size()), S.ini.c_str());
  for (const char* p = sec.data(); p < sec.data() + n && *p; p += std::strlen(p) + 1) {
    const char* eq = std::strchr(p, '=');
    if (!eq) continue;
    const std::string key = Trim(std::string(p, eq));
    const std::string value = Trim(eq + 1);
    if (key.empty() || key[0] == ';' || value.empty() || IsSettingKey(key)) continue;
    const std::vector<std::string> files = Resolve(key.c_str(), value);
    patch::Log("music: [Music] %s = %s -> %u file(s)", key.c_str(), value.c_str(), static_cast<unsigned>(files.size()));
  }
}

void RaceStart(uintptr_t race) {
  if (!g_applied) return;
  g_loading = false;
  g_menus_reached = true;
  if (race && race == g_race) {
    patch::Log("music: race %08x countdown set again (Restart Race): \"%s\" keeps playing%s", static_cast<unsigned>(race),
               g_race_key.c_str(), g_finished ? " (was finished: race music again)" : "");
    g_finished = false;
    g_podium_seen = false;
    return;
  }
  g_finished = false;
  g_podium_seen = false;
  g_last_podium = 0;
  std::string env;
  DWORD env_tick = 0;
  unsigned opens = 0;
  EnterCriticalSection(&g_env_cs);
  env = g_last_env;
  env_tick = g_last_env_tick;
  opens = g_env_opens;
  LeaveCriticalSection(&g_env_cs);
  std::string stem;
  const char* folder_event = nullptr;
  if (!env.empty()) SplitEnv(env, &stem, &folder_event);
  const int ev78 = GameEvent(kEvent78), ev74 = GameEvent(kEvent74);
  const char* num_event = ev78 >= 0 && ev78 < 6 ? kEventKey[ev78] : nullptr;
  const char* event = folder_event ? folder_event : num_event;
  patch::Log("music: race %08x starting; last track file %s (%lu ms ago, %u opened so far) -> track \"%s\", folder event "
             "%s, game event %d/%d (%s)%s",
             static_cast<unsigned>(race), env.empty() ? "(none seen)" : env.c_str(),
             env.empty() ? 0ul : static_cast<unsigned long>(GetTickCount() - env_tick), opens, stem.c_str(),
             folder_event ? folder_event : "?", ev78, ev74, num_event ? num_event : "?",
             folder_event && num_event && std::strcmp(folder_event, num_event) != 0 ? " - FOLDER AND EVENT DIFFER" : "");
  g_race = race;
  g_race_sim_seen = false;
  g_sim_stopped_at = 0;
  g_race_key.clear();
  g_race_files.clear();
  // Track's own key, then the event's, then Race=. Read now, so ini edits
  // between races count.
  const std::string keys[3] = {stem, event ? event : "", "Race"};
  for (const std::string& k : keys) {
    if (k.empty() || IsSettingKey(k)) continue;
    const std::string v = ReadKey(k.c_str());
    if (S.log) patch::Log("music:   [Music] %s = \"%s\"", k.c_str(), v.c_str());
    if (v.empty()) continue;
    g_race_files = Resolve(k.c_str(), v);
    g_race_key = k;
    break;
  }
  patch::Log("music: race music: %s (%u file(s))", g_race_key.empty() ? "none set, silence" : g_race_key.c_str(),
             static_cast<unsigned>(g_race_files.size()));
}

void RaceEnd(uintptr_t race) {
  if (!g_applied || !race || race != g_race) return;
  patch::Log("music: race %08x ended", static_cast<unsigned>(race));
  g_race = 0;
  g_race_key.clear();
  g_race_files.clear();
  g_finished = false;
  g_podium_seen = false;
  g_menu_resolved = false;  // re-read Menu= (ini edits count)
}

void RaceFinished() {
  if (!g_applied || !g_race) return;
  patch::Log("music: player finished the race: race music fades out (%d ms)", kQuickFadeMs);
  g_finished = true;
  Frame();
}

void VideoOpened(const char* path) {
  if (!g_applied || !path) return;
  const char* slash = std::strrchr(path, '\\');
  const std::string name = Lower(slash ? slash + 1 : path);
  if (!g_menus_reached && name.compare(0, 5, "intro") != 0) {
    g_menus_reached = true;
    patch::Log("music: menus reached (%s opened)", path);
  }
}

void DisplayMode(unsigned w, unsigned h) {
  if (!g_applied) return;
  const bool menus = w == 640 && h == 480;
  if (menus) {
    if (g_loading) patch::Log("music: back to 640x480 without a race: not loading");
    g_loading = false;
  } else if (!g_race && g_menus_reached && !g_loading) {
    g_loading = true;
    patch::Log("music: display mode %ux%u from the menus: a track is loading", w, h);
    Frame();
  }
}

void VideoPlaying(bool playing) {
  if (!g_applied) return;
  if (playing) {
    ++g_videos;
  } else if (g_videos > 0) {
    --g_videos;
    g_video_end = GetTickCount();
  }
  patch::Log("music: video %s (%ld playing)", playing ? "started" : "stopped", g_videos);
  if (playing) Frame();  // stop at once, not at the next frame
}

void OnActivate(bool active) {
  if (!g_applied) return;
  g_active = active;
  Frame();
}

void Frame() {
  if (!g_applied) return;
  const DWORD now = GetTickCount();
  if (!g_first_frame) {
    g_first_frame = now ? now : 1;
    patch::Log("music: first frame");
  }
  std::string key, why;
  const std::vector<std::string>* files = nullptr;
  static const std::vector<std::string> kNone;
  int volume = S.volume;
  bool paused = false;
  int fade = S.fade_ms;
  if (g_videos > 0) {
    why = "video playing";
  } else if (g_race && g_finished) {
    // After the finish: quiet through the podium cutscene, then the menu
    // music on the results.
    const uintptr_t game = *reinterpret_cast<uintptr_t*>(kGame);
    const int podium = game ? *reinterpret_cast<int*>(game + kPodium) : 0;
    if (podium != g_last_podium) {
      patch::Log("music: podium cutscene %s", podium ? "started" : "ended");
      g_last_podium = podium;
    }
    if (podium) g_podium_seen = true;
    const bool sim = SimRacing();
    if (sim) g_sim_stopped_at = 0;
    else if (!g_sim_stopped_at) g_sim_stopped_at = now;
    const bool stopped = !sim && now - g_sim_stopped_at >= kRacePauseMs;
    fade = kQuickFadeMs;
    if (podium) {
      why = "podium cutscene";
    } else if (g_podium_seen || stopped) {
      if (!g_menu_resolved) {
        g_menu_resolved = true;
        const std::string v = ReadKey("Menu");
        g_menu_files = v.empty() ? std::vector<std::string>() : Resolve("Menu", v);
      }
      key = g_menu_files.empty() ? "" : "menu";
      files = &g_menu_files;
      why = g_podium_seen ? "results, after the podium" : "results";
    } else {
      why = "race finished";
    }
  } else if (g_race) {
    const bool sim = SimRacing();
    if (sim) {
      g_race_sim_seen = true;
      g_sim_stopped_at = 0;
    } else if (g_race_sim_seen && !g_sim_stopped_at) {
      g_sim_stopped_at = now;
    }
    key = g_race_key.empty() ? "" : "race: " + g_race_key;
    files = &g_race_files;
    volume = S.race_volume;
    const bool race_paused = S.pause_in_race && g_race_sim_seen && g_sim_stopped_at && now - g_sim_stopped_at >= kRacePauseMs;
    paused = race_paused;
    why = race_paused ? "race paused" : "race";
  } else if (!g_menus_reached) {
    why = "intro, before the menus";
  } else if (g_loading) {
    why = "loading a track";
    fade = kQuickFadeMs;
  } else if (g_video_end && now - g_video_end < kVideoGapMs) {
    // Waiting: between videos, or before the intro videos start.
    if (g_posted_key == "?") return;
    key = g_posted_key == "menu" ? "menu" : "";
    files = &g_menu_files;
    why = "waiting after a video";
  } else {
    if (!g_menu_resolved) {
      g_menu_resolved = true;
      const std::string v = ReadKey("Menu");
      g_menu_files = v.empty() ? std::vector<std::string>() : Resolve("Menu", v);
    }
    key = g_menu_files.empty() ? "" : "menu";
    files = &g_menu_files;
    why = "menus";
  }
  if (!g_active && GetConfig().background_mute) {
    paused = true;
    why += ", game in the background";
  }
  Post(key, files ? *files : kNone, volume, paused, fade, why);
}

}  // namespace music
