#!/usr/bin/env python3
"""Binary patcher for the no-CD Mcm2.exe (1,577,005 bytes).

Usage:  python mcm2patch.py <original Mcm2.exe> <output exe>

The input is never modified. Every patch checks the original bytes first and
refuses to run on an unexpected file.

Patches (step 1: get the game to start on Windows 11):

  1. flip-retry   Startup measures the refresh rate by flipping the primary
                  surface ~10 times right after SetDisplayMode(640x480x16).
                  On Windows 10/11 the mode change is not always finished, so
                  Flip returns DDERR_SURFACELOST and the game quits silently
                  (WinMain returns 1). The three Flip calls in that loop now go
                  through a helper that restores the surface and retries.
  2. no-admin     Remove the startup "administrator" probe (writes
                  HKLM\\...\\IsAdmin and a temp file in the current directory),
                  which fails for standard users when Windows registry
                  virtualization does not apply.
"""
import struct
import sys


IMAGE_BASE = 0x400000
EXPECTED_SIZE = 1577005

# .text section: VA 0x1000, raw offset 0x1000, VirtualSize 0x153a7e, raw size
# 0x154000. The zero-filled tail of the raw data is used as a code cave.
TEXT_VA, TEXT_RAW = 0x1000, 0x1000
TEXT_VSIZE_OLD, TEXT_RAWSIZE = 0x153A7E, 0x154000
CAVE = 0x554A80

IAT_SLEEP = 0x5551AC  # KERNEL32!Sleep
DDERR_SURFACELOST = 0x887601C2


# Assembled bytes for each asm() call below, so keystone is optional.
PREBUILT = {
    (0x554a80, 'push edi mov edi, 300 retry: mov eax, dword ptr [ebx] push 1 push 0 push eax mov ecx, dword ptr [eax] call dword ptr [ecx + 0x2c] cmp eax, 0x887601c2 jne done mov eax, dword ptr [ebx] push eax mov ecx, dword ptr [eax] call dword ptr [ecx + 0x6c] push 10 call dword ptr [0x5551ac] dec edi jnz retry mov eax, 0x887601c2 done: pop edi ret'):
        bytes.fromhex('57bf2c0100008b036a016a00508b08ff512c3dc201768875188b03508b08ff516c6a0aff15ac5155004f75dab8c20176885fc3'),
    (0x4ce7d4, 'call 0x554a80'):
        bytes.fromhex('e8a7620800'),
    (0x4ce802, 'call 0x554a80'):
        bytes.fromhex('e879620800'),
    (0x4ce814, 'call 0x554a80'):
        bytes.fromhex('e867620800'),
    (0x4a1f14, 'jmp 0x4a2005'):
        bytes.fromhex('e9ec000000'),
}


def off(va):
    return va - IMAGE_BASE - TEXT_VA + TEXT_RAW


def asm(code, addr):
    """Assemble with keystone if it's installed, else use the stored bytes."""
    try:
        from keystone import Ks, KS_ARCH_X86, KS_MODE_32
    except ImportError:
        return PREBUILT[(addr, " ".join(code.split()))]
    enc, _ = Ks(KS_ARCH_X86, KS_MODE_32).asm(code, addr)
    return bytes(enc)


def patch(buf, va, old, new, name):
    o = off(va)
    cur = bytes(buf[o:o + len(old)])
    if cur == new.ljust(len(old), b"\x90")[: len(old)] and len(new) <= len(old):
        print(f"  {name}: already applied")
        return
    if cur != old:
        sys.exit(f"{name}: unexpected bytes at {va:#x}: {cur.hex()} (want {old.hex()})")
    if len(new) > len(old):
        sys.exit(f"{name}: replacement longer than original")
    buf[o:o + len(old)] = new.ljust(len(old), b"\x90")
    print(f"  {name}: patched {va:#x}")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    buf = bytearray(open(src, "rb").read())
    if len(buf) != EXPECTED_SIZE:
        sys.exit(f"unexpected file size {len(buf)}, want {EXPECTED_SIZE}")

    # Grow .text VirtualSize to cover the cave (it is already in the raw data).
    e_lfanew = struct.unpack_from("<I", buf, 0x3C)[0]
    nsec_off = e_lfanew + 6
    opt_size = struct.unpack_from("<H", buf, e_lfanew + 20)[0]
    sec0 = e_lfanew + 24 + opt_size
    assert buf[sec0:sec0 + 5] == b".text"
    vsize = struct.unpack_from("<I", buf, sec0 + 8)[0]
    if vsize == TEXT_VSIZE_OLD:
        struct.pack_into("<I", buf, sec0 + 8, TEXT_RAWSIZE)
        print("  .text VirtualSize -> 0x154000")
    elif vsize != TEXT_RAWSIZE:
        sys.exit(f"unexpected .text VirtualSize {vsize:#x}")

    # --- flip-retry helper in the cave -------------------------------------
    # In: ebx = pointer to the IDirectDrawSurface7* of the primary surface.
    # Out: eax = HRESULT of the last Flip. Preserves ebx, esi, edi, ebp.
    flip_helper = asm(f"""
        push edi
        mov edi, 300
    retry:
        mov eax, dword ptr [ebx]
        push 1
        push 0
        push eax
        mov ecx, dword ptr [eax]
        call dword ptr [ecx + 0x2c]
        cmp eax, {DDERR_SURFACELOST:#x}
        jne done
        mov eax, dword ptr [ebx]
        push eax
        mov ecx, dword ptr [eax]
        call dword ptr [ecx + 0x6c]
        push 10
        call dword ptr [{IAT_SLEEP:#x}]
        dec edi
        jnz retry
        mov eax, {DDERR_SURFACELOST:#x}
    done:
        pop edi
        ret
    """, CAVE)
    o = off(CAVE)
    existing = bytes(buf[o:o + len(flip_helper)])
    if existing not in (b"\0" * len(flip_helper), flip_helper):
        sys.exit("code cave is not empty")
    buf[o:o + len(flip_helper)] = flip_helper
    print(f"  flip-retry helper at {CAVE:#x} ({len(flip_helper)} bytes)")

    # Each site: mov eax,[ebx]; push 1; push 0; (push eax; mov ecx,[eax] |
    # mov edx,[eax]; push eax); call [reg+0x2c]  = 12 bytes -> call helper.
    sites = {
        0x4CE7D4: "8b036a016a00508b08ff512c",
        0x4CE802: "8b036a016a008b1050ff522c",
        0x4CE814: "8b036a016a00508b08ff512c",
    }
    for va, old in sites.items():
        patch(buf, va, bytes.fromhex(old), asm(f"call {CAVE:#x}", va), f"flip-retry {va:#x}")

    # --- no-admin ----------------------------------------------------------
    # 4a1f14: cmp eax,2 ; jne 4a2005  ->  jmp 4a2005
    patch(buf, 0x4A1F14, bytes.fromhex("83f8020f85e8000000"),
          asm("jmp 0x4a2005", 0x4A1F14), "no-admin")

    open(dst, "wb").write(buf)
    print(f"wrote {dst}")


if __name__ == "__main__":
    main()
