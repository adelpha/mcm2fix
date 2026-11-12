// mcm2sky: pull the sky boxes out of Motocross Madness 2, stitch each one into
// a single picture for editing, and cut an edited picture back into tiles that
// mcm2fix's texture replacement picks up (textures\<file>_<offset>.png).
//
//   mcm2sky extract [game folder] [-o output folder]
//   mcm2sky split <picture.png> [more pictures...] [-o output folder] [--seams average|none]
//
// With no arguments it extracts; a folder dropped on it extracts from that
// game folder; PNGs dropped on it are split. See tools/mcm2sky/README.md.
//
// Game file formats (from the game's own readers in Mcm2.exe):
//   * RES/ENV archives start "FAOE" and are encrypted: key k starts at 0xfa and
//     for each character c of the lower-case file name ("skies.res")
//     k = ((k + 3) ^ c) + k. Every byte after the 4-byte header decodes as
//     plain = k ^ cipher, then k += cipher (0x4628f0). Each archive member is
//     decoded from its start with the initial key (0x461ce0 resets it).
//   * Decoded, the archive is "RS2\0", count, then per member: name length,
//     name, offset, size. Offsets count from the end of the "FAOE" header.
//   * SKIES.RES holds the sky boxes as <name>.cub: a geometry block, then 48
//     textures of 128x128. Each texture is a dword 2 then a table of mip
//     offsets (each relative to its own position; the first is the table size)
//     and the mip levels from 1x1 up. A level is raw X1R5G5B5 when its size is
//     w*h*2, otherwise LZW (0x4a1940: 9-bit codes growing on code 0x101, 0x100
//     ends, 0x102 resets, MSB-first bits, a 32-bit header first).
//   * The game names a texture read from a stream "<member>_<offset hex>" with
//     the offset of its mip table in the decoded archive (fixes_textures.cpp),
//     e.g. terra12_51af51.
//   * A track's sky is the CubeFile= line of the .scn inside its .ENV.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image_write.h"

