// MenuMaX: RmlUi drawn with Direct3D 11 straight into the swap chain, at
// the window's resolution (see ui.h).
#pragma once
#include <d3d11.h>

#include "render.h"

namespace ui {

class RenderD3D11 : public Renderer {
 public:
  bool Init(ID3D11Device* device);
  void Shutdown();
  bool Ready() const override { return vs_ && ps_tex_ && ps_color_; }
  const char* Name() const override { return "Direct3D 11"; }
  // Start of a frame into rtv (w x h); End restores nothing (the caller
  // marks the context as changed).
  void Begin(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, UINT w, UINT h);
  void End() override;

  Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
  void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
  void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
  Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
  Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
  void ReleaseTexture(Rml::TextureHandle texture) override;
  void EnableScissorRegion(bool enable) override;
  void SetScissorRegion(Rml::Rectanglei region) override;
  void SetTransform(const Rml::Matrix4f* transform) override;

 private:
  void UpdateConstants(Rml::Vector2f translation);

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* ctx_ = nullptr;
  ID3D11VertexShader* vs_ = nullptr;
  ID3D11PixelShader* ps_tex_ = nullptr;
  ID3D11PixelShader* ps_color_ = nullptr;
  ID3D11InputLayout* layout_ = nullptr;
  ID3D11Buffer* cb_ = nullptr;
  ID3D11BlendState* blend_ = nullptr;
  ID3D11RasterizerState* raster_ = nullptr;
  ID3D11RasterizerState* raster_scissor_ = nullptr;
  ID3D11DepthStencilState* no_depth_ = nullptr;
  ID3D11SamplerState* sampler_ = nullptr;
  UINT width_ = 0, height_ = 0;
  bool scissor_ = false;
  bool has_transform_ = false;
  Rml::Matrix4f projection_;
  Rml::Matrix4f transform_;
};

}  // namespace ui
