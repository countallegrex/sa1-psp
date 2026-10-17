#!/usr/bin/env python3
"""Diff a Flycast execution trace against the static function list (M0 exit test).

Usage:
  trace_diff.py TRACE FUNCTIONS_JSON [--md OUT.md] [--seeds OUT.txt]
                [--executed OUT.txt]

TRACE           text file written by the patched Flycast (docs/m0-trace.md):
                  B vaddr size type branch   one line per distinct compiled block
                  D site target              one line per distinct dynamic jump/call
                Several sessions may be appended to the same file.
FUNCTIONS_JSON  Dream-Recomp's <id>.functions.json (entry/end in link space).

Nothing is read from the game binary. Addresses are normalised to the P0
link-space spelling (0x0Cxxxxxx) that the function list uses.
"""
from __future__ import annotations

import argparse
import bisect
import json
import sys
from collections import Counter

IMAGE_LO = 0x0C010000
IMAGE_HI = 0x0C010000 + 6_735_488          # 1ST_READ.BIN, US v1.005
BLOCKS = {                                   # docs/seam.md
    "A": (0x0C010000, 0x0C111200),
    "B": (0x0C5FA000, 0x0C66C000),
}

# Flycast BlockEndType (core/hw/sh4/dyna/decoder.h): (class << 3) | (sub << 1) | v
BET_STATIC_JUMP, BET_STATIC_CALL = 0, 2
BET_DYNAMIC_JUMP, BET_DYNAMIC_CALL = 8, 10


def norm(a: int) -> int:
    """P0/P1/P2/P3 spelling -> the 29-bit physical address (0x0Cxxxxxx for RAM)."""
    return a & 0x1FFFFFFF


def in_image(a: int) -> bool:
    return IMAGE_LO <= a < IMAGE_HI


def block_of(a: int) -> str:
    for name, (lo, hi) in BLOCKS.items():
        if lo <= a < hi:
            return name
    return "data" if in_image(a) else "outside"


def read_trace(path: str):
    blocks: dict[int, tuple[int, int, int]] = {}   # vaddr -> (size, type, branch)
    edges: set[tuple[int, int]] = set()
    sessions = 0
    with open(path, encoding="ascii", errors="replace") as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "#":
                sessions += "start" in line
                continue
            if parts[0] == "B" and len(parts) == 5:
                va, size, typ, br = (int(p, 16) for p in parts[1:])
                va = norm(va)
                old = blocks.get(va)
                if old is None or size > old[0]:
                    blocks[va] = (size, typ, norm(br))
            elif parts[0] == "D" and len(parts) == 3:
                edges.add((norm(int(parts[1], 16)), norm(int(parts[2], 16))))
    return blocks, edges, sessions


def read_functions(path: str):
    with open(path, encoding="utf-8") as f:
        fns = json.load(f)["functions"]
    out = sorted((int(fn["entry"], 16), int(fn["end"], 16), fn.get("origin", "")) for fn in fns)
    return out


def merge(ranges):
    out = []
    for lo, hi in sorted(ranges):
        if out and lo <= out[-1][1]:
            out[-1][1] = max(out[-1][1], hi)
        else:
            out.append([lo, hi])
    return out


def total(ranges) -> int:
    return sum(hi - lo for lo, hi in ranges)


def intersect(a, b) -> int:
    i = j = n = 0
    while i < len(a) and j < len(b):
        lo = max(a[i][0], b[j][0])
        hi = min(a[i][1], b[j][1])
        if lo < hi:
            n += hi - lo
        if a[i][1] < b[j][1]:
            i += 1
        else:
            j += 1
    return n


class FnIndex:
    def __init__(self, fns):
        self.fns = fns
        self.entries = {e for e, _, _ in fns}
        self.starts = [e for e, _, _ in fns]
        self.cover = merge((e, x) for e, x, _ in fns)
        self.cover_lo = [lo for lo, _ in self.cover]

    def covered(self, a: int) -> bool:
        i = bisect.bisect_right(self.cover_lo, a) - 1
        return i >= 0 and a < self.cover[i][1]

    def overlapping(self, lo: int, hi: int):
        """Indices of functions whose [entry, end) overlaps [lo, hi)."""
        i = bisect.bisect_right(self.starts, hi - 1)
        # function ranges can nest/overlap, so scan back a bounded distance
        k = i - 1
        while k >= 0 and k >= i - 64:
            e, x, _ = self.fns[k]
            if e < hi and lo < x:
                yield k
            k -= 1