namespace {

// ---------------------------------------------------------------- files

#ifdef _WIN32
const char kSep = '\\';
std::wstring Wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}
std::string Utf8(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
  return s;
}
FILE* Open(const std::string& path, const char* mode) { return _wfopen(Wide(path).c_str(), Wide(mode).c_str()); }
bool IsDir(const std::string& p) {
  DWORD a = GetFileAttributesW(Wide(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool IsFile(const std::string& p) {
  DWORD a = GetFileAttributesW(Wide(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
void MakeDir(const std::string& p) { CreateDirectoryW(Wide(p).c_str(), nullptr); }
std::vector<std::string> List(const std::string& dir) {
  std::vector<std::string> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(Wide(dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    std::string n = Utf8(fd.cFileName);
    if (n != "." && n != "..") out.push_back(n);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end());
  return out;
}
std::string ExeDir() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
  std::string p = Utf8(std::wstring(buf, n));
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? "." : p.substr(0, s);
}
std::string CurDir() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetCurrentDirectoryW(MAX_PATH * 2, buf);
  return Utf8(std::wstring(buf, n));
}
#else
const char kSep = '/';
FILE* Open(const std::string& path, const char* mode) { return fopen(path.c_str(), mode); }
bool IsDir(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool IsFile(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
void MakeDir(const std::string& p) { mkdir(p.c_str(), 0755); }
std::vector<std::string> List(const std::string& dir) {
  std::vector<std::string> out;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n != "." && n != "..") out.push_back(n);
    }
    closedir(d);
  }
  std::sort(out.begin(), out.end());
  return out;
}
std::string ExeDir() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  std::string p(buf, n);
  return p.substr(0, p.find_last_of('/'));
}
std::string CurDir() {
  char buf[4096];
  return getcwd(buf, sizeof(buf)) ? buf : ".";
}
#endif

std::string Join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  char last = a.back();
  return (last == '\\' || last == '/') ? a + b : a + kSep + b;
}
std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}
std::string BaseName(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? p : p.substr(s + 1);
}
std::string DirName(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? "." : p.substr(0, s);
}
std::string StripExt(const std::string& n) {
  size_t d = n.find_last_of('.');
  return d == std::string::npos ? n : n.substr(0, d);
}
// Finds an entry of `dir` by name, ignoring case (for Linux test runs).
std::string FindIn(const std::string& dir, const std::string& name) {
  std::string want = Lower(name);
  for (const std::string& e : List(dir))
    if (Lower(e) == want) return Join(dir, e);
  return "";
}

bool ReadAt(const std::string& path, long off, size_t len, std::vector<uint8_t>& out) {
  FILE* f = Open(path, "rb");
  if (!f) return false;
  out.resize(len);
  bool ok = fseek(f, off, SEEK_SET) == 0 && fread(out.data(), 1, len, f) == len;
  fclose(f);
  return ok;
}
long FileSize(const std::string& path) {
  FILE* f = Open(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return n;
}

[[noreturn]] void Fail(const std::string& msg);

// ---------------------------------------------------------------- archives

struct Member {
  std::string name;
  uint32_t offset = 0, size = 0;
};

uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

class Archive {
 public:
  bool Load(const std::string& path) {
    path_ = path;
    long size = FileSize(path);
    std::vector<uint8_t> head;
    if (size < 16 || !ReadAt(path, 0, 4, head) || std::memcmp(head.data(), "FAOE", 4) != 0) return false;
    key_ = 0xfa;
    for (char c : Lower(BaseName(path))) key_ = static_cast<uint8_t>(((key_ + 3) ^ static_cast<uint8_t>(c)) + key_);
    file_size_ = static_cast<uint32_t>(size - 4);
    std::vector<uint8_t> dir;
    if (!Read(0, std::min<uint32_t>(file_size_, 1 << 16), dir) || std::memcmp(dir.data(), "RS2", 4) != 0) return false;
    uint32_t n = U32(&dir[4]);
    size_t p = 8;
    for (uint32_t i = 0; i < n; ++i) {
      if (p + 4 > dir.size()) return false;
      uint32_t len = U32(&dir[p]);
      if (p + 4 + len + 8 > dir.size()) return false;
      Member m;
      m.name.assign(reinterpret_cast<const char*>(&dir[p + 4]), strnlen(reinterpret_cast<const char*>(&dir[p + 4]), len));
      m.offset = U32(&dir[p + 4 + len]);
      m.size = U32(&dir[p + 8 + len]);
      p += 12 + len;
      if (m.offset + static_cast<uint64_t>(m.size) > file_size_) return false;
      members_.push_back(m);
    }
    return true;
  }
  // Decodes `len` bytes from `off` (decoded-archive offset) with a fresh key,
  // as the game does when it opens a member.
  bool Read(uint32_t off, uint32_t len, std::vector<uint8_t>& out) const {
    if (!ReadAt(path_, 4 + static_cast<long>(off), len, out)) return false;
    uint8_t k = key_;
    for (uint8_t& c : out) {
      uint8_t x = c;
      c = k ^ x;
      k = static_cast<uint8_t>(k + x);
    }
    return true;
  }
  bool ReadMember(const Member& m, std::vector<uint8_t>& out) const { return Read(m.offset, m.size, out); }
  const std::vector<Member>& members() const { return members_; }

 private:
  std::string path_;
  uint8_t key_ = 0;
  uint32_t file_size_ = 0;
  std::vector<Member> members_;
};

// ---------------------------------------------------------------- LZW (0x4a1940)

bool Lzw(const uint8_t* src, size_t len, size_t want, std::vector<uint8_t>& out) {
  out.clear();
  out.reserve(want);
  size_t total_bits = len * 8, pos = 32;  // the first 32 bits are a header
  auto read = [&](int n) -> int {
    if (pos + n > total_bits) return 0x100;
    uint32_t v = 0;
    for (int i = 0; i < n; ++i, ++pos) v = (v << 1) | ((src[pos >> 3] >> (7 - (pos & 7))) & 1);
    return static_cast<int>(v);
  };
  const int kMax = 1 << 16;
  std::vector<int> prefix(kMax);
  std::vector<uint8_t> chr(kMax);
  std::vector<uint8_t> stack;
  int next = 0x103, width = 9;
  int old = read(width);
  if (old == 0x100) return out.size() >= want;
  if (old > 0xff) return false;
  out.push_back(static_cast<uint8_t>(old));
  uint8_t fin = static_cast<uint8_t>(old);
  while (out.size() < want) {
    int c = read(width);
    if (c == 0x100) break;
    if (c == 0x102) {
      next = 0x103;
      width = 9;
      old = read(width);
      if (old == 0x100) break;
      if (old > 0xff) return false;
      out.push_back(static_cast<uint8_t>(old));
      fin = static_cast<uint8_t>(old);
      continue;
    }
    if (c == 0x101) {
      if (++width > 16) return false;
      continue;
    }
    stack.clear();
    int cx;
    if (c >= next) {
      if (c > next) return false;
      stack.push_back(fin);
      cx = old;
    } else {
      cx = c;
    }
    while (cx > 0xff) {
      if (cx >= next || stack.size() > static_cast<size_t>(kMax)) return false;
      stack.push_back(chr[cx]);
      cx = prefix[cx];
    }
    stack.push_back(static_cast<uint8_t>(cx));
    fin = static_cast<uint8_t>(cx);
    out.insert(out.end(), stack.rbegin(), stack.rend());
    if (next < kMax) {
      prefix[next] = old;
      chr[next] = fin;
      ++next;
    }
    old = c;
  }
  if (out.size() < want) return false;
  out.resize(want);
  return true;
}

// ---------------------------------------------------------------- sky boxes

struct Tile {
  std::string name;          // e.g. terra12_51af51
  int size = 0;              // pixels across
  std::vector<uint8_t> rgb;  // size*size*3
};

struct Sky {
  std::string name;  // e.g. terra12
  std::vector<Tile> tiles;
};

uint8_t Expand5(unsigned v) { return static_cast<uint8_t>((v * 255 + 15) / 31); }  // as mcm2fix's dump

bool ParseCube(const Archive& ar, const Member& m, Sky& sky, std::string& err) {
  std::vector<uint8_t> c;
  if (!ar.ReadMember(m, c)) {
    err = "could not read";
    return false;
  }
  sky.name = Lower(StripExt(m.name));
  sky.tiles.clear();
  // The textures follow the geometry: find the first "2, table" start.
  size_t p = 0;
  for (size_t i = 0; i + 16 < c.size(); ++i) {
    if (U32(&c[i]) != 2) continue;
    uint32_t t0 = U32(&c[i + 4]);
    if (t0 % 4 || t0 < 12 || t0 > 0x30 || i + 4 + t0 > c.size()) continue;
    if (U32(&c[i + 8]) + 4 != t0 + 2) continue;  // 1x1 level: 2 raw bytes
    p = i + 4;
    break;
  }
  if (!p) {
    err = "no textures found";
    return false;
  }
  while (p < c.size()) {
    if (p < 4 || U32(&c[p - 4]) != 2) {
      err = "unexpected data between textures";
      return false;
    }
    uint32_t t0 = U32(&c[p]);
    int n = static_cast<int>(t0 / 4);  // n-1 levels
    if (t0 % 4 || n < 3 || n > 12 || p + t0 > c.size()) {
      err = "bad texture header";
      return false;
    }
    std::vector<uint32_t> offs(n);
    for (int i = 0; i < n; ++i) offs[i] = U32(&c[p + 4 * i]) + 4 * i;
    for (int i = 1; i < n; ++i)
      if (offs[i] <= offs[i - 1] || p + offs[i] > c.size()) {
        err = "bad mip table";
        return false;
      }
    Tile t;
    t.size = 1 << (n - 2);
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "_%x", static_cast<unsigned>(m.offset + p));
    t.name = sky.name + suffix;
    const size_t want = static_cast<size_t>(t.size) * t.size * 2;
    const uint8_t* lv = &c[p + offs[n - 2]];
    const size_t lv_len = offs[n - 1] - offs[n - 2];
    std::vector<uint8_t> px;
    if (lv_len == want) {
      px.assign(lv, lv + lv_len);
    } else if (!Lzw(lv, lv_len, want, px)) {
      err = "could not unpack " + t.name;
      return false;
    }
    t.rgb.resize(static_cast<size_t>(t.size) * t.size * 3);
    for (size_t i = 0; i < static_cast<size_t>(t.size) * t.size; ++i) {
      unsigned v = px[2 * i] | (px[2 * i + 1] << 8);
      t.rgb[3 * i] = Expand5((v >> 10) & 31);
      t.rgb[3 * i + 1] = Expand5((v >> 5) & 31);
      t.rgb[3 * i + 2] = Expand5(v & 31);
    }
    sky.tiles.push_back(std::move(t));
    p += offs[n - 1] + 4;
  }
  if (sky.tiles.size() != 48) {
    err = "expected 48 textures, found " + std::to_string(sky.tiles.size());
    return false;
  }
  for (const Tile& t : sky.tiles)
    if (t.size != sky.tiles[0].size) {
      err = "textures of different sizes";
      return false;
    }
  return true;
}

// ---------------------------------------------------------------- layout
//
// A sky box is a top face of 4x4 tiles and four sides of 4x2 tiles (upper
// row, then horizon row). In archive order: tiles 0-7 side A, 8-15 B, 16-23 C,
// 24-31 D, 32-47 the top face. Neighbouring tiles overlap by 1/16 of a tile
// (8 pixels at 128): each repeats the last 8 rows/columns of the one before.
//
// Round the horizon the sides join B -> A -> D -> C -> B. The top face's
// bottom edge (left to right) is A's top edge, its right edge (bottom to top)
// is D's, its top edge (right to left) is C's and its left edge (top to
// bottom) is B's. Checked on all 14 skies: those seams differ by ~2/255.
//
// The stitched picture puts the four sides in a row (B A D C) with the top
// face above A, so the horizon and the A/top seam are continuous in it.

struct Layout {
  int tile, ov, step, width, height;
  explicit Layout(int t) : tile(t), ov(t / 16), step(t - t / 16), width(16 * step + ov), height(6 * step + ov) {}
  int face() const { return 4 * step + ov; }
  int top_x() const { return 4 * step; }  // above A
  int ring_y() const { return 4 * step; }
  int side_x(char s) const { return static_cast<int>(std::string("BADC").find(s)) * 4 * step; }
  // Tile i's top-left corner in the picture.
  void Pos(int i, int& x, int& y) const {
    if (i >= 32) {
      x = top_x() + ((i - 32) % 4) * step;
      y = ((i - 32) / 4) * step;
    } else {
      char side = "ABCD"[i / 8];
      x = side_x(side) + (i % 4) * step;
      y = ring_y() + ((i % 8) / 4) * step;
    }
  }
};

struct Picture {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
  uint8_t* At(int x, int y) { return &rgba[(static_cast<size_t>(y) * w + x) * 4]; }
};

bool SavePng(const std::string& path, int w, int h, int comp, const uint8_t* data) {
  std::vector<uint8_t> bytes;
  auto sink = [](void* ctx, void* d, int n) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + n);
  };
  if (!stbi_write_png_to_func(sink, &bytes, w, h, comp, data, w * comp)) return false;
  FILE* f = Open(path, "wb");
  if (!f) return false;
  bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  ok = (fclose(f) == 0) && ok;
  return ok;
}

