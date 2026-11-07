// Minimal Direct3D 7 declarations, so the code builds with current Windows
// SDKs (which no longer ship d3d.h) as well as MinGW. Only what mcm2fix
// uses; layouts follow the documented DirectX 7 ABI.
#pragma once
#include <windows.h>

namespace d7 {

// Opaque interface types (methods are reached through their vtables).
struct Direct3D7;
struct Device7;
struct VertexBuffer7;

typedef DWORD Color;
typedef float Value;
typedef int PrimitiveType;   // D3DPRIMITIVETYPE
typedef int RenderState;     // D3DRENDERSTATETYPE
typedef int StageState;      // D3DTEXTURESTAGESTATETYPE
typedef int TransformState;  // D3DTRANSFORMSTATETYPE

struct Rect {
  LONG x1, y1, x2, y2;
};
struct Matrix {
  float m[4][4];
};
struct Viewport7 {
  DWORD dwX, dwY, dwWidth, dwHeight;
  float dvMinZ, dvMaxZ;
};
struct VertexBufferDesc {
  DWORD dwSize, dwCaps, dwFVF, dwNumVertices;
};

// {F5049E77-4861-11D2-A407-00A0C90629A8}
constexpr GUID kIID_IDirect3D7 = {0xf5049e77, 0x4861, 0x11d2,
                                  {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};

}  // namespace d7
