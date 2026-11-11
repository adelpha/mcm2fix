#include "oneshot.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../config.h"
#include "backend.h"

// One-shot sounds (race announcer: countdown voices, air horn, final lap).
// Plain 2D playback through waveOut, one stream per sound so they can
// overlap; Windows mixes them with the game's DirectSound output. Nothing
// here touches the game's sound objects. Volume is applied to the samples
// (waveOutSetVolume would change the whole game's volume on Vista and later).

namespace dd {
namespace {

struct Clip {
  bool tried = false, ok = false;
  WAVEFORMATEX fmt{};
  std::vector<char> data;
};

struct Voice {
  HWAVEOUT h = nullptr;
  WAVEHDR hdr{};
  std::string name;
};

std::map<std::string, Clip> g_clips;
std::vector<Voice*> g_voices;
unsigned g_played = 0, g_failed = 0;

std::string SoundPath(const char* file) {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (char* slash = std::strrchr(path, '\\')) slash[1] = 0;
  std::string p = path;
  p += "SOUNDS\\";
  p += file;
  return p;
}

uint32_t U32(const std::vector<char>& f, size_t o) {
  uint32_t v = 0;
  if (o + 4 <= f.size()) std::memcpy(&v, &f[o], 4);
  return v;
}

Clip& Load(const char* file) {
  Clip& c = g_clips[file];
  if (c.tried) return c;
  c.tried = true;
  const std::string path = SoundPath(file);
  std::vector<char> f;
  if (FILE* fp = std::fopen(path.c_str(), "rb")) {
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) f.insert(f.end(), buf, buf + n);
    std::fclose(fp);
  }
  if (f.empty()) {
    Log("announcer: %s not found (sound skipped)", path.c_str());
    return c;
  }
  if (f.size() < 12 || std::memcmp(&f[0], "RIFF", 4) != 0 || std::memcmp(&f[8], "WAVE", 4) != 0) {
    Log("announcer: %s is not a WAV file", path.c_str());
    return c;
  }
  bool have_fmt = false;
  for (size_t o = 12; o + 8 <= f.size();) {
    const uint32_t n = U32(f, o + 4);
    if (std::memcmp(&f[o], "fmt ", 4) == 0 && n >= 16) {
      std::memcpy(&c.fmt, &f[o + 8], 16);
      c.fmt.cbSize = 0;
      have_fmt = true;
    } else if (std::memcmp(&f[o], "data", 4) == 0) {
      const size_t end = std::min<size_t>(f.size(), o + 8 + n);
      c.data.assign(f.begin() + o + 8, f.begin() + end);
    }
    o += 8 + n + (n & 1);
  }
  if (!have_fmt || c.data.empty() || c.fmt.wFormatTag != WAVE_FORMAT_PCM ||
      (c.fmt.wBitsPerSample != 16 && c.fmt.wBitsPerSample != 8)) {
    Log("announcer: %s: unsupported (format %u, %u bits, %u bytes of data); needs 8 or 16-bit PCM", path.c_str(),
        c.fmt.wFormatTag, c.fmt.wBitsPerSample, static_cast<unsigned>(c.data.size()));
    c.data.clear();
    return c;
  }
  const float vol = GetConfig().announcer_volume / 100.0f;
  if (vol < 0.999f) {
    if (c.fmt.wBitsPerSample == 16) {
      int16_t* s = reinterpret_cast<int16_t*>(c.data.data());
      for (size_t i = 0; i < c.data.size() / 2; ++i) s[i] = static_cast<int16_t>(std::lround(s[i] * vol));
    } else {
      for (char& b : c.data) {
        const int v = static_cast<unsigned char>(b) - 128;
        b = static_cast<char>(128 + std::lround(v * vol));
      }
    }
  }
  c.ok = true;
  Log("announcer: %s loaded: %u Hz, %u channel%s, %u bits, %.2f s, volume %.0f%%", path.c_str(),
      static_cast<unsigned>(c.fmt.nSamplesPerSec), c.fmt.nChannels, c.fmt.nChannels == 1 ? "" : "s",
      c.fmt.wBitsPerSample, c.fmt.nAvgBytesPerSec ? c.data.size() / static_cast<double>(c.fmt.nAvgBytesPerSec) : 0.0,
      vol * 100);
  return c;
}

// Close streams that have finished.
void Reap() {
  for (size_t i = 0; i < g_voices.size();) {
    Voice* v = g_voices[i];
    if (!(v->hdr.dwFlags & WHDR_DONE)) {
      ++i;
      continue;
    }
    waveOutUnprepareHeader(v->h, &v->hdr, sizeof(v->hdr));
    waveOutClose(v->h);
    delete v;
    g_voices.erase(g_voices.begin() + i);
  }
}

}  // namespace

void OneShotPreload(const char* file) { Load(file); }

bool OneShotPlay(const char* file, const char* why) {
  Reap();
  Clip& c = Load(file);
  if (!c.ok) return false;
  if (g_voices.size() >= 16) {
    Log("announcer: %s not played, 16 sounds still playing", file);
    return false;
  }
  Voice* v = new Voice;
  v->name = file;
  MMRESULT r = waveOutOpen(&v->h, WAVE_MAPPER, &c.fmt, 0, 0, CALLBACK_NULL);
  if (r != MMSYSERR_NOERROR) {
    if (++g_failed <= 10) Log("announcer: %s: waveOutOpen failed (%u)", file, r);
    delete v;
    return false;
  }
  v->hdr.lpData = c.data.data();
  v->hdr.dwBufferLength = static_cast<DWORD>(c.data.size());
  r = waveOutPrepareHeader(v->h, &v->hdr, sizeof(v->hdr));
  if (r == MMSYSERR_NOERROR) r = waveOutWrite(v->h, &v->hdr, sizeof(v->hdr));
  if (r != MMSYSERR_NOERROR) {
    if (++g_failed <= 10) Log("announcer: %s: waveOutWrite failed (%u)", file, r);
    waveOutUnprepareHeader(v->h, &v->hdr, sizeof(v->hdr));
    waveOutClose(v->h);
    delete v;
    return false;
  }
  g_voices.push_back(v);
  ++g_played;
  Log("announcer: playing %s (%s; %u playing, %u played)", file, why, static_cast<unsigned>(g_voices.size()),
      g_played);
  return true;
}

void OneShotTick() {
  if (!g_voices.empty()) Reap();
}

}  // namespace dd
