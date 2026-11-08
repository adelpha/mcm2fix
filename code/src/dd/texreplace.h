// Texture replacements (ini TextureReplace) and texture dumping (ini
// TextureDump).
//
// Every texture the game uploads is identified by a 64-bit hash of its top
// level (size and pixels). TextureDump=1 saves each one once as
//   %LOCALAPPDATA%\Motocross Madness 2\textures_dump\<hash>.png
// and a picture with the same name (PNG, JPEG or BMP, any size) placed in
//   <game folder>\textures   or   %LOCALAPPDATA%\Motocross Madness 2\textures
// is drawn in its place, with full mipmaps generated on the GPU.
#pragma once
#include <cstdint>

#include "backend.h"

namespace dd {

uint64_t TextureHash(const uint8_t* bgra, UINT w, UINT h);

// True (and `out` filled) if a replacement picture exists for `hash`.
bool LoadReplacement(uint64_t hash, GpuTexture& out);

// Save the texture as PNG if TextureDump is on and it wasn't saved before.
void DumpTexture(uint64_t hash, const uint8_t* bgra, UINT w, UINT h);

}  // namespace dd
