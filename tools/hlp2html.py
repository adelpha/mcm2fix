#!/usr/bin/env python3
"""Convert a Windows Help file (.HLP, WinHelp 3.1/4.0) to one self-contained HTML page.

Written for MCM2HELP.HLP from Motocross Madness 2: Windows 10 and 11 cannot
open .HLP files, so mcm2fix opens the converted MCM2HELP.html instead (see
code/src/fixes_help.cpp).

    python3 tools/hlp2html.py MCM2HELP.HLP MCM2HELP.html

The page keeps the help file's own layout, fonts, colours and pictures, and
has working topic links, popups, picture hotspots, "Related Topics" buttons,
the Contents/Back/Next/Index/Find/Print bar, a keyword index and a text search.
Embedded .AVI clips are converted to H.264 MP4 if ffmpeg is on the PATH (the
stunt clips in MCM2HELP.HLP are Indeo 5, which browsers cannot play).

The file format follows "Windows Help File Format" by M. Winterhoff
(helpfile.txt in the helpdeco project). Needs Pillow for the pictures.
"""
import base64
import bisect
import html
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image


# ---------------------------------------------------------------- low level

def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def s16(b, o): return struct.unpack_from('<h', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def s32(b, o): return struct.unpack_from('<i', b, o)[0]


def lz77(src, maxout=None):
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        bits = src[i]
        i += 1
        for k in range(8):
            if i >= n:
                break
            if bits & (1 << k):
                if i + 1 >= n:
                    i = n
                    break
                w = src[i] | (src[i + 1] << 8)
                i += 2
                length, pos = (w >> 12) + 3, w & 0xfff
                start = len(out) - pos - 1
                for j in range(length):
                    out.append(out[start + j] if start + j >= 0 else 32)
            else:
                out.append(src[i])
                i += 1
            if maxout and len(out) >= maxout:
                return bytes(out[:maxout])
    return bytes(out)


def runlen(src):
    out = bytearray()
    i = 0
    while i < len(src):
        n = src[i]
        i += 1
        if n & 0x80:
            out += src[i:i + (n & 0x7f)]
            i += n & 0x7f
        else:
            if i < len(src):
                out += bytes([src[i]]) * n
            i += 1
    return bytes(out)


class Reader:
    def __init__(self, b, o=0):
        self.b, self.o = b, o

    def u8(self):
        v = self.b[self.o]; self.o += 1; return v

    def u16(self):
        v = u16(self.b, self.o); self.o += 2; return v

    def s16(self):
        v = s16(self.b, self.o); self.o += 2; return v

    def u32(self):
        v = u32(self.b, self.o); self.o += 4; return v

    def s32(self):
        v = s32(self.b, self.o); self.o += 4; return v

    def cus(self):  # compressed unsigned short
        v = self.u8()
        return (v >> 1) + 128 * self.u8() if v & 1 else v >> 1

    def css(self):  # compressed signed short
        v = self.u8()
        return (v >> 1) + 128 * self.u8() - 16384 if v & 1 else (v >> 1) - 64

    def cul(self):  # compressed unsigned long
        v = self.u16()
        return (v >> 1) + 32768 * self.u16() if v & 1 else v >> 1

    def csl(self):  # compressed signed long
        v = self.u16()
        return (v >> 1) + 32768 * self.u16() - 67108864 if v & 1 else (v >> 1) - 16384

    def sz(self):
        e = self.b.index(b'\0', self.o)
        v = self.b[self.o:e]
        self.o = e + 1
        return v


# ------------------------------------------------------------- file system

class HelpFile:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        if u32(self.d, 0) != 0x35F3F:
            raise ValueError('not a Windows help file')
        self.files = dict(self.btree(u32(self.d, 4) + 9,
                                     lambda r: (r.sz().decode('latin1'), r.u32())))

    def ifile(self, name):
        o = self.files[name]
        return self.d[o + 9:o + 9 + u32(self.d, o + 4)]

    def btree(self, off, leaf):
        b = self.d[off:]
        pagesize, root, nlev = u16(b, 4), s16(b, 26), s16(b, 32)
        pg = root
        for _ in range(nlev - 1):  # follow PreviousPage down to the first leaf
            pg = s16(b, 38 + pg * pagesize + 4)
        out = []
        while pg != -1:
            po = 38 + pg * pagesize
            r = Reader(b, po + 8)
            for _ in range(s16(b, po + 2)):
                out.append(leaf(r))
            pg = s16(b, po + 6)
        return out

    def btree_file(self, name, leaf):
        return self.btree(self.files[name] + 9, leaf) if name in self.files else []


def load_phrases(h):
    """Phrase table: Hall (|PhrIndex/|PhrImage) or old style (|Phrases)."""
    if '|PhrIndex' in h.files:
        idx, img = h.ifile('|PhrIndex'), h.ifile('|PhrImage')
        _, nent, _, imgsize, imgcsize, _ = struct.unpack_from('<6I', idx, 0)
        bc = u16(idx, 24) & 0xf
        words = idx[28:]
        state = [0, 0]  # mask, word index

        def bit():
            m, p = state
            if m & 0x80000000:
                p += 1
            m = ((m * 2) & 0xffffffff) + (1 if (m == 0 or m & 0x80000000) else 0)
            state[0], state[1] = m, p
            return (u32(words, p * 4) & m) != 0
        pos = [0]
        for _ in range(nent):
            n = 1
            while bit():
                n += 1 << bc
            if bit(): n += 1
            if bc > 1 and bit(): n += 2
            if bc > 2 and bit(): n += 4
            if bc > 3 and bit(): n += 8
            if bc > 4 and bit(): n += 16
            pos.append(pos[-1] + n)
        if imgcsize != imgsize:
            img = lz77(img, imgsize)
        return 'hall', [img[pos[i]:pos[i + 1]] for i in range(nent)]
    if '|Phrases' in h.files:
        p = h.ifile('|Phrases')
        n = u16(p, 0)
        sysf = h.ifile('|SYSTEM')
        if u16(sysf, 2) <= 16:
            offs = [u16(p, 4 + 2 * i) for i in range(n + 1)]
            data = p
            base = 4
        else:
            size = u32(p, 4)
            offs = [u16(p, 8 + 2 * i) for i in range(n + 1)]
            data = lz77(p[8 + 2 * (n + 1):], size)
            base = 2 * (n + 1)
            offs = [o - base for o in offs]
            base = 0
        return 'old', [data[base + offs[i]:base + offs[i + 1]] for i in range(n)]
    return None, []


def phrase_expand(kind, src, ph):
    out = bytearray()
    i = 0
    if kind == 'hall':
        while i < len(src):
            ch = src[i]; i += 1
            if ch & 1 == 0:
                out += ph[ch >> 1]
            elif ch & 3 == 1:
                out += ph[ch * 64 + 64 + src[i]]; i += 1
            elif ch & 7 == 3:
                c = (ch >> 3) + 1; out += src[i:i + c]; i += c
            elif ch & 15 == 7:
                out += b' ' * ((ch >> 4) + 1)
            else:
                out += b'\0' * ((ch >> 4) + 1)
    else:
        while i < len(src):
            ch = src[i]; i += 1
            if ch == 0 or ch > 15:
                out.append(ch)
            else:
                n = (ch * 256 - 256 + src[i]); i += 1
                out += ph[n >> 1]
                if n & 1:
                    out += b' '
    return bytes(out)


# ------------------------------------------------------------------ topics

def load_links(h):
    """All TOPICLINKs of |TOPIC in order, with their TOPICPOS and TOPICOFFSET."""
    sysf = h.ifile('|SYSTEM')
    minor, flags = u16(sysf, 2), u16(sysf, 10)
    if minor <= 16:
        raise ValueError('Windows 3.0 help files are not supported')
    blocksize = 2048 if flags == 8 else 4096
    compressed = flags in (4, 8)
    dsize = 16384 if compressed else blocksize - 12
    kind, ph = load_phrases(h)
    t = h.ifile('|TOPIC')
    buf, starts = bytearray(), []
    for i in range(0, len(t), blocksize):
        starts.append(len(buf))
        blk = t[i + 12:i + blocksize]
        buf += lz77(blk, dsize) if compressed else blk
    shift = dsize.bit_length() - 1 if compressed else None

    def lin(pos):
        if compressed:
            return starts[pos >> shift] + (pos & (dsize - 1)) - 12
        return starts[(pos - 12) // dsize] + (pos - 12) % dsize

    def blockof(pos):
        return pos >> shift if compressed else (pos - 12) // dsize

    links, pos, cc, curblk = [], 12, 0, 0
    while 0 < pos and lin(pos) + 21 <= len(buf):
        o = lin(pos)
        bs, dl2, _, nxt, dl1 = struct.unpack_from('<iiiii', buf, o)
        rt = buf[o + 20]
        blk = blockof(pos)
        if blk != curblk:
            curblk, cc = blk, 0
        ld1 = bytes(buf[o + 21:o + dl1])
        ld2 = bytes(buf[o + dl1:o + bs])
        ld2 = phrase_expand(kind, ld2, ph) if (dl2 > bs - dl1 and kind) else ld2[:dl2]
        links.append(dict(pos=pos, rt=rt, ld1=ld1, ld2=ld2, toff=(blk << 15) + cc))
        if rt in (0x20, 0x23):
            r = Reader(ld1, 0)
            r.cul()
            cc += r.cus()
        if nxt <= pos:
            break
        pos = nxt
    return links


def parse_para(r, rt):
    p = {}
    if rt == 0x23:
        col = r.s16()
        if col == -1:
            return None
        r.s16(); r.u8()
        p['col'] = col
    r.u8(); r.u8(); r.u16()
    bits = r.u16()
    if bits & 1: r.csl()
    if bits & 2: p['above'] = r.css()
    if bits & 4: p['below'] = r.css()
    if bits & 8: p['lines'] = r.css()
    if bits & 0x10: p['left'] = r.css()
    if bits & 0x20: p['right'] = r.css()
    if bits & 0x40: p['first'] = r.css()
    if bits & 0x100:
        p['border'] = r.u8(); r.s16()
    if bits & 0x200:
        tabs = []
        for _ in range(r.css()):
            t = r.cus()
            if t & 0x4000: r.cus()
            tabs.append(t & 0x3fff)
        p['tabs'] = tabs
    p['align'] = 'right' if bits & 0x400 else 'center' if bits & 0x800 else 'left'
    return p


def events(link):
    """Decode one displayable TOPICLINK into a list of formatting events."""
    ev = []
    rt, ld1, d2 = link['rt'], link['ld1'], link['ld2']
    r = Reader(ld1, 0)
    r.csl()
    tp = 0

    def text():
        nonlocal tp
        e = d2.find(b'\0', tp)
        if e < 0: e = len(d2)
        s = d2[tp:e]; tp = e + 1
        return s
    r.cus()
    if rt == 0x23:
        nc, tt = r.u8(), r.u8()
        if tt in (0, 2): r.s16()
        ev.append(('table', tt, [(r.s16(), r.s16()) for _ in range(nc)]))
    while True:
        p = parse_para(r, rt)
        if p is None:
            break
        ev.append(('para', p))
        while True:
            s = text()
            if s: ev.append(('text', s))
            c = r.u8()
            if c == 0xff: break
            if c == 0x20: r.s32()
            elif c == 0x21: r.s16()
            elif c == 0x80: ev.append(('font', r.s16()))
            elif c == 0x81: ev.append(('br',))
            elif c == 0x82: ev.append(('par',))
            elif c == 0x83: ev.append(('tab',))
            elif c in (0x86, 0x87, 0x88):
                typ = r.u8(); size = r.csl()
                if typ == 0x22: r.cus()
                st = r.o
                if typ in (3, 0x22):
                    if r.s16() == 0:
                        ev.append(('pic', c, '|bm%d' % r.s16()))
                    else:
                        ev.append(('picdata', c, ld1[r.o:st + size]))
                elif typ == 5:
                    r.s16(); r.s16(); r.s16()
                    ev.append(('embed', c, r.sz().decode('cp1252')))
                r.o = st + size
            elif c == 0x89: ev.append(('hotend',))
            elif c == 0x8b: ev.append(('text', b'\xa0'))
            elif c == 0x8c: ev.append(('text', b''))
            elif c in (0xc8, 0xcc):
                n = r.s16()
                ev.append(('macro', c, ld1[r.o:r.o + n].split(b'\0')[0].decode('cp1252')))
                r.o += n
            elif c in (0xe0, 0xe1):
                ev.append(('jumpoff', c, r.u32()))
            elif c in (0xe2, 0xe3, 0xe6, 0xe7):
                ev.append(('jump', c, r.u32()))
            elif c in (0xea, 0xeb, 0xee, 0xef):
                n = r.s16(); st = r.o
                r.u8(); r.u32()
                ev.append(('xjump', c))
                r.o = st + n
            else:
                ev.append(('stop', c))
                break
        if rt != 0x23:
            break
    return ev


# ---------------------------------------------------------------- pictures

def decode_pictures(data):
    """Pictures of an SHG/MRB container: list of (PIL image, hotspots)."""
    _, n = struct.unpack_from('<HH', data, 0)
    res = []
    for k in range(n):
        po = u32(data, 4 + 4 * k)
        r = Reader(data, po)
        ptype, pack = r.u8(), r.u8()
        if ptype not in (5, 6):
            continue  # metafiles are not supported
        r.cul(); r.cul()
        r.cus(); bc = r.cus()
        w, hgt = r.cul(), r.cul()
        cu = r.cul(); r.cul()
        cs, hs = r.cul(), r.cul()
        co, ho = r.u32(), r.u32()
        pal = []
        if bc <= 8:
            pal = [data[r.o + 4 * i:r.o + 4 * i + 4] for i in range(cu or (1 << bc))]
        raw = data[po + co:po + co + cs]
        if pack in (2, 3): raw = lz77(raw)
        if pack in (1, 3): raw = runlen(raw)
        stride = ((w * bc + 31) // 32) * 4
        raw = bytes(raw[:stride * hgt]).ljust(stride * hgt, b'\0')
        if bc in (1, 4, 8):
            if not pal:
                pal = [b'\0\0\0\0', b'\xff\xff\xff\0']
            img = Image.frombuffer('P', (w, hgt), raw, 'raw', 'P;%d' % bc if bc < 8 else 'P', stride, -1)
            img.putpalette(b''.join(p[2:3] + p[1:2] + p[0:1] for p in pal).ljust(768, b'\0'))
            img = img.convert('RGB')
        elif bc == 24:
            img = Image.frombuffer('RGB', (w, hgt), raw, 'raw', 'BGR', stride, -1)
        else:
            continue
        res.append((img, parse_hotspots(data[po + ho:po + ho + hs]) if hs and ho else []))
    return res


def parse_hotspots(b):
    n, msize = u16(b, 1), u32(b, 3)
    o, hs = 7, []
    for _ in range(n):
        x, y, w, hh = struct.unpack_from('<4H', b, o + 3)
        hs.append(dict(id=b[o], x=x, y=y, w=w, h=hh, hash=u32(b, o + 11)))
        o += 15
    o += msize
    for i in range(n):
        e = b.index(b'\0', o); o = e + 1
        e = b.index(b'\0', o); hs[i]['target'] = b[o:e].decode('cp1252'); o = e + 1
    return hs


# ------------------------------------------------------------- the helpers

HASH_TABLE = bytes([
    0x00, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
    0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
    0xF0, 0x0B, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0x0C, 0xFF,
    0x0A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0D,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
    0x80, 0x81, 0x82, 0x83, 0x0B, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
    0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
    0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
])


def context_hash(name):
    h = 0
    for ch in name.encode('cp1252'):
        t = HASH_TABLE[ch]
        h = (h * 43 + (t - 256 if t > 127 else t)) & 0xffffffff
    return h


SYMBOL = {0xb7: '•', 0xde: '⇒', 0xae: '→', 0xac: '←', 0xad: '↑',
          0xaf: '↓', 0xd2: '®', 0xd3: '©', 0xd4: '™', 0xb4: '×'}


def font_table(h):
    f = h.ifile('|FONT')
    nf, nd, fo, do = struct.unpack_from('<4H', f, 0)
    if fo >= 12:
        raise ValueError('multimedia font descriptors are not supported')
    w = (do - fo) // nf
    names = [f[fo + i * w:fo + (i + 1) * w].split(b'\0')[0].decode('latin1') for i in range(nf)]
    out = []
    for i in range(nd):
        o = do + i * 11
        a, hp, fam, fi = struct.unpack_from('<BBBH', f, o)
        out.append(dict(bold=a & 1, ital=a & 2, under=a & 4, strike=a & 8, smallcaps=a & 32,
                        size=hp / 2, face=names[fi] if fi < nf else 'Arial', fam=fam,
                        fg='#%02x%02x%02x' % tuple(f[o + 5:o + 8])))
    return out


def system_info(h):
    s = h.ifile('|SYSTEM')
    info = dict(title='Help', windows={})
    r = Reader(s, 12)
    while r.o + 4 <= len(s):
        t, n = r.u16(), r.u16()
        d = s[r.o:r.o + n]
        r.o += n
        if t == 1:
            info['title'] = d.split(b'\0')[0].decode('cp1252')
        elif t == 3:
            info['contents'] = u32(d, 0)
        elif t == 6 and n >= 90:
            name = d[12:21].split(b'\0')[0].decode('latin1').lower()
            flags = u16(d, 0)
            rgb = u32(d, 82) if flags & 0x100 else None
            nsr = u32(d, 86) if flags & 0x200 else None
            info['windows'][name] = dict(rgb=rgb, nsr=nsr)
    return info


def css_color(c):
    return '#%02x%02x%02x' % (c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff)


def px(v):
    # Help file metrics here are half points; 1 half point = 2/3 pixel at 96 dpi.
    return round(v * 2 / 3, 1)


# ------------------------------------------------------------ HTML output

class Converter:
    def __init__(self, path, ffmpeg=True, log=print):
        self.log = log
        self.h = HelpFile(path)
        self.base = os.path.splitext(os.path.basename(path))[0].upper()
        self.info = system_info(self.h)
        self.fonts = font_table(self.h)
        links = load_links(self.h)
        ctx = self.h.btree_file('|CONTEXT', lambda r: (r.u32(), r.u32()))
        self.ctx = dict(ctx)
        self.topics = []
        for l in links:
            if l['rt'] == 2:
                hdr = struct.unpack_from('<7i', l['ld1'], 0)
                strs = l['ld2'].split(b'\0')
                self.topics.append(dict(toff=l['toff'], title=strs[0].decode('cp1252'),
                                        macros=[m.decode('cp1252') for m in strs[1:] if m],
                                        browse_next=hdr[2], browse_prev=hdr[1],
                                        nonscroll=hdr[4], scroll=hdr[5], links=[]))
            elif self.topics and l['rt'] in (0x20, 0x23):
                self.topics[-1]['links'].append(l)
        self.toffs = [t['toff'] for t in self.topics]
        for i, t in enumerate(self.topics):
            t['id'] = 't%d' % i
        self.pictures = {}
        self.videos = {}
        self.ffmpeg = ffmpeg and shutil.which('ffmpeg')
        self.used_pics = set()
        self.keywords = self.load_keywords('K')
        self.alinks = self.load_keywords('A')
        self.popups = set()

    # --- lookups
    def topic_at(self, toff):
        if toff in (None, -1, 0xffffffff):
            return None
        i = bisect.bisect_right(self.toffs, toff) - 1
        return self.topics[i]['id'] if i >= 0 else None

    def topic_by_hash(self, hv):
        return self.topic_at(self.ctx.get(hv))

    def topic_by_name(self, name):
        return self.topic_by_hash(context_hash(name))

    def load_keywords(self, letter):
        data_name = '|%sWDATA' % letter if letter != 'K' else '|KWDATA'
        tree = '|%sWBTREE' % letter if letter != 'K' else '|KWBTREE'
        if tree not in self.h.files:
            return {}
        data = self.h.ifile(data_name)
        out = {}
        for kw, count, off in self.h.btree_file(tree, lambda r: (r.sz(), r.s16(), r.u32())):
            ids = []
            for i in range(count):
                t = self.topic_at(u32(data, off + 4 * i))
                if t and t not in ids:
                    ids.append(t)
            out.setdefault(kw.decode('cp1252'), []).extend(ids)
        return out

    def picture(self, name):
        if name not in self.pictures:
            pics = decode_pictures(self.h.ifile(name)) if name in self.h.files else []
            if not pics:
                self.pictures[name] = None
            else:
                img, hot = pics[-1]  # highest resolution last
                buf = io.BytesIO()
                img.save(buf, 'PNG', optimize=True)
                self.pictures[name] = dict(w=img.width, h=img.height, hot=hot, key=name.strip('|'),
                                           src='data:image/png;base64,' + base64.b64encode(buf.getvalue()).decode())
        return self.pictures[name]

    def video(self, name):
        if name in self.videos:
            return self.videos[name]
        src = None
        if name in self.h.files and self.ffmpeg:
            with tempfile.TemporaryDirectory() as td:
                a, m = os.path.join(td, 'in.avi'), os.path.join(td, 'out.mp4')
                open(a, 'wb').write(self.h.ifile(name))
                r = subprocess.run([self.ffmpeg, '-v', 'error', '-y', '-i', a, '-an', '-c:v', 'libx264',
                                    '-pix_fmt', 'yuv420p', '-crf', '20', '-preset', 'slow',
                                    '-movflags', '+faststart', m], capture_output=True)
                if r.returncode == 0:
                    src = 'data:video/mp4;base64,' + base64.b64encode(open(m, 'rb').read()).decode()
                else:
                    self.log('ffmpeg failed for %s: %s' % (name, r.stderr.decode(errors='replace')))
        elif name in self.h.files:
            self.log('ffmpeg not found: skipping video %s' % name)
        self.videos[name] = src
        return src

    # --- macros
    def macro_attr(self, macro):
        """Turn a WinHelp macro into data-* attributes the page script understands."""
        m = macro.strip()
        name = re.match(r'[A-Za-z]+', m)
        name = name.group(0).lower() if name else ''
        args = re.findall(r"`([^']*)'|\"([^\"]*)\"", m)
        args = [a or b for a, b in args]
        if name in ('back', 'next', 'prev', 'search', 'find', 'print', 'contents', 'exit'):
            return 'data-cmd="%s"' % ('index' if name == 'search' else name)
        if name in ('jumpid', 'ji', 'popupid', 'pi', 'jumpcontents', 'jc') and args:
            t = self.topic_by_name(args[-1]) if name not in ('jumpcontents', 'jc') else self.topics[0]['id']
            if t:
                if name in ('popupid', 'pi'):
                    self.popups.add(t)
                    return 'data-popup="%s"' % t
                return 'href="#%s"' % t
        if name in ('alink', 'al', 'klink', 'kl') and args:
            kws = [k.strip() for k in args[0].split(';') if k.strip()]
            table = self.alinks if name in ('alink', 'al') else self.keywords
            ids = []
            for k in kws:
                for t in table.get(k, []):
                    if t not in ids:
                        ids.append(t)
            return "data-related='%s'" % html.escape(json.dumps(ids), quote=True)
        return 'data-cmd="none" title="%s"' % html.escape(m)

    # --- topic rendering
    def font_css(self, n):
        f = self.fonts[n] if 0 <= n < len(self.fonts) else self.fonts[0]
        fam = {1: 'monospace', 2: 'serif', 3: 'sans-serif', 4: 'cursive', 5: 'fantasy'}.get(f['fam'], 'sans-serif')
        s = ["font-family:'%s',%s" % (f['face'], fam), 'font-size:%gpt' % f['size'],
             'font-weight:%s' % ('bold' if f['bold'] else 'normal'),
             'font-style:%s' % ('italic' if f['ital'] else 'normal'), 'color:%s' % f['fg']]
        deco = [d for d, on in (('underline', f['under']), ('line-through', f['strike'])) if on]
        if deco:
            s.append('text-decoration:' + ' '.join(deco))
        if f['smallcaps']:
            s.append('font-variant:small-caps')
        return ';'.join(s)

    def render_links(self, links):
        out = []
        st = dict(font=0, para=None, pieces=None, tab_at=None, in_hot=False, table=None, row=None, col=None)

        def font_open():
            return '<span class="f%d">' % st['font']

        def para_start(p):
            st['para'] = p
            st['pieces'] = [font_open()]
            st['tab_at'] = None
            st['empty'] = True

        def para_end():
            p = st['para']
            if p is None:
                return
            if st['in_hot']:
                st['pieces'].append('</span></a>' + font_open())
                st['in_hot'] = False
            st['pieces'].append('</span>')
            body = ''.join(st['pieces'])
            first = p.get('first', 0)
            if st['tab_at'] is not None and first < 0:
                i = st['tab_at']
                body = '<span class="hang" style="width:%gpx">%s</span>%s' % (
                    px(-first), ''.join(st['pieces'][:i]), ''.join(st['pieces'][i:]))
            style = []
            if p.get('left'): style.append('margin-left:%gpx' % px(p['left']))
            if p.get('right'): style.append('margin-right:%gpx' % px(p['right']))
            if first: style.append('text-indent:%gpx' % px(first))
            if p.get('above'): style.append('margin-top:%gpx' % px(p['above']))
            if p.get('below'): style.append('margin-bottom:%gpx' % px(p['below']))
            if p['align'] != 'left': style.append('text-align:%s' % p['align'])
            cls = 'p' + (' e' if st['empty'] else '')
            if st['empty']:
                body = '<span class="f%d">&nbsp;</span>' % st['font']
            target = st['cell'] if st['table'] is not None else out
            target.append('<div class="%s" style="%s">%s</div>' % (cls, ';'.join(style), body))
            st['pieces'] = None

        def ensure_para():
            if st['pieces'] is None:
                para_start(st['para'] or {'align': 'left'})

        def add(s, content=True):
            ensure_para()
            st['pieces'].append(s)
            if content:
                st['empty'] = False

        def end_table():
            if st['table'] is not None:
                close_cell()
                if st['row'] is not None:
                    st['table'].append(''.join(st['row']))
                out.append(''.join(st['table']) + '</tr></table>')
                st['row'] = None
                st['table'] = None

        def close_cell():
            if st['row'] is not None and st['cell'] is not None:
                st['row'].append('<td style="%s">%s</td>' % (st['cellstyle'], ''.join(st['cell'])))
                st['cell'] = None

        for link in links:
            evs = events(link)
            if link['rt'] == 0x20:
                end_table()
            for e in evs:
                k = e[0]
                if k == 'table':
                    _, tt, cols = e
                    sig = (tt, tuple(cols))
                    if st['table'] is None or st.get('tsig') != sig:
                        end_table()
                        st['tsig'] = sig
                        st['table'] = ['<table class="ht">']
                        st['row'] = None
                        st['tcols'] = cols
                    else:
                        close_cell()
                        st['table'].append(''.join(st['row']) + '</tr>')
                    st['row'] = ['<tr>']
                    st['cell'] = None
                    st['col'] = None
                elif k == 'para':
                    # A paragraph left open with nothing in it was never
                    # ended by 0x82 (WinHelp does not show it).
                    if st['pieces'] is not None and not st['empty']:
                        para_end()
                    st['pieces'] = None
                    p = e[1]
                    if 'col' in p and st['table'] is not None and p['col'] != st['col']:
                        close_cell()
                        st['col'] = p['col']
                        cols = st['tcols']
                        c = cols[p['col']] if p['col'] < len(cols) else (0, 0)
                        pad = px(c[1]) if p['col'] else 0
                        st['cellstyle'] = 'width:%gpx;padding-left:%gpx' % (px(c[0]), pad)
                        st['cell'] = []
                    para_start(p)
                elif k == 'text':
                    s = e[1]
                    f = self.fonts[st['font']] if st['font'] < len(self.fonts) else None
                    if f and f['face'].lower() == 'symbol':
                        t = ''.join(SYMBOL.get(b, chr(b)) for b in s)
                    else:
                        t = s.decode('cp1252', errors='replace')
                    add(html.escape(t).replace('  ', ' &nbsp;'))
                elif k == 'font':
                    st['font'] = e[1]
                    if st['pieces'] is not None:
                        add('</span>' + font_open(), False)
                elif k == 'br':
                    add('<br>')
                elif k == 'par':
                    ensure_para()
                    para_end()
                elif k == 'tab':
                    if st['pieces'] is not None and st['tab_at'] is None and (st['para'] or {}).get('first', 0) < 0:
                        st['pieces'].append('</span>')
                        st['tab_at'] = len(st['pieces'])
                        st['pieces'].append(font_open())
                    else:
                        add('<span class="tab"></span>')
                elif k in ('pic', 'picdata'):
                    pic = self.picture(e[2]) if k == 'pic' else None
                    if pic:
                        cls = {0x86: 'pc', 0x87: 'pl', 0x88: 'pr'}[e[1]]
                        if pic['w'] > 400:
                            cls = 'pc'  # nothing can wrap beside a banner
                        hot = ''
                        for hs in pic['hot']:
                            attr = self.hotspot_attr(hs)
                            if attr:
                                hot += '<a class="hs" %s style="left:%dpx;top:%dpx;width:%dpx;height:%dpx"></a>' % (
                                    attr, hs['x'], hs['y'], hs['w'], hs['h'])
                        add('<span class="pic %s"><img data-bm="%s" width="%d" height="%d" alt="">%s</span>' % (
                            cls, pic['key'], pic['w'], pic['h'], hot))
                elif k == 'embed':
                    add(self.embed_html(e[2]))
                elif k == 'macro':
                    add('</span><a %s class="%s">%s' % (self.macro_attr(e[2]), 'hl' if e[1] == 0xc8 else 'hn', font_open()), False)
                    st['in_hot'] = True
                elif k == 'jump':
                    t = self.topic_by_hash(e[2])
                    popup = e[1] in (0xe2, 0xe6)
                    green = e[1] in (0xe2, 0xe3)
                    cls = ('pop' if popup else 'jmp') + (' hl' if green else ' hn')
                    if t and popup:
                        self.popups.add(t)
                        add('</span><a href="#%s" data-popup="%s" class="%s">%s' % (t, t, cls, font_open()), False)
                    elif t:
                        add('</span><a href="#%s" class="%s">%s' % (t, cls, font_open()), False)
                    else:
                        add('</span><a class="%s">%s' % (cls, font_open()), False)
                    st['in_hot'] = True
                elif k == 'jumpoff':
                    t = self.topic_at(e[2])
                    add('</span><a href="#%s" class="jmp hl">%s' % (t, font_open()), False)
                    st['in_hot'] = True
                elif k == 'xjump':
                    add('</span><a class="hl">%s' % font_open(), False)
                    st['in_hot'] = True
                elif k == 'hotend':
                    if st['in_hot']:
                        add('</span></a>' + font_open(), False)
                        st['in_hot'] = False
        if st['pieces'] is not None and not st['empty']:
            para_end()
        end_table()
        return ''.join(out)

    def hotspot_attr(self, hs):
        i = hs['id']
        if i in (0xe2, 0xe6, 0xe3, 0xe7):
            t = self.topic_by_hash(hs['hash']) or self.topic_by_name(hs['target'])
            if not t:
                return None
            if i in (0xe2, 0xe6):
                self.popups.add(t)
                return 'href="#%s" data-popup="%s"' % (t, t)
            return 'href="#%s"' % t
        if i in (0xc8, 0xcc):
            return self.macro_attr(hs['target'])
        return None

    def embed_html(self, s):
        if s.startswith('!'):  # {button label, macro}
            label, _, macro = s[1:].partition(',')
            return '<button class="btn" %s>%s</button>' % (self.macro_attr(macro), html.escape(label) or '&#8203;')
        if s.startswith('*'):  # {mci flags, [helpfile+]file.avi}
            parts = s[1:].split(',', 2)
            name = parts[-1].split('+')[-1]
            src = self.video(name)
            if src:
                return ('<video class="mci" src="%s" autoplay loop muted playsinline '
                        'controls width="320" height="240"></video>' % src)
            return '<span class="missing">[video %s]</span>' % html.escape(name)
        return ''

    def convert(self):
        topics_html = []
        nav = []
        for t in self.topics:
            ns, sc = [], []
            for l in t['links']:
                if t['nonscroll'] not in (-1, 0xffffffff) and t['nonscroll'] <= l['pos'] < t['scroll']:
                    ns.append(l)
                else:
                    sc.append(l)
            t['ns_html'] = self.render_links(ns) if ns else ''
            t['sc_html'] = self.render_links(sc)
        for t in self.topics:
            nx = self.topic_at(t['browse_next']) if t['browse_next'] not in (-1, 0xffffffff) else None
            pv = self.topic_at(t['browse_prev']) if t['browse_prev'] not in (-1, 0xffffffff) else None
            attrs = 'id="%s" data-title="%s"' % (t['id'], html.escape(t['title'], quote=True))
            if nx: attrs += ' data-next="%s"' % nx
            if pv: attrs += ' data-prev="%s"' % pv
            if t['id'] in self.popups: attrs += ' data-popuponly="1"'
            topics_html.append('<section class="topic" %s hidden>%s<div class="sr">%s</div></section>' % (
                attrs, ('<div class="nsr">%s</div>' % t['ns_html']) if t['ns_html'] else '', t['sc_html']))
            if t['title']:
                nav.append([t['id'], t['title']])
        index = sorted(([k, v] for k, v in self.keywords.items() if v), key=lambda kv: kv[0].lower())
        # Start where WinHelp would: the contents topic, unless that is an
        # empty topic whose only job is a JumpId macro (as in MCM2HELP.HLP).
        start = self.topic_at(self.info.get('contents', 0)) or self.topics[0]['id']
        st = self.topics[int(start[1:])]
        if not st['title']:
            for m in st['macros']:
                a = self.macro_attr(m)
                if a.startswith('href="#'):
                    start = a[7:-1]
                    break
            else:
                start = next((t['id'] for t in self.topics if t['title']), start)
        w = self.info['windows'].get('main') or next(iter(self.info['windows'].values()), {})
        bg = css_color(w['rgb']) if w.get('rgb') is not None else '#ffffff'
        nsr = css_color(w['nsr']) if w.get('nsr') is not None else bg
        font_css = '\n'.join('.f%d{%s}' % (i, self.font_css(i)) for i in range(len(self.fonts)))
        page = PAGE.replace('{{TITLE}}', html.escape(self.info['title'])) \
            .replace('{{BG}}', bg).replace('{{NSR}}', nsr).replace('{{FONTS}}', font_css) \
            .replace('{{TOPICS}}', '\n'.join(topics_html)) \
            .replace('{{DATA}}', json.dumps(dict(start=start, index=index, titles=nav,
                                                 pics={p['key']: p['src'] for p in self.pictures.values() if p}))
                     .replace('</', '<\\/'))
        return page


PAGE = r'''<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{{TITLE}}</title>
<style>
html{background:{{BG}}}
body{margin:0;background:{{BG}};color:#000;font:9pt Arial,sans-serif}
#page{max-width:600px;margin:0 auto;background:{{BG}};min-height:100vh;box-shadow:0 0 24px rgba(0,0,0,.18)}
.nsr{background:{{NSR}};position:sticky;top:0;z-index:5;padding:2px 0 0 19px;overflow:hidden}
.sr{padding:0 10px 24px 19px}
.p{margin:0;white-space:normal;overflow-wrap:break-word}
.p.e{min-height:1em}
.hang{display:inline-block;text-indent:0}
.tab{display:inline-block;width:2.5em}
.pic{display:inline-block;position:relative;vertical-align:bottom;text-indent:0;max-width:100%}
.pic img{display:block;max-width:100%;height:auto}
.pic.pl{float:left;margin:0 8px 4px 0}
.pic.pr{float:right;margin:0 0 4px 8px}
.sr::after{content:"";display:block;clear:both}
a.hs{position:absolute;display:block;cursor:pointer}
a{cursor:pointer;color:inherit}
a.hl{color:#008000 !important;text-decoration:underline}
a.hl *{color:#008000 !important}
a.pop.hl{text-decoration:underline dotted}
a.hn{text-decoration:none}
.nsr a.hn:hover{text-decoration:underline}
table.ht{border-collapse:collapse;margin:0}
table.ht td{vertical-align:top;padding:0}
.btn{width:14px;height:14px;padding:0;vertical-align:middle;border:1px solid #888;border-radius:2px;
  background:linear-gradient(#fafafa,#d4d4d4);cursor:pointer;margin:0 2px 0 0;font-size:7pt;line-height:1}
video.mci{display:block;max-width:100%;height:auto;background:#000}
.missing{color:#888}
{{FONTS}}
#overlay{position:fixed;inset:0;background:rgba(0,0,0,.08);z-index:20}
#popup{position:fixed;z-index:21;background:#ffffe8;border:1px solid #444;box-shadow:3px 3px 10px rgba(0,0,0,.35);
  padding:6px 10px;max-width:min(560px,92vw);max-height:80vh;overflow:auto}
#popup .sr{padding:0}
#panel{position:fixed;z-index:21;inset:40px 0 auto 0;margin:0 auto;width:min(480px,94vw);background:#f4f4f8;color:#222;
  border:1px solid #535372;box-shadow:0 8px 30px rgba(0,0,0,.35);font:10pt Arial,sans-serif;border-radius:4px}
#panel h2{margin:0;padding:8px 12px;font-size:11pt;background:#535372;color:#fff;border-radius:4px 4px 0 0}
#panel input{box-sizing:border-box;width:calc(100% - 24px);margin:10px 12px 6px;padding:5px 6px;font:10pt Arial,sans-serif}
#panel ul{list-style:none;margin:0;padding:0 0 8px;max-height:min(60vh,420px);overflow:auto}
#panel li{padding:3px 14px;cursor:pointer}
#panel li:hover,#panel li.sel{background:#000080;color:#fff}
#panel li.sub{padding-left:30px}
#panel .none{padding:6px 14px;color:#666}
@media print{#page{box-shadow:none;max-width:none}.nsr{position:static}}
</style></head>
<body><div id="page">
{{TOPICS}}
</div>
<script>
const DATA={{DATA}};
let cur=null;
const $=id=>document.getElementById(id);
function show(id,push){
  const t=$(id);if(!t||!t.classList.contains('topic'))return;
  closeAll();
  if(cur)cur.hidden=true;
  cur=t;t.hidden=false;
  document.title=(t.dataset.title?t.dataset.title+' - ':'')+{{TITLE_JSON}};
  window.scrollTo(0,0);
  t.querySelectorAll('video').forEach(v=>{v.currentTime=0;v.play().catch(()=>{});});
}
function route(){
  let id=location.hash.slice(1);
  if(!$(id)||!$(id).classList.contains('topic'))id=DATA.start;
  const t=$(id);
  if(t&&t.dataset.popuponly&&cur){showPopup(id);return;}
  show(id);
}
function closeAll(){['overlay','popup','panel'].forEach(i=>{const e=$(i);if(e)e.remove();});}
function overlay(){const o=document.createElement('div');o.id='overlay';o.onclick=closeAll;document.body.appendChild(o);}
function place(el,ev){
  const r=el.getBoundingClientRect();let x=16,y=16;
  if(ev&&ev.clientX!==undefined){x=ev.clientX-r.width/2;y=ev.clientY+12;}
  x=Math.max(8,Math.min(x,innerWidth-r.width-8));
  if(y+r.height>innerHeight-8)y=Math.max(8,(ev?ev.clientY:innerHeight)-r.height-12);
  el.style.left=x+'px';el.style.top=y+'px';
}
function showPopup(id,ev){
  const t=$(id);if(!t)return;
  closeAll();overlay();
  const p=document.createElement('div');p.id='popup';
  p.innerHTML=t.querySelector('.sr').innerHTML;
  p.onclick=e=>{if(!e.target.closest('a,button,video'))closeAll();};
  document.body.appendChild(p);place(p,ev);
  p.querySelectorAll('video').forEach(v=>v.play().catch(()=>{}));
}
function panel(title,items,filterable){
  closeAll();overlay();
  const p=document.createElement('div');p.id='panel';
  p.innerHTML='<h2></h2>'+(filterable?'<input type="search" placeholder="Type a word to find">':'')+'<ul></ul>';
  p.querySelector('h2').textContent=title;
  document.body.appendChild(p);
  const ul=p.querySelector('ul'),inp=p.querySelector('input');
  function fill(q){
    ul.innerHTML='';
    const list=typeof items==='function'?items(q||''):items;
    if(!list.length){ul.innerHTML='<div class="none">Nothing found.</div>';return;}
    list.slice(0,400).forEach(([label,id,sub])=>{
      const li=document.createElement('li');li.textContent=label;if(sub)li.className='sub';
      if(id)li.onclick=()=>{closeAll();location.hash=id;};
      ul.appendChild(li);
    });
  }
  fill('');
  if(inp){inp.oninput=()=>fill(inp.value);inp.focus();
    inp.onkeydown=e=>{if(e.key==='Enter'){const li=ul.querySelector('li');if(li&&li.onclick)li.onclick();}};}
}
function titleOf(id){const t=$(id);return t?(t.dataset.title||id):id;}
function indexItems(q){
  q=q.toLowerCase();const out=[];
  DATA.index.forEach(([kw,ids])=>{
    if(q&&!kw.toLowerCase().includes(q))return;
    if(ids.length===1)out.push([kw,ids[0]]);
    else{out.push([kw,null]);ids.forEach(id=>out.push([titleOf(id),id,1]));}
  });
  return out;
}
const textCache={};
function findItems(q){
  q=q.trim().toLowerCase();
  if(!q)return DATA.titles.map(([id,t])=>[t,id]);
  const words=q.split(/\s+/);
  return DATA.titles.filter(([id,t])=>{
    const s=textCache[id]||(textCache[id]=($(id).textContent||'').toLowerCase());
    return words.every(w=>s.includes(w));
  }).map(([id,t])=>[t,id]);
}
function cmd(c){
  if(c==='back')history.back();
  else if(c==='next'||c==='prev'){const n=cur&&cur.dataset[c];if(n)location.hash=n;}
  else if(c==='index')panel('Index',indexItems,true);
  else if(c==='find')panel('Find',findItems,true);
  else if(c==='print')window.print();
  else if(c==='contents')location.hash=DATA.start;
}
document.addEventListener('click',e=>{
  const a=e.target.closest('[data-popup],[data-cmd],[data-related]');
  if(!a)return;
  e.preventDefault();
  if(a.dataset.popup)showPopup(a.dataset.popup,e);
  else if(a.dataset.cmd)cmd(a.dataset.cmd);
  else if(a.dataset.related){
    const ids=JSON.parse(a.dataset.related).filter(id=>!cur||id!==cur.id);
    if(ids.length===1){closeAll();location.hash=ids[0];}
    else panel('Related Topics',ids.map(id=>[titleOf(id),id]));
  }
});
document.addEventListener('keydown',e=>{if(e.key==='Escape')closeAll();});
document.querySelectorAll('img[data-bm]').forEach(i=>{i.src=DATA.pics[i.dataset.bm]||'';});
window.addEventListener('hashchange',route);
route();
</script>
</body></html>
'''


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(src)[0] + '.html'
    c = Converter(src)
    page = c.convert().replace('{{TITLE_JSON}}', json.dumps(c.info['title']))
    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write(page)
    print('%s: %d topics, %d pictures, %d videos, %d index keywords -> %s (%d KB)' % (
        src, len(c.topics), sum(1 for p in c.pictures.values() if p),
        sum(1 for v in c.videos.values() if v), len(c.keywords), dst, len(page) // 1024))


if __name__ == '__main__':
    main()
