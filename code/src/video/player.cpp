// The game's video player, without DirectShow.
//
// Mcm2.exe plays ui\Intro.avi, ui\Credits.avi and ui\events.avi through a
// small overlay class (vtable 0x55a1f8). Its setup (0x4a3ad0) builds a
// DirectShow amstream graph that decodes into a system DirectDraw surface;
// every frame it copies that surface into its own texture (+0x2c). Indeo 5
// is not part of Windows 11, so we replace the setup: the AVI is read and
// decoded here, audio goes to waveOut, and the game's own per-frame code
// keeps running against two small stand-ins for the amstream objects it
// calls (the multimedia stream at +0x50 and the stream sample at +0x64).
//
// Object layout used (offsets into the overlay):
//   +0x18 render context ([+0x48] = a surface with the game's pixel format)
//   +0x2c texture drawn by the overlay     +0x30/+0x34 video width/height
//   +0x38 frame source, copied to +0x2c    +0x3c RECT {0,0,w,h}
//   +0x50 IMultiMediaStream                +0x58 the game's IDirectDraw7
//   +0x64 IDirectDrawStreamSample          +0x70/+0x74 end callback, argument
#include <windows.h>
#include <ddraw.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <set>

#include "../patch.h"
#include "adpcm.h"
#include "audio_out.h"
#include "avi.h"
#include "indeo5.h"
#include "player.h"

namespace video {
namespace {

constexpr uintptr_t kVideoInit = 0x4A3AD0;
const uint8_t kVideoInitEntry[] = {0x81, 0xEC, 0xF4, 0x02, 0x00, 0x00};  // sub esp,2F4h
constexpr uintptr_t kOverlayBaseInit = 0x46A220;

constexpr HRESULT kPending = 0x00040001;  // MS_S_PENDING
constexpr int64_t kUnits = 10000000;      // STREAM_TIME units per second

template <typename T>
T& At(void* base, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}

int64_t Now() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<int64_t>(static_cast<double>(c.QuadPart) * kUnits / freq.QuadPart);
}

// YUV (BT.601, studio range) to the surface's RGB format.
struct PixelPacker {
  int bytes = 0;
  int rs = 0, rb = 0, gs = 0, gb = 0, bs = 0, bb = 0;  // shift and bit count
  static void Mask(DWORD m, int* shift, int* bits) {
    *shift = 0;
    *bits = 0;
    if (!m) return;
    while (!(m & 1)) {
      m >>= 1;
      ++*shift;
    }
    while (m & 1) {
      m >>= 1;
      ++*bits;
    }
  }
  bool Init(const DDPIXELFORMAT& pf) {
    if (!(pf.dwFlags & DDPF_RGB)) return false;
    bytes = static_cast<int>(pf.dwRGBBitCount / 8);
    if (bytes != 2 && bytes != 4) return false;
    Mask(pf.dwRBitMask, &rs, &rb);
    Mask(pf.dwGBitMask, &gs, &gb);
    Mask(pf.dwBBitMask, &bs, &bb);
    return rb && gb && bb;
  }
  uint32_t Pack(int r, int g, int b) const {
    return (static_cast<uint32_t>(r >> (8 - rb)) << rs) |
           (static_cast<uint32_t>(g >> (8 - gb)) << gs) |
           (static_cast<uint32_t>(b >> (8 - bb)) << bs);
  }
};

uint8_t Clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

void ConvertPicture(const YuvPicture& pic, const PixelPacker& px, uint8_t* dst, LONG pitch,
                    int w, int h) {
  w = std::min(w, pic.width);
  h = std::min(h, pic.height);
  for (int y = 0; y < h; ++y) {
    const uint8_t* yl = pic.plane[0] + y * pic.stride[0];
    const int cy = std::min(y / 4, pic.chroma_h - 1);
    const uint8_t* ul = pic.plane[1] + cy * pic.stride[1];
    const uint8_t* vl = pic.plane[2] + cy * pic.stride[2];
    uint8_t* out = dst + y * pitch;
    for (int x = 0; x < w; ++x) {
      const int cx = std::min(x / 4, pic.chroma_w - 1);
      const int c = (yl[x] - 16) * 298;
      const int d = ul[cx] - 128, e = vl[cx] - 128;
      const uint32_t v = px.Pack(Clamp8((c + 409 * e + 128) >> 8),
                                 Clamp8((c - 100 * d - 208 * e + 128) >> 8),
                                 Clamp8((c + 516 * d + 128) >> 8));
      if (px.bytes == 2) {
        reinterpret_cast<uint16_t*>(out)[x] = static_cast<uint16_t>(v);
      } else {
        reinterpret_cast<uint32_t*>(out)[x] = v | 0xff000000u;
      }
    }
  }
}

// The decoded video and its playback clock. Stands in for the game's
// IAMMultiMediaStream; only the IMultiMediaStream methods it calls do work.
class Stream final : public IUnknown {
 public:
  AviFile avi;
  PcmAudio pcm;
  std::unique_ptr<Indeo5Decoder> decoder;
  AudioOut audio;

