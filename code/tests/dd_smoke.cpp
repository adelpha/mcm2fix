// Smoke test for the DirectDraw 7 / Direct3D 7 renderer (src/dd/), without the
// game: draws a small scene the way Mcm2.exe does (flip chain, Z-buffer,
// 16-bit textures, colour keys, transformed and screen-space vertices,
// ProcessVertices, GDI text, keyed sprite blits), reads the back buffer and
// checks a few pixels. Runs on Windows or under Wine.
//
//   dd_smoke.exe [d3d9|d3d11] [exclusive]   (writes mcm2fix.ini next to itself)
//
// Exit code 0 = all checks passed. Details go to stdout and the mcm2fix log.
#include <windows.h>
#include <ddraw.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../src/config.h"
#include "../src/prof.h"
#include "../src/simd.h"
#include "../src/dd/d3d7.h"
#include "../src/dd/directdraw.h"
#include "../src/dd/gdihook.h"
#include "../src/dd/motionblur.h"

using namespace d3d7;

namespace {

int g_fail = 0;

void Check(bool ok, const char* what) {
  std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

IDirectDrawSurface7* MakeTexture(IDirectDraw7* dd, UINT w, UINT h, DWORD r, DWORD g, DWORD b, DWORD a) {
  DDSURFACEDESC2 d{};
  d.dwSize = sizeof(d);
  d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  d.dwWidth = w;
  d.dwHeight = h;
  d.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
  d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
  d.ddpfPixelFormat.dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
  d.ddpfPixelFormat.dwRGBBitCount = 16;
  d.ddpfPixelFormat.dwRBitMask = r;
  d.ddpfPixelFormat.dwGBitMask = g;
  d.ddpfPixelFormat.dwBBitMask = b;
  d.ddpfPixelFormat.dwRGBAlphaBitMask = a;
  IDirectDrawSurface7* s = nullptr;
  dd->CreateSurface(&d, &s, nullptr);
  return s;
}

void Fill16(IDirectDrawSurface7* s, WORD (*f)(UINT x, UINT y)) {
  DDSURFACEDESC2 d{};
  d.dwSize = sizeof(d);
  if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr))) return;
  for (UINT y = 0; y < d.dwHeight; ++y)
    for (UINT x = 0; x < d.dwWidth; ++x)
      reinterpret_cast<WORD*>(static_cast<BYTE*>(d.lpSurface) + y * d.lPitch)[x] = f(x, y);
  s->Unlock(nullptr);
}

// Pixel of a 565 surface as 8-bit r g b.
void ReadRGB(IDirectDrawSurface7* s, UINT x, UINT y, int out[3]) {
  DDSURFACEDESC2 d{};
  d.dwSize = sizeof(d);
  out[0] = out[1] = out[2] = -1;
  if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) return;
  WORD p = reinterpret_cast<WORD*>(static_cast<BYTE*>(d.lpSurface) + y * d.lPitch)[x];
  s->Unlock(nullptr);
  out[0] = ((p >> 11) & 31) * 255 / 31;
  out[1] = ((p >> 5) & 63) * 255 / 63;
  out[2] = (p & 31) * 255 / 31;
}

bool Near(const int c[3], int r, int g, int b, int tol = 24) {
  return std::abs(c[0] - r) <= tol && std::abs(c[1] - g) <= tol && std::abs(c[2] - b) <= tol;
}

void Identity(D3DMATRIX& m) {
  std::memset(&m, 0, sizeof(m));
  m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1;
}

}  // namespace

