// MenuMaX: see gameart.h.
//
// UI\<NAME>.DAT is an "RS2" archive: "RS2\0", u32 count, then per entry
// {u32 name length, name\0, u32 offset, u32 size}. The menu sprites in it
// are TGA files: 16-bit 5:5:5 (types 2 raw and 10 run-length), magenta
// (0x7C1F) the colour key.
#include "gameart.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "../patch.h"

namespace ui {

namespace {

struct Archive {
  std::vector<uint8_t> data;
  std::map<std::string, std::pair<uint32_t, uint32_t>> entries;  // lower-case name with '\' -> offset, size
  bool ok = false;
};

std::mutex g_mu;
std::map<std::string, Archive> g_archives;  // by upper-case archive name

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c == '/') c = '\\';
    else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
  uint32_t v = 0;
  if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
  return v;
}

std::wstring GameDir() {
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring s(buf);
  const size_t slash = s.find_last_of(L'\\');
  return slash == std::wstring::npos ? std::wstring(L".\\") : s.substr(0, slash + 1);
}

Archive& Open(const std::string& name) {
  auto it = g_archives.find(name);
  if (it != g_archives.end()) return it->second;
  Archive& a = g_archives[name];
  std::wstring path = GameDir() + L"UI\\";
  for (char c : name) path += static_cast<wchar_t>(static_cast<unsigned char>(c));
  path += L".DAT";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    patch::Log("MenuMaX: game art: %ls could not be opened", path.c_str());
    return a;
  }
  const DWORD size = GetFileSize(f, nullptr);
  a.data.resize(size);
  DWORD got = 0;
  ReadFile(f, a.data.data(), size, &got, nullptr);
  CloseHandle(f);
  if (got != size || size < 8 || std::memcmp(a.data.data(), "RS2", 3) != 0) {
    patch::Log("MenuMaX: game art: %ls is not an RS2 archive", path.c_str());
    a.data.clear();
    return a;
  }
  const uint32_t count = U32(a.data, 4);
  size_t p = 8;
  for (uint32_t i = 0; i < count && p + 4 <= a.data.size(); ++i) {
    const uint32_t len = U32(a.data, p);
    p += 4;
    if (len == 0 || p + len + 8 > a.data.size()) break;
    std::string n(reinterpret_cast<const char*>(a.data.data() + p), strnlen(reinterpret_cast<const char*>(a.data.data() + p), len));
    p += len;
    const uint32_t off = U32(a.data, p), sz = U32(a.data, p + 4);
    p += 8;
    if (static_cast<size_t>(off) + sz <= a.data.size()) a.entries[Lower(n)] = {off, sz};
  }
  a.ok = true;
  patch::Log("MenuMaX: game art: %ls, %u entries", path.c_str(), static_cast<unsigned>(a.entries.size()));
  return a;
}

struct Options {
  bool black = false, opaque = false;
  bool bar = false;  // a slider track's bar only (the rows above the tick marks)
  int frame = 0;
};

