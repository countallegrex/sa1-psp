#!/usr/bin/env python3
"""Cross-image entry seeds for the code overlays and the main image (docs/m1-overlays.md section 9).

Usage:
  overlay_xref.py [--write] [NAME ...]

Discovery runs on one image at a time, so it misses functions that only a
pointer in another image (or a pointer in the 0x0C... form in its own image)
reaches.  The game calls those through a register, and the runtime traps
("indirect call to an address with no function"): Windy Valley, Ice Cap,
Twinkle Park, Speed Highway and Lost World start through a level-indexed
table in 1ST_READ.BIN (0x8C19DE2C, 0x8C19E0E0) that points into each stage's
overlay; Red Mountain 2 and Final Egg 2-3 call main-image functions whose only
pointers are in STG05/STG10 data.  This tool finds three kinds of seeds:

  own    aligned words in an overlay's own image (0x8C... or 0x0C... form)
         inside its code range: function tables in its data
  main   aligned words in 1ST_READ.BIN inside an overlay's code range: every
         overlay shares its slot with others, so a word alone does not say
         whose it is
  Both only where the address lands just after the end of a block (rts,
  bra, braf, jmp or rte and its delay slot, or nop padding; zero padding
  only before a mov.l rN,@-r15 or sts.l pr,@-r15; own words also where the
  address starts with one of those):
  the code ranges hold data tables too (ADV0100's 485 words at a 12-byte
  stride point into one), and a function start follows a block end.
  back   aligned words in any overlay image inside the main image's code
         blocks, for the main config: under the block-end test, or starting
         with a prologue (one main image: no ambiguity), or right after a
         pointer into the same overlay image (a record {name, function})

Without --write it prints the counts and the seeds; with --write it adds them
to the configs' extra lists and <name>_seeds.txt (as tools/overlays.py does
for trace seeds), the main seeds to recomp/sa1_us_1005/overlay_main_seeds.txt
and the main config's extra.  Then: tools/overlays.py build, the main
discovery (recomp/sa1_us_1005/README.md) and emit_sa1.py.

Reads ../private/ only; writes addresses only.  Part of sa1-psp (MIT).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import overlays as ov  # noqa: E402

MAIN_TOML = os.path.join(ov.REPO, "recomp", "sa1_us_1005", "sa1_us_1005.toml")
MAIN_SEEDS = os.path.join(ov.REPO, "recomp", "sa1_us_1005", "overlay_main_seeds.txt")
MAIN_FNS = os.path.join(ov.REPO, "recomp", "out", "dream_seeded", "sa1_us_1005.functions.json")
MAIN_BASE = 0x0C010000
MAIN_CODE = ((0x0C010000, 0x0C111200), (0x0C5FA000, 0x0C66C000))   # emit_sa1.py BLOCK_A, BLOCK_B


def phys(a: int) -> int:
    return a & 0x1FFFFFFF


def words(data: bytes):
    for off in range(0, len(data) - 3, 4):
        yield struct.unpack_from("<I", data, off)[0]


def half(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0] if 0 <= off <= len(data) - 2 else -1


def ends_block(h: int) -> bool:
    """rts, rte, bra, braf/jmp (0x0n23 / 0x4n2B)."""
    return h in (0x000B, 0x002B) or (h & 0xF000) == 0xA000 or (h & 0xF0FF) in (0x0023, 0x402B)


def after_block_end(data: bytes, off: int) -> bool:
    """The code at off starts right after a block end and its delay slot,
    possibly after nop padding (0x0009)."""
    k = off - 2
    while k >= 0 and half(data, k) in (0x0009, 0x0000) and off - k <= 32:
        if ends_block(half(data, k - 2)) or (half(data, k) == 0 and off - k >= 4 and prologue(data, off)):
            return True
        k -= 2
    return ends_block(half(data, off - 4))


def prologue(data: bytes, off: int) -> bool:
    """mov.l rN,@-r15 or sts.l pr,@-r15: how the game's functions start."""
    h = half(data, off)
    return (h & 0xFF0F) == 0x2F06 or h == 0x4F22


