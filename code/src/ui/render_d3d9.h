// MenuMaX: RmlUi drawn with Direct3D 9 (Renderer=D3D9, Windows XP) straight
// into the back buffer, at the window's resolution (see ui.h).
//
// Fixed-function only (no shaders, so it runs on every Direct3D 9 card):
// vertices are transformed on the CPU into screen positions (menus are a few
// hundred vertices) and drawn with DrawIndexedPrimitiveUP, so nothing lives in
// the default pool and a device reset loses nothing. Textures are managed and
// keep a CPU copy, so a new device (Fullscreen=Exclusive switches make one)
// gets them made again the next time they are used.
#pragma once
#include "render.h"

struct IDirect3DSurface9;

namespace ui {

class RenderD3D9 : public Renderer {
 public:
  ~RenderD3D9() override;
  bool Ready() const override { return true; }
  const char* Name() const override { return "Direct3D 9"; }
  // Start of a frame into `target` (w x h): sets every state it uses through
  // the cached setters (backend9.h), so the game's drawing is not disturbed.
  void Begin(IDirect3DSurface9* target, unsigned w, unsigned h);
  void End() override;
  // The Direct3D 9 device is about to go away: its textures are let go
  // (their pictures are kept and made again on the next device).
  void DeviceGone();

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
  struct Texture;
  Texture* NewTexture(std::vector<uint8_t>& bgra, unsigned w, unsigned h, const char* what);
  bool MakeTexture(Texture* t);

  bool drawing_ = false;
  unsigned width_ = 0, height_ = 0;
  bool has_transform_ = false;
  Rml::Matrix4f transform_;  // projection x element transform
  std::vector<Texture*> textures_;
  bool logged_caps_ = false;
};

}  // namespace ui