// TGA (types 2 and 10; 16, 24 or 32 bits) -> B, G, R, A.
bool DecodeTga(const uint8_t* d, size_t n, const Options& o, std::vector<uint8_t>& px, unsigned& w, unsigned& h,
               std::string* err) {
  if (n < 18) return *err = "too short", false;
  const unsigned idlen = d[0], cmap = d[1], type = d[2];
  const unsigned cmlen = d[5] | (d[6] << 8), cmbits = d[7];
  w = d[12] | (d[13] << 8);
  h = d[14] | (d[15] << 8);
  const unsigned bpp = d[16], desc = d[17];
  if ((type != 2 && type != 10) || (bpp != 16 && bpp != 24 && bpp != 32) || !w || !h || w > 4096 || h > 4096) {
    char b[96];
    std::snprintf(b, sizeof b, "TGA type %u, %u bits, %ux%u not read", type, bpp, w, h);
    return *err = b, false;
  }
  size_t p = 18 + idlen + (cmap ? cmlen * ((cmbits + 7) / 8) : 0);
  const unsigned bytes = bpp / 8;
  px.assign(static_cast<size_t>(w) * h * 4, 0);
  auto put = [&](size_t i, const uint8_t* s) {
    uint8_t* q = &px[i * 4];
    bool key;
    if (bpp == 16) {
      const unsigned v = s[0] | (s[1] << 8);
      const unsigned r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
      q[0] = static_cast<uint8_t>((b << 3) | (b >> 2));
      q[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
      q[2] = static_cast<uint8_t>((r << 3) | (r >> 2));
      key = (v & 0x7FFF) == 0x7C1F;
      if (o.black && (v & 0x7FFF) == 0) key = true;
    } else {
      q[0] = s[0];
      q[1] = s[1];
      q[2] = s[2];
      key = s[0] >= 248 && s[1] <= 8 && s[2] >= 248;
      if (o.black && !s[0] && !s[1] && !s[2]) key = true;
    }
    q[3] = (key && !o.opaque) ? 0 : (bpp == 32 ? s[3] : 255);
    if (!q[3]) q[0] = q[1] = q[2] = 0;
  };
  const size_t total = static_cast<size_t>(w) * h;
  size_t i = 0;
  if (type == 2) {
    if (p + total * bytes > n) return *err = "TGA data cut short", false;
    for (; i < total; ++i) put(i, d + p + i * bytes);
  } else {
    while (i < total) {
      if (p >= n) return *err = "TGA data cut short", false;
      const unsigned head = d[p++];
      const unsigned count = (head & 0x7F) + 1;
      if (head & 0x80) {
        if (p + bytes > n) return *err = "TGA data cut short", false;
        for (unsigned k = 0; k < count && i < total; ++k) put(i++, d + p);
        p += bytes;
      } else {
        if (p + count * bytes > n) return *err = "TGA data cut short", false;
        for (unsigned k = 0; k < count && i < total; ++k) put(i++, d + p + k * bytes);
        p += count * bytes;
      }
    }
  }
  // Rows are stored bottom first unless the descriptor says top first.
  if (!(desc & 0x20)) {
    std::vector<uint8_t> row(w * 4);
    for (unsigned y = 0; y < h / 2; ++y) {
      uint8_t* a = &px[static_cast<size_t>(y) * w * 4];
      uint8_t* b = &px[static_cast<size_t>(h - 1 - y) * w * 4];
      std::memcpy(row.data(), a, w * 4);
      std::memcpy(a, b, w * 4);
      std::memcpy(b, row.data(), w * 4);
    }
  }
  if (o.bar) {
    // The rows down to the first that is see-through in the middle.
    unsigned bar = 0;
    while (bar < h && px[(static_cast<size_t>(bar) * w + w / 2) * 4 + 3]) ++bar;
    std::memset(&px[static_cast<size_t>(bar) * w * 4], 0, static_cast<size_t>(h - bar) * w * 4);
  }
  if (o.frame > 0) {
    const unsigned f = static_cast<unsigned>(o.frame);
    for (unsigned y = f; y + f < h; ++y)
      for (unsigned x = f; x + f < w; ++x) std::memset(&px[(static_cast<size_t>(y) * w + x) * 4], 0, 4);
  }
  return true;
}

}  // namespace

bool LoadGameArt(const std::string& rel_in, std::vector<uint8_t>& bgra, unsigned& w, unsigned& h, std::string* err) {
  std::string rel = rel_in, opts;
  const size_t q = rel.find('?');
  if (q != std::string::npos) {
    opts = rel.substr(q + 1);
    rel.resize(q);
  }
  Options o;
  for (size_t a = 0; a <= opts.size();) {
    size_t b = opts.find('&', a);
    if (b == std::string::npos) b = opts.size();
    const std::string t = opts.substr(a, b - a);
    if (t == "black") o.black = true;
    else if (t == "opaque") o.opaque = true;
    else if (t == "bar") o.bar = true;
    else if (t.compare(0, 6, "frame=") == 0) o.frame = std::atoi(t.c_str() + 6);
    a = b + 1;
  }
  const size_t slash = rel.find_first_of("/\\");
  if (slash == std::string::npos) return *err = "no archive name", false;
  std::string arch = rel.substr(0, slash);
  for (char& c : arch)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  const std::string entry = Lower(rel.substr(slash + 1));
  std::lock_guard<std::mutex> l(g_mu);
  Archive& a = Open(arch);
  if (!a.ok) return *err = "archive UI\\" + arch + ".DAT could not be read", false;
  auto it = a.entries.find(entry);
  if (it == a.entries.end()) return *err = "no " + entry + " in UI\\" + arch + ".DAT", false;
  return DecodeTga(a.data.data() + it->second.first, it->second.second, o, bgra, w, h, err);
}

}  // namespace ui
