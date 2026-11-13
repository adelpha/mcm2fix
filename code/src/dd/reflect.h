// Reflections on bikes and helmets (ini Reflections, Direct3D 11 only).
// See reflect.cpp.
#pragma once
#include <d3d11.h>
#include <windows.h>

#include <cstdint>
#include <string>

namespace dd {

// One 3D draw of the frame as the device sent it, kept so the live
// reflection cube (Reflections=2) can draw it again from the rider's place.
// The device's dynamic vertex/index buffers are reused, so a draw is only
// replayed while those were not started over (vgen/igen).
struct ReflectReplay {
  ID3D11BlendState* blend;
  ID3D11DepthStencilState* depth;
  ID3D11ShaderResourceView* srv[2];  // AddRef'd while kept
  ID3D11SamplerState* smp[3];
  ID3D11VertexShader* vs;
  ID3D11PixelShader* ps;
  float world[16];
  uint8_t pc[160];  // the pixel shader constants as drawn
  UINT pc_size;
  D3D11_PRIMITIVE_TOPOLOGY topo;
  bool indexed;
  UINT nidx, ioff, count;
  INT base_vertex;
  unsigned vgen, igen;
};

// Settings (read from mcm2fix.ini once).
int ReflectMode();  // 0 off, 1 sky cube, 2 live cube
// The game's sky box tiles are textures named <sky>_<offset>, sky one of
// the SKIES.RES cubes (terraNN, burnNN, windNN).
bool IsSkyTextureName(const std::string& n);
// Bike and rider draws come from one game function (0x441ed0); the hook
// says which object is drawing (nullptr outside).
const void* ReflectCurrentObject();
// Install the game hooks (object draw, part transform); once.
void ReflectInstallHooks();
// A device is going away: drop the draw cache (it holds the device's states).
void ReflectForget();

}  // namespace dd
