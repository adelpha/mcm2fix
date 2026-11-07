#!/usr/bin/env python3
"""Make Mcm2.exe load mcm2fix.dll instead of d3drm.dll.

Usage:  python mcm2patch.py <original Mcm2.exe> <output exe>

The input is never modified. The only change to the exe is the name of one
imported DLL: d3drm.dll (not shipped with Windows 11; the game used just
D3DRMVectorRotate from it) becomes mcm2fix.dll, which provides that function
and applies every other fix in memory when the game starts. See
code/README.md.

Supported input: the no-CD Mcm2.exe, 1,577,005 bytes,
SHA-256 345e4f644fe2237108472acbcde3123b186d2475dd5a23480321c78084746f9e.
"""
import hashlib
import struct
import sys

ORIGINAL_SHA256 = "345e4f644fe2237108472acbcde3123b186d2475dd5a23480321c78084746f9e"

# .text section header values in the original file.
TEXT_VSIZE_OLD, TEXT_RAWSIZE = 0x153A7E, 0x154000
TEXT_VA, TEXT_RAW = 0x1000, 0x1000

# Import descriptor for d3drm.dll (file offset) and its Name RVA.
# .rdata has raw offset == RVA, so the RVA is also the file offset.
D3DRM_DESCRIPTOR = 0x169EE4
D3DRM_NAME_RVA = 0x16AE0E

# New DLL name goes in the zero-filled tail of .text (raw data already there).
NEW_NAME_RVA = 0x154A80
NEW_NAME = b"mcm2fix.dll\0"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != ORIGINAL_SHA256:
        sys.exit(f"unsupported Mcm2.exe (SHA-256 {digest}); expected the original no-CD exe")
    buf = bytearray(data)

    # Grow .text VirtualSize so the new string is inside the mapped section.
    e_lfanew = struct.unpack_from("<I", buf, 0x3C)[0]
    opt_size = struct.unpack_from("<H", buf, e_lfanew + 20)[0]
    sec0 = e_lfanew + 24 + opt_size
    assert buf[sec0:sec0 + 5] == b".text"
    assert struct.unpack_from("<I", buf, sec0 + 8)[0] == TEXT_VSIZE_OLD
    struct.pack_into("<I", buf, sec0 + 8, TEXT_RAWSIZE)

    name_off = NEW_NAME_RVA - TEXT_VA + TEXT_RAW
    assert buf[name_off:name_off + len(NEW_NAME)] == b"\0" * len(NEW_NAME)
    buf[name_off:name_off + len(NEW_NAME)] = NEW_NAME

    # IMAGE_IMPORT_DESCRIPTOR.Name is at +12.
    assert struct.unpack_from("<I", buf, D3DRM_DESCRIPTOR + 12)[0] == D3DRM_NAME_RVA
    assert buf[D3DRM_NAME_RVA:D3DRM_NAME_RVA + 10] == b"d3drm.dll\0"
    struct.pack_into("<I", buf, D3DRM_DESCRIPTOR + 12, NEW_NAME_RVA)

    open(dst, "wb").write(buf)
    print(f"wrote {dst} (imports mcm2fix.dll instead of d3drm.dll)")


if __name__ == "__main__":
    main()
