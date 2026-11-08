// Microsoft ADPCM (WAVE_FORMAT_ADPCM) to 16-bit PCM.
#pragma once
#include <cstdint>
#include <vector>

namespace video {

struct PcmAudio {
  int channels = 0;
  uint32_t rate = 0;
  std::vector<int16_t> samples;  // interleaved
};

// fmt is the stream's WAVEFORMATEX with its extra bytes; in is the
// concatenated audio data.
bool DecodeMsAdpcm(const std::vector<uint8_t>& fmt, const std::vector<uint8_t>& in,
                   PcmAudio* out);

}  // namespace video
