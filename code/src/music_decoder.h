// One open music file, decoded to 16-bit PCM (music_decoders.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace music {

class Decoder {
 public:
  virtual ~Decoder() = default;
  // Reads up to n frames (channels interleaved samples each); 0 = end of file.
  virtual size_t Read(int16_t* out, size_t n) = 0;
  // Back to the start (looping); false if the file can't seek.
  virtual bool Rewind() = 0;

  int channels = 0;
  unsigned rate = 0;
  uint64_t frames = 0;  // length in frames, 0 if unknown (MP3)
  const char* format = "";
};

// .wav, .flac or .mp3 by extension.
bool IsMusicFile(const char* path);
// Opens path (by extension); on failure returns null and says why.
std::unique_ptr<Decoder> OpenDecoder(const char* path, std::string* why);

}  // namespace music
