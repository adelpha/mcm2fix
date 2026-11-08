#include "audio_out.h"

namespace video {

bool AudioOut::Start(const PcmAudio& pcm, size_t first_frame) {
  Stop();
  if (pcm.channels <= 0 || !pcm.rate) return false;
  const size_t total = pcm.samples.size() / pcm.channels;
  if (first_frame >= total) return false;

  WAVEFORMATEX f{};
  f.wFormatTag = WAVE_FORMAT_PCM;
  f.nChannels = static_cast<WORD>(pcm.channels);
  f.nSamplesPerSec = pcm.rate;
  f.wBitsPerSample = 16;
  f.nBlockAlign = static_cast<WORD>(2 * pcm.channels);
  f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
  if (waveOutOpen(&out_, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
    out_ = nullptr;
    return false;
  }
  hdr_ = WAVEHDR{};
  hdr_.lpData = const_cast<LPSTR>(
      reinterpret_cast<const char*>(pcm.samples.data() + first_frame * pcm.channels));
  hdr_.dwBufferLength = static_cast<DWORD>((total - first_frame) * f.nBlockAlign);
  if (waveOutPrepareHeader(out_, &hdr_, sizeof(hdr_)) != MMSYSERR_NOERROR) {
    Stop();
    return false;
  }
  prepared_ = true;
  if (waveOutWrite(out_, &hdr_, sizeof(hdr_)) != MMSYSERR_NOERROR) {
    Stop();
    return false;
  }
  return true;
}

void AudioOut::Stop() {
  if (!out_) return;
  waveOutReset(out_);
  if (prepared_) waveOutUnprepareHeader(out_, &hdr_, sizeof(hdr_));
  prepared_ = false;
  waveOutClose(out_);
  out_ = nullptr;
}

}  // namespace video