  bool Open(const char* path) {
    if (!avi.Open(path)) {
      patch::Log("video: %s: %s", path, avi.error.c_str());
      return false;
    }
    if (avi.fourcc != 0x30355649 && avi.fourcc != 0x30357669) {  // IV50 / iv50
      patch::Log("video: %s: unsupported codec %.4s", path,
                 reinterpret_cast<const char*>(&avi.fourcc));
      return false;
    }
    decoder = CreateIndeo5Decoder(static_cast<int>(avi.width), static_cast<int>(avi.height));
    if (!decoder) {
      patch::Log("video: %s: no Indeo 5 decoder in this build", path);
      return false;
    }
    if (!avi.audio.empty()) {
      std::vector<uint8_t> bytes;
      for (const Chunk& c : avi.audio) bytes.insert(bytes.end(), c.data, c.data + c.size);
      if (!DecodeMsAdpcm(avi.audio_format, bytes, &pcm)) {
        patch::Log("video: %s: audio format not supported, playing without sound", path);
        pcm = PcmAudio{};
      }
    }
    duration_ = static_cast<int64_t>(avi.frames.size()) * avi.scale * kUnits / avi.rate;
    patch::Log("video: %s: %ux%u, %zu frames at %.2f fps, %s", path, avi.width, avi.height,
               avi.frames.size(), static_cast<double>(avi.rate) / avi.scale,
               pcm.samples.empty() ? "no sound" : "with sound");
    return true;
  }

  int64_t Time() const { return running_ ? pos_ + (Now() - started_) : pos_; }
  int FrameAt(int64_t t) const {
    int64_t f = t * avi.rate / (static_cast<int64_t>(avi.scale) * kUnits);
    return static_cast<int>(std::clamp<int64_t>(f, 0, static_cast<int64_t>(avi.frames.size()) - 1));
  }

