// Direct3D 9 pieces shared by backend9.cpp, device9.cpp and msaa.cpp.
//
// d3d9.h defines D3DFVF_*, D3DCLEAR_* and D3DPV_* as macros, which clash
// with the Direct3D 7 constants in d3d7.h; they are undefined here, so code
// that includes this header uses the k* values below for Direct3D 9.
#pragma once
#include <d3d9.h>

#undef D3DFVF_POSITION_MASK
#undef D3DFVF_XYZ
#undef D3DFVF_XYZRHW
#undef D3DFVF_XYZB1
#undef D3DFVF_XYZB2
#undef D3DFVF_XYZB3
#undef D3DFVF_XYZB4
#undef D3DFVF_XYZB5
#undef D3DFVF_NORMAL
#undef D3DFVF_RESERVED1
#undef D3DFVF_DIFFUSE
#undef D3DFVF_SPECULAR
#undef D3DFVF_TEXCOUNT_MASK
#undef D3DFVF_TEXCOUNT_SHIFT
#undef D3DCLEAR_TARGET
#undef D3DCLEAR_ZBUFFER
#undef D3DCLEAR_STENCIL
#undef D3DPV_DONOTCOPYDATA
#undef D3D_OK

#include "backend.h"

namespace dd {

// Direct3D 9 values of the undefined macros.
constexpr DWORD kFvf9XYZRHW = 0x004, kFvf9XYZB1 = 0x006, kFvf9Diffuse = 0x040,
                kFvf9Specular = 0x080, kFvf9Tex1 = 0x100, kFvf9Tex2 = 0x200;
constexpr DWORD kClear9Target = 1, kClear9ZBuffer = 2, kClear9Stencil = 4;

// Cached state setters, so redundant changes are skipped (shared by the
// blitter and the device).
void RS9(D3DRENDERSTATETYPE s, DWORD v);
void TSS9(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v);
void SS9(DWORD stage, D3DSAMPLERSTATETYPE t, DWORD v);
void Tex9(DWORD stage, IDirect3DBaseTexture9* t);
// Forget the cached state (after a device reset).
void InvalidateState9();
// Device resets so far (cached state older than a reset is stale).
unsigned ResetCount9();
const D3DCAPS9& Caps9();
// 1x1 white texture (stages that use a texture but have none).
IDirect3DTexture9* WhiteTex9();
// Direct3D 9 only draws between BeginScene and EndScene (Wine does not
// check, real drivers return D3DERR_INVALIDCALL). Draws open a scene when
// none is open; copies, GetDC, readbacks, Reset and Present close it.
void BeginScene9();
void EndScene9();

}  // namespace dd
