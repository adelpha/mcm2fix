#include "texreplace.h"

#include <objbase.h>
#include <wincodec.h>

#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../config.h"

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
  wchar_t buf[MAX_PATH] = {};
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (!n || n >= MAX_PATH) return L"";
  return std::wstring(buf) + L"\\Motocross Madness 2";
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

// hash -> picture file, from both replacement folders (the per-user one wins).
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
      if (ParseHash(fd.cFileName, &hash)) index[hash] = dir + L"\\" + fd.cFileName;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  Log("texture replacement: %u pictures found", static_cast<unsigned>(index.size()));
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

bool LoadReplacement(uint64_t hash, GpuTexture& out) {
  if (!GetConfig().texture_replace) return false;
  const auto& index = Index();
  auto it = index.find(hash);
  if (it == index.end()) return false;
  std::vector<uint8_t> px;
  UINT w = 0, h = 0;
  if (!Decode(it->second, px, w, h)) {
    LogOnce("texture replacement %016llx: could not read the picture",
            static_cast<unsigned long long>(hash));
    return false;
  }
  out.Reset();
  Backend& b = B();
  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 0;  // full chain
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  if (FAILED(b.device->CreateTexture2D(&td, nullptr, &out.tex))) return false;
  out.tex->GetDesc(&td);
  b.device->CreateShaderResourceView(out.tex, nullptr, &out.srv);
  if (!out.srv) {
    out.Reset();
    return false;
  }
  b.ctx->UpdateSubresource(out.tex, 0, nullptr, px.data(), w * 4, 0);
  b.ctx->GenerateMips(out.srv);
  out.width = w;
  out.height = h;
  out.levels = td.MipLevels;
  LogOnce("texture replacement %016llx: %ux%u", static_cast<unsigned long long>(hash), w, h);
  return true;
}

void DumpTexture(uint64_t hash, const uint8_t* bgra, UINT w, UINT h) {
  if (!GetConfig().texture_dump) return;
  static std::set<uint64_t> done;
  if (!done.insert(hash).second) return;
  static std::wstring dir;
  if (dir.empty()) {
    std::wstring user = UserDir();
    if (user.empty()) return;
    CreateDirectoryW(user.c_str(), nullptr);
    dir = user + L"\\textures_dump";
    CreateDirectoryW(dir.c_str(), nullptr);
  }
  wchar_t name[32];
  std::swprintf(name, 32, L"\\%016llx.png", static_cast<unsigned long long>(hash));
  std::wstring path = dir + name;
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
  IWICImagingFactory* f = Wic();
  if (!f) return;
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
  if (!ok) {
    DeleteFileW(path.c_str());
    LogOnce("texture dump failed for %016llx", static_cast<unsigned long long>(hash));
  }
}

}  // namespace dd
