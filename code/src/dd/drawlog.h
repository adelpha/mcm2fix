// DrawLog=1 (debugging): measures how the game's draw calls could be merged.
// Every draw during a race is recorded with the game code that asked for it
// and a fingerprint of all the state it is drawn with. Consecutive draws with
// the same fingerprint and a mergeable shape (indexed or plain triangle lists
// and fans of untransformed vertices) are what a batching layer could send as
// one draw. Every 5 seconds the log gets a summary per call site; a few times
// a run, one whole frame is listed draw by draw.
#pragma once
#include <cstdint>

namespace dd {

class Surface;

struct DrawLogRec {
  uint8_t api = 0;       // 0 DrawPrimitive, 1 DrawIndexedPrimitive, 2 *Strided, 3 *VB
  uint8_t prim = 0;      // D3DPRIMITIVETYPE
  bool tl = false;       // screen-space vertices
  bool clip = false;     // ProcessVertices output (clip space)
  uint16_t stride = 0;
  uint32_t fvf_sig = 0;  // layout signature (stride, normal/colour/uv offsets)
  uint32_t count = 0;    // vertices
  uint32_t nidx = 0;     // indices (0 = not indexed)
  const Surface* tex0 = nullptr;
  const Surface* tex1 = nullptr;
  uint64_t state = 0;    // render states, stage states, textures, matrices, viewport, target
  uint32_t matrices = 0; // world, view, projection and viewport alone
};

bool DrawLogOn();
// From Device::Submit, before anything is drawn.
void DrawLogNote(const DrawLogRec& r);
// After each displayed frame.
void DrawLogFrameEnd();

}  // namespace dd
