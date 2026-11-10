#!/usr/bin/env python3
"""Summarise a Profile=1 run (see code/src/prof.cpp).

    tools/perf_report.py perf-<stamp>.csv [perf-samples-<stamp>.txt]
        [--exe Mcm2_fixed.exe] [--dll mcm2fix.dll (unstripped)]

Prints frame-time statistics per state (menu / race), the cost of each timed
section, and the sampling profile with game addresses grouped by function
(function starts = call targets in the exe's disassembly, via objdump) and
mcm2fix addresses named with addr2line.
"""
import argparse
import bisect
import csv
import re
import statistics
import subprocess
import sys
from collections import defaultdict


def pct(v, p):
    if not v:
        return 0.0
    s = sorted(v)
    return s[min(len(s) - 1, int(p * (len(s) - 1) + 0.5))]


def report_csv(path):
    rows = list(csv.DictReader(open(path)))
    if not rows:
        print("no frames")
        return
    zones = [k[:-3] for k in rows[0] if k.endswith("_ms") and k not in ("frame_ms", "busy_ms", "gpu_ms_lagged")]
    counters = [k for k in rows[0] if not k.endswith("_ms") and not k.endswith("_n") and k not in ("frame", "t_s", "race", "gpu_ms_lagged")]
    for race in ("1", "0"):
        rs = [r for r in rows if r["race"] == race]
        if len(rs) < 10:
            continue
        ft = [float(r["frame_ms"]) for r in rs]
        busy = [float(r["busy_ms"]) for r in rs]
        gpu = [float(r["gpu_ms_lagged"]) for r in rs if float(r["gpu_ms_lagged"]) >= 0]
        dur = sum(ft) / 1000
        print(f"== {'race' if race == '1' else 'menu/other'}: {len(rs)} frames, {dur:.1f} s, {len(rs) / dur:.1f} fps")
        print(f"   frame ms: mean {statistics.mean(ft):.2f} p50 {pct(ft, .5):.2f} p90 {pct(ft, .9):.2f} "
              f"p99 {pct(ft, .99):.2f} p99.9 {pct(ft, .999):.2f} max {max(ft):.2f}")
        print(f"   busy ms:  mean {statistics.mean(busy):.2f} p50 {pct(busy, .5):.2f} p99 {pct(busy, .99):.2f}")
        if gpu:
            print(f"   gpu ms:   mean {statistics.mean(gpu):.2f} p50 {pct(gpu, .5):.2f} p99 {pct(gpu, .99):.2f} max {max(gpu):.2f}")
        med = pct(ft, .5)
        hitches = [r for r in rs if float(r["frame_ms"]) > max(med * 1.6, med + 3)]
        print(f"   hitches (>1.6x median): {len(hitches)}  ({len(hitches) / dur:.2f}/s)")
        steps = defaultdict(int)
        for r in rs:
            steps[int(r["steps"])] += 1
        print("   sim steps per frame:", dict(sorted(steps.items())))
        print("   zone                 ms/frame   p99     max   calls/frame")
        for z in zones:
            v = [float(r[z + "_ms"]) for r in rs]
            n = [int(r[z + "_n"]) for r in rs]
            if max(v) <= 0:
                continue
            print(f"   {z:18s} {statistics.mean(v):8.3f} {pct(v, .99):7.3f} {max(v):7.2f}   {statistics.mean(n):8.1f}")
        print("   per frame:", ", ".join(f"{c} {statistics.mean(float(r[c]) for r in rs):.1f}" for c in counters))
        # What the worst frames had in common.
        worst = sorted(rs, key=lambda r: -float(r["frame_ms"]))[:15]
        print("   worst frames:")
        for r in worst:
            big = sorted(((float(r[z + "_ms"]), z) for z in zones if z not in ("tick", "flip", "draw", "present")),
                         reverse=True)[:5]
            print(f"     #{r['frame']} t={float(r['t_s']):.2f}s {float(r['frame_ms']):.2f} ms steps {r['steps']} "
                  f"uploads {r['uploads']} readbacks {r['readbacks']} creates {r['tex_creates']}: "
                  + ", ".join(f"{z} {ms:.2f}" for ms, z in big))


