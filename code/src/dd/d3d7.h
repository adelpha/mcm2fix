// Direct3D 7 interface and type declarations for the D3D11 backend.
//
// Current Windows SDKs no longer ship d3d.h / d3dtypes.h / d3dcaps.h, so the
// parts mcm2fix implements are declared here from the documented DirectX 7
// ABI. Interfaces are abstract structs whose virtual methods are in vtable
// order; DirectDraw types come from ddraw.h, which the SDK still has.
#pragma once
#include <windows.h>
#include <ddraw.h>

namespace d3d7 {

typedef DWORD D3DCOLOR;
typedef float D3DVALUE;

struct D3DVECTOR {
  float x, y, z;
};
struct D3DMATRIX {
  float m[4][4];
};
struct D3DRECT {
  LONG x1, y1, x2, y2;
};
struct D3DCOLORVALUE {
  float r, g, b, a;
};
struct D3DVIEWPORT7 {
  DWORD dwX, dwY, dwWidth, dwHeight;
  float dvMinZ, dvMaxZ;
};
struct D3DMATERIAL7 {
  D3DCOLORVALUE diffuse, ambient, specular, emissive;
  float power;
};
struct D3DLIGHT7 {
  DWORD dltType;
  D3DCOLORVALUE dcvDiffuse, dcvSpecular, dcvAmbient;
  D3DVECTOR dvPosition, dvDirection;
  float dvRange, dvFalloff, dvAttenuation0, dvAttenuation1, dvAttenuation2, dvTheta, dvPhi;
};
struct D3DCLIPSTATUS {
  DWORD dwFlags, dwStatus;
  float minx, maxx, miny, maxy, minz, maxz;
};
struct D3DVERTEXBUFFERDESC {
  DWORD dwSize, dwCaps, dwFVF, dwNumVertices;
};
struct D3DDP_PTRSTRIDE {
  void* lpvData;
  DWORD dwStride;
};
struct D3DDRAWPRIMITIVESTRIDEDDATA {
  D3DDP_PTRSTRIDE position, normal, diffuse, specular, textureCoords[8];
};

struct D3DPRIMCAPS {
  DWORD dwSize, dwMiscCaps, dwRasterCaps, dwZCmpCaps, dwSrcBlendCaps, dwDestBlendCaps,
      dwAlphaCmpCaps, dwShadeCaps, dwTextureCaps, dwTextureFilterCaps, dwTextureBlendCaps,
      dwTextureAddressCaps, dwStippleWidth, dwStippleHeight;
};
struct D3DDEVICEDESC7 {
  DWORD dwDevCaps;
  D3DPRIMCAPS dpcLineCaps, dpcTriCaps;
  DWORD dwDeviceRenderBitDepth, dwDeviceZBufferBitDepth;
  DWORD dwMinTextureWidth, dwMinTextureHeight, dwMaxTextureWidth, dwMaxTextureHeight;
  DWORD dwMaxTextureRepeat, dwMaxTextureAspectRatio, dwMaxAnisotropy;
  float dvGuardBandLeft, dvGuardBandTop, dvGuardBandRight, dvGuardBandBottom;
  float dvExtentsAdjust;
  DWORD dwStencilCaps, dwFVFCaps, dwTextureOpCaps;
  WORD wMaxTextureBlendStages, wMaxSimultaneousTextures;
  DWORD dwMaxActiveLights;
  float dvMaxVertexW;
  GUID deviceGUID;
  WORD wMaxUserClipPlanes, wMaxVertexBlendMatrices;
  DWORD dwVertexProcessingCaps, dwReserved1, dwReserved2, dwReserved3, dwReserved4;
};

typedef HRESULT(CALLBACK* LPD3DENUMDEVICESCALLBACK7)(char* desc, char* name, D3DDEVICEDESC7*,
                                                     void* ctx);
typedef HRESULT(CALLBACK* LPD3DENUMPIXELFORMATSCALLBACK)(DDPIXELFORMAT*, void* ctx);

struct IDirect3DDevice7;
struct IDirect3DVertexBuffer7;

struct IDirect3D7 : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE EnumDevices(LPD3DENUMDEVICESCALLBACK7 cb, void* ctx) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateDevice(REFCLSID cls, IDirectDrawSurface7* rt,
                                                 IDirect3DDevice7** out) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateVertexBuffer(D3DVERTEXBUFFERDESC* desc,
                                                       IDirect3DVertexBuffer7** out,
                                                       DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE EnumZBufferFormats(REFCLSID dev,
                                                       LPD3DENUMPIXELFORMATSCALLBACK cb,
                                                       void* ctx) = 0;
  virtual HRESULT STDMETHODCALLTYPE EvictManagedTextures() = 0;
};

struct IDirect3DDevice7 : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetCaps(D3DDEVICEDESC7* desc) = 0;
  virtual HRESULT STDMETHODCALLTYPE EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK cb,
                                                       void* ctx) = 0;
  virtual HRESULT STDMETHODCALLTYPE BeginScene() = 0;
  virtual HRESULT STDMETHODCALLTYPE EndScene() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D7** out) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetRenderTarget(IDirectDrawSurface7* s, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetRenderTarget(IDirectDrawSurface7** s) = 0;
  virtual HRESULT STDMETHODCALLTYPE Clear(DWORD count, D3DRECT* rects, DWORD flags,
                                          D3DCOLOR color, float z, DWORD stencil) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetTransform(DWORD type, D3DMATRIX* m) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetTransform(DWORD type, D3DMATRIX* m) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetViewport(D3DVIEWPORT7* vp) = 0;
  virtual HRESULT STDMETHODCALLTYPE MultiplyTransform(DWORD type, D3DMATRIX* m) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT7* vp) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetMaterial(D3DMATERIAL7* m) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL7* m) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetLight(DWORD i, D3DLIGHT7* l) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetLight(DWORD i, D3DLIGHT7* l) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetRenderState(DWORD state, DWORD value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetRenderState(DWORD state, DWORD* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE BeginStateBlock() = 0;
  virtual HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD* handle) = 0;
  virtual HRESULT STDMETHODCALLTYPE PreLoad(IDirectDrawSurface7* s) = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawPrimitive(DWORD prim, DWORD fvf, void* v, DWORD n,
                                                  DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(DWORD prim, DWORD fvf, void* v,
                                                         DWORD nv, WORD* idx, DWORD ni,
                                                         DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetClipStatus(D3DCLIPSTATUS* s) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS* s) = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawPrimitiveStrided(DWORD prim, DWORD fvf,
                                                         D3DDRAWPRIMITIVESTRIDEDDATA* d,
                                                         DWORD n, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveStrided(
      DWORD prim, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA* d, DWORD nv, WORD* idx, DWORD ni,
      DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawPrimitiveVB(DWORD prim, IDirect3DVertexBuffer7* vb,
                                                    DWORD start, DWORD n, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveVB(DWORD prim,
                                                           IDirect3DVertexBuffer7* vb,
                                                           DWORD start, DWORD nv, WORD* idx,
                                                           DWORD ni, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE ComputeSphereVisibility(D3DVECTOR* c, float* r, DWORD n,
                                                            DWORD flags, DWORD* out) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, IDirectDrawSurface7** t) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirectDrawSurface7* t) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, DWORD type,
                                                         DWORD* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, DWORD type,
                                                         DWORD value) = 0;
  virtual HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* passes) = 0;
  virtual HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD h) = 0;
  virtual HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD h) = 0;
  virtual HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD h) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateStateBlock(DWORD type, DWORD* h) = 0;
  virtual HRESULT STDMETHODCALLTYPE Load(IDirectDrawSurface7* dst, POINT* dp,
                                         IDirectDrawSurface7* src, RECT* sr, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE LightEnable(DWORD i, BOOL on) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD i, BOOL* on) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD i, float* p) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD i, float* p) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetInfo(DWORD id, void* info, DWORD size) = 0;
};