int main(int argc, char** argv) {
  const char* api = argc > 1 ? argv[1] : "d3d9";
  // "gputext" anywhere after the api: GDI drawn as GPU text (not in the back
  // buffer, so the GDI pixel checks are skipped; look at the window instead).
  bool gputext = false, profile = false;
  for (int i = 2; i < argc; ++i) gputext |= std::strcmp(argv[i], "gputext") == 0;
  int sample_us = 1000;
  for (int i = 2; i < argc; ++i) profile |= std::strcmp(argv[i], "profile") == 0;
  for (int i = 2; i < argc; ++i)
    if (std::strcmp(argv[i], "nosample") == 0) sample_us = 0;
  for (int i = 2; i < argc; ++i)
    if (std::strncmp(argv[i], "us=", 3) == 0) sample_us = std::atoi(argv[i] + 3);
  bool exclusive = false;  // "exclusive": Fullscreen=Exclusive
  bool plain = false;      // "plain": Simd=0 (no SSE2 paths)
  bool motion_blur = false;  // "motionblur": MotionBlur=1 and its own scene and checks at the end
  for (int i = 2; i < argc; ++i) motion_blur |= std::strcmp(argv[i], "motionblur") == 0;
  for (int i = 2; i < argc; ++i) plain |= std::strcmp(argv[i], "plain") == 0;
  for (int i = 2; i < argc; ++i) exclusive |= std::strcmp(argv[i], "exclusive") == 0;
  char ini[MAX_PATH];
  GetModuleFileNameA(nullptr, ini, MAX_PATH);
  std::strcpy(std::strrchr(ini, '\\') + 1, "mcm2fix.ini");
  FILE* f = std::fopen(ini, "w");
  std::fprintf(f, "[mcm2fix]\nRenderer=%s\nAntialiasing=4\nAlphaToCoverage=2\nAlphaLog=1\nVSync=0\nD3D9Log=1\nDDStats=1\nGdiLog=1\nTextLog=1\nGpuText=%d\nTextureReplace=0\nProfile=%d\nProfileSampleUs=%d\n", api,
               gputext ? 1 : 0, profile ? 1 : 0, sample_us);
  if (exclusive) std::fprintf(f, "Fullscreen=Exclusive\n");
  if (motion_blur) std::fprintf(f, "MotionBlur=1\nMotionBlurLog=1\n");
  std::fprintf(f, "Simd=%d\nSimdLog=1\n", plain ? 0 : 1);
  std::fclose(f);
  // No game: Mcm2.exe's image range (where the renderer reads game globals
  // such as the game object pointer at 0x5743ac) reads as zeros.
  VirtualAlloc(reinterpret_cast<void*>(0x400000), 0x300000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  LoadConfig();
  simd::Init();
  prof::Start();
  dd::InstallGdiHooks();

  WNDCLASSA wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "dd_smoke";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowExA(0, "dd_smoke", "dd_smoke", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 800, 600,
                              nullptr, nullptr, wc.hInstance, nullptr);

  IDirectDraw7* dd = nullptr;
  HRESULT hr = dd::CreateDirectDraw(nullptr, reinterpret_cast<void**>(&dd), d3d7::IID_IDirectDraw7, nullptr);
  Check(SUCCEEDED(hr) && dd, "DirectDrawCreateEx");
  if (!dd) return 1;
  Check(SUCCEEDED(dd->SetCooperativeLevel(hwnd, DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE)), "SetCooperativeLevel");
  Check(SUCCEEDED(dd->SetDisplayMode(640, 480, 16, 0, 0)), "SetDisplayMode 640x480x16");

  DDSURFACEDESC2 d{};
  d.dwSize = sizeof(d);
  d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
  d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
  d.dwBackBufferCount = 1;
  IDirectDrawSurface7* primary = nullptr;
  Check(SUCCEEDED(dd->CreateSurface(&d, &primary, nullptr)), "primary + back buffer");
  DDSCAPS2 bc{};
  bc.dwCaps = DDSCAPS_BACKBUFFER;
  IDirectDrawSurface7* back = nullptr;
  primary->GetAttachedSurface(&bc, &back);
  Check(back != nullptr, "back buffer");

  DDSURFACEDESC2 zd{};
  zd.dwSize = sizeof(zd);
  zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  zd.dwWidth = 640;
  zd.dwHeight = 480;
  zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
  zd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
  zd.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
  zd.ddpfPixelFormat.dwZBufferBitDepth = 16;
  zd.ddpfPixelFormat.dwZBitMask = 0xffff;
  IDirectDrawSurface7* z = nullptr;
  dd->CreateSurface(&zd, &z, nullptr);
  Check(z && SUCCEEDED(back->AddAttachedSurface(z)), "Z-buffer");

  IDirect3D7* d3d = nullptr;
  dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d));
  IDirect3DDevice7* dev = nullptr;
  Check(d3d && SUCCEEDED(d3d->CreateDevice(IID_IDirect3DTnLHalDevice, back, &dev)) && dev, "CreateDevice");
  if (!dev) return 1;

  // Textures: a red 565 one, a 4444 one (green, left half transparent), and a
  // 565 sprite with a magenta colour key in the middle.
  IDirectDrawSurface7* red = MakeTexture(dd, 64, 64, 0xf800, 0x07e0, 0x001f, 0);
  Fill16(red, [](UINT, UINT) -> WORD { return 0xf800; });
  IDirectDrawSurface7* green = MakeTexture(dd, 64, 64, 0x0f00, 0x00f0, 0x000f, 0xf000);
  Fill16(green, [](UINT x, UINT) -> WORD { return x < 32 ? 0x00f0 : 0xf0f0; });
  IDirectDrawSurface7* sprite = MakeTexture(dd, 32, 32, 0xf800, 0x07e0, 0x001f, 0);
  Fill16(sprite, [](UINT x, UINT y) -> WORD {
    return (x >= 8 && x < 24 && y >= 8 && y < 24) ? 0xf81f : 0x001f;  // magenta key inside blue
  });
  DDCOLORKEY key{0xf81f, 0xf81f};
  sprite->SetColorKey(DDCKEY_SRCBLT, &key);

  struct TL {
    float x, y, z, rhw;
    DWORD c;
    float u, v;
  };
  struct V3 {
    float x, y, z;
    DWORD c;
    float u, v;
  };
  const DWORD fvf_tl = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | (1 << D3DFVF_TEXCOUNT_SHIFT);
  const DWORD fvf_3d = D3DFVF_XYZ | D3DFVF_DIFFUSE | (1 << D3DFVF_TEXCOUNT_SHIFT);

  D3DVERTEXBUFFERDESC vbd{sizeof(vbd), 0, fvf_3d, 4};
  IDirect3DVertexBuffer7* src_vb = nullptr;
  d3d->CreateVertexBuffer(&vbd, &src_vb, 0);
  D3DVERTEXBUFFERDESC tbd{sizeof(tbd), 0, fvf_tl, 4};
  IDirect3DVertexBuffer7* dst_vb = nullptr;
  d3d->CreateVertexBuffer(&tbd, &dst_vb, 0);
  Check(src_vb && dst_vb, "vertex buffers");

  D3DVIEWPORT7 vp{0, 0, 640, 480, 0.0f, 1.0f};
  dev->SetViewport(&vp);
  D3DMATRIX I, P;
  Identity(I);
  // Perspective: 90 degrees, near 1, far 100.
  std::memset(&P, 0, sizeof(P));
  P.m[0][0] = 0.75f;
  P.m[1][1] = 1.0f;
  P.m[2][2] = 100.0f / 99.0f;
  P.m[2][3] = 1.0f;
  P.m[3][2] = -100.0f / 99.0f;

  int frames = 0;
  for (; frames < 30; ++frames) {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff202020, 1.0f, 0);
    dev->BeginScene();
    dev->SetRenderState(RS_ZENABLE, 1);
    dev->SetRenderState(RS_ZWRITEENABLE, 1);
    dev->SetRenderState(RS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(RS_LIGHTING, 0);
    dev->SetTextureStageState(0, TSS_COLOROP, 4);  // modulate
    dev->SetTextureStageState(0, TSS_COLORARG1, 2);
    dev->SetTextureStageState(0, TSS_COLORARG2, 0);
    dev->SetTextureStageState(0, TSS_ALPHAOP, 4);
    dev->SetTextureStageState(0, TSS_ALPHAARG1, 2);
    dev->SetTextureStageState(0, TSS_ALPHAARG2, 0);
    dev->SetTextureStageState(0, TSS_MAGFILTER, 2);
    dev->SetTextureStageState(0, TSS_MINFILTER, 2);

    // 1. Red screen-space quad, left part (x 40-200, y 40-200), as a fan.
    dev->SetTexture(0, red);
    TL q[4] = {{40, 40, 0.5f, 1, 0xffffffff, 0, 0},
               {200, 40, 0.5f, 1, 0xffffffff, 1, 0},
               {200, 200, 0.5f, 1, 0xffffffff, 1, 1},
               {40, 200, 0.5f, 1, 0xffffffff, 0, 1}};
    dev->DrawPrimitive(D3DPT_TRIANGLEFAN, fvf_tl, q, 4, 0);

    // 2. Untransformed quad in front (z 5), top right, green 4444 with alpha
    //    test GREATER 0 (left half of the texture transparent).
    dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &I);
    dev->SetTransform(D3DTRANSFORMSTATE_VIEW, &I);
    dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &P);
    dev->SetTexture(0, green);
    dev->SetRenderState(RS_ALPHATESTENABLE, 1);
    dev->SetRenderState(RS_ALPHAFUNC, 5);
    dev->SetRenderState(RS_ALPHAREF, 0);
    // Blended + depth write + alpha test GREATER 0: a cut-out, which takes
    // the alpha-to-coverage path (MSAA is on).
    dev->SetRenderState(RS_ALPHABLENDENABLE, 1);
    dev->SetRenderState(RS_SRCBLEND, 5);
    dev->SetRenderState(RS_DESTBLEND, 6);
    V3 w[4] = {{0.5f, 0.5f, 5, 0xffffffff, 0, 0},
               {4.5f, 0.5f, 5, 0xffffffff, 1, 0},
               {0.5f, 4.5f, 5, 0xffffffff, 0, 1},
               {4.5f, 4.5f, 5, 0xffffffff, 1, 1}};
    WORD idx[6] = {0, 1, 2, 1, 3, 2};
    dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, fvf_3d, w, 4, idx, 6, 0);
    dev->SetRenderState(RS_ALPHATESTENABLE, 0);
    dev->SetRenderState(RS_ALPHABLENDENABLE, 0);

    // 3. ProcessVertices + DrawIndexedPrimitiveVB: a white-ish (vertex colour
    //    yellow) quad bottom left, untextured (colour op select diffuse).
    void* p = nullptr;
    DWORD sz = 0;
    src_vb->Lock(0, &p, &sz);
    V3 pv[4] = {{-4.5f, -4.5f, 5, 0xffffff00, 0, 0},
                {-0.5f, -4.5f, 5, 0xffffff00, 1, 0},
                {-4.5f, -0.5f, 5, 0xffffff00, 0, 1},
                {-0.5f, -0.5f, 5, 0xffffff00, 1, 1}};
    std::memcpy(p, pv, sizeof(pv));
    src_vb->Unlock();
    dst_vb->ProcessVertices(D3DVOP_TRANSFORM | D3DVOP_CLIP, 0, 4, src_vb, 0, dev, 0);
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, TSS_COLOROP, 3);  // select arg2 = diffuse
    dev->SetTextureStageState(0, TSS_ALPHAOP, 3);
    dev->DrawIndexedPrimitiveVB(D3DPT_TRIANGLELIST, dst_vb, 0, 4, idx, 6, 0);

    // 4. Linear table fog (start 1, end 10): a white quad at z 20 is all fog
    //    (teal); one at z 2 is mostly white.
    float fs = 1.0f, fe = 10.0f;
    DWORD fsd, fed;
    std::memcpy(&fsd, &fs, 4);
    std::memcpy(&fed, &fe, 4);
    dev->SetRenderState(RS_FOGENABLE, 1);
    dev->SetRenderState(RS_FOGTABLEMODE, 3);
    dev->SetRenderState(RS_FOGSTART, fsd);
    dev->SetRenderState(RS_FOGEND, fed);
    dev->SetRenderState(RS_FOGCOLOR, 0xff008080);
    V3 far_q[4] = {{1, -3, 20, 0xffffffff, 0, 0}, {3, -3, 20, 0xffffffff, 1, 0},
                   {1, -1, 20, 0xffffffff, 0, 1}, {3, -1, 20, 0xffffffff, 1, 1}};
    dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, fvf_3d, far_q, 4, idx, 6, 0);
    V3 near_q[4] = {{0.5f, -0.3f, 2, 0xffffffff, 0, 0}, {0.7f, -0.3f, 2, 0xffffffff, 1, 0},
                    {0.5f, -0.1f, 2, 0xffffffff, 0, 1}, {0.7f, -0.1f, 2, 0xffffffff, 1, 1}};
    dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, fvf_3d, near_q, 4, idx, 6, 0);
    dev->SetRenderState(RS_FOGENABLE, 0);
    // 4c. Draw batching (DrawBatch=1): three quads in a row with the same
    //     state, as a fan, a strip and an indexed list, then a fourth after a
    //     world matrix change (the first three must keep the old matrix).
    //     At z 5: pixel x = 320 + 48 x, pixel y = 240 - 48 y.
    //     Drawn with "select arg 2" (the vertex colour); between the draws
    //     arg 1, which that op doesn't read, changes to a black texture
    //     factor: the batch carries on, and the colours must not change.
    dev->SetRenderState(RS_TEXTUREFACTOR, 0xff000000);
    dev->SetTextureStageState(0, TSS_COLOROP, 3);  // select arg 2
    dev->SetTextureStageState(0, TSS_ALPHAOP, 3);
    V3 fa[4] = {{-2.0f, 2, 5, 0xff00ffff, 0, 0}, {-1.6f, 2, 5, 0xff00ffff, 0, 0},
                {-1.6f, 3, 5, 0xff00ffff, 0, 0}, {-2.0f, 3, 5, 0xff00ffff, 0, 0}};
    dev->DrawPrimitive(D3DPT_TRIANGLEFAN, fvf_3d, fa, 4, 0);
    dev->SetTextureStageState(0, TSS_COLORARG1, 3);  // texture factor (black), not read by select arg 2
    dev->SetTextureStageState(0, TSS_ALPHAARG1, 3);
    V3 st[4] = {{-1.4f, 2, 5, 0xffff00ff, 0, 0}, {-1.0f, 2, 5, 0xffff00ff, 0, 0},
                {-1.4f, 3, 5, 0xffff00ff, 0, 0}, {-1.0f, 3, 5, 0xffff00ff, 0, 0}};
    dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, fvf_3d, st, 4, 0);
    V3 li[4] = {{-0.8f, 2, 5, 0xff0000ff, 0, 0}, {-0.4f, 2, 5, 0xff0000ff, 0, 0},
                {-0.8f, 3, 5, 0xff0000ff, 0, 0}, {-0.4f, 3, 5, 0xff0000ff, 0, 0}};
    dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, fvf_3d, li, 4, idx, 6, 0);
    dev->SetTextureStageState(0, TSS_COLOROP, 4);  // back to modulate: ends the batch first
    dev->SetTextureStageState(0, TSS_ALPHAOP, 4);
    dev->SetTextureStageState(0, TSS_COLORARG1, 2);
    dev->SetTextureStageState(0, TSS_ALPHAARG1, 2);
    D3DMATRIX down = I;
    down.m[3][1] = -1.5f;
    dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &down);
    V3 gr[4] = {{-2.0f, 2, 5, 0xff808080, 0, 0}, {-1.6f, 2, 5, 0xff808080, 0, 0},
                {-1.6f, 3, 5, 0xff808080, 0, 0}, {-2.0f, 3, 5, 0xff808080, 0, 0}};
    dev->DrawPrimitive(D3DPT_TRIANGLEFAN, fvf_3d, gr, 4, 0);
    dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &I);
    // 4b. A screen-space line (like the speedometer needle: colour with alpha
    //     0, texture still set): drawn LineWidth (3) pixels wide.
    dev->SetTexture(0, red);
    TL line[2] = {{450, 460.0f, 0.5f, 1, 0x00ff00ff, 0, 0}, {630, 460.0f, 0.5f, 1, 0x00ff00ff, 0, 0}};
    dev->DrawPrimitive(D3DPT_LINELIST, fvf_tl, line, 2, 0);
    dev->SetTexture(0, nullptr);
    dev->EndScene();

    // 5. Keyed sprite blit onto the back buffer at (400, 300).
    RECT sr{0, 0, 32, 32};
    back->BltFast(400, 300, sprite, &sr, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT);

    // 6. GDI text.
    HDC dc = nullptr;
    if (SUCCEEDED(back->GetDC(&dc))) {
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(255, 255, 255));
      TextOutA(dc, 10, 450, "dd_smoke", 8);
      HFONT font = CreateFontA(15, 0, 0, 0, FW_BOLD, TRUE, 0, 0, ANSI_CHARSET, 0, 0, PROOF_QUALITY, 0, "Arial");
      HGDIOBJ oldf = SelectObject(dc, font);
      SetTextColor(dc, RGB(0, 0, 0));
      TextOutA(dc, 400, 200, "SUPERCROSS", 10);
      SetTextColor(dc, RGB(254, 185, 122));
      TextOutA(dc, 401, 201, "SUPERCROSS", 10);
      HRGN clip = CreateRectRgn(400, 230, 470, 250);
      SelectClipRgn(dc, clip);
      SetTextColor(dc, RGB(255, 255, 255));
      TextOutA(dc, 400, 232, "Clipped text here", 17);
      SelectClipRgn(dc, nullptr);
      DeleteObject(clip);
      RECT dr{400, 260, 620, 280};
      SetBkMode(dc, OPAQUE);
      SetBkColor(dc, RGB(64, 64, 64));
      SetTextColor(dc, RGB(204, 204, 204));
      DrawTextA(dc, "Follow the arrow as you race through easy, open terrain", -1, &dr,
                DT_END_ELLIPSIS | DT_SINGLELINE | DT_VCENTER | DT_CENTER);
      SetBkMode(dc, TRANSPARENT);
      HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
      RECT frr{398, 228, 472, 252};
      FrameRect(dc, &frr, white);
      DeleteObject(white);
      SelectObject(dc, oldf);
      DeleteObject(font);
      HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
      RECT fr{560, 400, 600, 430};
      FillRect(dc, &fr, red);
      // A second, far-away call: pixels between the two must not be
      // copied back (they were never fetched from the GPU).
      RECT fr2{4, 4, 8, 8};
      FillRect(dc, &fr2, red);
      DeleteObject(red);
      back->ReleaseDC(dc);
      // Copy the "SUPERCROSS" text away and back to the top left: GPU text
      // must travel with blits (menus are built on offscreen surfaces).
      static IDirectDrawSurface7* off = nullptr;
      if (!off) {
        DDSURFACEDESC2 od{};
        od.dwSize = sizeof(od);
        od.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        od.dwWidth = 128;
        od.dwHeight = 32;
        od.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
        dd->CreateSurface(&od, &off, nullptr);
      }
      if (off) {
        RECT from{396, 196, 524, 228}, all{0, 0, 128, 32}, to{10, 10, 138, 42};
        off->Blt(&all, back, &from, DDBLT_WAIT, nullptr);
        back->Blt(&to, off, &all, DDBLT_WAIT, nullptr);
      }
      // A tooltip like the game's: text drawn on a system-memory surface,
      // then blitted to the screen.
      static IDirectDrawSurface7* tip = nullptr;
      if (!tip) {
        DDSURFACEDESC2 td{};
        td.dwSize = sizeof(td);
        td.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        td.dwWidth = 134;
        td.dwHeight = 14;
        td.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        dd->CreateSurface(&td, &tip, nullptr);
      }
      if (tip) {
        DDBLTFX fx{};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = 0x4208;  // dark grey in 565
        tip->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        HDC tdc = nullptr;
        if (SUCCEEDED(tip->GetDC(&tdc))) {
          HFONT tf = CreateFontA(12, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, PROOF_QUALITY, 0, "Arial");
          HGDIOBJ of = SelectObject(tdc, tf);
          SetBkMode(tdc, OPAQUE);
          SetBkColor(tdc, RGB(64, 64, 64));
          SetTextColor(tdc, RGB(204, 204, 204));
          RECT tr{0, 0, 134, 14};
          DrawTextA(tdc, "Start a single player race event", -1, &tr,
                    DT_END_ELLIPSIS | DT_SINGLELINE | DT_VCENTER | DT_CENTER);
          SelectObject(tdc, of);
          DeleteObject(tf);
          tip->ReleaseDC(tdc);
        }
        RECT all{0, 0, 134, 14}, to{10, 50, 144, 64};
        back->Blt(&to, tip, &all, DDBLT_WAIT, nullptr);
      }
    }

    if (frames == 29) break;  // check the last frame before flipping
    primary->Flip(nullptr, DDFLIP_WAIT);
  }

  int c[3];
  ReadRGB(back, 120, 120, c);
  std::printf("     red quad pixel %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 255, 0, 0), "screen-space textured fan (red)");
  // Green quad: x 0.5..4.5 at z 5 -> ndc x 0.075..0.675 -> pixels 344..536;
  // y 0.5..4.5 -> ndc 0.1..0.9 -> pixels 24..216 (top). Left half transparent.
  ReadRGB(back, 500, 120, c);
  std::printf("     green quad right half %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 0, 255, 0), "transformed textured quad, opaque half (green)");
  ReadRGB(back, 360, 120, c);
  std::printf("     green quad left half %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 32, 32, 32), "alpha-tested half shows the clear colour");
  // Yellow quad: x -4.5..-0.5 -> ndc -0.675..-0.075 -> pixels 104..296;
  // y -4.5..-0.5 -> ndc -0.9..-0.1 -> pixels 264..456.
  ReadRGB(back, 200, 360, c);
  std::printf("     processed quad %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 255, 255, 0), "ProcessVertices + DrawIndexedPrimitiveVB (yellow)");
  // Far fog quad: x 1..3 at z 20 -> pixels 332..356, y -3..-1 -> 252..276.
  ReadRGB(back, 344, 264, c);
  std::printf("     far fogged quad %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 0, 128, 128), "linear fog, far quad is fog colour");
  // Near quad: x 0.5..0.7 at z 2 -> pixels 380..404, y -0.3..-0.1 -> 252..276.
  // Fog factor (10-2)/9 = 0.89 white.
  ReadRGB(back, 392, 264, c);
  std::printf("     near fogged quad %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 227, 241, 241, 30), "linear fog, near quad mostly unfogged");
  ReadRGB(back, 233, 120, c);
  std::printf("     batched fan %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 0, 255, 255), "batched draws: fan (cyan)");
  ReadRGB(back, 262, 120, c);
  std::printf("     batched strip %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 255, 0, 255), "batched draws: strip (magenta)");
  ReadRGB(back, 291, 120, c);
  std::printf("     batched list %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 0, 0, 255), "batched draws: indexed list (blue)");
  ReadRGB(back, 233, 192, c);
  std::printf("     after a matrix change %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 128, 128, 128), "batched draws: matrix change ends the batch (grey moved down)");
  ReadRGB(back, 404, 304, c);
  std::printf("     sprite border %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 0, 0, 255), "keyed sprite, opaque border (blue)");
  ReadRGB(back, 416, 316, c);
  std::printf("     sprite key %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 32, 32, 32), "keyed sprite, key colour skipped");
  ReadRGB(back, 600, 450, c);
  Check(Near(c, 32, 32, 32), "background (clear colour)");
  ReadRGB(back, 540, 460, c);
  std::printf("     wide line centre %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 255, 0, 255), "wide line, centre row (magenta)");
  ReadRGB(back, 540, 459, c);
  std::printf("     wide line row above %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 255, 0, 255, 40), "wide line, row above also drawn");
  ReadRGB(back, 540, 456, c);
  std::printf("     4 rows above %d %d %d\n", c[0], c[1], c[2]);
  Check(Near(c, 32, 32, 32), "wide line, 4 rows above untouched");
  if (!gputext) {
    ReadRGB(back, 580, 415, c);
    std::printf("     GDI fill %d %d %d\n", c[0], c[1], c[2]);
    Check(Near(c, 255, 0, 0), "GDI FillRect (red)");
    ReadRGB(back, 556, 415, c);
    Check(Near(c, 32, 32, 32), "GDI leaves pixels beside the fill alone");
  }

  primary->Flip(nullptr, DDFLIP_WAIT);
  if (motion_blur) {
    // MotionBlur: a full-screen striped 3D scene, a bike heading straight
    // into the screen at 68 mph, then a screen-space HUD square. The edges
    // of the scene must be blurred, the middle and the HUD square sharp.
    IDirectDrawSurface7* stripes = MakeTexture(dd, 64, 64, 0xf800, 0x07e0, 0x001f, 0);
    Fill16(stripes, [](UINT x, UINT) -> WORD { return (x / 4) & 1 ? 0xffff : 0x0000; });
    static uint8_t bike[0x800], race[0x100];
    *reinterpret_cast<void**>(bike + 0x740) = race;
    *reinterpret_cast<void**>(race + 0x38) = bike;
    const float vel[3] = {0.0f, 0.0f, 100.0f};  // ft/s, straight ahead
    WORD idx[6] = {0, 1, 2, 1, 3, 2};
    std::memcpy(bike + 0x64, vel, sizeof(vel));
    for (int fr = 0; fr < 3; ++fr) {
      dd::MotionBlurBikeStep(bike);
      dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff202020, 1.0f, 0);
      dev->BeginScene();
      dev->SetRenderState(RS_ZENABLE, 1);
      dev->SetRenderState(RS_ZWRITEENABLE, 1);
      dev->SetRenderState(RS_ZFUNC, 4);  // less-equal
      dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &I);
      dev->SetTransform(D3DTRANSFORMSTATE_VIEW, &I);
      dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &P);
      dev->SetTextureStageState(0, TSS_COLOROP, 2);  // select texture
      dev->SetTextureStageState(0, TSS_ALPHAOP, 2);
      dev->SetTextureStageState(0, TSS_COLORARG1, 2);
      dev->SetTextureStageState(0, TSS_ALPHAARG1, 2);
      dev->SetTextureStageState(0, TSS_MAGFILTER, 1);  // point
      dev->SetTextureStageState(0, TSS_MINFILTER, 1);
      dev->SetTexture(0, stripes);
      V3 bg[4] = {{-8, 6, 5, 0xffffffff, 0, 0}, {8, 6, 5, 0xffffffff, 10, 0},
                  {-8, -6, 5, 0xffffffff, 0, 1}, {8, -6, 5, 0xffffffff, 10, 1}};
      dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, fvf_3d, bg, 4, idx, 6, 0);
      // More 3D draws (a scene has hundreds), hidden behind the stripes.
      for (int k = 0; k < 20; ++k) {
        V3 t[4] = {{0, 0, 50, 0xffffffff, 0, 0}, {0.1f, 0, 50, 0xffffffff, 1, 0},
                   {0, 0.1f, 50, 0xffffffff, 0, 1}, {0.1f, 0.1f, 50, 0xffffffff, 1, 1}};
        dev->SetRenderState(RS_ZBIAS, k & 1);  // keeps them from merging
        dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, fvf_3d, t, 4, idx, 6, 0);
      }
      // HUD: screen space, no depth test, white.
      dev->SetRenderState(RS_ZENABLE, 0);
      dev->SetTexture(0, nullptr);
      dev->SetTextureStageState(0, TSS_COLOROP, 3);  // select diffuse
      dev->SetTextureStageState(0, TSS_ALPHAOP, 3);
      TL hud[4] = {{590, 10, 0, 1, 0xffffffff, 0, 0}, {630, 10, 0, 1, 0xffffffff, 0, 0},
                   {630, 50, 0, 1, 0xffffffff, 0, 0}, {590, 50, 0, 1, 0xffffffff, 0, 0}};
      dev->DrawPrimitive(D3DPT_TRIANGLEFAN, fvf_tl, hud, 4, 0);
      dev->EndScene();
      if (fr == 2) break;
      primary->Flip(nullptr, DDFLIP_WAIT);
    }
    // Stripes are 4 texels of 64 over 1/10 of the quad: about 3.2 pixels.
    auto spread = [&](int y, int x0, int x1, int& lo, int& hi) {
      lo = 255, hi = 0;
      for (int x = x0; x < x1; ++x) {
        int px[3];
        ReadRGB(back, x, y, px);
        lo = px[0] < lo ? px[0] : lo;
        hi = px[0] > hi ? px[0] : hi;
      }
    };
    int lo = 0, hi = 0;
    spread(240, 310, 330, lo, hi);
    std::printf("     motion blur middle: red %d..%d\n", lo, hi);
    Check(lo < 40 && hi > 215, "motion blur: middle of the screen stays sharp");
    spread(240, 4, 24, lo, hi);
    std::printf("     motion blur left edge: red %d..%d\n", lo, hi);
    Check(lo > 50 && hi < 205, "motion blur: edge of the screen blurred");
    int px[3];
    ReadRGB(back, 610, 30, px);
    std::printf("     HUD square %d %d %d\n", px[0], px[1], px[2]);
    Check(Near(px, 255, 255, 255, 8), "motion blur: HUD drawn after the blur, sharp");
    ReadRGB(back, 591, 11, px);
    Check(Near(px, 255, 255, 255, 8), "motion blur: HUD corner not smeared");
    primary->Flip(nullptr, DDFLIP_WAIT);
    stripes->Release();
  }
  bool hold = false;
  for (int i = 2; i < argc; ++i) hold |= std::strcmp(argv[i], "hold") == 0;
  if (hold) {
    // No 3D draws for a while: the renderer label should appear. Keep the
    // window up so it can be looked at.
    Sleep(700);
    for (int i = 0; i < 40; i++) {
      primary->Flip(nullptr, DDFLIP_WAIT);
      Sleep(100);
    }
  }
  if (profile) {
    // Frames for the profiler, one of them slow (a hitch), then its files.
    for (int i = 0; i < 400; i++) {
      dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff202020, 1.0f, 0);
      if (i == 200) Sleep(40);
      volatile double x = 0;
      for (int k = 0; k < 20000; ++k) x += k * 0.5;
      primary->Flip(nullptr, DDFLIP_WAIT);
    }
    prof::Stop();
  }
  dev->Release();
  d3d->Release();
  std::printf("%s: %d check(s) failed\n", api, g_fail);
  return g_fail ? 1 : 0;
}
