// Minimal AVI (RIFF) reader for the game's videos: one video stream and
// at most one audio stream, read from a memory-mapped file.
#pragma once
#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace video {

struct Chunk {
  const uint8_t* data;
  uint32_t size;
};

class AviFile {
 public:
  ~AviFile();
  bool Open(const char* path);

  // Video
  uint32_t width = 0, height = 0;
  uint32_t fourcc = 0;  // strh/strf compression, e.g. 'IV50'
  uint32_t rate = 0, scale = 1;  // frames per second = rate / scale
  std::vector<Chunk> frames;

  // Audio (empty format when there is none)
  std::vector<uint8_t> audio_format;  // WAVEFORMATEX + extra bytes
  std::vector<Chunk> audio;

  std::string error;

 private:
  void Close();
  bool ParseList(const uint8_t* p, const uint8_t* end, int depth);
  bool ParseMovi(const uint8_t* p, const uint8_t* end);

#ifdef _WIN32
  HANDLE file_ = INVALID_HANDLE_VALUE;
  HANDLE map_ = nullptr;
#else
  std::vector<uint8_t> contents_;  // non-Windows builds (tests) read the whole file
#endif
  const uint8_t* base_ = nullptr;
  size_t size_ = 0;

  int stream_ = -1;  // index of the strl being parsed
  int video_stream_ = -1, audio_stream_ = -1;
  uint32_t stream_type_ = 0;
};

}  // namespace video
