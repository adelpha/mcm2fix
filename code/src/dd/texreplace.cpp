#include "texreplace.h"

#include <objbase.h>
#include <wincodec.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../config.h"
#include "../xp_compat.h"

namespace dd {

namespace {

const CLSID kWicFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
const GUID kBgra32 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};
const GUID kPngContainer = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};

IWICImagingFactory* Wic() {
  static IWICImagingFactory* factory = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    // The game initialises COM for its video player; make sure this thread
    // has it too. A "changed mode" result still leaves COM usable.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT hr = CoCreateInstance(kWicFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) {
      Log("texture replacement: WIC unavailable (%08lx)", hr);
      factory = nullptr;
    }
  }
  return factory;
}

std::wstring UserDir() {
  const std::wstring local = xp::LocalAppDataW();
  if (local.empty()) return L"";
  return local + L"\\Motocross Madness 2";
}

std::wstring GameDir() {
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring s(buf);
  size_t slash = s.find_last_of(L'\\');
  return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

bool ParseHash(const wchar_t* name, uint64_t* out) {
  uint64_t v = 0;
  for (int i = 0; i < 16; ++i) {
    wchar_t c = name[i];
    int d;
    if (c >= L'0' && c <= L'9') d = c - L'0';
    else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
    else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
    else return false;
    v = (v << 4) | static_cast<uint64_t>(d);
  }
  wchar_t next = name[16];
  if (next != L'.' && next != L'_' && next != L'-' && next != L' ') return false;
  *out = v;
  return true;
}

std::map<std::string, std::wstring>& NameIndex() {
  static std::map<std::string, std::wstring> index;
  return index;
}

std::string Narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) {
    if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
    s += c < 128 ? static_cast<char>(c) : '?';
  }
  return s;
}

// hash -> picture file, from both replacement folders (the per-user one wins).
// Pictures not named by a hash go into NameIndex() by file name.
const std::map<uint64_t, std::wstring>& Index() {
  static std::map<uint64_t, std::wstring> index;
  static bool built = false;
  if (built) return index;
  built = true;
  const std::wstring dirs[] = {GameDir() + L"\\textures", UserDir() + L"\\textures"};
  for (const std::wstring& dir : dirs) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      const wchar_t* ext = std::wcsrchr(fd.cFileName, L'.');
      if (!ext || (_wcsicmp(ext, L".png") && _wcsicmp(ext, L".jpg") && _wcsicmp(ext, L".jpeg") &&
                   _wcsicmp(ext, L".bmp")))
        continue;
      uint64_t hash;
      if (ParseHash(fd.cFileName, &hash)) {
        index[hash] = dir + L"\\" + fd.cFileName;
      } else {
        std::wstring stem(fd.cFileName, static_cast<size_t>(ext - fd.cFileName));
        NameIndex()[Narrow(stem)] = dir + L"\\" + fd.cFileName;
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  Log("texture replacement: %u pictures named by hash, %u by texture name",
      static_cast<unsigned>(index.size()), static_cast<unsigned>(NameIndex().size()));
  return index;
}

bool Decode(const std::wstring& path, std::vector<uint8_t>& px, UINT& w, UINT& h) {
  IWICImagingFactory* f = Wic();
  if (!f) return false;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* conv = nullptr;
  bool ok = false;
  if (SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnDemand, &dec)) &&
      SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
      SUCCEEDED(conv->Initialize(frame, kBgra32, WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom)) &&
      SUCCEEDED(conv->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 16384 && h <= 16384) {
    px.resize(static_cast<size_t>(w) * h * 4);
    ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()));
  }
  if (conv) conv->Release();
  if (frame) frame->Release();
  if (dec) dec->Release();
  return ok;
}

}  // namespace

uint64_t TextureHash(const uint8_t* bgra, UINT w, UINT h) {
  // FNV-1a, 64-bit.
  uint64_t x = 14695981039346656037ull;
  auto mix = [&](uint8_t b) {
    x ^= b;
    x *= 1099511628211ull;
  };
  for (int i = 0; i < 4; ++i) mix(static_cast<uint8_t>(w >> (i * 8)));
  for (int i = 0; i < 4; ++i) mix(static_cast<uint8_t>(h >> (i * 8)));
  const size_t n = static_cast<size_t>(w) * h * 4;
  for (size_t i = 0; i < n; ++i) mix(bgra[i]);
  return x;
}

bool ExactTextureHashNeeded() {
  const Config& cfg = GetConfig();
  return cfg.texture_dump || (cfg.texture_replace && !Index().empty());
}

uint64_t FastTextureHash(const uint8_t* bgra, UINT w, UINT h) {
  // 8 bytes a step, multiply and fold (not the same values as TextureHash).
  uint64_t x = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(w) << 32 | h);
  const size_t n = static_cast<size_t>(w) * h * 4;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v;
    std::memcpy(&v, bgra + i, 8);
    x = (x ^ v) * 0xff51afd7ed558ccdull;
    x ^= x >> 29;
  }
  for (; i < n; ++i) x = (x ^ bgra[i]) * 1099511628211ull;
  return x ^ (x >> 31);
}