def analyse(trace_path: str, fn_path: str):
    blocks, edges, sessions = read_trace(trace_path)
    fns = read_functions(fn_path)
    idx = FnIndex(fns)

    site_type = {va + size - 4: typ for va, (size, typ, _) in blocks.items()
                 if typ in (BET_DYNAMIC_CALL, BET_DYNAMIC_JUMP)}

    static_calls = {br for size, typ, br in blocks.values() if typ == BET_STATIC_CALL}
    static_jumps = {br for size, typ, br in blocks.values() if typ == BET_STATIC_JUMP}
    dyn_calls = {t for s, t in edges if site_type.get(s) == BET_DYNAMIC_CALL}
    dyn_jumps = {t for s, t in edges if site_type.get(s) == BET_DYNAMIC_JUMP}
    dyn_call_sites = {s for s, t in edges if site_type.get(s) == BET_DYNAMIC_CALL}

    # Only targets that were actually reached count (a compiled block exists there).
    reached = set(blocks)
    calls = {a for a in static_calls | dyn_calls if a in reached and in_image(a)}
    # Code the game loads at run time (overlays above the image; IP.BIN and
    # syscall glue below it). Static discovery on 1ST_READ.BIN never sees it.
    def in_ram(a: int) -> bool:
        return 0x0C000000 <= a < 0x0D000000
    calls_low = {a for a in static_calls | dyn_calls
                 if a in reached and in_ram(a) and a < IMAGE_LO}
    calls_ovl = {a for a in static_calls | dyn_calls
                 if a in reached and in_ram(a) and a >= IMAGE_HI}
    ovl_bytes = total(merge((va, va + size) for va, (size, _, _) in blocks.items()
                            if in_ram(va) and va >= IMAGE_HI))
    low_bytes = total(merge((va, va + size) for va, (size, _, _) in blocks.items()
                            if in_ram(va) and va < IMAGE_LO))
    exec_ranges = merge((va, va + size) for va, (size, _, _) in blocks.items() if in_image(va))
    exec_bytes = total(exec_ranges)
    exec_in_known = intersect(exec_ranges, idx.cover)

    # Functions entered: entry is a call target, or any executed block lies in it.
    called_known = {a for a in calls if a in idx.entries}
    touched = set()
    for va, (size, _, _) in blocks.items():
        if in_image(va):
            for k in idx.overlapping(va, va + size):
                touched.add(k)
    executed_known = {fns[k][0] for k in touched} | called_known

    # Executed but not discovered.
    new_call_mid = sorted(a for a in calls if a not in idx.entries and idx.covered(a))
    new_call_gap = sorted(a for a in calls if a not in idx.entries and not idx.covered(a))
    tail_gap = sorted(a for a in (static_jumps | dyn_jumps)
                      if a in reached and in_image(a) and not idx.covered(a)
                      and a not in calls)
    seeds = sorted(set(new_call_mid) | set(new_call_gap) | set(tail_gap))

    # Executed code with no discovered function and no seed covering its start.
    gap_runs = []
    for lo, hi in exec_ranges:
        a = lo
        # split executed runs at discovered coverage
        while a < hi:
            if idx.covered(a):
                a += 2
                continue
            b = a
            while b < hi and not idx.covered(b):
                b += 2
            gap_runs.append((a, b))
            a = b
    seed_set = set(seeds)
    unseeded_runs = [(lo, hi) for lo, hi in gap_runs
                     if not any(lo <= s < hi for s in seed_set)]

    outside = sorted(va for va in blocks if not in_image(va))
    in_data = sorted(va for va in blocks if in_image(va) and block_of(va) == "data")

    never = [(e, x, o) for e, x, o in fns if e not in executed_known]
    never_by_block = Counter(block_of(e) for e, _, _ in never)
    # Union, not sum: discovered ranges nest, and an executed function's
    # range can contain a never-executed entry.
    exec_fn_cover = merge((e, x) for e, x, _ in fns if e in executed_known)
    never_bytes_by_block = Counter()
    for name in BLOCKS:
        nr = merge((e, x) for e, x, _ in never if block_of(e) == name)
        never_bytes_by_block[name] = total(nr) - intersect(nr, exec_fn_cover)
    exec_by_block = Counter(block_of(e) for e in executed_known)

    # The exit-test ratio: of the function entries the run actually called,
    # how many had static discovery already found?
    call_ratio = len(called_known) / len(calls) if calls else 0.0
    byte_ratio = exec_in_known / exec_bytes if exec_bytes else 0.0

    return {
        "sessions": sessions,
        "blocks": len(blocks),
        "blocks_in_image": sum(1 for va in blocks if in_image(va)),
        "edges": len(edges),
        "dyn_call_sites": len(dyn_call_sites),
        "dyn_call_targets": len({a for a in dyn_calls if in_image(a)}),
        "functions": len(fns),
        "calls": len(calls),
        "calls_low": len(calls_low),
        "calls_ovl": len(calls_ovl),
        "ovl_bytes": ovl_bytes,
        "low_bytes": low_bytes,
        "all_ratio": len(called_known) / (len(calls) + len(calls_ovl)) if calls else 0.0,
        "called_known": len(called_known),
        "executed_known": len(executed_known),
        "exec_by_block": exec_by_block,
        "exec_bytes": exec_bytes,
        "exec_in_known": exec_in_known,
        "call_ratio": call_ratio,
        "byte_ratio": byte_ratio,
        "new_call_mid": new_call_mid,
        "new_call_gap": new_call_gap,
        "tail_gap": tail_gap,
        "seeds": seeds,
        "gap_runs": gap_runs,
        "unseeded_runs": unseeded_runs,
        "outside": outside,
        "in_data": in_data,
        "never": len(never),
        "never_by_block": never_by_block,
        "never_bytes_by_block": never_bytes_by_block,
        "executed_entries": sorted(executed_known | set(seeds)),
    }


