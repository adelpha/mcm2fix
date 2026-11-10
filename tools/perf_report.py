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
    def __init__(self, dll):
        self.dll = dll
        self.base = 0
        self.cache = {}
        if dll:
            out = subprocess.run(["objdump", "-p", dll], capture_output=True, text=True).stdout
            m = re.search(r"ImageBase\s+([0-9a-f]+)", out)
            self.base = int(m.group(1), 16) if m else 0x10000000

    def name(self, off):
        if not self.dll:
            return f"mcm2fix+{off:x}"
        if off not in self.cache:
            out = subprocess.run(["i686-w64-mingw32-addr2line", "-f", "-C", "-e", self.dll, hex(self.base + off)],
                                 capture_output=True, text=True).stdout.split("\n")
            fn = out[0] if out else "?"
            fn = re.sub(r"\(.*", "", fn)
            self.cache[off] = fn
        return self.cache[off]


def report_samples(path, exe, dll):
    em, dm = ExeMap(exe), DllMap(dll)
    zones = {}
    rows = defaultdict(list)
    for line in open(path):
        if line.startswith("#"):
            print(line.rstrip())
            continue
        p = line.split()
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
    print("\n== inclusive: game functions on the stack (call site -> calling function)")
    agg = defaultdict(int)
    for zone, where, n in rows["incl"]:
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
