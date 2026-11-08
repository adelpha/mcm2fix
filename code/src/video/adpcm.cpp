#include "adpcm.h"

#include <algorithm>
#include <cstring>

namespace video {
namespace {

const int kAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614,
                        768, 614, 512, 409, 307, 230, 230, 230};
const int kDefaultCoef[7][2] = {{256, 0}, {512, -256}, {0, 0},   {192, 64},
                                {240, 0}, {460, -208}, {392, -232}};

uint16_t Rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

struct State {
  int c1, c2, delta, s1, s2;
  int16_t Next(int nib) {
    int pred = (s1 * c1 + s2 * c2) / 256;  // truncating, as Windows does
    int signed_nib = nib >= 8 ? nib - 16 : nib;
    pred += signed_nib * delta;
    pred = std::clamp(pred, -32768, 32767);
    s2 = s1;
    s1 = pred;
    delta = (kAdapt[nib] * delta) >> 8;
    if (delta < 16) delta = 16;
    return static_cast<int16_t>(pred);
  }
};

}  // namespace

bool DecodeMsAdpcm(const std::vector<uint8_t>& fmt, const std::vector<uint8_t>& in,
                   PcmAudio* out) {
  if (fmt.size() < 20 || Rd16(fmt.data()) != 2) return false;
  const int channels = Rd16(fmt.data() + 2);
  uint32_t rate;
  std::memcpy(&rate, fmt.data() + 4, 4);
  const int block = Rd16(fmt.data() + 12);
  if (channels < 1 || channels > 2 || block < 7 * channels) return false;
  int coef[256][2];
  int ncoef = 7;
  std::memcpy(coef, kDefaultCoef, sizeof(kDefaultCoef));
  if (fmt.size() >= 22) {
    int n = Rd16(fmt.data() + 20);
    if (n > 0 && n <= 256 && fmt.size() >= 22 + 4 * static_cast<size_t>(n)) {
      ncoef = n;
      for (int i = 0; i < n; ++i) {
        coef[i][0] = static_cast<int16_t>(Rd16(fmt.data() + 22 + 4 * i));
        coef[i][1] = static_cast<int16_t>(Rd16(fmt.data() + 24 + 4 * i));
      }
    }
  }

  out->channels = channels;
  out->rate = rate;
  out->samples.clear();
  out->samples.reserve(in.size() * 2 + 16);
  for (size_t pos = 0; pos + 7 * channels <= in.size(); pos += block) {
    const uint8_t* b = in.data() + pos;
    const size_t len = std::min<size_t>(block, in.size() - pos);
    State st[2];
    for (int c = 0; c < channels; ++c) {
      int pi = b[c];
      if (pi >= ncoef) pi = 0;
      st[c].c1 = coef[pi][0];
      st[c].c2 = coef[pi][1];
      st[c].delta = static_cast<int16_t>(Rd16(b + channels + 2 * c));
      st[c].s1 = static_cast<int16_t>(Rd16(b + 3 * channels + 2 * c));
      st[c].s2 = static_cast<int16_t>(Rd16(b + 5 * channels + 2 * c));
    }
    for (int c = 0; c < channels; ++c) out->samples.push_back(static_cast<int16_t>(st[c].s2));
    for (int c = 0; c < channels; ++c) out->samples.push_back(static_cast<int16_t>(st[c].s1));
    int c = 0;
    for (size_t i = 7 * channels; i < len; ++i) {
      out->samples.push_back(st[c].Next(b[i] >> 4));
      c = (c + 1) % channels;
      out->samples.push_back(st[c].Next(b[i] & 15));
      c = (c + 1) % channels;
    }
  }
  return true;
}

}  // namespace video
