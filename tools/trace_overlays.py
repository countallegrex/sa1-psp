#!/usr/bin/env python3
"""Find which disc files the code executed outside 1ST_READ.BIN came from.

Usage:
  trace_overlays.py TRACE DATA_DIR [--prs] [--md OUT.md]

TRACE     trace from the patched Flycast (docs/m0-trace.md); uses its
          "M vaddr hhhh hhhh ..." records (first bytes of each block that ran
          outside the 1ST_READ.BIN image) and "B" records (block sizes).
DATA_DIR  the extracted disc filesystem (outside the repo, read-only).
--prs     also try every *.PRS file, decompressed (slower).

For each cluster of out-of-image code, sampled blocks are searched for in
every candidate file; the (file, load address) pair with the most hits wins,
and then every block of the cluster is checked against that file. Clusters
that host several overlays over time are split by repeating the vote on the
blocks still unexplained. Prints a table of overlays: file, load address,
size, executed blocks and bytes. Nothing from the files is written out.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from collections import Counter, defaultdict

IMAGE_LO = 0x0C010000
IMAGE_HI = 0x0C010000 + 6_735_488
CLUSTER_GAP = 0x10000


def prs_decompress(src: bytes) -> bytes:
    """SEGA PRS (LZ77 variant used across Dreamcast titles)."""
    out = bytearray()
    i = 0
    cmd = 0
    left = 0

    def bit() -> int:
        nonlocal i, cmd, left
        if left == 0:
            cmd = src[i]
            i += 1
            left = 8
        b = cmd & 1
        cmd >>= 1
        left -= 1
        return b

    try:
        while True:
            if bit():
                out.append(src[i])
                i += 1
                continue
            if bit():
                v = src[i] | (src[i + 1] << 8)
                i += 2
                if v == 0:
                    break
                off = (v >> 3) - 0x2000
                size = v & 7
                if size == 0:
                    size = src[i] + 1
                    i += 1
                else:
                    size += 2
            else:
                size = ((bit() << 1) | bit()) + 2
                off = src[i] - 0x100
                i += 1
            start = len(out) + off
            if start < 0:
                return b""
            for k in range(size):
                out.append(out[start + k])
    except IndexError:
        return b""
    return bytes(out)


def read_trace(path: str):
    sizes: dict[int, int] = {}
    mem: dict[int, bytes] = {}
    with open(path, encoding="ascii", errors="replace") as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == "B" and len(p) == 5:
                va = int(p[1], 16) & 0x1FFFFFFF
                sizes[va] = max(sizes.get(va, 0), int(p[2], 16))
            elif p[0] == "M" and len(p) >= 2:
                va = int(p[1], 16) & 0x1FFFFFFF
                bs = b"".join(struct.pack("<H", int(x, 16)) for x in p[2:])
                if len(bs) > len(mem.get(va, b"")):
                    mem[va] = bs
    return sizes, mem


def clusters(addrs):
    out = []
    for a in sorted(addrs):
        if out and a - out[-1][-1] < CLUSTER_GAP:
            out[-1].append(a)
        else:
            out.append([a])
    return out


def load_candidates(data_dir: str, prs: bool):
    files = {}
    for root, _, names in os.walk(data_dir):
        for n in names:
            low = n.lower()
            path = os.path.join(root, n)
            rel = os.path.relpath(path, data_dir).replace("\\", "/")
            if low == "1st_read.bin":
                continue
            if low.endswith(".bin"):
                with open(path, "rb") as f:
                    files[rel] = f.read()
            elif prs and low.endswith(".prs"):
                with open(path, "rb") as f:
                    d = prs_decompress(f.read())
                if d:
                    files[rel + " (PRS)"] = d
    return files


def matches(data: bytes, base: int, va: int, bs: bytes) -> bool:
    o = va - base
    return 0 <= o and o + len(bs) <= len(data) and data[o:o + len(bs)] == bs


def attribute(cluster, mem, files, samples=40):
    """Return [(file, base, [vaddrs])] and the list of unexplained vaddrs."""
    pending = [va for va in cluster if len(mem.get(va, b"")) >= 12]
    short = [va for va in cluster if va not in pending]
    found = []
    while pending:
        pick = sorted(pending, key=lambda va: -len(mem[va]))
        step = max(1, len(pick) // samples)
        votes = Counter()
        for va in pick[::step][:samples]:
            bs = mem[va]
            for name, data in files.items():
                o = data.find(bs)
                while o >= 0:
                    votes[(name, va - o)] += 1
                    o = data.find(bs, o + 2)
                    if o > 0 and votes[(name, va - o)] > 4:
                        break
        if not votes:
            break
        (name, base), n = votes.most_common(1)[0]
        if n < 3:
            break
        data = files[name]
        hit = [va for va in pending if matches(data, base, va, mem[va])]
        if not hit:
            break
        found.append((name, base, hit))
        pending = [va for va in pending if va not in set(hit)]
    # short records: attribute to an overlay already found if the bytes agree
    rest = list(pending)
    for va in short:
        bs = mem.get(va, b"")
        for name, base, hit in found:
            if bs and matches(files[name], base, va, bs):
                hit.append(va)
                break
        else:
            rest.append(va)
    return found, rest


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("trace")
    ap.add_argument("data_dir")
    ap.add_argument("--prs", action="store_true")
    ap.add_argument("--md")
    a = ap.parse_args(argv)

    sizes, mem = read_trace(a.trace)
    outside = [va for va in sizes
               if 0x0C000000 <= va < 0x0D000000 and not (IMAGE_LO <= va < IMAGE_HI)]
    files = load_candidates(a.data_dir, a.prs)

    per_overlay = defaultdict(list)
    unexplained = []
    for cl in clusters(outside):
        found, rest = attribute(cl, mem, files)
        for name, base, hit in found:
            per_overlay[(name, base)].extend(hit)
        if rest:
            unexplained.append((min(rest), max(rest), len(rest), sum(sizes[v] for v in rest)))
    rows = []
    for (name, base), hit in per_overlay.items():
        exe = sum(sizes[v] for v in hit)
        top = max(hit)
        rows.append((name, base, len(files[name]), len(hit), exe, min(hit), top + sizes[top]))

    L = ["| File | Load address | File bytes | Blocks executed | Bytes executed | Executed span |",
         "|---|---|---|---|---|---|"]
    for name, base, size, nb, eb, lo, hi in sorted(rows, key=lambda r: r[1]):
        L.append(f"| `{name}` | `0x{0x80000000 | base:08X}` | {size:,} | {nb:,} | {eb:,} | "
                 f"`0x{lo:08X}`-`0x{hi:08X}` |")
    if unexplained:
        L.append("")
        L.append("Unattributed out-of-image blocks (no file matched):")
        L.append("")
        for lo, hi, nb, eb in unexplained:
            L.append(f"- `0x{lo:08X}`-`0x{hi:08X}`: {nb:,} blocks, {eb:,} bytes")
    other = [va for va in sizes if not (0x0C000000 <= va < 0x0D000000)]
    if other:
        L.append("")
        L.append(f"Blocks outside main RAM: {len(other)} "
                 f"({', '.join(f'0x{v:08X}' for v in sorted(other)[:8])})")
    text = "\n".join(L)
    print(text)
    if a.md:
        with open(a.md, "w", encoding="utf-8", newline="\n") as f:
            f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
