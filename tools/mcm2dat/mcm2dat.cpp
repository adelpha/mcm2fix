// mcm2dat: unpacks Motocross Madness 2's UI\*.DAT archives.
//
// Each menu page is one "RS2" archive: the page's sprites (16-bit .tga), its
// layout (.dtm, INI-like text from a Visual Basic form designer) and
// sometimes a .wav. This copies every file out unchanged and also saves each
// .tga as a .png next to it, so any image viewer opens them.
//
//   mcm2dat <file.DAT or folder> [more...] [-o <output folder>]
//
// Drag a .DAT file or the whole UI folder onto mcm2dat.exe. Output goes to
// <output folder>\<DAT name>\<path inside the archive>; the default output
// folder is "mcm2dat-out" next to mcm2dat.exe. Files that are not RS2
// archives are skipped. The game's files are only read.
//
// Archive layout: "RS2\0", u32 count, then per file
// { u32 name length (with its NUL), name, u32 offset, u32 size }.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_TGA
#include "../mcm2sky/third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../mcm2sky/third_party/stb_image_write.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool ReadFileAll(const std::string& path, std::vector<unsigned char>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  bool ok = n >= 0;
  if (ok) {
    out.resize(static_cast<size_t>(n));
    ok = n == 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
  }
  std::fclose(f);
  return ok;
}

bool WriteFileAll(const std::string& path, const unsigned char* data, size_t n) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = n == 0 || std::fwrite(data, 1, n, f) == n;
  return std::fclose(f) == 0 && ok;
}

void MakeDirs(const std::string& path) {  // every folder in path (not the last part)
  for (size_t i = 0; i < path.size(); ++i)
    if ((path[i] == '\\' || path[i] == '/') && i > 2) CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
}

std::string BaseName(const std::string& p) {
  const size_t s = p.find_last_of("\\/");
  std::string b = s == std::string::npos ? p : p.substr(s + 1);
  const size_t dot = b.rfind('.');
  return dot == std::string::npos ? b : b.substr(0, dot);
}

bool EndsWithNoCase(const std::string& s, const char* tail) {
  const size_t m = std::strlen(tail);
  return s.size() >= m && _stricmp(s.c_str() + s.size() - m, tail) == 0;
}

uint32_t Get32(const std::vector<unsigned char>& d, size_t at) {
  uint32_t v;
  std::memcpy(&v, d.data() + at, 4);
  return v;
}

// Keeps a name inside the output folder: no drive, no "..", no leading slash.
std::string SafeRelative(std::string name) {
  for (char& c : name)
    if (c == '/') c = '\\';
  std::string out, part;
  for (size_t i = 0; i <= name.size(); ++i) {
    if (i == name.size() || name[i] == '\\') {
      if (!part.empty() && part != "." && part != ".." && part.find(':') == std::string::npos) {
        if (!out.empty()) out += '\\';
        out += part;
      }
      part.clear();
    } else {
      part += name[i];
    }
  }
  return out;
}

int g_files = 0, g_pngs = 0, g_errors = 0;

