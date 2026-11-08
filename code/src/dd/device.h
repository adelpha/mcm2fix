// IDirect3DDevice7 and IDirect3DVertexBuffer7 for the D3D11 backend.
//
// The fixed-function pipeline is emulated with one shader pair driven by
// constants (two texture stages, fog, alpha test, specular). Vertices of any
// FVF are converted on the CPU into one common layout before drawing.
#pragma once
#include <windows.h>
#include <ddraw.h>

#include <cstdint>
#include <map>
#include <vector>

#include "backend.h"
#include "d3d7.h"

namespace dd {

class DirectDraw;
class Surface;

// Where each part of a vertex lives for a given FVF code.
struct FvfLayout {
  UINT stride = 0;
  bool tl = false;  // XYZRHW (already transformed)
  int diffuse = -1, specular = -1, normal = -1;
  int ntex = 0;
  int tex[8] = {};
  int tex_dim[8] = {};
  static FvfLayout From(DWORD fvf);
};

class VertexBuffer final : public d3d7::IDirect3DVertexBuffer7 {
 public:
  explicit VertexBuffer(const d3d7::D3DVERTEXBUFFERDESC& desc);
  STDMETHOD(QueryInterface)(REFIID riid, void** out) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(Lock)(DWORD flags, void** data, DWORD* size) override;
  STDMETHOD(Unlock)() override;
  STDMETHOD(ProcessVertices)(DWORD op, DWORD dst_index, DWORD count,
                             d3d7::IDirect3DVertexBuffer7* src, DWORD src_index,
                             d3d7::IDirect3DDevice7* dev, DWORD flags) override;
  STDMETHOD(GetVertexBufferDesc)(d3d7::D3DVERTEXBUFFERDESC* desc) override;
  STDMETHOD(Optimize)(d3d7::IDirect3DDevice7* dev, DWORD flags) override;
  STDMETHOD(ProcessVerticesStrided)(DWORD op, DWORD dst_index, DWORD count,
                                    d3d7::D3DDRAWPRIMITIVESTRIDEDDATA* d, DWORD fvf,
                                    d3d7::IDirect3DDevice7* dev, DWORD flags) override;

  DWORD Fvf() const { return desc_.dwFVF; }
  const uint8_t* Data() const { return data_.data(); }
  DWORD Count() const { return desc_.dwNumVertices; }
  const FvfLayout& Layout() const { return layout_; }

  // Clip-space positions written by ProcessVertices, so drawing the result
  // can let the GPU clip against the near plane, as Direct3D 7 did.
  struct Processed {
    float clip[4];
    float screen[4];  // what was written to the buffer
  };
  // True if vertices [first, first+n) still hold what ProcessVertices wrote.
  bool HasClipData(DWORD first, DWORD n) const;
  const Processed* ClipData() const { return processed_.data(); }
  const d3d7::D3DVIEWPORT7& ClipViewport() const { return processed_vp_; }

 private:
  LONG refs_ = 1;
  d3d7::D3DVERTEXBUFFERDESC desc_;
  FvfLayout layout_;
  std::vector<uint8_t> data_;
  std::vector<Processed> processed_;
  std::vector<uint8_t> processed_valid_;
  d3d7::D3DVIEWPORT7 processed_vp_{};
};

class Device : public d3d7::IDirect3DDevice7 {
 public:
  Device(DirectDraw* dd, Surface* rt, REFCLSID cls);
  virtual ~Device();
  bool Init();
  static void FillCaps(d3d7::D3DDEVICEDESC7& desc, const GUID& guid);

