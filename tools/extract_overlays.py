#!/usr/bin/env python3
"""Decompress the code overlays the recompiler needs (docs/m1-overlays.md).

Usage:
  extract_overlays.py DATA_DIR OUT_DIR [NAME ...]

DATA_DIR  the extracted disc filesystem (read-only; never written).
OUT_DIR   where the decompressed files go, e.g. ../private/overlays/sa1_us_1005.
          Must be outside the repository and outside DATA_DIR.
NAME      overlay names (file stem under SONICADV/, e.g. ADV00); default: the
          set in recomp/sa1_us_1005/overlays/*.toml.

Writes OUT_DIR/<NAME>.bin (the decompressed file, byte for byte what the game
copies to its slot) and prints size, SHA-256 and the estimated end of the code
part of each (find_code_end). Nothing is written inside the repository.

Each overlay is one SHC link: an optional 32-byte header of entry pointers
(ADVERTISE and the EVnnnn files; padded with 0 or 0xFF), code, `nop`-padded
to 32 bytes, then data. find_code_end walks the code by recursive descent
from the first code byte and from every SHC prologue (`sts.l pr,@-r15`) that has a matching epilogue
(`lds.l @r15+,pr` then `rts`), takes the furthest instruction or literal-pool
word reached, rounds up to 32 bytes, and then keeps extending over the next
32-byte chunk while a linear decode from it reaches an `rts` with no invalid
word on the way (leaf functions with no prologue, reached only through
pointers), or while an SHC prologue follows `nop` padding within the next
64 bytes (a function after a literal pool, as in ADV0100).
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import os
import re
import struct
import sys
from bisect import bisect_left, bisect_right

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "recomp", "emitter"))

from trace_overlays import prs_decompress  # noqa: E402
from sh4emit.decode import Instr  # noqa: E402

CONFIG_DIR = os.path.join(REPO, "recomp", "sa1_us_1005", "overlays")


def configured_names() -> list[str]:
    names = []
    for path in sorted(glob.glob(os.path.join(CONFIG_DIR, "*.toml"))):
        with open(path, encoding="utf-8") as f:
            m = re.search(r'^file\s*=\s*"([^"]+)"', f.read(), re.M)
        if m:
            names.append(os.path.splitext(os.path.basename(m.group(1)))[0])
    return names


RETURNS = ("RTS", "RTE", "JMP", "BRAF", "BRA")


def descend(hw, seeds, limit, seen):
    """Recursive descent over halfwords hw; returns (reach, pool_end, clean)."""
    reach = pool = 0
    clean = True
    work = list(seeds)
    while work:
        o = work.pop()
        while 0 <= o < limit and o not in seen:
            seen.add(o)
            ins = Instr(o, hw[o >> 1])
            if ins.op == "INVALID":
                clean = False
                break
            reach = max(reach, o + 2)
            if ins.op in ("MOV_L_PC", "MOV_W_PC", "MOVA"):
                a = ins.pcrel_address()
                if 0 <= a < limit:
                    pool = max(pool, a + (2 if ins.op == "MOV_W_PC" else 4))
            if ins.op in ("BT", "BF", "BT_S", "BF_S", "BRA", "BSR"):
                t = ins.branch_target()
                if 0 <= t < limit:
                    work.append(t)
            if ins.op in RETURNS:
                seen.add(o + 2)
                reach = max(reach, o + 4)   # delay slot
                break
            o += 2
    return reach, pool, clean


def leaf_at(hw, o, limit, span=0x200) -> bool:
    """Linear decode from o reaches an rts with no invalid word on the way."""
    end = min(limit, o + span)
    while o < end:
        op = Instr(o, hw[o >> 1]).op
        if op == "INVALID":
            return False
        if op == "RTS":
            return o + 2 < limit and Instr(o + 2, hw[(o + 2) >> 1]).op != "INVALID"
        o += 2
    return False


def header(data: bytes) -> list[int]:
    """Entry pointers of the 32-byte header, or [] when the file starts with code."""
    words = struct.unpack_from("<8I", data)
    if not 0x0C000000 <= (words[0] & 0x1FFFFFFF) < 0x0D000000:
        return []
    if any(w not in (0, 0xFFFFFFFF) and not 0x0C000000 <= (w & 0x1FFFFFFF) < 0x0D000000 for w in words):
        return []
    return [w for w in words if w not in (0, 0xFFFFFFFF)]


def find_code_end(data: bytes, start: int = 0) -> int:
    """Offset of the first byte of the data part (32-byte aligned)."""
    n = len(data) // 2
    hw = struct.unpack_from("<%dH" % n, data)
    limit = n * 2
    pro = [i * 2 for i, h in enumerate(hw) if h == 0x4F22]   # sts.l pr,@-r15
    epi = [i * 2 for i, h in enumerate(hw) if h == 0x4F26]   # lds.l @r15+,pr
    rts = [i * 2 for i, h in enumerate(hw) if h == 0x000B]
    seeds, last = [start], start
    for p in pro:
        j = bisect_right(epi, p)
        if j < len(epi) and epi[j] - p < 0x1000:
            k = bisect_left(rts, epi[j])
            # a gap of more than 16 KB without a function means we are in data
            if k < len(rts) and rts[k] - epi[j] <= 16 and p - last <= 0x4000:
                seeds.append(p)
                last = rts[k]
    seen: set[int] = set()
    reach, pool, _ = descend(hw, seeds, limit, seen)
    end = (max(reach, pool) + 31) & ~31
    while end < limit:
        # a function right after the literal pool and its padding (0x0009 or 0x0900)
        nxt = next((o for o in range(end, min(end + 64, limit - 16), 2)
                    if hw[(o >> 1) - 1] in (0x0009, 0x0900) and prologue_at(hw, o)), None)
        if leaf_at(hw, end, limit):
            start = end
        elif nxt is not None:
            start = nxt
        else:
            break
        r, pl, _ = descend(hw, [start], limit, seen)
        end = max(end + 32, (max(r, pl) + 31) & ~31)
    return end


def prologue_at(hw, o) -> bool:
    """An SHC function entry: pushes of r8-r14 (mov.l rN,@-r15), then sts.l pr,@-r15."""
    for k in range(8):
        h = hw[(o >> 1) + k] if (o >> 1) + k < len(hw) else 0
        if h == 0x4F22:
            return True
        if (h & 0xFF0F) != 0x2F06 or not 8 <= (h >> 4 & 0xF) <= 14:
            return False
    return False


def inside(path: str, root: str) -> bool:
    path, root = os.path.realpath(path), os.path.realpath(root)
    return os.path.commonpath([path, root]) == root


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("data_dir")
    ap.add_argument("out_dir")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args(argv)

    if inside(a.out_dir, REPO) or inside(a.out_dir, a.data_dir):
        sys.exit("extract_overlays: OUT_DIR must be outside the repository and the dump")
    names = a.names or configured_names()
    if not names:
        sys.exit("extract_overlays: no overlay names given or configured")
    os.makedirs(a.out_dir, exist_ok=True)
    for name in names:
        src = os.path.join(a.data_dir, "SONICADV", name + ".PRS")
        with open(src, "rb") as f:
            packed = f.read()
        data = prs_decompress(packed)
        if not data:
            sys.exit(f"extract_overlays: {src}: PRS stream did not decode")
        dst = os.path.join(a.out_dir, name + ".bin")
        with open(dst, "wb") as f:
            f.write(data)
        hdr = header(data)
        start = 0x20 if hdr else 0
        end = find_code_end(data, start)
        print(f"{name:10s} {len(packed):>9,} -> {len(data):>9,}  code 0x{start:X}-0x{end:X}  "
              f"{hashlib.sha256(data).hexdigest()}")
        if hdr:
            print(f"{'':10s} header: {' '.join(f'0x{w:08X}' for w in hdr)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
