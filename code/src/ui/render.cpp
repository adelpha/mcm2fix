// MenuMaX: see render.h.
#include "render.h"

#include <windows.h>

#include "../dd/backend.h"
#include "../dd/texdecode.h"
#include "gameart.h"

namespace ui {

namespace {

std::wstring Widen(const Rml::String& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  for (wchar_t& c : w)
    if (c == L'/') c = L'\\';
  return w;
}

}  // namespace

bool Renderer::LoadPicture(const Rml::String& source, std::vector<uint8_t>& px, unsigned& w, unsigned& h) {
  // "/dat/<ARCHIVE>/<path>": the game's own art (gameart.h), unless a PNG of
  // that name is under UI\MaX\dat\.
  const bool game_art = source.size() > 4 && (source.compare(0, 4, "dat/") == 0 || source.compare(0, 4, "dat\\") == 0);
  Rml::String file = source;
  if (game_art) {
    file.resize(file.find('?') == Rml::String::npos ? file.size() : file.find('?'));
    const size_t dot = file.find_last_of('.');
    if (dot != Rml::String::npos && dot > file.find_last_of("/\\")) file.resize(dot);
    file += ".png";
  }
  std::wstring path = Widen(file);
  if (!(path.size() > 1 && (path[1] == L':' || (path[0] == L'\\' && path[1] == L'\\')))) path = base_dir_ + path;
  UINT pw = 0, ph = 0;
  std::string err;
  if (game_art && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    if (!LoadGameArt(source.substr(4), px, w, h, &err)) {
      ++stats_.texture_failures;
      dd::Log("MenuMaX: game art %s could not be read: %s", source.c_str(), err.c_str());
      return false;
    }
    for (size_t i = 0; i < px.size(); i += 4)
      for (int k = 0; k < 3; ++k) px[i + k] = static_cast<uint8_t>((px[i + k] * px[i + 3] + 127) / 255);
    dd::Log("MenuMaX: game art %s (%ux%u)", source.c_str(), w, h);
    return true;
  }
  if (!dd::DecodePicture(path, px, pw, ph, &err)) {
    ++stats_.texture_failures;
    dd::Log("MenuMaX: picture %ls could not be read: %s", path.c_str(), err.c_str());
    return false;
  }
  // Straight alpha -> premultiplied (B, G, R, A).
  for (size_t i = 0; i < px.size(); i += 4)
    for (int k = 0; k < 3; ++k) px[i + k] = static_cast<uint8_t>((px[i + k] * px[i + 3] + 127) / 255);
  w = pw;
  h = ph;
  dd::Log("MenuMaX: picture %ls (%ux%u)", path.c_str(), w, h);
  return true;
}

}  // namespace ui
