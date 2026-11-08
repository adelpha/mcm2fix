// View distance and model level of detail (ini ViewDistance, ModelLod).
//
// View distance: the game's Fog object (vtable 0x557794, built at
// 0x4e3c56) keeps a far distance between 256 (worst) and 768 (best) world
// units, set by the detail level (the +/- detail keys, 0x463d00), with the
// fog starting 128 units before it. The camera's far plane (and so the
// frustum culling) and the fog both come from it. ViewDistance scales those
// three numbers where the object is created.
//
// Objects (trees, fences, buildings) have their own draw distance per
// detail level: the int at +0xc of each 32-byte row of the tables at
// 0x5707e8 (hardware) and 0x5706a8 (software), 768 at the best level. The
// object pass (0x45c5ef) skips anything further away. ViewDistance scales
// those as well.
//
// Tree detail: vegetation within the first two ints of the same rows
// (150 / 75 at the best level) is drawn with its near, full-resolution
// version, blended out between the two; beyond, it is a dense low-res
// billboard (0x457650). TreeDetailDistance scales both.
//
// Texture detail: the game streams each texture at a resolution chosen from
// how big it is on screen (0x444efe computes log2 of the wanted size and
// passes it to 0x515f90, capped at 9 = 512 px); far objects such as trees
// get small versions, which MipBias can't sharpen. TextureLod adds that
// many levels to every request (1 = twice the resolution).
//
// Model LOD: each model picks a level by its projected size times
// 1/factor (0x445d00, factor from the detail table at 0x57a548: 1.0 at the
// best detail). ModelLod multiplies that, so a model keeps its detailed
// version ModelLod times further away.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "config.h"
#include "patch.h"

namespace {

constexpr uintptr_t kFogFar = 0x4E3C34;    // push 768.0f (imm32 of 68 xx xx xx xx at 0x4e3c33)
constexpr uintptr_t kFogNear = 0x4E3C2F;   // push 256.0f
constexpr uintptr_t kFogStart = 0x4E3C2A;  // push 128.0f
constexpr uintptr_t kLodScale = 0x445D00;  // fld dword [0x555748] (1.0f)

float g_model_lod = 1.0f;
float g_texture_lod = 0.0f;

constexpr uintptr_t kTextureRequest = 0x515F90;
constexpr uintptr_t kTextureRequestResume = 0x515F9A;
const uint8_t kTextureRequestEntry[] = {0xD9, 0x44, 0x24, 0x04,                // fld [esp+4]
                                        0xD8, 0x1D, 0x08, 0xD8, 0x55, 0x00};  // fcomp [0x55d808]

void ApplyTextureLod(float levels) {
  if (!patch::Matches(kTextureRequest, kTextureRequestEntry, sizeof(kTextureRequestEntry))) {
    patch::Log("texture lod: code differs, not applied");
    return;
  }
  g_texture_lod = levels;
  auto* t = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return;
  size_t n = 0;
  auto put = [&](std::initializer_list<uint8_t> b) {
    for (uint8_t x : b) t[n++] = x;
  };
  auto put32 = [&](uint32_t v) {
    std::memcpy(t + n, &v, 4);
    n += 4;
  };
  put({0xD9, 0x44, 0x24, 0x04});  // fld [esp+4]
  put({0xD8, 0x05});              // fadd [g_texture_lod]
  put32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_texture_lod)));
  put({0xD9, 0x5C, 0x24, 0x04});  // fstp [esp+4]
  for (uint8_t b : kTextureRequestEntry) put({b});
  put({0x68});
  put32(static_cast<uint32_t>(kTextureRequestResume));  // push resume
  put({0xC3});                                          // ret
  FlushInstructionCache(GetCurrentProcess(), t, n);
  patch::Jump(kTextureRequest, reinterpret_cast<uintptr_t>(t), sizeof(kTextureRequestEntry));
  patch::Log("texture lod +%.2f", levels);
}

constexpr uintptr_t kObjectTables[] = {0x5707E8, 0x5706A8};
constexpr int kObjectRange[2][10] = {{256, 256, 320, 384, 448, 512, 576, 640, 768, 768},
                                     {256, 256, 256, 384, 384, 448, 512, 512, 640, 768}};

void ScaleTreeDetail(float scale) {
  for (int t = 0; t < 2; ++t) {
    for (int i = 0; i < 10; ++i) {
      for (int f = 0; f < 2; ++f) {
        int* p = reinterpret_cast<int*>(kObjectTables[t] + i * 32 + f * 4);
        const int v = static_cast<int>(*p * scale + 0.5f);
        patch::Write(reinterpret_cast<uintptr_t>(p), &v, 4);
      }
    }
  }
  patch::Log("tree detail distance x%.2f", scale);
}

void ScaleObjectRanges(float scale) {
  for (int t = 0; t < 2; ++t) {
    for (int i = 0; i < 10; ++i) {
      const uintptr_t at = kObjectTables[t] + i * 32 + 0xC;
      const int original = kObjectRange[t][i];
      if (!patch::Matches(at, &original, 4)) {
        patch::Log("object distance: table %d row %d differs, not applied", t, i);
        return;
      }
    }
    for (int i = 0; i < 10; ++i) {
      const int v = static_cast<int>(kObjectRange[t][i] * scale + 0.5f);
      patch::Write(kObjectTables[t] + i * 32 + 0xC, &v, 4);
    }
  }
}

void ScaleImmediate(uintptr_t at, float original, float scale) {
  uint32_t want;
  std::memcpy(&want, &original, 4);
  if (!patch::Matches(at, &want, 4)) {
    patch::Log("view distance: code at %08x differs, not applied", static_cast<unsigned>(at));
    return;
  }
  float v = original * scale;
  patch::Write(at, &v, 4);
}

}  // namespace

void ApplyDetailFixes() {
  const Config& c = GetConfig();
  if (c.view_distance != 1.0f) {
    ScaleImmediate(kFogFar, 768.0f, c.view_distance);
    ScaleImmediate(kFogNear, 256.0f, c.view_distance);
    ScaleImmediate(kFogStart, 128.0f, c.view_distance);
    ScaleObjectRanges(c.view_distance);
    patch::Log("view distance x%.2f", c.view_distance);
  }
  if (c.tree_detail != 1.0f) ScaleTreeDetail(c.tree_detail);
  if (c.texture_lod != 0.0f) ApplyTextureLod(c.texture_lod);
  if (c.model_lod != 1.0f) {
    const uint8_t original[] = {0xD9, 0x05, 0x48, 0x57, 0x55, 0x00};  // fld dword [0x555748]
    if (!patch::Matches(kLodScale, original, sizeof(original))) {
      patch::Log("model lod: code differs, not applied");
    } else {
      g_model_lod = c.model_lod;
      uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_model_lod));
      patch::Write(kLodScale + 2, &addr, 4);
      patch::Log("model lod x%.2f", c.model_lod);
    }
  }
}
