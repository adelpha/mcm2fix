# Step 3: Direct3D 11 renderer

Mcm2.exe draws through DirectDraw 7 and Direct3D 7 (immediate mode, from
`DirectDrawCreateEx`). mcm2fix.dll points that import (IAT 0x5553EC) at its
own implementation in `code/src/dd/`, which draws with Direct3D 11.

## Layout

| File | Role |
|---|---|
| `backend.*` | D3D11 device on the high-performance GPU, borderless window + flip-model swap chain, 2D blitter, present with aspect-correct scaling, stats (`DDStats=1`). |
| `directdraw.*` | `IDirectDraw7` and `IDirect3D7`. Display modes are virtual: `SetDisplayMode` only changes the size of the game's surfaces. |
| `surface.*` | `IDirectDrawSurface7`, palette, clipper, gamma. Each surface has a CPU copy in the game's pixel format and a BGRA8 texture; whichever was written last is the newest, and copies only happen when the other side is needed. |
| `device.*` | `IDirect3DDevice7` and vertex buffers: one uber-shader for the fixed-function pipeline (two texture stages, fog, alpha test, specular, colour key). |
| `hook.cpp` | Installs the IAT hook and the two intro-video patches. |

## Things worth knowing

- **GDI text.** The game draws HUD and menu text with `GetDC` on the 3D back
  buffer every frame. Render targets are created `GDI_COMPATIBLE`, and
  `GetDC` uses `IDXGISurface1::GetDC` on the GPU texture so nothing is read
  back (reading back 1280x1024 per frame halved the framerate).
- **Half-pixel offset.** D3D7 samples at integer pixel positions; the vertex
  shader shifts by half a pixel.
- **Foliage.** Trees and flags are drawn blended, with depth writes and alpha
  test `GREATER 0`. Filtered edge pixels then hide whatever is drawn behind
  them later (a stair-step fringe, also in the original game). For those
  draws the alpha-test threshold is raised to `AlphaCut` (default 128). The
  terrain detail layer has alpha test off and is not affected.
- **Transparent texels** get the average colour of their visible neighbours
  on upload, so filtering and mip levels do not mix grey into leaf edges.
- **Intro videos** use DirectShow amstream with Windows' own DirectDraw (an
  `IDirectDraw` v1 query returns the system DirectDraw in normal mode). The
  game's stream is switched to 16-bit (`GetFormat` patch at 0x4A3C28) and its
  frame wait (0x4A3DE0) falls back to a synchronous `Update` when the
  asynchronous one does not complete. Each frame is copied into our surface.

## Later

- Decode the Indeo videos ourselves and drop DirectShow, amstream, the system
  DirectDraw and the Indeo codec (Windows 11 does not ship it).
- Draw text on the GPU instead of through GDI.
