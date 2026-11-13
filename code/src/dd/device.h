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
struct ReflectReplay;

class DirectDraw;
class Surface;

// DrawBatch=1: why a batch of merged draws was sent (logged as counts).
enum BatchBreak : int {
  kBreakRenderState,
  kBreakStageState,
  kBreakTexture,
  kBreakTransform,
  kBreakViewport,
  kBreakTarget,
  kBreakLighting,
  kBreakClear,
  kBreakScene,
  kBreakSurface,   // Blt, Lock, GetDC, colour key, LOD ... on any surface
  kBreakFlip,
  kBreakScreen,    // a screen-space draw
  kBreakShape,     // a draw that is not merged (lines, points, processed vertices, bad indices)
  kBreakLayout,    // a different vertex format
  kBreakFull,
  kBreakOther,
  kBreakCount
};
// Sends the device's merged draws, if any, before something that could see
// or change what they draw.
void FlushPendingDraws(BatchBreak why);

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
    float obj[3];     // the source vertex (object space)
    uint32_t mat;     // which entry of Matrix() transformed it
  };
  struct Mat {
    float m[16];
  };
  // The world*view*projection a Processed::mat refers to (null if stale).
  const float* Matrix(uint32_t id) const {
    const uint32_t i = id & 0xfff;
    if ((id >> 12) != (mat_epoch_ & 0xfffff) || i >= mats_.size()) return nullptr;
    return mats_[i].m;
  }
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
  std::vector<Mat> mats_;
  uint32_t mat_epoch_ = 0;
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
    const uint8_t* game_verts = nullptr;  // the game's own vertex array (before SimSmooth), if known
  };
  void Submit(const Draw& d);
  bool BatchAdd(const Draw& d);
  bool StageStateUnused(DWORD stage, DWORD type) const;
  HRESULT SetTransformNow(DWORD type, d3d7::D3DMATRIX* m);
 public:
  void FlushBatch(BatchBreak why);
 private:
  void NoteDrawLog(const Draw& d);  // DrawLog=1 (drawlog.cpp)
  // MotionBlur (motionblur.cpp): every draw is noted (`tl`: screen-space
  // vertices); the first screen-space draw without depth test after the 3D
  // scene blurs the scene before it is drawn, so the HUD stays sharp.
  // Returns true when it kept the draw back: the race direction arrow is
  // drawn after the blur (MotionBlurReplay), so it stays sharp.
  bool MotionBlurNote(const Draw& d, bool tl);
  bool MotionBlurPass(float fu, float fv, float len, float inner, float outer);
  void MotionBlurReplay();
  bool ArrowKeepBack(const Draw& d);  // a 3D draw of the arrow: copied and kept back
  bool SubmitWideLine(const Draw& d);
  void SubmitDraw(const Draw& d);
  void OccludeText(const Draw& d);
  // Reflections (reflect.cpp).
  bool ReflectWants(const Draw& d);
  void ReflectSky(const Draw& d);
  void ReflectPass(const Draw& d);
  void ReflectRecord(const ReflectReplay& r);
  bool ReflectRecording() const;
 public:
  void ReflectEndScene(ID3D11InputLayout* game_layout, UINT game_stride);
 private:
  // Direct3D 9 (device9.cpp): the fixed-function pipeline does the work.
  bool Init9();
  HRESULT Clear9(DWORD count, d3d7::D3DRECT* rects, DWORD flags, d3d7::D3DCOLOR color, float z);
  void Submit9(const Draw& d);
  bool BindTarget9(bool& has_depth, bool full_clear);
  void ApplyState9(int mode, bool has_depth);
  // Binds the render target (or its multisampled stand-in) and depth.
  // `full_clear`: the caller will overwrite all of the colour buffer.
  bool BindTarget(bool& has_depth, bool full_clear = false);
  // `shadow`: the terrain variant with rider shadows (bound by gpushadow.cpp).
  void ApplyState(bool tl, bool depth_clip, bool has_depth, bool shadow = false);
  ID3D11BlendState* GetBlend(bool a2c);
  // Cut-out draws that write depth, which get alpha-to-coverage when the
  // scene is multisampled (ini AlphaToCoverage):
  //  kBlendedCutout: blended + alpha test > 0 (leaves, flags, bike numbers);
  //    without coverage these get AlphaCut.
  //  kTestedCutout: not blended, alpha test against a reference above 0
  //    (fences, metal arches, tents, bikes, desert plants).
  enum Cutout { kNoCutout, kBlendedCutout, kTestedCutout };
  Cutout CutoutDraw() const;
  // Alpha-to-coverage for this draw (cut-out, MSAA target, ini on), logged.
  bool UseAlphaToCoverage(Cutout cutout);
  ID3D11DepthStencilState* GetDepth(bool has_depth);
  ID3D11RasterizerState* GetRaster(bool depth_clip);
  ID3D11SamplerState* GetSampler(int stage);
  // Maps room for `bytes` of vertices and has fill(ctx, dst) write them.
  UINT PushVertices(UINT bytes, void (*fill)(void* ctx, void* dst), void* ctx);
  UINT PushIndices(const void* data, UINT bytes);

  LONG refs_ = 1;
  // What BindTarget bound.
  ID3D11RenderTargetView* cur_rtv_ = nullptr;
  ID3D11DepthStencilView* cur_dsv_ = nullptr;
  bool cur_msaa_ = false;
  DirectDraw* dd_;
  Surface* rt_ = nullptr;
  GUID guid_;
  DWORD rs_[256] = {};
  DWORD tss_[8][32] = {};
  Surface* tex_[8] = {};
  std::vector<uint8_t> line_verts_;   // screen-space lines drawn wide (LineWidth)
  std::vector<uint8_t> label_verts_;  // HUD name labels drawn larger (hudtext.cpp)
  d3d7::D3DMATRIX world_{}, view_{}, proj_{};
  d3d7::D3DMATRIX world_set_{}, view_set_{}, proj_set_{};  // as the game set them (world_/view_ may be smoothed)
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
  unsigned vgen_ = 0, igen_ = 0;  // times vbuf_/ibuf_ were started over (reflection replays)
  bool submitting_object_ = false;  // a bike/rider draw, sent unbatched (reflections)
  ID3D11Buffer* vs_cb_ = nullptr;
  ID3D11Buffer* ps_cb_ = nullptr;
  std::map<uint32_t, ID3D11BlendState*> blend_cache_;
  std::map<uint32_t, ID3D11DepthStencilState*> depth_cache_;
  std::map<uint32_t, ID3D11RasterizerState*> raster_cache_;
  std::map<uint32_t, ID3D11SamplerState*> sampler_cache_;
  // DrawBatch=1: 3D triangle draws in a row with the same state, merged.
  struct Batch {
    std::vector<uint8_t> verts;  // the game's vertices, its own layout
    std::vector<WORD> idx;       // triangle list
    FvfLayout layout;
    DWORD nverts = 0;
    unsigned draws = 0;
    bool receiver = false;  // terrain patches (rider shadows)
  };
  Batch batch_, batch_out_;
  bool flushing_ = false;
  std::vector<uint8_t> scratch_;
  std::vector<WORD> scratch_idx_;
  // Direct3D 9: what BindTarget9 bound.
  IDirect3DSurface9* cur_rt9_ = nullptr;
  IDirect3DSurface9* cur_ds9_ = nullptr;
  UINT cur_rt9_w_ = 0, cur_rt9_h_ = 0;
  unsigned draws9_ = 0;  // for the first-frames log
};

}  // namespace dd