  STDMETHOD(QueryInterface)(REFIID riid, void** out) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetCaps)(d3d7::D3DDEVICEDESC7* desc) override;
  STDMETHOD(EnumTextureFormats)(d3d7::LPD3DENUMPIXELFORMATSCALLBACK cb, void* ctx) override;
  STDMETHOD(BeginScene)() override;
  STDMETHOD(EndScene)() override;
  STDMETHOD(GetDirect3D)(d3d7::IDirect3D7** out) override;
  STDMETHOD(SetRenderTarget)(IDirectDrawSurface7* s, DWORD flags) override;
  STDMETHOD(GetRenderTarget)(IDirectDrawSurface7** s) override;
  STDMETHOD(Clear)(DWORD count, d3d7::D3DRECT* rects, DWORD flags, d3d7::D3DCOLOR color, float z,
                   DWORD stencil) override;
  STDMETHOD(SetTransform)(DWORD type, d3d7::D3DMATRIX* m) override;
  STDMETHOD(GetTransform)(DWORD type, d3d7::D3DMATRIX* m) override;
  STDMETHOD(SetViewport)(d3d7::D3DVIEWPORT7* vp) override;
  STDMETHOD(MultiplyTransform)(DWORD type, d3d7::D3DMATRIX* m) override;
  STDMETHOD(GetViewport)(d3d7::D3DVIEWPORT7* vp) override;
  STDMETHOD(SetMaterial)(d3d7::D3DMATERIAL7* m) override;
  STDMETHOD(GetMaterial)(d3d7::D3DMATERIAL7* m) override;
  STDMETHOD(SetLight)(DWORD i, d3d7::D3DLIGHT7* l) override;
  STDMETHOD(GetLight)(DWORD i, d3d7::D3DLIGHT7* l) override;
  STDMETHOD(SetRenderState)(DWORD state, DWORD value) override;
  STDMETHOD(GetRenderState)(DWORD state, DWORD* value) override;
  STDMETHOD(BeginStateBlock)() override;
  STDMETHOD(EndStateBlock)(DWORD* handle) override;
  STDMETHOD(PreLoad)(IDirectDrawSurface7* s) override;
  STDMETHOD(DrawPrimitive)(DWORD prim, DWORD fvf, void* v, DWORD n, DWORD flags) override;
  STDMETHOD(DrawIndexedPrimitive)(DWORD prim, DWORD fvf, void* v, DWORD nv, WORD* idx, DWORD ni,
                                  DWORD flags) override;
  STDMETHOD(SetClipStatus)(d3d7::D3DCLIPSTATUS* s) override;
  STDMETHOD(GetClipStatus)(d3d7::D3DCLIPSTATUS* s) override;
  STDMETHOD(DrawPrimitiveStrided)(DWORD prim, DWORD fvf, d3d7::D3DDRAWPRIMITIVESTRIDEDDATA* d,
                                  DWORD n, DWORD flags) override;
  STDMETHOD(DrawIndexedPrimitiveStrided)(DWORD prim, DWORD fvf,
                                         d3d7::D3DDRAWPRIMITIVESTRIDEDDATA* d, DWORD nv,
                                         WORD* idx, DWORD ni, DWORD flags) override;
  STDMETHOD(DrawPrimitiveVB)(DWORD prim, d3d7::IDirect3DVertexBuffer7* vb, DWORD start, DWORD n,
                             DWORD flags) override;
  STDMETHOD(DrawIndexedPrimitiveVB)(DWORD prim, d3d7::IDirect3DVertexBuffer7* vb, DWORD start,
                                    DWORD nv, WORD* idx, DWORD ni, DWORD flags) override;
  STDMETHOD(ComputeSphereVisibility)(d3d7::D3DVECTOR* c, float* r, DWORD n, DWORD flags,
                                     DWORD* out) override;
  STDMETHOD(GetTexture)(DWORD stage, IDirectDrawSurface7** t) override;
  STDMETHOD(SetTexture)(DWORD stage, IDirectDrawSurface7* t) override;
  STDMETHOD(GetTextureStageState)(DWORD stage, DWORD type, DWORD* value) override;
  STDMETHOD(SetTextureStageState)(DWORD stage, DWORD type, DWORD value) override;
  STDMETHOD(ValidateDevice)(DWORD* passes) override;
  STDMETHOD(ApplyStateBlock)(DWORD h) override;
  STDMETHOD(CaptureStateBlock)(DWORD h) override;
  STDMETHOD(DeleteStateBlock)(DWORD h) override;
  STDMETHOD(CreateStateBlock)(DWORD type, DWORD* h) override;
  STDMETHOD(Load)(IDirectDrawSurface7* dst, POINT* dp, IDirectDrawSurface7* src, RECT* sr,
                  DWORD flags) override;
  STDMETHOD(LightEnable)(DWORD i, BOOL on) override;
  STDMETHOD(GetLightEnable)(DWORD i, BOOL* on) override;
  STDMETHOD(SetClipPlane)(DWORD i, float* p) override;
  STDMETHOD(GetClipPlane)(DWORD i, float* p) override;
  STDMETHOD(GetInfo)(DWORD id, void* info, DWORD size) override;

  // World * view * projection, row-vector convention (v' = v * M).
  const float* Wvp();
  const d3d7::D3DVIEWPORT7& Viewport() const { return viewport_; }

  static constexpr int kStages = 2;

 private:
  struct Draw {
    DWORD prim = 0;
    const uint8_t* verts = nullptr;
    FvfLayout layout;
    DWORD first = 0, count = 0;  // vertex range used
    const WORD* idx = nullptr;   // relative to first
    DWORD nidx = 0;
    const VertexBuffer* vb = nullptr;  // for ProcessVertices clip data
  };
  void Submit(const Draw& d);
  bool BindTarget(bool& has_depth);
  void ApplyState(bool tl, bool depth_clip, bool has_depth);
  ID3D11BlendState* GetBlend();
  ID3D11DepthStencilState* GetDepth(bool has_depth);
  ID3D11RasterizerState* GetRaster(bool depth_clip);
  ID3D11SamplerState* GetSampler(int stage);
  UINT PushVertices(const void* data, UINT bytes);
  UINT PushIndices(const void* data, UINT bytes);

  LONG refs_ = 1;
  DirectDraw* dd_;
  Surface* rt_ = nullptr;
  GUID guid_;
  DWORD rs_[256] = {};
  DWORD tss_[8][32] = {};
  Surface* tex_[8] = {};
  d3d7::D3DMATRIX world_{}, view_{}, proj_{};
  d3d7::D3DMATRIX world_set_{}, view_set_{};  // as the game set them (world_/view_ may be smoothed)
  float wvp_[16] = {};
  bool wvp_dirty_ = true;
  d3d7::D3DVIEWPORT7 viewport_{};
  d3d7::D3DMATERIAL7 material_{};
  std::map<DWORD, d3d7::D3DLIGHT7> lights_;
  std::map<DWORD, BOOL> light_on_;
  d3d7::D3DCLIPSTATUS clip_status_{};

  ID3D11Buffer* vbuf_ = nullptr;
  UINT vbuf_size_ = 0, vbuf_pos_ = 0;
  ID3D11Buffer* ibuf_ = nullptr;
  UINT ibuf_size_ = 0, ibuf_pos_ = 0;
  ID3D11Buffer* vs_cb_ = nullptr;
  ID3D11Buffer* ps_cb_ = nullptr;
  std::map<uint32_t, ID3D11BlendState*> blend_cache_;
  std::map<uint32_t, ID3D11DepthStencilState*> depth_cache_;
  std::map<uint32_t, ID3D11RasterizerState*> raster_cache_;
  std::map<uint32_t, ID3D11SamplerState*> sampler_cache_;
  std::vector<uint8_t> scratch_;
  std::vector<WORD> scratch_idx_;
};

}  // namespace dd
