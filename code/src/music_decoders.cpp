// WAV, FLAC and MP3 decoding for music.cpp: dr_libs (David Reid, public
// domain / MIT No Attribution), code/third_party/dr_libs.
//
// No SIMD paths: the DLL must run on CPUs without SSE2 (Pentium II/Athlon).
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_WCHAR
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_WCHAR
#define DR_FLAC_NO_OGG
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_SIMD

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "../third_party/dr_libs/dr_flac.h"
#include "../third_party/dr_libs/dr_mp3.h"
#include "../third_party/dr_libs/dr_wav.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "music_decoder.h"

#include <cctype>
#include <cstring>
#include <vector>

namespace music {
namespace {

class WavDecoder final : public Decoder {
 public:
  ~WavDecoder() override { drwav_uninit(&w_); }
  bool Open(const char* path) {
    if (!drwav_init_file(&w_, path, nullptr)) return false;
    channels = static_cast<int>(w_.channels);
    rate = w_.sampleRate;
    frames = w_.totalPCMFrameCount;
    format = "WAV";
    return true;
  }
  size_t Read(int16_t* out, size_t n) override {
    return static_cast<size_t>(drwav_read_pcm_frames_s16(&w_, n, out));
  }
  bool Rewind() override { return drwav_seek_to_pcm_frame(&w_, 0) != 0; }

 private:
  drwav w_{};
};

class FlacDecoder final : public Decoder {
 public:
  ~FlacDecoder() override {
    if (f_) drflac_close(f_);
  }
  bool Open(const char* path) {
    f_ = drflac_open_file(path, nullptr);
    if (!f_) return false;
    channels = f_->channels;
    rate = f_->sampleRate;
    frames = f_->totalPCMFrameCount;
    format = "FLAC";
    return true;
  }
  size_t Read(int16_t* out, size_t n) override {
    return static_cast<size_t>(drflac_read_pcm_frames_s16(f_, n, out));
  }
  bool Rewind() override { return drflac_seek_to_pcm_frame(f_, 0) != 0; }

 private:
  drflac* f_ = nullptr;
};

class Mp3Decoder final : public Decoder {
 public:
  ~Mp3Decoder() override {
    if (open_) drmp3_uninit(&m_);
  }
  bool Open(const char* path) {
    if (!drmp3_init_file(&m_, path, nullptr)) return false;
    open_ = true;
    channels = static_cast<int>(m_.channels);
    rate = m_.sampleRate;
    frames = 0;  // counting MP3 frames means reading the whole file: left unknown
    format = "MP3";
    return true;
  }
  size_t Read(int16_t* out, size_t n) override {
    return static_cast<size_t>(drmp3_read_pcm_frames_s16(&m_, n, out));
  }
  bool Rewind() override { return drmp3_seek_to_pcm_frame(&m_, 0) != 0; }

 private:
  drmp3 m_{};
  bool open_ = false;
};

std::string Ext(const char* path) {
  const char* dot = std::strrchr(path, '.');
  const char* slash = std::strrchr(path, '\\');
  if (!dot || (slash && dot < slash)) return std::string();
  std::string e(dot + 1);
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

}  // namespace

bool IsMusicFile(const char* path) {
  const std::string e = Ext(path);
  return e == "wav" || e == "flac" || e == "mp3";
}

std::unique_ptr<Decoder> OpenDecoder(const char* path, std::string* why) {
  const std::string e = Ext(path);
  if (e == "wav") {
    auto d = std::make_unique<WavDecoder>();
    if (d->Open(path)) return d;
    *why = "not a WAV file dr_wav can read (PCM, IEEE float, ADPCM, A-law and mu-law are supported)";
  } else if (e == "flac") {
    auto d = std::make_unique<FlacDecoder>();
    if (d->Open(path)) return d;
    *why = "not a FLAC file (Ogg FLAC is not supported)";
  } else if (e == "mp3") {
    auto d = std::make_unique<Mp3Decoder>();
    if (d->Open(path)) return d;
    *why = "not an MP3 file";
  } else {
    *why = "not .wav, .flac or .mp3";
  }
  return nullptr;
}

}  // namespace music
