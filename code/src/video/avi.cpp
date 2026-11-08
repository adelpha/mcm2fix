#include "avi.h"

#include <cstring>

namespace video {
namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

uint32_t Rd32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

int Digit(uint8_t c) { return c >= '0' && c <= '9' ? c - '0' : -1; }

}  // namespace

AviFile::~AviFile() { Close(); }

void AviFile::Close() {
  if (base_) UnmapViewOfFile(base_);
  if (map_) CloseHandle(map_);
  if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
  base_ = nullptr;
  map_ = nullptr;
  file_ = INVALID_HANDLE_VALUE;
}

bool AviFile::Open(const char* path) {
  file_ = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file_ == INVALID_HANDLE_VALUE) {
    error = "cannot open file";
    return false;
  }
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(file_, &sz) || sz.QuadPart < 12 || sz.QuadPart > 0x7fffffff) {
    error = "bad file size";
    return false;
  }
  size_ = static_cast<size_t>(sz.QuadPart);
  map_ = CreateFileMappingA(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (map_) base_ = static_cast<const uint8_t*>(MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0));
  if (!base_) {
    error = "cannot map file";
    return false;
  }
  if (Rd32(base_) != FourCC('R', 'I', 'F', 'F') || Rd32(base_ + 8) != FourCC('A', 'V', 'I', ' ')) {
    error = "not an AVI file";
    return false;
  }
  uint32_t riff = Rd32(base_ + 4);
  const uint8_t* end = base_ + (riff + 8 < size_ ? riff + 8 : size_);
  if (!ParseList(base_ + 12, end, 0)) return false;
  if (video_stream_ < 0 || !width || !height || frames.empty() || !rate) {
    error = "no video stream";
    return false;
  }
  return true;
}

bool AviFile::ParseList(const uint8_t* p, const uint8_t* end, int depth) {
  while (p + 8 <= end) {
    uint32_t id = Rd32(p), sz = Rd32(p + 4);
    const uint8_t* body = p + 8;
    if (sz > static_cast<size_t>(end - body)) sz = static_cast<uint32_t>(end - body);
    if (id == FourCC('L', 'I', 'S', 'T') && sz >= 4) {
      uint32_t type = Rd32(body);
      if (type == FourCC('m', 'o', 'v', 'i')) {
        if (!ParseMovi(body + 4, body + sz)) return false;
      } else if (depth < 4) {
        if (type == FourCC('s', 't', 'r', 'l')) {
          ++stream_;
          stream_type_ = 0;
        }
        if (!ParseList(body + 4, body + sz, depth + 1)) return false;
      }
    } else if (id == FourCC('s', 't', 'r', 'h') && sz >= 32) {
      stream_type_ = Rd32(body);
      if (stream_type_ == FourCC('v', 'i', 'd', 's') && video_stream_ < 0) {
        video_stream_ = stream_;
        fourcc = Rd32(body + 4);
        scale = Rd32(body + 20);
        rate = Rd32(body + 24);
        if (!scale) scale = 1;
      } else if (stream_type_ == FourCC('a', 'u', 'd', 's') && audio_stream_ < 0) {
        audio_stream_ = stream_;
      }
    } else if (id == FourCC('s', 't', 'r', 'f')) {
      if (stream_ == video_stream_ && sz >= 40) {
        width = Rd32(body + 4);
        height = Rd32(body + 8);
        uint32_t comp = Rd32(body + 16);
        if (comp) fourcc = comp;
      } else if (stream_ == audio_stream_ && sz >= 16) {
        audio_format.assign(body, body + sz);
      }
    }
    p = body + sz + (sz & 1);
  }
  return true;
}

bool AviFile::ParseMovi(const uint8_t* p, const uint8_t* end) {
  while (p + 8 <= end) {
    uint32_t id = Rd32(p), sz = Rd32(p + 4);
    const uint8_t* body = p + 8;
    if (sz > static_cast<size_t>(end - body)) sz = static_cast<uint32_t>(end - body);
    if (id == FourCC('L', 'I', 'S', 'T') && sz >= 4) {
      ParseMovi(body + 4, body + sz);  // 'rec ' groups
    } else {
      int d0 = Digit(static_cast<uint8_t>(id)), d1 = Digit(static_cast<uint8_t>(id >> 8));
      if (d0 >= 0 && d1 >= 0) {
        int stream = d0 * 10 + d1;
        if (stream == video_stream_) {
          frames.push_back({body, sz});  // empty chunks repeat the previous frame
        } else if (stream == audio_stream_ && sz) {
          audio.push_back({body, sz});
        }
      }
    }
    p = body + sz + (sz & 1);
  }
  return true;
}

}  // namespace video