def known(cfg: dict, fns_path: str) -> set[int]:
    have = {phys(x) for x in cfg.get("functions", {}).get("extra", [])}
    if os.path.exists(fns_path):
        for f in json.load(open(fns_path))["functions"]:
            if f.get("origin") != "branch":
                have.add(phys(int(f["entry"], 16)))
    return have


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args(argv)
    main_img = open(os.path.join(ov.DATA, "1ST_READ.BIN"), "rb").read()
    main_words = {phys(w) for w in words(main_img) if w & 1 == 0}
    imgs = {}
    for path in ov.configs():
        name = os.path.splitext(os.path.basename(path))[0]
        cfg = ov.load(path)
        o = cfg["sa1_overlay"]
        stem = os.path.splitext(os.path.basename(o["file"]))[0]
        imgs[name] = (path, cfg, open(os.path.join(ov.BINS, stem + ".bin"), "rb").read())
    total = 0
    for name, (path, cfg, data) in sorted(imgs.items()):
        if a.names and name not in a.names:
            continue
        o = cfg["sa1_overlay"]
        slot, lo, hi = phys(o["slot"]), phys(o["code_lo"]), phys(o["code_hi"])
        have = known(cfg, os.path.join(ov.OUT, "dream", name, "%s.functions.json" % cfg["game"]["id"]))
        own = {phys(w) for w in words(data) if lo <= phys(w) < hi and w & 1 == 0 and (after_block_end(data, phys(w) - slot) or prologue(data, phys(w) - slot))} - have
        cross = {w for w in main_words if lo <= w < hi and after_block_end(data, w - slot)} - have - own
        new = sorted(own | cross)
        if new:
            total += len(new)
            print("%-9s own %3d main %3d: %s" % (name, len(own), len(cross), " ".join("0x%08X" % s for s in new[:12])
                                                + (" ..." if len(new) > 12 else "")))
            if a.write:
                ov.add_seeds(path, name, [s for s in new])
    # back: overlay data -> main image code
    have = set()
    if os.path.exists(MAIN_FNS):
        have = {phys(int(f["entry"], 16)) for f in json.load(open(MAIN_FNS))["functions"] if f.get("origin") != "branch"}
    have |= {int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{8}", open(MAIN_TOML, encoding="utf-8").read())}
    back = set()
    for name, (_, cfg, data) in imgs.items():
        slot = phys(cfg["sa1_overlay"]["slot"])
        prev = None
        for w in words(data):
            p = phys(w)
            if w & 1 == 0 and any(lo <= p < hi for lo, hi in MAIN_CODE):
                # a record {pointer into this image, function} (STG05's object
                # layouts: name, then 0x8C06C080), or the block-end / prologue test
                if (after_block_end(main_img, p - MAIN_BASE) or prologue(main_img, p - MAIN_BASE)
                        or (prev is not None and slot <= phys(prev) < slot + len(data))):
                    back.add(p)
            prev = w
    back = sorted(back - have)
    print("main (back) %d: %s" % (len(back), " ".join("0x%08X" % s for s in back[:16]) + (" ..." if len(back) > 16 else "")))
    if a.write and back:
        old = [l.strip() for l in open(MAIN_SEEDS, encoding="ascii") if l.strip()]
        with open(MAIN_SEEDS, "a", encoding="ascii", newline="\n") as f:
            for s in back:
                if "0x%08X" % s not in old:
                    f.write("0x%08X\n" % s)
        t = open(MAIN_TOML, encoding="utf-8").read()
        m = re.search(r"^extra = \[(.*?)\n\]", t, re.M | re.S)
        add = "\n    # overlay data pointers into the main image (tools/overlay_xref.py)\n    " + \
              ",\n    ".join(", ".join("0x%08X" % s for s in back[i:i + 6]) for i in range(0, len(back), 6)) + ","
        body = m.group(1).rstrip()
        if not body.endswith(","):
            body += ","
        t = t[:m.start(1)] + body + add + t[m.end(1):]
        open(MAIN_TOML, "w", encoding="utf-8", newline="\n").write(t)
    print("overlay seeds %d, main seeds %d%s" % (total, len(back), "" if a.write else " (dry run)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
