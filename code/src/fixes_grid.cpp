// Crash on a track's edge (Iron Mountain, 2026-11-13: access violation at
// 0x48546f, turning a tight corner high up on the map).
//
// A terrain query (ret 18h, 6 arguments; on the stack below 0x483286, 0x482db2,
// 0x50245b, 0x503593, 0x5036ca, 0x50cc44 in the crash) walks the segments of a line
// across the terrain. For each segment midpoint it works out the 16 x 16
// terrain patch it lies in: col/row = fistp(mid / (2 * cell) - 0.5) into
// [ebp-14h] / [ebp-0Ch], then looks the patch up through a row table
// (0x57232c: row * 16, or 0x572370: row * 17 for the vertex grid). The
// game only turns 16 into 15; a midpoint just outside the map (x or z a
// hair below 0) gives -1, the lookup reads before the patch array and the
// game reads through a garbage pointer.
//
// Both lookups (0x485440 and 0x485609) now clamp col and row to 0..15,
// which is what the game's other patch lookups do (e.g. 0x48490e). Points
// on the map give the same indices as before. Each clamp outside the
// original 16 -> 15 is logged (the first 20, then a count every 100).
#include <windows.h>

#include <cstdint>

#include "patch.h"

namespace {

long g_clamps = 0;

}  // namespace

extern "C" void __cdecl GridClampSlow(uint32_t caller, int32_t* col, int32_t* row, uint32_t site) {
  const int32_t c = *col, r = *row;
  if (c > 15 || c < 0) *col = c < 0 ? 0 : 15;
  if (r > 15 || r < 0) *row = r < 0 ? 0 : 15;
  if (c == 16 && r >= 0 && r <= 16) return;  // the game's own case
  if (r == 16 && c >= 0 && c <= 16) return;
  const long n = ++g_clamps;
  if (n <= 20 || n % 100 == 0)
    patch::Log("grid: terrain patch index col %ld row %ld clamped to %ld %ld at %08lx (called from %08lx), %ld so far",
               static_cast<long>(c), static_cast<long>(r), static_cast<long>(*col), static_cast<long>(*row),
               static_cast<unsigned long>(site), static_cast<unsigned long>(caller), n);
}

// Called in place of the index loads: ecx = col, eax = row (both clamped);
// the second form also does the original xor ebx,ebx. The x87 stack is
// empty at both sites.
#if defined(_MSC_VER)
__declspec(naked) void GridClampA() {
  __asm {
    mov ecx, [ebp-14h]
    mov eax, [ebp-0Ch]
    cmp ecx, 15
    ja slow
    cmp eax, 15
    ja slow
    ret
  slow:
    pushad
    push 485440h
    lea edx, [ebp-0Ch]
    push edx
    lea edx, [ebp-14h]
    push edx
    push dword ptr [ebp+4]
    call GridClampSlow
    add esp, 16
    popad
    mov ecx, [ebp-14h]
    mov eax, [ebp-0Ch]
    ret
  }
}
__declspec(naked) void GridClampB() {
  __asm {
    mov ecx, [ebp-14h]
    mov eax, [ebp-0Ch]
    cmp ecx, 15
    ja slow
    cmp eax, 15
    ja slow
    xor ebx, ebx
    ret
  slow:
    pushad
    push 485609h
    lea edx, [ebp-0Ch]
    push edx
    lea edx, [ebp-14h]
    push edx
    push dword ptr [ebp+4]
    call GridClampSlow
    add esp, 16
    popad
    mov ecx, [ebp-14h]
    mov eax, [ebp-0Ch]
    xor ebx, ebx
    ret
  }
}
#else
extern "C" void GridClampA();
extern "C" void GridClampB();
#define MCM2FIX_GRID_STUB(NAME, SITE, TAIL)        \
  ".globl _" NAME "\n"                             \
  "_" NAME ":\n"                                   \
  "  movl -0x14(%ebp), %ecx\n"                     \
  "  movl -0xc(%ebp), %eax\n"                      \
  "  cmpl $15, %ecx\n"                             \
  "  ja 1f\n"                                      \
  "  cmpl $15, %eax\n"                             \
  "  ja 1f\n"                                      \
  TAIL                                             \
  "  ret\n"                                        \
  "1:\n"                                           \
  "  pushal\n"                                     \
  "  pushl $" SITE "\n"                            \
  "  leal -0xc(%ebp), %edx\n"                      \
  "  pushl %edx\n"                                 \
  "  leal -0x14(%ebp), %edx\n"                     \
  "  pushl %edx\n"                                 \
  "  pushl 4(%ebp)\n"                              \
  "  call _GridClampSlow\n"                        \
  "  addl $16, %esp\n"                             \
  "  popal\n"                                      \
  "  movl -0x14(%ebp), %ecx\n"                     \
  "  movl -0xc(%ebp), %eax\n"                      \
  TAIL                                             \
  "  ret\n"
asm(".text\n"
    MCM2FIX_GRID_STUB("GridClampA", "0x485440", "")
    MCM2FIX_GRID_STUB("GridClampB", "0x485609", "  xorl %ebx, %ebx\n"));
#endif

void ApplyGridFix() {
  // mov ecx,[ebp-14h]; cmp ecx,10h; jne; mov ecx,0Fh; mov [ebp-14h],ecx;
  // mov eax,[ebp-0Ch]; cmp eax,10h; jne; mov eax,0Fh; mov [ebp-0Ch],eax
  static const uint8_t a[] = {0x8b, 0x4d, 0xec, 0x83, 0xf9, 0x10, 0x75, 0x08, 0xb9, 0x0f, 0x00,
                              0x00, 0x00, 0x89, 0x4d, 0xec, 0x8b, 0x45, 0xf4, 0x83, 0xf8, 0x10,
                              0x75, 0x08, 0xb8, 0x0f, 0x00, 0x00, 0x00, 0x89, 0x45, 0xf4};
  // mov eax,[ebp-0Ch]; mov ecx,[ebp-14h]; xor ebx,ebx; (then mov eax,[eax*4+572370h])
  static const uint8_t b[] = {0x8b, 0x45, 0xf4, 0x8b, 0x4d, 0xec, 0x33, 0xdb, 0x8b, 0x04, 0x85, 0x70, 0x23, 0x57, 0x00};
  const bool ok_a = patch::Matches(0x485440, a, sizeof(a));
  const bool ok_b = patch::Matches(0x485609, b, sizeof(b));
  if (ok_a) patch::Call(0x485440, reinterpret_cast<const void*>(&GridClampA), sizeof(a));
  if (ok_b) patch::Call(0x485609, reinterpret_cast<const void*>(&GridClampB), 8);
  patch::Log("grid: terrain patch lookups clamped to 0..15: 485440 %s, 485609 %s", ok_a ? "patched" : "NOT patched (bytes differ)",
             ok_b ? "patched" : "NOT patched (bytes differ)");
}
