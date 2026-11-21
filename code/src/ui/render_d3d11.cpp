// MenuMaX: see render_d3d11.h.
#include "render_d3d11.h"

#include <RmlUi/Core/Vertex.h>

#include <cstring>
#include <vector>

#include "../dd/backend.h"
#include "../patch.h"

namespace ui {

namespace {

// RmlUi vertices: pixel position, premultiplied colour, texture position.
// Everything it hands over is premultiplied (pictures are premultiplied on
// loading), so one blend mode does for all.
const char kShader[] = R"(
cbuffer Params : register(b0) {
  float4x4 transform;  // projection x element transform (pixels -> clip space)
  float4 translation;  // xy
};
struct VsIn { float2 pos : POSITION; float4 col : COLOR; float2 uv : TEXCOORD; };
struct VsOut { float4 pos : SV_Position; float4 col : COLOR; float2 uv : TEXCOORD; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
VsOut vs_main(VsIn i) {
  VsOut o;
  o.pos = mul(transform, float4(i.pos + translation.xy, 0, 1));
  o.col = i.col;
  o.uv = i.uv;
  return o;
}
float4 ps_tex(VsOut i) : SV_Target { return tex.Sample(smp, i.uv) * i.col; }
float4 ps_color(VsOut i) : SV_Target { return i.col; }
)";

struct Constants {
  float transform[16];
  float translation[4];
};

struct Geometry {
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  UINT count = 0;
};

struct Texture {
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
};

}  // namespace

bool RenderD3D11::Init(ID3D11Device* device) {
  device_ = device;
  ID3DBlob* vs = dd::Compile(kShader, "vs_main", "vs_4_0");
  ID3DBlob* pt = dd::Compile(kShader, "ps_tex", "ps_4_0");
  ID3DBlob* pc = dd::Compile(kShader, "ps_color", "ps_4_0");
  if (vs) {
    device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
    const D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(el, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &layout_);
  }
  if (pt) device->CreatePixelShader(pt->GetBufferPointer(), pt->GetBufferSize(), nullptr, &ps_tex_);
  if (pc) device->CreatePixelShader(pc->GetBufferPointer(), pc->GetBufferSize(), nullptr, &ps_color_);
  dd::SafeRelease(vs);
  dd::SafeRelease(pt);
  dd::SafeRelease(pc);
  static_assert(sizeof(Rml::Vertex) == 20, "RmlUi vertex layout");

  D3D11_BUFFER_DESC cb{};
  cb.ByteWidth = sizeof(Constants);
  cb.Usage = D3D11_USAGE_DEFAULT;
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  device->CreateBuffer(&cb, nullptr, &cb_);

  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  device->CreateBlendState(&bd, &blend_);

  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  device->CreateRasterizerState(&rd, &raster_);
  rd.ScissorEnable = TRUE;
  device->CreateRasterizerState(&rd, &raster_scissor_);

  D3D11_DEPTH_STENCIL_DESC dd{};
  dd.DepthEnable = FALSE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
  device->CreateDepthStencilState(&dd, &no_depth_);

  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  device->CreateSamplerState(&sd, &sampler_);

  const bool ok = vs_ && ps_tex_ && ps_color_ && layout_ && cb_ && blend_ && raster_ && raster_scissor_ && no_depth_ && sampler_;
  dd::Log("MenuMaX: Direct3D 11 drawing %s (shaders %d, layout %d, states %d)", ok ? "ready" : "FAILED",
          vs_ && ps_tex_ && ps_color_, layout_ != nullptr, blend_ && raster_ && raster_scissor_ && no_depth_ && sampler_);
  if (!ok) Shutdown();
  return ok;
}

void RenderD3D11::Shutdown() {
  dd::SafeRelease(vs_);
  dd::SafeRelease(ps_tex_);
  dd::SafeRelease(ps_color_);
  dd::SafeRelease(layout_);
  dd::SafeRelease(cb_);
  dd::SafeRelease(blend_);
  dd::SafeRelease(raster_);
  dd::SafeRelease(raster_scissor_);
  dd::SafeRelease(no_depth_);
  dd::SafeRelease(sampler_);
}

void RenderD3D11::Begin(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h) {
  ctx_ = ctx;
  width_ = w;
  height_ = h;
  scissor_ = false;
  has_transform_ = false;
  projection_ = Rml::Matrix4f::ProjectOrtho(0, static_cast<float>(w), static_cast<float>(h), 0, -10000, 10000);
  transform_ = projection_;
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
  ctx->RSSetViewports(1, &vp);
  ctx->RSSetState(raster_);
  ctx->OMSetDepthStencilState(no_depth_, 0);
  const float factor[4] = {0, 0, 0, 0};
  ctx->OMSetBlendState(blend_, factor, 0xffffffff);
  ctx->IASetInputLayout(layout_);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs_, nullptr, 0);
  ctx->VSSetConstantBuffers(0, 1, &cb_);
  ctx->PSSetSamplers(0, 1, &sampler_);
  ctx->GSSetShader(nullptr, nullptr, 0);
}

void RenderD3D11::End() {
  if (!ctx_) return;
  ID3D11ShaderResourceView* none = nullptr;
  ctx_->PSSetShaderResources(0, 1, &none);
  ctx_->RSSetState(raster_);
  ctx_ = nullptr;
}

void RenderD3D11::UpdateConstants(Rml::Vector2f translation) {
  Constants c{};
  std::memcpy(c.transform, transform_.data(), sizeof(c.transform));
  c.translation[0] = translation.x;
  c.translation[1] = translation.y;
  ctx_->UpdateSubresource(cb_, 0, nullptr, &c, 0, 0);
}