def h(a: int) -> str:
    return f"0x{a:08X}"


def report(r) -> str:
    L = []
    L.append(f"Trace: {r['sessions']} session(s), {r['blocks']:,} distinct blocks "
             f"({r['blocks_in_image']:,} in the image), {r['edges']:,} distinct dynamic edges "
             f"from {r['dyn_call_sites']:,} dynamic call sites.")
    L.append("")
    L.append("| Measure | Value |")
    L.append("|---|---|")
    L.append(f"| Distinct call targets executed (in image) | {r['calls']:,} |")
    L.append(f"| of which in the static list | {r['called_known']:,} |")
    L.append(f"| **Coverage, call targets found statically** | **{100*r['call_ratio']:.2f} %** |")
    L.append(f"| Distinct call targets executed in run-time-loaded overlays (above the image) | {r['calls_ovl']:,} |")
    L.append(f"| Coverage, call targets found statically, image + overlays | {100*r['all_ratio']:.2f} % |")
    L.append(f"| Distinct call targets executed below the image (IP.BIN, syscall glue) | {r['calls_low']:,} |")
    L.append(f"| Executed code bytes (in image) | {r['exec_bytes']:,} |")
    L.append(f"| Executed code bytes in overlays / below the image | {r['ovl_bytes']:,} / {r['low_bytes']:,} |")
    L.append(f"| of which inside discovered functions | {r['exec_in_known']:,} |")
    L.append(f"| **Coverage, executed bytes inside discovered functions** | **{100*r['byte_ratio']:.2f} %** |")
    L.append(f"| Discovered functions executed (called or any block inside) | {r['executed_known']:,} of {r['functions']:,} "
             f"(A {r['exec_by_block'].get('A', 0):,}, B {r['exec_by_block'].get('B', 0):,}) |")
    L.append(f"| Discovered functions never executed | {r['never']:,} "
             f"(A {r['never_by_block'].get('A', 0):,} = {r['never_bytes_by_block'].get('A', 0):,} bytes; "
             f"B {r['never_by_block'].get('B', 0):,} = {r['never_bytes_by_block'].get('B', 0):,} bytes not shared with an executed function) |")
    L.append(f"| Executed call targets inside a discovered function, not at its entry | {len(r['new_call_mid']):,} |")
    L.append(f"| Executed call targets outside every discovered function | {len(r['new_call_gap']):,} |")
    L.append(f"| Executed jump targets outside every discovered function | {len(r['tail_gap']):,} |")
    L.append(f"| **New seeds (union of the three rows above)** | **{len(r['seeds']):,}** |")
    L.append(f"| Executed runs outside discovered functions | {len(r['gap_runs']):,} "
             f"({sum(b - a for a, b in r['gap_runs']):,} B) |")
    L.append(f"| of which with no seed inside | {len(r['unseeded_runs']):,} |")
    L.append(f"| Blocks executed in the data region of the image | {len(r['in_data']):,} |")
    L.append(f"| Blocks executed outside the image | {len(r['outside']):,} |")
    L.append("")
    if r["unseeded_runs"]:
        L.append("Executed runs outside discovered functions with no seed (first 40):")
        L.append("")
        for a, b in r["unseeded_runs"][:40]:
            L.append(f"- {h(a)}-{h(b)} ({b - a} B, block {block_of(a)})")
        L.append("")
    if r["outside"]:
        lo, hi = r["outside"][0], r["outside"][-1]
        L.append(f"Blocks outside the image span {h(lo)}-{h(hi)}.")
        L.append("")
    return "\n".join(L)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("trace")
    ap.add_argument("functions_json")
    ap.add_argument("--md", help="write the summary table here as well as stdout")
    ap.add_argument("--seeds", help="write new seed addresses here, one per line")
    ap.add_argument("--executed", help="write executed function entries (known + new) here")
    a = ap.parse_args(argv)

    r = analyse(a.trace, a.functions_json)
    text = report(r)
    print(text)
    if a.md:
        with open(a.md, "w", encoding="utf-8", newline="\n") as f:
            f.write(text + "\n")
    if a.seeds:
        with open(a.seeds, "w", encoding="ascii", newline="\n") as f:
            for s in r["seeds"]:
                f.write(f"{h(s)}\n")
    if a.executed:
        with open(a.executed, "w", encoding="ascii", newline="\n") as f:
            for s in r["executed_entries"]:
                f.write(f"{h(s)}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
