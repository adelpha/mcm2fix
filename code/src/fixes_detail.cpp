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
// Model LOD: each model picks a level by its projected size times
// 1/factor (0x445d00, factor from the detail table at 0x57a548: 1.0 at the
// best detail). ModelLod multiplies that, so a model keeps its detailed
// version ModelLod times further away.
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "config.h"
#include "patch.h"

namespace {

constexpr uintptr_t kFogFar = 0x4E3C34;    // push 768.0f (imm32 of 68 xx xx xx xx at 0x4e3c33)
constexpr uintptr_t kFogNear = 0x4E3C2F;   // push 256.0f
constexpr uintptr_t kFogStart = 0x4E3C2A;  // push 128.0f
constexpr uintptr_t kLodScale = 0x445D00;  // fld dword [0x555748] (1.0f)

float g_model_lod = 1.0f;

constexpr uintptr_t kObjectTables[] = {0x5707E8, 0x5706A8};
constexpr int kObjectRange[2][10] = {{256, 256, 320, 384, 448, 512, 576, 640, 768, 768},
                                     {256, 256, 256, 384, 384, 448, 512, 512, 640, 768}};

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