  // Decodes up to frame n. Returns the picture to show, or nullptr if
  // nothing changed.
  const YuvPicture* DecodeTo(int n) {
    if (n == shown_) return nullptr;
    if (n < decoded_) {
      decoder->Reset();
      decoded_ = -1;
      have_pic_ = false;
    }
    while (decoded_ < n) {
      ++decoded_;
      const Chunk& c = avi.frames[decoded_];
      YuvPicture p;
      if (c.size && decoder->Decode(c.data, c.size, &p)) {
        pic_ = p;
        have_pic_ = true;
      }
    }
    shown_ = n;
    return have_pic_ ? &pic_ : nullptr;
  }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    if (out) *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --refs_;
    if (!r) delete this;
    return r;
  }
  // IMultiMediaStream, in vtable order
  virtual HRESULT STDMETHODCALLTYPE GetInformation(DWORD*, DWORD*) { return E_NOTIMPL; }
  virtual HRESULT STDMETHODCALLTYPE GetMediaStream(REFGUID, void** out) {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  virtual HRESULT STDMETHODCALLTYPE EnumMediaStreams(LONG, void** out) {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  virtual HRESULT STDMETHODCALLTYPE GetState(int* state) {
    if (!state) return E_POINTER;
    *state = running_ ? 1 : 0;  // STREAMSTATE_RUN / STOP
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE SetState(int state) {
    if (state == 1 && !running_) {
      started_ = Now();
      running_ = true;
      StartAudio();
    } else if (state == 0 && running_) {
      pos_ = Time();
      running_ = false;
      audio.Stop();
    }
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE GetTime(int64_t* t) {
    if (!t) return E_POINTER;
    *t = Time();
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE GetDuration(int64_t* d) {
    if (!d) return E_POINTER;
    *d = duration_;
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE Seek(int64_t t) {
    pos_ = std::clamp<int64_t>(t, 0, duration_);
    started_ = Now();
    if (running_) StartAudio();
    return S_OK;
  }
  virtual HRESULT STDMETHODCALLTYPE GetEndOfStreamEventHandle(HANDLE* h) {
    if (h) *h = nullptr;
    return E_NOTIMPL;
  }

 private:
  ~Stream() = default;
  void StartAudio() {
    if (pcm.samples.empty()) return;
    const size_t frame = static_cast<size_t>(pos_ * pcm.rate / kUnits);
    if (!audio.Start(pcm, frame)) audio.Stop();
  }

  ULONG refs_ = 1;
  int64_t duration_ = 0;
  int64_t pos_ = 0;
  int64_t started_ = 0;
  bool running_ = false;
  int decoded_ = -1, shown_ = -1;
  YuvPicture pic_{};
  bool have_pic_ = false;
};

std::set<const void*> g_samples;

// Stands in for the IDirectDrawStreamSample: CompletionStatus delivers the
// frame for the current time into the frame source surface.
class Sample final : public IUnknown {
 public:
  Sample(Stream* s, IDirectDrawSurface7* target) : stream_(s), target_(target) {
    stream_->AddRef();
    target_->AddRef();
    g_samples.insert(this);
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    if (SUCCEEDED(target_->GetSurfaceDesc(&d))) {
      packer_ok_ = packer_.Init(d.ddpfPixelFormat);
      w_ = static_cast<int>(d.dwWidth);
      h_ = static_cast<int>(d.dwHeight);
    }
  }
  bool Ok() const { return packer_ok_; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    if (out) *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --refs_;
    if (!r) delete this;
    return r;
  }
  // IStreamSample, in vtable order
  virtual HRESULT STDMETHODCALLTYPE GetMediaStream(void** out) {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  virtual HRESULT STDMETHODCALLTYPE GetSampleTimes(void*, void*, void*) { return E_NOTIMPL; }
  virtual HRESULT STDMETHODCALLTYPE SetSampleTimes(const void*, const void*) { return E_NOTIMPL; }
  virtual HRESULT STDMETHODCALLTYPE Update(DWORD, HANDLE, void*, DWORD_PTR) { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE CompletionStatus(DWORD, DWORD) {
    const YuvPicture* pic = stream_->DecodeTo(stream_->FrameAt(stream_->Time()));
    if (!pic) return kPending;
    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    if (FAILED(target_->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) {
      return kPending;
    }
    ConvertPicture(*pic, packer_, static_cast<uint8_t*>(d.lpSurface), d.lPitch, w_, h_);
    target_->Unlock(nullptr);
    return S_OK;
  }

 private:
  ~Sample() {
    g_samples.erase(this);
    target_->Release();
    stream_->Release();
  }
  ULONG refs_ = 1;
  Stream* stream_;
  IDirectDrawSurface7* target_;
  PixelPacker packer_;
  bool packer_ok_ = false;
  int w_ = 0, h_ = 0;
};

using InitFn = void*(__fastcall*)(void* self, void* edx, void* parent, const char* file,
                                  void* cb, void* arg);
InitFn g_original_init = nullptr;

IDirectDrawSurface7* MakeSurface(IDirectDraw7* dd, DDSURFACEDESC2 d, DWORD caps) {
  d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  d.ddsCaps = DDSCAPS2{};
  d.ddsCaps.dwCaps = caps;
  IDirectDrawSurface7* s = nullptr;
  if (FAILED(dd->CreateSurface(&d, &s, nullptr))) return nullptr;
  return s;
}

void* __fastcall VideoInit(void* self, void* edx, void* parent, const char* file, void* cb,
                           void* arg) {
  Stream* stream = new Stream;
  if (!stream->Open(file)) {
    stream->Release();
    return g_original_init(self, edx, parent, file, cb, arg);
  }

  reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(kOverlayBaseInit)(self, nullptr,
                                                                             parent);
  auto* dd = At<IDirectDraw7*>(At<void*>(parent, 4), 0x190);
  At<IDirectDraw7*>(self, 0x58) = dd;
  At<void*>(self, 0x70) = cb;
  At<void*>(self, 0x74) = arg;

  const int w = static_cast<int>(stream->avi.width), h = static_cast<int>(stream->avi.height);
  DDSURFACEDESC2 d{};
  d.dwSize = sizeof(d);
  auto* ref = At<IDirectDrawSurface7*>(At<void*>(self, 0x18), 0x48);
  bool ok = dd && ref && SUCCEEDED(ref->GetSurfaceDesc(&d));
  d.dwWidth = static_cast<DWORD>(w);
  d.dwHeight = static_cast<DWORD>(h);
  IDirectDrawSurface7* tex = ok ? MakeSurface(dd, d, 0x2800) : nullptr;  // as the game asks
  IDirectDrawSurface7* src =
      tex ? MakeSurface(dd, d, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY) : nullptr;
  Sample* sample = src ? new Sample(stream, src) : nullptr;
  if (!sample || !sample->Ok()) {
    patch::Log("video: %s: could not create its surfaces", file);
    if (sample) sample->Release();
    if (src) src->Release();
    if (tex) tex->Release();
    stream->Release();
    using DestroyFn = void(__fastcall*)(void*, void*);
    reinterpret_cast<DestroyFn>((*reinterpret_cast<void***>(self))[2])(self, nullptr);
    return nullptr;
  }
  At<IDirectDrawSurface7*>(self, 0x2c) = tex;
  At<int>(self, 0x30) = w;
  At<int>(self, 0x34) = h;
  At<IDirectDrawSurface7*>(self, 0x38) = src;
  RECT& r = At<RECT>(self, 0x3c);
  r = RECT{0, 0, w, h};
  At<IUnknown*>(self, 0x50) = stream;
  At<IUnknown*>(self, 0x64) = sample;
  return self;
}

}  // namespace

bool IsOwnSample(const void* sample) { return g_samples.count(sample) != 0; }

void ApplyVideoPlayer() {
  if (!patch::Matches(kVideoInit, kVideoInitEntry, sizeof(kVideoInitEntry))) {
    patch::Log("video player: setup code differs, not applied");
    return;
  }
  // Trampoline to the original setup, for files we cannot play.
  auto* t = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return;
  std::memcpy(t, kVideoInitEntry, sizeof(kVideoInitEntry));
  size_t n = sizeof(kVideoInitEntry);
  t[n++] = 0x68;  // push resume; ret
  const uint32_t resume = static_cast<uint32_t>(kVideoInit + sizeof(kVideoInitEntry));
  std::memcpy(t + n, &resume, 4);
  n += 4;
  t[n++] = 0xC3;
  FlushInstructionCache(GetCurrentProcess(), t, n);
  g_original_init = reinterpret_cast<InitFn>(t);
  patch::Jump(kVideoInit, reinterpret_cast<uintptr_t>(&VideoInit), sizeof(kVideoInitEntry));
  patch::Log("video player: applied");
}

}  // namespace video
