// Indeo Video 5 (IV50) decoder interface.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace video {

// A decoded picture in YUV 4:1:0 (chroma at a quarter of the width and height).
struct YuvPicture {
  const uint8_t* plane[3];  // Y, U, V
  int stride[3];
  int width, height;          // luma size
  int chroma_w, chroma_h;
};

class Indeo5Decoder {
 public:
  virtual ~Indeo5Decoder() = default;
  // Decodes one frame. Returns true and fills pic when a picture is ready.
  virtual bool Decode(const uint8_t* data, size_t size, YuvPicture* pic) = 0;
  // Forgets all reference frames (before decoding from the start again).
  virtual void Reset() = 0;
};

// Returns nullptr when no decoder is available in this build.
std::unique_ptr<Indeo5Decoder> CreateIndeo5Decoder(int width, int height);

}  // namespace video
