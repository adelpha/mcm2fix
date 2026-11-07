// Fixes found in step 1 (see docs/step1-launch-crash.md).
#include <windows.h>
#include <cstdint>

#include "config.h"
#include "patch.h"

namespace {

constexpr HRESULT kSurfaceLost = static_cast<HRESULT>(0x887601C2);  // DDERR_SURFACELOST
constexpr DWORD kFlipWait = 0x1;                                     // DDFLIP_WAIT

// IDirectDrawSurface7 vtable slots used here.
constexpr int kFlip = 0x2C / 4;
constexpr int kRestore = 0x6C / 4;

using FlipFn = HRESULT(WINAPI*)(void* self, void* target, DWORD flags);
using RestoreFn = HRESULT(WINAPI*)(void* self);

// Right after SetDisplayMode the game flips the primary ~12 times to time the
// refresh rate. On Windows 10/11 the mode change may not have settled yet and
// Flip returns DDERR_SURFACELOST, which the game treated as fatal.
extern "C" HRESULT __cdecl FlipWithRetry(void** surface_slot) {
  HRESULT hr = kSurfaceLost;
  for (int tries = 0; tries < 300; ++tries) {
    void* surf = *surface_slot;
    void** vtbl = *static_cast<void***>(surf);
    hr = reinterpret_cast<FlipFn>(vtbl[kFlip])(surf, nullptr, kFlipWait);
    if (hr != kSurfaceLost) return hr;
    reinterpret_cast<RestoreFn>(vtbl[kRestore])(surf);
    Sleep(10);
  }
  return hr;
}

// Called from the game with ebx = IDirectDrawSurface7** (the primary).
// Returns the HRESULT in eax; preserves ebx, esi, edi, ebp.
#if defined(_MSC_VER)
__declspec(naked) void FlipThunk() {
  __asm {
    push ebx
    call FlipWithRetry
    add esp, 4
    ret
  }
}
#else
extern "C" void FlipThunk();
asm(".text\n"
    ".globl _FlipThunk\n"
    "_FlipThunk:\n"
    "  pushl %ebx\n"
    "  call _FlipWithRetry\n"
    "  addl $4, %esp\n"
    "  ret\n");
#endif

void ApplyFlipRetry() {
  // mov eax,[ebx]; push 1; push 0; (push eax; mov ecx,[eax] | mov edx,[eax]; push eax);
  // call [reg+2c]  -> call FlipThunk
  static const uint8_t site_a[] = {0x8b, 0x03, 0x6a, 0x01, 0x6a, 0x00,
                                   0x50, 0x8b, 0x08, 0xff, 0x51, 0x2c};
  static const uint8_t site_b[] = {0x8b, 0x03, 0x6a, 0x01, 0x6a, 0x00,
                                   0x8b, 0x10, 0x50, 0xff, 0x52, 0x2c};
  const struct { uintptr_t addr; const uint8_t* bytes; } sites[] = {
      {0x4CE7D4, site_a}, {0x4CE802, site_b}, {0x4CE814, site_a}};
  for (const auto& s : sites) {
    if (!patch::Matches(s.addr, s.bytes, 12)) {
      patch::Log("flip-retry: unexpected bytes at %08x, skipped", static_cast<unsigned>(s.addr));
      continue;
    }
    patch::Call(s.addr, reinterpret_cast<const void*>(&FlipThunk), 12);
  }
  patch::Log("flip-retry: done");
}

// The startup "administrator" probe writes HKLM\...\IsAdmin and a temp file
// in the current directory; it fails for standard users. Skip it:
// 4a1f14: cmp eax,2 ; jne 4a2005  ->  jmp 4a2005
void ApplyNoAdmin() {
  static const uint8_t original[] = {0x83, 0xf8, 0x02, 0x0f, 0x85, 0xe8, 0x00, 0x00, 0x00};
  if (!patch::Matches(0x4A1F14, original, sizeof(original))) {
    patch::Log("no-admin: unexpected bytes, skipped");
    return;
  }
  patch::Jump(0x4A1F14, 0x4A2005, sizeof(original));
  patch::Log("no-admin: done");
}

}  // namespace

void ApplyStep1Fixes() {
  // Interim: the game races between its threads on multi-core CPUs. Pinned to
  // one core it starts and plays its movies. To be replaced by a real fix.
  if (GetConfig().one_core) {
    SetProcessAffinityMask(GetCurrentProcess(), 1);
    patch::Log("one-core: pinned to CPU 0");
  } else {
    patch::Log("one-core: off (mcm2fix.ini OneCore=0)");
  }
  ApplyFlipRetry();
  ApplyNoAdmin();
}
