// Performance profiling (ini Profile=1): per-frame timing of the game's tick
// and the renderer, frame-time statistics, GPU time, and a sampling profiler
// of the game thread. See prof.cpp for what is written where.
#pragma once
#include <cstdint>

namespace prof {

// Timed sections. Each one adds its inclusive time and call count to the
// frame it runs in (game thread only).
enum Zone : int {
  kTick,          // FixedTick: the game's whole tick
  kSim,           // one fixed simulation step (or the menu's single update)
  kSimControl,    //   ControlInterface update (input)
  kSimManager,    //   manager update
  kSimState,      //   world state update (physics, AI, ...)
  kDraw,          // game Draw (vfunc 0x20): scene traversal and our draw calls
  kSmoothBegin,   //   SimSmooth: rebuild blended objects before drawing
  kSmoothEnd,     //   SimSmooth: put them back after
  kGamePresent,   // game Present (vfunc 0x24), includes Flip
  kPostFrame,     // the game's post-frame calls
  kFlip,          // our Flip: everything from the game's Flip call
  kCapWait,       //   FrameCap sleep
  kHudFlush,      //   HUD text flush
  kCompose,       //   picture to the window: scale blit, GPU text, label
  kSwapPresent,   //   IDXGISwapChain::Present / IDirect3DSwapChain9::Present (vsync wait shows here)
  kSubmit,        // one draw call: everything in our device
  kVertexPrep,    //   convert vertices to our format and copy them to the GPU
  kStateApply,    //   bind target, translate and set render state
  kDriverDraw,    //   the Draw / DrawIndexed / Draw*PrimitiveUP call itself
  kSmoothVerts,   // SimSmooth vertex blending for a draw
  kUpload,        // CPU copy of a surface sent to its GPU texture
  kReadback,      // GPU texture read back to the CPU copy
  kKeyed,         // colour-keyed texture copy built
  kBlt,           // Blt / BltFast
  kLock,          // Lock (may read back)
  kGetDC,         // GetDC .. ReleaseDC
  kClear,         // Device::Clear
  kGdiText,       // the game's TextOut / DrawText calls (GPU text or GDI)
  kZoneCount
};

enum Counter : int {
  cSteps,        // simulation steps this frame
  cDraws,
  cDrawsTL,      // screen-space draws (HUD, 2D)
  cDrawsIndexed,
  cVerts,
  cIndices,
  cTexChanges,   // stage 0 texture different from the previous draw
  cStateChanges, // blend/depth/raster object different from the previous draw
  cTargetChanges,
  cUploads,
  cUploadKB,
  cReadbacks,
  cLocks,
  cBlts,
  cGetDCs,
  cClears,
  cTexCreates,   // GPU textures created
  cCounterCount
};

bool On();

void ZoneEnter(Zone z, int64_t* start, int* prev);
void ZoneLeave(Zone z, int64_t start, int prev);

struct Scope {
  explicit Scope(Zone z) : z_(z) {
    if (On()) ZoneEnter(z, &start_, &prev_);
  }
  ~Scope() {
    if (start_) ZoneLeave(z_, start_, prev_);
  }
  // End this section and start another one.
  void Switch(Zone z) {
    if (start_) ZoneLeave(z_, start_, prev_);
    start_ = 0;
    z_ = z;
    if (On()) ZoneEnter(z, &start_, &prev_);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Zone z_;
  int64_t start_ = 0;
  int prev_ = -1;
};

void Count(Counter c, unsigned n = 1);

// Set by the device: the texture / state objects of this draw, to count
// changes between draws.
void NoteDrawState(const void* tex0, const void* blend, const void* depth, const void* raster);
void NoteTarget(const void* rtv);

// Called once per displayed frame, after the picture was handed to the
// swap chain. Inside the game's tick the frame is closed at the end of the
// tick instead (TickEnd), so all of the tick's work counts for that frame.
void FrameEnd();
void TickEnd();

// GPU time of a frame (measured a few frames late).
void GpuFrameTime(double ms);

// Start (at DLL load, on the game's main thread).
void Start();
void Stop();

}  // namespace prof

#define PROF_ZONE(z) ::prof::Scope prof_scope_##z(::prof::z)
