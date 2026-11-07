// Installs the D3D11 backend: Mcm2.exe's DirectDrawCreateEx import is pointed
// at mcm2fix's own DirectDraw implementation.
#include <windows.h>

#include "../patch.h"
#include "backend.h"
#include "directdraw.h"

// Declared outside the anonymous namespace: there, GCC sees no class that
// implements it and turns every call into a pure-virtual call.
namespace video {
struct IStreamSample : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetMediaStream(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetSampleTimes(void*, void*, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetSampleTimes(const void*, const void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE Update(DWORD flags, HANDLE ev, void* apc, DWORD_PTR data) = 0;
  virtual HRESULT STDMETHODCALLTYPE CompletionStatus(DWORD flags, DWORD timeout) = 0;
};
}  // namespace video

namespace {

using video::IStreamSample;

constexpr uintptr_t kIatDirectDrawCreateEx = 0x5553EC;

// The intro video loop (0x4A3DE0) asks its DirectShow sample whether the
// next frame is decoded: CompletionStatus(COMPSTAT_WAIT, 25 ms). With the
// system DirectDraw in normal mode, the asynchronous update it queued never
// completes, so the picture freezes on the first frame. When it stays
// pending, cancel it and decode the frame synchronously instead.

constexpr HRESULT kPending = 0x00040001;  // MS_S_PENDING
constexpr DWORD kWait = 2, kAbort = 4;    // COMPSTAT_WAIT, COMPSTAT_ABORT

HRESULT __stdcall VideoStep(IStreamSample* s) {
  static int pending = 0;
  HRESULT hr = s->CompletionStatus(kWait, 25);
  if (hr == kPending && ++pending < 3) return hr;
  if (hr != S_OK) {
    dd::LogOnce("video: frame status %08lx, decoding synchronously", hr);
    if (hr == kPending) s->CompletionStatus(kAbort, 0);
    hr = s->Update(0, nullptr, nullptr, 0);
    if (FAILED(hr)) dd::LogOnce("video: synchronous update failed %08lx", hr);
  }
  pending = 0;
  return hr;
}

// The video setup (0x4A3C2D) asks the DirectShow video stream for its
// format; by default that is the desktop's 32-bit format, which the game's
// Indeo videos stop decoding into after one frame. Ask for the 16-bit
// format the game ran in originally.
HRESULT __stdcall VideoGetFormat(IUnknown* stream, DDSURFACEDESC* cur, void** pal,
                                 DDSURFACEDESC* desired, DWORD* flags) {
  using GetFn = HRESULT(__stdcall*)(IUnknown*, DDSURFACEDESC*, void**, DDSURFACEDESC*, DWORD*);
  using SetFn = HRESULT(__stdcall*)(IUnknown*, const DDSURFACEDESC*, void*);
  void** vt = *reinterpret_cast<void***>(stream);
  HRESULT hr = reinterpret_cast<GetFn>(vt[9])(stream, cur, pal, desired, flags);
  if (FAILED(hr) || !cur) return hr;
  dd::Log("video: stream format %lux%lu, %lu bpp (flags %lx)", cur->dwWidth, cur->dwHeight,
          cur->ddpfPixelFormat.dwRGBBitCount, cur->dwFlags);
  if (cur->ddpfPixelFormat.dwRGBBitCount != 16) {
    DDSURFACEDESC d = *cur;
    d.dwFlags |= DDSD_PIXELFORMAT;
    d.ddpfPixelFormat = DDPIXELFORMAT{};
    d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB;
    d.ddpfPixelFormat.dwRGBBitCount = 16;
    d.ddpfPixelFormat.dwRBitMask = 0xf800;
    d.ddpfPixelFormat.dwGBitMask = 0x07e0;
    d.ddpfPixelFormat.dwBBitMask = 0x001f;
    HRESULT set = reinterpret_cast<SetFn>(vt[10])(stream, &d, nullptr);
    dd::Log("video: switching the stream to 16-bit -> %08lx", set);
    if (SUCCEEDED(set)) *cur = d;
  }
  return hr;
}

void PatchVideoFormat() {
  const uint8_t original[] = {0x8b, 0x10, 0x53, 0x51, 0x50, 0xff, 0x52, 0x24};
  if (!patch::Matches(0x4A3C28, original, sizeof(original))) {
    patch::Log("video format patch: unexpected bytes, skipped");
    return;
  }
  const uint8_t head[] = {0x53, 0x51, 0x50};  // push ebx; push ecx; push eax
  patch::Write(0x4A3C28, head, sizeof(head));
  patch::Call(0x4A3C2B, reinterpret_cast<const void*>(&VideoGetFormat), 5);
  patch::Log("video format patch: applied");
}

void PatchVideoLoop() {
  const uint8_t original[] = {0x8b, 0x46, 0x64, 0x6a, 0x19, 0x6a, 0x02, 0x50,
                              0x8b, 0x08, 0xff, 0x51, 0x1c};
  if (!patch::Matches(0x4A3DE0, original, sizeof(original))) {
    patch::Log("video loop patch: unexpected bytes, skipped");
    return;
  }
  const uint8_t head[] = {0x8b, 0x46, 0x64, 0x50};  // mov eax,[esi+64h]; push eax
  patch::Write(0x4A3DE0, head, sizeof(head));
  patch::Call(0x4A3DE4, reinterpret_cast<const void*>(&VideoStep), 9);
  patch::Log("video loop patch: applied");
}

}  // namespace

void ApplyD3D11Backend() {
  patch::HookIat(kIatDirectDrawCreateEx, reinterpret_cast<void*>(&dd::CreateDirectDraw));
  PatchVideoFormat();
  PatchVideoLoop();
  patch::Log("d3d11 backend: on");
}