namespace {

bool LoadPicture(const std::wstring& path, const char* label, GpuTexture& out) {
  std::vector<uint8_t> px;
  UINT w = 0, h = 0;
  if (!Decode(path, px, w, h)) {
    LogOnce("texture replacement %s: could not read the picture", label);
    return false;
  }
  if (!CreateTextureFromBGRA(out, px.data(), w, h)) {
    LogOnce("texture replacement %s: could not create a %ux%u texture", label, w, h);
    return false;
  }
  return true;
}

std::wstring DumpDir() {
  static std::wstring dir;
  if (dir.empty()) {
    std::wstring user = UserDir();
    if (user.empty()) return L"";
    CreateDirectoryW(user.c_str(), nullptr);
    dir = user + L"\\textures_dump";
    CreateDirectoryW(dir.c_str(), nullptr);
  }
  return dir;
}

bool WritePng(const std::wstring& path, const uint8_t* bgra, UINT w, UINT h) {
  IWICImagingFactory* f = Wic();
  if (!f) return false;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  bool ok = false;
  if (SUCCEEDED(f->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
      SUCCEEDED(f->CreateEncoder(kPngContainer, nullptr, &enc)) &&
      SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
      SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
      SUCCEEDED(frame->SetSize(w, h))) {
    WICPixelFormatGUID fmt = kBgra32;
    if (SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
        SUCCEEDED(frame->WritePixels(h, w * 4, w * h * 4, const_cast<BYTE*>(bgra))) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit()))
      ok = true;
  }
  if (frame) frame->Release();
  if (enc) enc->Release();
  if (stream) stream->Release();
  if (!ok) DeleteFileW(path.c_str());
  return ok;
}

std::string g_pending_name;
std::string g_copy_name;

}  // namespace

void SetPendingTextureName(const char* name) { g_pending_name = name ? name : ""; }

void SetCopyTextureName(const char* name) { g_copy_name = name ? name : ""; }

std::string TakeCopyTextureName() {
  std::string s;
  s.swap(g_copy_name);
  return s;
}

std::string TakePendingTextureName() {
  std::string s;
  s.swap(g_pending_name);
  return s;
}

std::string TextureKey(const char* name) {
  std::string s = name ? name : "";
  const size_t dot = s.find_last_of('.');
  const size_t sep = s.find_last_of("\\/");
  if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) s.resize(dot);
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    else if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
             c == '>' || c == '|' || c == ' ')
      c = '_';
  }
  while (!s.empty() && (s[0] == '_' || s[0] == '.')) s.erase(0, 1);
  return s;
}

bool LoadReplacement(uint64_t hash, GpuTexture& out) {
  if (!GetConfig().texture_replace) return false;
  const auto& index = Index();
  auto it = index.find(hash);
  if (it == index.end()) return false;
  char label[24];
  std::snprintf(label, sizeof(label), "%016llx", static_cast<unsigned long long>(hash));
  if (!LoadPicture(it->second, label, out)) return false;
  LogOnce("texture replacement %s: %ux%u", label, out.width, out.height);
  return true;
}

bool LoadReplacementByName(const std::string& key, GpuTexture& out) {
  if (!GetConfig().texture_replace || key.empty()) return false;
  Index();
  auto it = NameIndex().find(key);
  if (it == NameIndex().end()) return false;
  // One GPU copy per name, shared by every size the game streams in.
  static std::map<std::string, GpuTexture> cache;
  auto c = cache.find(key);
  if (c == cache.end()) {
    GpuTexture t;
    if (!LoadPicture(it->second, key.c_str(), t)) return false;
    c = cache.emplace(key, t).first;
    Log("texture replacement \"%s\": %ux%u", key.c_str(), t.width, t.height);
  }
  out.Reset();
  out = c->second.Share();
  out.levels = c->second.levels;
  return true;
}

void DumpNamedTexture(const std::string& key, const uint8_t* bgra, UINT w, UINT h) {
  if (!GetConfig().texture_dump || key.empty()) return;
  // Keep the largest version the game streamed in.
  static std::map<std::string, uint64_t> biggest;
  const uint64_t area = static_cast<uint64_t>(w) * h;
  uint64_t& best = biggest[key];
  if (area <= best) return;
  best = area;
  const std::wstring dir = DumpDir();
  if (dir.empty()) return;
  std::wstring path = dir + L"\\";
  for (char c : key) path += static_cast<wchar_t>(static_cast<unsigned char>(c));
  path += L".png";
  if (!WritePng(path, bgra, w, h)) {
    LogOnce("texture dump failed for \"%s\"", key.c_str());
    return;
  }
  static unsigned logged = 0;
  if (++logged <= 300) Log("texture dump \"%s\" %ux%u", key.c_str(), w, h);
}

void DumpTexture(uint64_t hash, const uint8_t* bgra, UINT w, UINT h) {
  if (!GetConfig().texture_dump) return;
  static std::set<uint64_t> done;
  if (!done.insert(hash).second) return;
  const std::wstring dir = DumpDir();
  if (dir.empty()) return;
  wchar_t name[32];
  std::swprintf(name, 32, L"\\%016llx.png", static_cast<unsigned long long>(hash));
  std::wstring path = dir + name;
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
  if (!WritePng(path, bgra, w, h))
    LogOnce("texture dump failed for %016llx", static_cast<unsigned long long>(hash));
}

}  // namespace dd
