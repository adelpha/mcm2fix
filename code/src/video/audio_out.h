// Plays a block of 16-bit PCM through waveOut (always present on Windows).
#pragma once
#include <windows.h>
#include <mmsystem.h>

#include <cstdint>

#include "adpcm.h"

namespace video {

class AudioOut {
 public:
  ~AudioOut() { Stop(); }
  // Starts playing pcm from the given sample frame. pcm must outlive playback.
  bool Start(const PcmAudio& pcm, size_t first_frame);
  void Stop();

 private:
  HWAVEOUT out_ = nullptr;
  WAVEHDR hdr_{};
  bool prepared_ = false;
};

}  // namespace video