bool LoadPng(const std::string& path, Picture& pic) {
  FILE* f = Open(path, "rb");
  if (!f) return false;
  std::vector<uint8_t> bytes;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
  fclose(f);
  int comp = 0;
  uint8_t* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &pic.w, &pic.h, &comp, 4);
  if (!px) return false;
  pic.rgba.assign(px, px + static_cast<size_t>(pic.w) * pic.h * 4);
  stbi_image_free(px);
  return true;
}

// ---------------------------------------------------------------- game folder

bool IsGameDir(const std::string& d) {
  std::string res = FindIn(d, "RES");
  return !res.empty() && !FindIn(res, "SKIES.RES").empty();
}

std::string FindGame(const std::string& hint) {
  std::vector<std::string> tries;
  if (!hint.empty()) tries = {hint, Join(hint, "mcm2"), DirName(hint)};
  std::string exe = ExeDir(), cur = CurDir();
  for (const std::string& d : {exe, cur}) {
    tries.push_back(d);
    tries.push_back(Join(d, "mcm2"));
    tries.push_back(DirName(d));
    tries.push_back(Join(DirName(d), "mcm2"));
    tries.push_back(Join(DirName(DirName(d)), "mcm2"));
  }
  for (const std::string& d : tries)
    if (IsDir(d) && IsGameDir(d)) return d;
  return "";
}

