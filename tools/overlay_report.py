#!/usr/bin/env python3
"""Render the per-overlay table of docs/overlays-sonic.md from build output.

Usage:
  overlay_report.py [--sizes FILE] [--reached NAME,...]

Reads recomp/out/overlays/stats/<name>.json (written by tools/overlays.py
build) and the psp-gcc -Os .text sizes, one "<name> <bytes>" line per
overlay (default recomp/out/overlays/psp_sizes.txt, written by
recomp/out/overlays/psp_sizes.sh inside WSL).  --reached lists the files the
desktop run's scripted pad reaches.  Prints Markdown: the table, then the
totals per slot.  Only names and numbers; nothing from the disc.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "recomp", "out", "overlays")
SLOT = {0x8C900000: "area", 0x8CB80000: "event", 0x8C920000: "area 2"}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--sizes", default=os.path.join(OUT, "psp_sizes.txt"))
    ap.add_argument("--reached", default="")
    a = ap.parse_args(argv)
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    reached = {n.strip().lower() for n in a.reached.split(",") if n.strip()}
    sizes = {}
    if os.path.exists(a.sizes):
        for line in open(a.sizes):
            n, s = line.split()
            sizes[n] = int(s)
    rows = []
    for p in sorted(glob.glob(os.path.join(OUT, "stats", "*.json"))):
        st = json.load(open(p))
        rows.append(st)
    if not rows:
        sys.exit("overlay_report: no stats; run tools/overlays.py build first")

    print("| File | Role | Slot | Code (file offsets) | Code bytes | Functions found | Bytes decoded | Seeds | C functions | Unresolved `jsr`/`jmp` | MIPS `.text` | Ratio | Flycast trace (found/executed call targets, executed bytes) | Scripted pad |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    tot = {}
    for st in rows:
        n = st["name"]
        base = st["slot"]
        lo, hi = st["code_lo"] - base, st["code_hi"] - base
        code = hi - lo
        mips = sizes.get(n)
        ins = st.get("emit_instructions", 0)
        ratio = "%.2fx" % (mips / (2 * ins)) if mips and ins else "-"
        dec = "%s / %s (%.1f %%)" % (format(st["decoded"], ","), format(st["code_bytes_dream"], ","),
                                     100 * st["decoded"] / st["code_bytes_dream"]) if "decoded" in st else "-"
        if st.get("trace_bytes", 0) >= 64:
            tr = "%d/%d calls, %s B" % (st["trace_found"], st["trace_calls"], format(st["trace_bytes"], ","))
        elif st.get("trace_bytes"):
            # a file that starts with the same bytes as another area file
            tr = "no (%d B match by bytes only)" % st["trace_bytes"]
        else:
            tr = "no"
        unres = st.get("emit_calls_indirect", 0) + st.get("emit_ijumps", 0)
        print("| `%s` | %s | %s | `0x%X`–`0x%X` | %s | %d | %s | %d | %d | %d | %s | %s | %s | %s |" % (
            os.path.basename(st["file"]).replace(".PRS", ""), st["role"], SLOT.get(base, hex(base)), lo, hi,
            format(code, ","), st["functions"], dec, st.get("seeds", 0), st.get("emit_functions", 0), unres,
            format(mips, ",") if mips else "-", ratio, tr, "**yes**" if n in reached else "no"))
        t = tot.setdefault(SLOT.get(base, hex(base)), {"n": 0, "mips": 0, "max": (0, ""), "code": 0})
        t["n"] += 1
        t["code"] += code
        t["mips"] += mips or 0
        if (mips or 0) > t["max"][0]:
            t["max"] = (mips, os.path.basename(st["file"]).replace(".PRS", ""))
    print()
    print("| Slot | Files | SH-4 code bytes | MIPS `.text`, sum | Largest file |")
    print("|---|---|---|---|---|")
    allm = allc = alln = 0
    for k, t in tot.items():
        print("| %s | %d | %s | %s | `%s`, %s |" % (k, t["n"], format(t["code"], ","), format(t["mips"], ","),
                                                  t["max"][1], format(t["max"][0], ",")))
        allm += t["mips"]
        allc += t["code"]
        alln += t["n"]
    print("| **all** | **%d** | **%s** | **%s** | |" % (alln, format(allc, ","), format(allm, ",")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