void Unpack(const std::string& dat, const std::string& out_root) {
  std::vector<unsigned char> d;
  if (!ReadFileAll(dat, d)) {
    std::printf("  cannot read %s\n", dat.c_str());
    ++g_errors;
    return;
  }
  if (d.size() < 8 || std::memcmp(d.data(), "RS2", 4) != 0) {
    std::printf("  %s: not an RS2 archive, skipped\n", dat.c_str());
    return;
  }
  const uint32_t count = Get32(d, 4);
  const std::string dir = out_root + "\\" + BaseName(dat);
  std::printf("%s: %u files -> %s\n", dat.c_str(), count, dir.c_str());
  std::string index;
  size_t p = 8;
  for (uint32_t i = 0; i < count; ++i) {
    if (p + 4 > d.size()) break;
    const uint32_t len = Get32(d, p);
    p += 4;
    if (len == 0 || len > 1024 || p + len + 8 > d.size()) {
      std::printf("  bad directory entry %u, stopping\n", i);
      ++g_errors;
      break;
    }
    const std::string name(reinterpret_cast<const char*>(d.data() + p), strnlen(reinterpret_cast<const char*>(d.data() + p), len));
    p += len;
    const uint32_t off = Get32(d, p), size = Get32(d, p + 4);
    p += 8;
    char line[1200];
    std::snprintf(line, sizeof(line), "%-48s offset %8u  size %8u\r\n", name.c_str(), off, size);
    index += line;
    if (off > d.size() || size > d.size() - off) {
      std::printf("  %s: out of range, skipped\n", name.c_str());
      ++g_errors;
      continue;
    }
    const std::string rel = SafeRelative(name);
    if (rel.empty()) continue;
    const std::string path = dir + "\\" + rel;
    MakeDirs(path);
    if (!WriteFileAll(path, d.data() + off, size)) {
      std::printf("  cannot write %s\n", path.c_str());
      ++g_errors;
      continue;
    }
    ++g_files;
    if (EndsWithNoCase(name, ".tga")) {
      int w = 0, h = 0, n = 0;
      unsigned char* px = stbi_load_from_memory(d.data() + off, static_cast<int>(size), &w, &h, &n, 0);
      if (!px) {
        std::printf("  %s: could not decode (%s)\n", name.c_str(), stbi_failure_reason());
        ++g_errors;
        continue;
      }
      const std::string png = path.substr(0, path.size() - 4) + ".png";
      if (stbi_write_png(png.c_str(), w, h, n, px, w * n)) ++g_pngs;
      else {
        std::printf("  cannot write %s\n", png.c_str());
        ++g_errors;
      }
      stbi_image_free(px);
    }
  }
  MakeDirs(dir + "\\x");
  WriteFileAll(dir + "\\_contents.txt", reinterpret_cast<const unsigned char*>(index.data()), index.size());
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> inputs;
  std::string out;
  for (int i = 1; i < argc; ++i) {
    if ((!std::strcmp(argv[i], "-o") || !std::strcmp(argv[i], "/o")) && i + 1 < argc) out = argv[++i];
    else inputs.push_back(argv[i]);
  }
  if (inputs.empty()) {
    std::printf("mcm2dat: unpacks Motocross Madness 2 UI\\*.DAT archives (images also saved as PNG)\n\n"
                "  mcm2dat <file.DAT or folder> [more...] [-o <output folder>]\n\n"
                "Or drag a .DAT file or the UI folder onto mcm2dat.exe.\n"
                "Default output: the mcm2dat-out folder next to mcm2dat.exe.\n\nPress Enter to close.");
    std::getchar();
    return 1;
  }
  if (out.empty()) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string e = exe;
    out = e.substr(0, e.find_last_of('\\')) + "\\mcm2dat-out";
  }
  while (!out.empty() && (out.back() == '\\' || out.back() == '/')) out.pop_back();
  MakeDirs(out + "\\x");
  for (const std::string& in : inputs) {
    const DWORD attr = GetFileAttributesA(in.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
      std::printf("%s: not found\n", in.c_str());
      ++g_errors;
    } else if (attr & FILE_ATTRIBUTE_DIRECTORY) {
      WIN32_FIND_DATAA fd;
      HANDLE h = FindFirstFileA((in + "\\*.dat").c_str(), &fd);
      if (h == INVALID_HANDLE_VALUE) {
        std::printf("%s: no .DAT files\n", in.c_str());
        continue;
      }
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) Unpack(in + "\\" + fd.cFileName, out);
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    } else {
      Unpack(in, out);
    }
  }
  std::printf("\nDone: %d files, %d PNGs, %d problems. Output: %s\nPress Enter to close.", g_files, g_pngs, g_errors,
              out.c_str());
  std::getchar();
  return g_errors ? 2 : 0;
}