class ExeMap:
    def __init__(self, exe):
        self.starts = []
        if not exe:
            return
        out = subprocess.run(["objdump", "-d", "--no-show-raw-insn", exe], capture_output=True, text=True).stdout
        targets = set(int(m.group(1), 16) for m in re.finditer(r"call\s+0x([0-9a-f]+)", out))
        self.starts = sorted(targets)

    def func(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        return self.starts[i] if i >= 0 else a


class DllMap:
    """mcm2fix offsets to function names from the (unstripped) DLL's symbol table."""

    def __init__(self, dll):
        self.syms = []
        if dll:
            out = subprocess.run(["objdump", "-p", dll], capture_output=True, text=True).stdout
            m = re.search(r"ImageBase\s+([0-9a-f]+)", out)
            base = int(m.group(1), 16) if m else 0x10000000
            out = subprocess.run(["i686-w64-mingw32-nm", "-C", "--defined-only", dll], capture_output=True,
                                 text=True).stdout
            for line in out.splitlines():
                p = line.split(None, 2)
                if len(p) == 3 and p[1] in "tT":
                    self.syms.append((int(p[0], 16) - base, p[2]))
            self.syms.sort()
            self.offs = [a for a, _ in self.syms]

    def name(self, off):
        if not self.syms:
            return f"+{off:x}"
        i = bisect.bisect_right(self.offs, off) - 1
        return self.syms[i][1] if i >= 0 else f"+{off:x}"


def report_samples(path, exe, dll):
    em, dm = ExeMap(exe), DllMap(dll)
    zones = {}
    rows = defaultdict(list)
    dll_base, dll_end = 0, 0

    def fix(where):
        # Exact addresses that happen to be odd were written as "anon@<addr>".
        if where.startswith("anon@"):
            a = int(where[5:], 16)
            if 0x401000 <= a < 0x556000:
                return f"exe:{a:06x}"
            if dll_base <= a < dll_end:
                return f"mcm2fix+{a - dll_base:05x}"
        return where

    for line in open(path):
        if line.startswith("#"):
            m = re.search(r"dll base ([0-9a-f]+) code end \+([0-9a-f]+)", line)
            if m:
                dll_base = int(m.group(1), 16)
                dll_end = dll_base + int(m.group(2), 16)
            print(line.rstrip())
            continue
        p = line.split()
        if len(p) == 4:
            p[2] = fix(p[2])
        if p[0] == "zone":
            zones[p[1]] = int(p[2])
        else:
            rows[p[0]].append((p[1], p[2], int(p[3])))
    total = sum(zones.values()) or 1
    print("\n== samples per zone (innermost timed section)")
    for z, n in sorted(zones.items(), key=lambda kv: -kv[1]):
        print(f"   {z:16s} {n:7d} {100 * n / total:5.1f}%")

    def label(where):
        if where.startswith("exe:"):
            a = int(where[4:], 16)
            return f"game fn {em.func(a):06x}" if em.starts else where
        if where.startswith("mcm2fix+"):
            return "mcm2fix " + dm.name(int(where[8:], 16))
        return where

    print("\n== where the game thread was (self), by function")
    agg = defaultdict(int)
    for zone, where, n in rows["self"]:
        agg[label(where)] += n
    for k, n in sorted(agg.items(), key=lambda kv: -kv[1])[:60]:
        print(f"   {100 * n / total:5.1f}% {n:6d}  {k}")
    print("\n== self by zone and function (top 80)")
    agg = defaultdict(int)
    for zone, where, n in rows["self"]:
        agg[(zone, label(where))] += n
    for (z, k), n in sorted(agg.items(), key=lambda kv: -kv[1])[:80]:
        print(f"   {100 * n / total:5.1f}% {z:14s} {k}")
    print("\n== game call sites that called into DLLs (execall), by calling function")
    agg = defaultdict(int)
    for zone, where, n in rows["execall"]:
        if not where.startswith("exe:"):
            continue
        a = int(where[4:], 16)
        agg[(zone, f"{em.func(a):06x} (ret {a:06x})")] += n
    for (z, k), n in sorted(agg.items(), key=lambda kv: -kv[1])[:50]:
        print(f"   {100 * n / total:5.1f}% {z:14s} {k}")
    print("\n== mcm2fix code that called into Windows / driver (dllcall)")
    agg = defaultdict(int)
    for zone, where, n in rows["dllcall"]:
        agg[(zone, label(where))] += n
    for (z, k), n in sorted(agg.items(), key=lambda kv: -kv[1])[:40]:
        print(f"   {100 * n / total:5.1f}% {z:14s} {k}")
    if rows["via"]:
        print("\n== call chains into Windows / drivers (via), top per zone")
        byzone = defaultdict(list)
        for zone, where, n in rows["via"]:
            byzone[zone].append((n, where))
        for z in sorted(byzone, key=lambda z: -sum(n for n, _ in byzone[z])):
            items = sorted(byzone[z], reverse=True)
            tot = sum(n for n, _ in items)
            print(f"   -- {z}: {tot} samples outside game/mcm2fix code")
            for n, where in items[:15]:
                chain = where.split("<")
                if chain[-1].startswith("exe:"):
                    a = int(chain[-1][4:], 16)
                    chain[-1] = f"exe:{a:06x} (fn {em.func(a):06x})" if em.starts else chain[-1]
                print(f"   {100 * n / total:5.1f}% {n:6d}  " + " < ".join(chain))
    print("\n== inclusive: game functions on the stack (call site -> calling function)")
    agg = defaultdict(int)
    for zone, where, n in rows["incl"]:
        if not where.startswith("exe:"):
            continue
        a = int(where[4:], 16)
        agg[em.func(a)] = max(agg[em.func(a)], n)  # a function's sites overlap; keep the largest
    for f, n in sorted(agg.items(), key=lambda kv: -kv[1])[:70]:
        print(f"   {100 * n / total:5.1f}% {n:6d}  {f:06x}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("samples", nargs="?")
    ap.add_argument("--exe")
    ap.add_argument("--dll")
    a = ap.parse_args()
    report_csv(a.csv)
    if a.samples:
        report_samples(a.samples, a.exe, a.dll)


if __name__ == "__main__":
    sys.exit(main())