Rml::CompiledGeometryHandle RenderD3D11::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
  if (!device_ || vertices.empty() || indices.empty()) return 0;
  auto* g = new Geometry;
  D3D11_BUFFER_DESC bd{};
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Rml::Vertex));
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA sd{vertices.data(), 0, 0};
  device_->CreateBuffer(&bd, &sd, &g->vb);
  bd.ByteWidth = static_cast<UINT>(indices.size() * sizeof(int));
  bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
  sd.pSysMem = indices.data();
  device_->CreateBuffer(&bd, &sd, &g->ib);
  g->count = static_cast<UINT>(indices.size());
  if (!g->vb || !g->ib) {
    dd::LogOnce("MenuMaX: could not make a geometry buffer (%u vertices)", static_cast<unsigned>(vertices.size()));
    ReleaseGeometry(reinterpret_cast<Rml::CompiledGeometryHandle>(g));
    return 0;
  }
  ++stats_.geometry;
  return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
}

void RenderD3D11::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) {
  auto* g = reinterpret_cast<Geometry*>(geometry);
  if (!g || !ctx_) return;
  UpdateConstants(translation);
  const UINT stride = sizeof(Rml::Vertex), offset = 0;
  ctx_->IASetVertexBuffers(0, 1, &g->vb, &stride, &offset);
  ctx_->IASetIndexBuffer(g->ib, DXGI_FORMAT_R32_UINT, 0);
  if (auto* t = reinterpret_cast<Texture*>(texture)) {
    ctx_->PSSetShader(ps_tex_, nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, &t->srv);
  } else {
    ctx_->PSSetShader(ps_color_, nullptr, 0);
  }
  ctx_->DrawIndexed(g->count, 0, 0);
  ++stats_.draws;
}

void RenderD3D11::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
  auto* g = reinterpret_cast<Geometry*>(geometry);
  if (!g) return;
  dd::SafeRelease(g->vb);
  dd::SafeRelease(g->ib);
  delete g;
}

Rml::TextureHandle RenderD3D11::LoadTexture(Rml::Vector2i& dims, const Rml::String& source) {
  std::vector<uint8_t> px;
  unsigned w = 0, h = 0;
  if (!LoadPicture(source, px, w, h)) return 0;
  // Mipmapped, so pictures made bigger than they are shown (HD art) stay smooth.
  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 0;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  auto* t = new Texture;
  ID3D11DeviceContext* ctx = dd::B().ctx;
  if (SUCCEEDED(device_->CreateTexture2D(&td, nullptr, &t->tex)) &&
      SUCCEEDED(device_->CreateShaderResourceView(t->tex, nullptr, &t->srv))) {
    ctx->UpdateSubresource(t->tex, 0, nullptr, px.data(), w * 4, 0);
    ctx->GenerateMips(t->srv);
  } else {
    ++stats_.texture_failures;
    dd::Log("MenuMaX: texture %ux%u for %s could not be made", w, h, source.c_str());
    ReleaseTexture(reinterpret_cast<Rml::TextureHandle>(t));
    return 0;
  }
  dims.x = static_cast<int>(w);
  dims.y = static_cast<int>(h);
  ++stats_.textures;
  return reinterpret_cast<Rml::TextureHandle>(t);
}

Rml::TextureHandle RenderD3D11::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dims) {
  if (!device_ || dims.x <= 0 || dims.y <= 0 || source.size() < static_cast<size_t>(dims.x) * dims.y * 4) return 0;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = static_cast<UINT>(dims.x);
  td.Height = static_cast<UINT>(dims.y);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // RmlUi: premultiplied R, G, B, A
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sd{source.data(), td.Width * 4, 0};
  auto* t = new Texture;
  if (FAILED(device_->CreateTexture2D(&td, &sd, &t->tex)) ||
      FAILED(device_->CreateShaderResourceView(t->tex, nullptr, &t->srv))) {
    ++stats_.texture_failures;
    dd::LogOnce("MenuMaX: generated texture %dx%d could not be made", dims.x, dims.y);
    ReleaseTexture(reinterpret_cast<Rml::TextureHandle>(t));
    return 0;
  }
  ++stats_.textures;
  return reinterpret_cast<Rml::TextureHandle>(t);
}

void RenderD3D11::ReleaseTexture(Rml::TextureHandle texture) {
  auto* t = reinterpret_cast<Texture*>(texture);
  if (!t) return;
  dd::SafeRelease(t->srv);
  dd::SafeRelease(t->tex);
  delete t;
}

void RenderD3D11::EnableScissorRegion(bool enable) {
  if (log_scissors > 0 && enable != scissor_) patch::Log("MenuMaX: clipping %s", enable ? "on" : "off");
  scissor_ = enable;
  if (ctx_) ctx_->RSSetState(enable ? raster_scissor_ : raster_);
}

void RenderD3D11::SetScissorRegion(Rml::Rectanglei region) {
  if (!ctx_) return;
  D3D11_RECT r{region.Left(), region.Top(), region.Right(), region.Bottom()};
  ctx_->RSSetScissorRects(1, &r);
  ++stats_.scissors;
  if (log_scissors > 0) {
    --log_scissors;
    patch::Log("MenuMaX: clipping to %ld,%ld - %ld,%ld (target %ux%u)", r.left, r.top, r.right, r.bottom, width_, height_);
  }
}

void RenderD3D11::SetTransform(const Rml::Matrix4f* transform) {
  has_transform_ = transform != nullptr;
  transform_ = transform ? projection_ * *transform : projection_;
}

}  // namespace ui