struct IDirect3DVertexBuffer7 : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Lock(DWORD flags, void** data, DWORD* size) = 0;
  virtual HRESULT STDMETHODCALLTYPE Unlock() = 0;
  virtual HRESULT STDMETHODCALLTYPE ProcessVertices(DWORD op, DWORD dst_index, DWORD count,
                                                    IDirect3DVertexBuffer7* src,
                                                    DWORD src_index, IDirect3DDevice7* dev,
                                                    DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetVertexBufferDesc(D3DVERTEXBUFFERDESC* desc) = 0;
  virtual HRESULT STDMETHODCALLTYPE Optimize(IDirect3DDevice7* dev, DWORD flags) = 0;
  virtual HRESULT STDMETHODCALLTYPE ProcessVerticesStrided(DWORD op, DWORD dst_index,
                                                           DWORD count,
                                                           D3DDRAWPRIMITIVESTRIDEDDATA* d,
                                                           DWORD fvf, IDirect3DDevice7* dev,
                                                           DWORD flags) = 0;
};

// IIDs / device GUIDs.
constexpr GUID IID_IDirect3D7 = {0xf5049e77, 0x4861, 0x11d2,
                                 {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
constexpr GUID IID_IDirect3DHALDevice = {0x84e63de0, 0x46aa, 0x11cf,
                                         {0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e}};
constexpr GUID IID_IDirect3DTnLHalDevice = {0xf5049e78, 0x4861, 0x11d2,
                                            {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
constexpr GUID IID_IDirect3DRGBDevice = {0xa4665c60, 0x2673, 0x11cf,
                                         {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}};
constexpr GUID IID_IDirectDraw7 = {0x15e65ec0, 0x3b9c, 0x11d2,
                                   {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
constexpr GUID IID_IDirectDrawSurface7 = {0x06675a80, 0x3b9b, 0x11d2,
                                          {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
constexpr GUID IID_IDirectDrawGammaControl = {0x69c11c3e, 0xb46b, 0x11d1,
                                              {0xad, 0x7a, 0x00, 0xc0, 0x4f, 0xc2, 0x9b, 0x4e}};
constexpr GUID IID_IUnknown_ = {0x00000000, 0x0000, 0x0000,
                                {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

constexpr GUID IID_IDirect3DDevice7 = {0xf5049e79, 0x4861, 0x11d2,
                                       {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
constexpr GUID IID_IDirect3DVertexBuffer7 = {0xf5049e7d, 0x4861, 0x11d2,
                                             {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};

constexpr HRESULT D3D_OK = DD_OK;
constexpr HRESULT D3DENUMRET_CANCEL = DDENUMRET_CANCEL;
constexpr HRESULT D3DENUMRET_OK = DDENUMRET_OK;

// Constants used by the backend.
enum : DWORD {
  D3DPV_DONOTCOPYDATA = 1,
  D3DCULL_NONE = 1, D3DCULL_CW = 2, D3DCULL_CCW = 3,
  D3DTOP_DISABLE = 1,
  D3DPT_POINTLIST = 1, D3DPT_LINELIST, D3DPT_LINESTRIP, D3DPT_TRIANGLELIST,
  D3DPT_TRIANGLESTRIP, D3DPT_TRIANGLEFAN,

  D3DFVF_POSITION_MASK = 0x00E, D3DFVF_XYZ = 0x002, D3DFVF_XYZRHW = 0x004,
  D3DFVF_XYZB1 = 0x006, D3DFVF_XYZB2 = 0x008, D3DFVF_XYZB3 = 0x00A, D3DFVF_XYZB4 = 0x00C,
  D3DFVF_XYZB5 = 0x00E, D3DFVF_NORMAL = 0x010, D3DFVF_RESERVED1 = 0x020,
  D3DFVF_DIFFUSE = 0x040, D3DFVF_SPECULAR = 0x080, D3DFVF_TEXCOUNT_MASK = 0xF00,
  D3DFVF_TEXCOUNT_SHIFT = 8,

  D3DCLEAR_TARGET = 1, D3DCLEAR_ZBUFFER = 2, D3DCLEAR_STENCIL = 4,

  D3DTRANSFORMSTATE_WORLD = 1, D3DTRANSFORMSTATE_VIEW = 2, D3DTRANSFORMSTATE_PROJECTION = 3,
  D3DTRANSFORMSTATE_TEXTURE0 = 16,

  D3DVOP_LIGHT = 1 << 10, D3DVOP_TRANSFORM = 1, D3DVOP_CLIP = 4, D3DVOP_EXTENTS = 8,
};

// Render states (D3DRENDERSTATETYPE).
enum : DWORD {
  RS_ANTIALIAS = 2, RS_TEXTUREPERSPECTIVE = 4, RS_ZENABLE = 7, RS_FILLMODE = 8,
  RS_SHADEMODE = 9, RS_LINEPATTERN = 10, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15,
  RS_LASTPIXEL = 16, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23,
  RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_DITHERENABLE = 26, RS_ALPHABLENDENABLE = 27,
  RS_FOGENABLE = 28, RS_SPECULARENABLE = 29, RS_ZVISIBLE = 30, RS_STIPPLEDALPHA = 33,
  RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35, RS_FOGSTART = 36, RS_FOGEND = 37,
  RS_FOGDENSITY = 38, RS_EDGEANTIALIAS = 40, RS_COLORKEYENABLE = 41, RS_ZBIAS = 47,
  RS_RANGEFOGENABLE = 48, RS_STENCILENABLE = 52, RS_TEXTUREFACTOR = 60, RS_WRAP0 = 128,
  RS_CLIPPING = 136, RS_LIGHTING = 137, RS_EXTENTS = 138, RS_AMBIENT = 139,
  RS_FOGVERTEXMODE = 140, RS_COLORVERTEX = 141, RS_LOCALVIEWER = 142,
  RS_NORMALIZENORMALS = 143, RS_COLORKEYBLENDENABLE = 144,
  RS_DIFFUSEMATERIALSOURCE = 145, RS_SPECULARMATERIALSOURCE = 146,
  RS_AMBIENTMATERIALSOURCE = 147, RS_EMISSIVEMATERIALSOURCE = 148,
  RS_VERTEXBLEND = 151, RS_CLIPPLANEENABLE = 152,
};

// Texture stage states (D3DTEXTURESTAGESTATETYPE).
enum : DWORD {
  TSS_COLOROP = 1, TSS_COLORARG1, TSS_COLORARG2, TSS_ALPHAOP, TSS_ALPHAARG1, TSS_ALPHAARG2,
  TSS_BUMPENVMAT00, TSS_BUMPENVMAT01, TSS_BUMPENVMAT10, TSS_BUMPENVMAT11, TSS_TEXCOORDINDEX,
  TSS_ADDRESS, TSS_ADDRESSU, TSS_ADDRESSV, TSS_BORDERCOLOR, TSS_MAGFILTER, TSS_MINFILTER,
  TSS_MIPFILTER, TSS_MIPMAPLODBIAS, TSS_MAXMIPLEVEL, TSS_MAXANISOTROPY, TSS_BUMPENVLSCALE,
  TSS_BUMPENVLOFFSET, TSS_TEXTURETRANSFORMFLAGS,
};

}  // namespace d3d7
