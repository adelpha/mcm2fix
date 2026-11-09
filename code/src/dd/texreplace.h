// Texture replacements (ini TextureReplace) and texture dumping (ini
// TextureDump).
//
// Every texture the game uploads is identified by a 64-bit hash of its top
// level (size and pixels). TextureDump=1 saves each one once as
//   %LOCALAPPDATA%\Motocross Madness 2\textures_dump\<hash>.png
// and a picture with the same name (PNG, JPEG or BMP, any size) placed in
//   <game folder>\textures   or   %LOCALAPPDATA%\Motocross Madness 2\textures
// is drawn in its place, with full mipmaps generated on the GPU.
//
// Textures the game loads by name (most of them: from the .RES archives or
// TGA files) are also known by that name. Those are dumped as <name>.png
// (largest size seen) and a picture called <name>.png etc. replaces the
// texture at every size the game streams it in. The name is lower case, its
// extension dropped and folder separators turned into '_'.
#pragma once
#include <cstdint>
#include <string>

#include "backend.h"

namespace dd {

uint64_t TextureHash(const uint8_t* bgra, UINT w, UINT h);

// True (and `out` filled) if a replacement picture exists for `hash`.
bool LoadReplacement(uint64_t hash, GpuTexture& out);

// Save the texture as PNG if TextureDump is on and it wasn't saved before.
void DumpTexture(uint64_t hash, const uint8_t* bgra, UINT w, UINT h);

// Texture names: fixes_textures.cpp sets the name of the texture the game is
// about to create; Surface::Create takes it for the next texture surface.
void SetPendingTextureName(const char* name);  // nullptr clears
std::string TakePendingTextureName();
// Name of the texture being created, for the next copy into a drawn surface.
void SetCopyTextureName(const char* name);  // nullptr clears
std::string TakeCopyTextureName();
std::string TextureKey(const char* name);  // "Riders\Rider01.tga" -> "riders_rider01"

bool LoadReplacementByName(const std::string& key, GpuTexture& out);
void DumpNamedTexture(const std::string& key, const uint8_t* bgra, UINT w, UINT h);

}  // namespace dd