std::vector<Sky> LoadSkies(const std::string& game) {
  std::string path = FindIn(FindIn(game, "RES"), "SKIES.RES");
  Archive ar;
  if (!ar.Load(path)) Fail("could not read " + path + " (is it the original game file?)");
  std::vector<Sky> skies;
  for (const Member& m : ar.members()) {
    if (Lower(m.name).size() < 4 || Lower(m.name).substr(m.name.size() - 4) != ".cub") continue;
    Sky s;
    std::string err;
    if (!ParseCube(ar, m, s, err)) {
      std::printf("  skipped %s: %s\n", m.name.c_str(), err.c_str());
      continue;
    }
    skies.push_back(std::move(s));
  }
  return skies;
}

struct Track {
  std::string file, scene, sky;
};

std::vector<Track> LoadTracks(const std::string& game) {
  std::vector<Track> out;
  std::string tera = FindIn(game, "TERAFORM");
  if (tera.empty()) return out;
  for (const std::string& group : List(tera)) {
    std::string gdir = Join(tera, group);
    if (!IsDir(gdir)) continue;
    for (const std::string& f : List(gdir)) {
      if (Lower(f).size() < 4 || Lower(f).substr(f.size() - 4) != ".env") continue;
      Archive ar;
      if (!ar.Load(Join(gdir, f))) continue;
      for (const Member& m : ar.members()) {
        std::string ln = Lower(m.name);
        if (ln.size() < 4 || ln.substr(ln.size() - 4) != ".scn") continue;
        std::vector<uint8_t> d;
        if (!ar.ReadMember(m, d)) continue;
        std::string text(d.begin(), d.end());
        Track t;
        t.file = group + "\\" + StripExt(f);
        auto value = [&](const char* key) {
          size_t k = Lower(text).find(Lower(key));
          if (k == std::string::npos) return std::string();
          size_t e = text.find_first_of("\r\n", k);
          std::string v = text.substr(k + std::strlen(key), e == std::string::npos ? std::string::npos : e - k - std::strlen(key));
          v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
          return v;
        };
        t.scene = value("SceneName=");
        std::string cube = value("CubeFile=");
        t.sky = Lower(cube) == "none" || cube.empty() ? "" : Lower(StripExt(cube));
        out.push_back(t);
        break;
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------- extract

void Extract(const std::string& game, std::string out) {
  if (out.empty()) out = Join(ExeDir(), "skies");
  std::printf("Game folder:   %s\nOutput folder: %s\n\n", game.c_str(), out.c_str());
  std::vector<Sky> skies = LoadSkies(game);
  if (skies.empty()) Fail("no sky boxes found in RES\\SKIES.RES");
  MakeDir(out);
  std::vector<Track> tracks = LoadTracks(game);

  std::string manifest =
      "; mcm2sky tile list: <sky> <tile size> then the 48 texture names in archive order.\n"
      "; split reads this to name the tiles; keep it next to the pictures.\n";
  for (const Sky& s : skies) {
    Layout L(s.tiles[0].size);
    Picture pic;
    pic.w = L.width;
    pic.h = L.height;
    pic.rgba.assign(static_cast<size_t>(pic.w) * pic.h * 4, 0);
    for (int i = 0; i < 48; ++i) {
      int x0, y0;
      L.Pos(i, x0, y0);
      const Tile& t = s.tiles[i];
      for (int y = 0; y < t.size; ++y)
        for (int x = 0; x < t.size; ++x) {
          uint8_t* d = pic.At(x0 + x, y0 + y);
          const uint8_t* src = &t.rgb[(static_cast<size_t>(y) * t.size + x) * 3];
          d[0] = src[0];
          d[1] = src[1];
          d[2] = src[2];
          d[3] = 255;
        }
    }
    std::string file = Join(out, s.name + "_sky.png");
    if (!SavePng(file, pic.w, pic.h, 4, pic.rgba.data())) Fail("could not write " + file);
    std::string used;
    for (const Track& t : tracks)
      if (t.sky == s.name) used += (used.empty() ? "" : ", ") + t.file.substr(t.file.find('\\') + 1);
    std::printf("  %-17s %dx%d  %s\n", (s.name + "_sky.png").c_str(), pic.w, pic.h, used.empty() ? "(no track uses it)" : used.c_str());
    manifest += s.name + " " + std::to_string(s.tiles[0].size);
    for (const Tile& t : s.tiles) manifest += " " + t.name;
    manifest += "\n";
  }
  std::string mf = Join(out, "skies.txt");
  if (FILE* f = Open(mf, "wb")) {
    fwrite(manifest.data(), 1, manifest.size(), f);
    fclose(f);
  }

  if (!tracks.empty()) {
    std::string list = "Track                 Sky        Name\r\n";
    for (const Track& t : tracks) {
      char line[256];
      std::snprintf(line, sizeof(line), "%-21s %-10s %s\r\n", t.file.c_str(), t.sky.empty() ? "(none)" : t.sky.c_str(), t.scene.c_str());
      list += line;
    }
    std::string tf = Join(out, "tracks.txt");
    if (FILE* f = Open(tf, "wb")) {
      fwrite(list.data(), 1, list.size(), f);
      fclose(f);
    }
  }
  std::printf(
      "\nWrote %zu sky pictures, skies.txt (tile names) and tracks.txt (which track uses which sky).\n"
      "Edit a picture (keep its size, or scale it by a whole number such as 2x), then drop\n"
      "it on mcm2sky.exe to cut it back into tiles.\n",
      skies.size());
}

// ---------------------------------------------------------------- split

struct Manifest {
  std::map<std::string, std::pair<int, std::vector<std::string>>> skies;
  bool Load(const std::string& path) {
    FILE* f = Open(path, "rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    size_t p = 0;
    while (p < text.size()) {
      size_t e = text.find('\n', p);
      std::string line = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
      p = e == std::string::npos ? text.size() : e + 1;
      if (line.empty() || line[0] == ';') continue;
      std::vector<std::string> w;
      size_t q = 0;
      while (q < line.size()) {
        size_t s = line.find_first_not_of(" \t\r", q);
        if (s == std::string::npos) break;
        size_t t = line.find_first_of(" \t\r", s);
        w.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
        q = t == std::string::npos ? line.size() : t;
      }
      if (w.size() == 50) skies[w[0]] = {std::atoi(w[1].c_str()), std::vector<std::string>(w.begin() + 2, w.end())};
    }
    return !skies.empty();
  }
};

// Pixels the game shows at the same place (the horizon wrap and the top
// face's edges against the sides' top rows) are grouped and each group is set
// to its average, so the box has no seams. Corners belong to three faces.
void SyncSeams(Picture& pic, const Layout& L, int k, bool apply, int& worst) {
  worst = 0;
  const int ov = L.ov * k, F = L.face() * k, step = L.step * k;
  const int tx = L.top_x() * k, ry = L.ring_y() * k;
  std::map<int, int> parent;  // union-find over pixel indices
  auto find = [&](int a) {
    if (!parent.count(a)) parent[a] = a;
    while (parent[a] != a) a = parent[a] = parent[parent[a]];
    return a;
  };
  auto pair = [&](int x1, int y1, int x2, int y2) {
    const uint8_t* a = pic.At(x1, y1);
    const uint8_t* b = pic.At(x2, y2);
    for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(a[c] - b[c]));
    int ra = find(y1 * pic.w + x1), rb = find(y2 * pic.w + x2);
    if (ra != rb) parent[ra] = rb;
  };
  // Horizon wraps: the last columns repeat side B's first ones.
  for (int y = ry; y < pic.h; ++y)
    for (int x = 0; x < ov; ++x) pair(x, y, 16 * step + x, y);
  // Top face edges against the top rows of D, C and B (A touches it already).
  const int dx = L.side_x('D') * k, cx = L.side_x('C') * k, bx = L.side_x('B') * k;
  for (int r = 0; r < ov; ++r)
    for (int i = 0; i < F; ++i) {
      pair(dx + i, ry + r, tx + F - ov + r, F - 1 - i);  // right edge, bottom to top
      pair(cx + i, ry + r, tx + F - 1 - i, ov - 1 - r);  // top edge, right to left
      pair(bx + i, ry + r, tx + ov - 1 - r, i);          // left edge, top to bottom
    }
  if (!apply) return;
  std::map<int, std::vector<int>> groups;
  for (auto& kv : parent) groups[find(kv.first)].push_back(kv.first);
  for (auto& g : groups) {
    int sum[3] = {0, 0, 0};
    for (int idx : g.second)
      for (int c = 0; c < 3; ++c) sum[c] += pic.rgba[static_cast<size_t>(idx) * 4 + c];
    const int n = static_cast<int>(g.second.size());
    for (int idx : g.second) {
      uint8_t* p = &pic.rgba[static_cast<size_t>(idx) * 4];
      for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
      p[3] = 255;
    }
  }
}

void Split(const std::vector<std::string>& files, std::string out, bool seams, const std::string& game_hint) {
  for (const std::string& file : files) {
    std::printf("%s\n", file.c_str());
    // Tile names: skies.txt beside the picture, else read the game.
    Manifest man;
    std::string base = Lower(BaseName(file));
    if (!man.Load(Join(DirName(file), "skies.txt"))) {
      std::string game = FindGame(game_hint);
      if (game.empty()) Fail("no skies.txt next to the picture and the game folder was not found (use --game)");
      for (const Sky& s : LoadSkies(game)) {
        std::vector<std::string> names;
        for (const Tile& t : s.tiles) names.push_back(t.name);
        man.skies[s.name] = {s.tiles[0].size, names};
      }
    }
    // The sky is the longest known name the file name starts with.
    std::string sky;
    for (const auto& kv : man.skies)
      if (base.compare(0, kv.first.size(), kv.first) == 0 &&
          (base.size() == kv.first.size() || !std::isalnum(static_cast<unsigned char>(base[kv.first.size()]))) &&
          kv.first.size() > sky.size())
        sky = kv.first;
    if (sky.empty()) Fail("the file name must start with the sky's name, e.g. terra12_sky.png");
    const int tile = man.skies[sky].first;
    const std::vector<std::string>& names = man.skies[sky].second;
    Layout L(tile);

    Picture pic;
    if (!LoadPng(file, pic)) Fail("could not read the picture (PNG only)");
    if (pic.w % L.width || pic.h * L.width != pic.w * L.height)
      Fail("the picture is " + std::to_string(pic.w) + "x" + std::to_string(pic.h) + "; it must be " +
           std::to_string(L.width) + "x" + std::to_string(L.height) + " or a whole multiple of that");
    const int k = pic.w / L.width;
    int worst = 0;
    SyncSeams(pic, L, k, seams, worst);
    if (seams)
      std::printf("  seams: matched the overlapping edges (largest difference was %d/255)\n", worst);
    else
      std::printf("  seams: left as they are (largest difference %d/255)\n", worst);

    std::string dir = out.empty() ? Join(DirName(file), "textures") : out;
    MakeDir(dir);
    const int ts = tile * k;
    std::vector<uint8_t> rgb(static_cast<size_t>(ts) * ts * 3);
    for (int i = 0; i < 48; ++i) {
      int x0, y0;
      L.Pos(i, x0, y0);
      x0 *= k;
      y0 *= k;
      for (int y = 0; y < ts; ++y)
        for (int x = 0; x < ts; ++x) {
          const uint8_t* s = pic.At(x0 + x, y0 + y);
          uint8_t* d = &rgb[(static_cast<size_t>(y) * ts + x) * 3];
          d[0] = s[0];
          d[1] = s[1];
          d[2] = s[2];
        }
      std::string tf = Join(dir, names[i] + ".png");
      if (!SavePng(tf, ts, ts, 3, rgb.data())) Fail("could not write " + tf);
    }
    std::printf("  wrote 48 tiles of %dx%d to %s\n", ts, ts, dir.c_str());
  }
  std::printf(
      "\nCopy the tiles into %%LOCALAPPDATA%%\\Motocross Madness 2\\textures\\ (TextureReplace=1 in mcm2fix.ini).\n");
}

// ---------------------------------------------------------------- main

bool g_pause = false;

void Pause() {
  if (!g_pause) return;
  std::printf("\nPress Enter to close.");
  std::fflush(stdout);
  std::getchar();
}

[[noreturn]] void Fail(const std::string& msg) {
  std::printf("\nError: %s\n", msg.c_str());
  Pause();
  std::exit(1);
}

const char kUsage[] =
    "mcm2sky - Motocross Madness 2 sky box tool (mcm2fix)\n"
    "\n"
    "  mcm2sky extract [game folder] [-o folder]\n"
    "      Writes every sky box as one picture, <sky>_sky.png, into the folder\n"
    "      (default: skies\\ next to mcm2sky.exe), plus skies.txt and tracks.txt.\n"
    "  mcm2sky split <picture.png>... [-o folder] [--seams average|none]\n"
    "      Cuts edited pictures back into the game's tiles, named for mcm2fix's\n"
    "      textures folder (default: textures\\ next to the picture).\n"
    "\n"
    "Double-click: extract (finds the game next to or above the tool).\n"
    "Drop the game folder on it: extract from that folder.\n"
    "Drop edited pictures on it: split them.\n";

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
#ifdef _WIN32
  (void)argv;
  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  for (int i = 1; i < wargc; ++i) args.push_back(Utf8(wargv[i]));
  LocalFree(wargv);
  DWORD procs[4];
  g_pause = GetConsoleProcessList(procs, 4) <= 1;  // own console: started from Explorer
#else
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
#endif
  std::string mode, out, game;
  bool seams = true;
  std::vector<std::string> inputs;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if ((a == "-o" || a == "--out") && i + 1 < args.size()) {
      out = args[++i];
    } else if (a == "--game" && i + 1 < args.size()) {
      game = args[++i];
    } else if (a == "--seams" && i + 1 < args.size()) {
      std::string v = Lower(args[++i]);
      if (v != "average" && v != "none") Fail("--seams takes average or none");
      seams = v == "average";
    } else if (a == "-h" || a == "--help" || a == "/?") {
      std::printf("%s", kUsage);
      return 0;
    } else if (mode.empty() && (a == "extract" || a == "split")) {
      mode = a;
    } else {
      inputs.push_back(a);
    }
  }
  if (mode.empty()) {
    bool pngs = !inputs.empty();
    for (const std::string& f : inputs) pngs = pngs && IsFile(f) && Lower(f).size() > 4 && Lower(f).substr(f.size() - 4) == ".png";
    mode = pngs ? "split" : "extract";
  }
  if (mode == "extract") {
    if (inputs.size() > 1) Fail(std::string("too many arguments\n\n") + kUsage);
    if (!inputs.empty()) game = inputs[0];
    std::string dir = FindGame(game);
    if (dir.empty())
      Fail("could not find the game (a folder with RES\\SKIES.RES). Drop the game folder on mcm2sky.exe,\n"
           "or run: mcm2sky extract \"C:\\path\\to\\Motocross Madness 2\"");
    Extract(dir, out);
  } else {
    if (inputs.empty()) Fail(std::string("no pictures given\n\n") + kUsage);
    Split(inputs, out, seams, game);
  }
  Pause();
  return 0;
}
