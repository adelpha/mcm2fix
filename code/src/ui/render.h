// MenuMaX: what the Direct3D 11 and Direct3D 9 RmlUi renderers share
// (render_d3d11.h, render_d3d9.h).
#pragma once
#include <RmlUi/Core/RenderInterface.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

class Renderer : public Rml::RenderInterface {
 public:
  // Statistics for the log (reset by the caller).
  struct Stats {
    unsigned geometry = 0, draws = 0, textures = 0, texture_failures = 0, scissors = 0;
  };

  virtual ~Renderer() = default;
  virtual bool Ready() const = 0;
  // Ends a frame started with the API's own Begin.
  virtual void End() = 0;
  virtual const char* Name() const = 0;
  Stats& GetStats() { return stats_; }
  // Clipping rectangles still to log (diagnostics).
  int log_scissors = 0;
  // Folder that relative picture paths are read from (with trailing slash).
  void SetBaseDir(const std::wstring& dir) { base_dir_ = dir; }

 protected:
  // Reads a picture (relative paths from the base folder) as premultiplied
  // B, G, R, A rows; logs and counts failures.
  bool LoadPicture(const Rml::String& source, std::vector<uint8_t>& px, unsigned& w, unsigned& h);

  std::wstring base_dir_;
  Stats stats_;
};

}  // namespace ui
