#!/usr/bin/env python3
"""Recompile one Sonic Adventure code overlay (an SONICADV/*.PRS file) to C.

    python recomp/emitter/emit_overlay.py recomp/sa1_us_1005/overlays/adv00.toml

Inputs: the overlay config (committed; the [sa1_overlay] table gives the slot,
the code range and the SHA-256 of the decompressed file), the decompressed
file it points at (tools/extract_overlays.py, under ../private/, never
committed), the discovery output for it (Dream-Recomp, docs/m1-overlays.md),
the seam table, and the main image's emitted game_table.c, whose functions
the overlay calls directly.  Output goes to recomp/out/overlays/emitter/<name>/
(git-ignored): ovl_<name>_NNN.c, ovl_<name>.h, ovl_<name>_table.c (the
function table and the sa1_overlay record the loader hook installs) and
report.txt.

Functions are named ovl_<name>_8c9xxxxx: overlays that share a slot share
addresses, so the address alone does not name a function."""

import argparse
import hashlib
import json
import os
import re
import sys
import time
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from sh4emit.image import Image, phys  # noqa: E402
from sh4emit.emit import Program  # noqa: E402
from emit_sa1 import read_seam, BLOCK_A, BLOCK_B  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
# docs/m0-trace.md section 5; the end of each range is the next slot's start
SLOTS = {"area": (0x0C900000, 0x0CB80000), "event": (0x0CB80000, 0x0CEB0000),
         "movie": (0x0CEB0000, 0x0CF00000)}


def read_main_table(path):
    """{phys addr: C name} from the main image's generated game_table.c."""
    out = {}
    pat = re.compile(r"\{0x([0-9a-f]{8})u, (fn_[0-9a-f]{8})\}")
    with open(path, encoding="ascii") as f:
        for line in f:
            m = pat.search(line)
            if m:
                out[int(m.group(1), 16)] = m.group(2)
    return out


def classify_external(prog, main):
    """Count call/tail targets outside the overlay's code, by where they land."""
    kinds = {}
    missing = set()
    for cfg in prog.cfgs.values():
        for b in cfg.blocks.values():
            if b.kind not in ("call", "tail") or b.target is None or prog.env.in_code(b.target):
                continue
            t = phys(b.target)
            if BLOCK_A[0] <= t < BLOCK_A[1]:
                k = "ext_main" if t in main else "ext_main_missing"
                if t not in main:
                    missing.add(t)
            elif t in prog.seam:
                k = "ext_seam"
            elif prog.in_sdk(t):
                k = "ext_sdk_not_in_seam"
                missing.add(t)
            elif any(lo <= t < hi for lo, hi in SLOTS.values()):
                k = "ext_other_slot"
            else:
                k = "ext_other"
            kinds[k] = kinds.get(k, 0) + 1
    return kinds, sorted(missing)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("config", help="recomp/sa1_us_1005/overlays/<name>.toml")
    ap.add_argument("--bin", help="decompressed overlay (default: [binary] path of the config)")
    ap.add_argument("--functions", help="Dream-Recomp functions.json "
                    "(default: recomp/out/overlays/dream/<name>/sa1_ovl_<name>.functions.json)")
    ap.add_argument("--main-table", default=os.path.join(REPO, "recomp", "out", "emitter", "game_table.c"),
                    help="the main image's generated game_table.c")
    ap.add_argument("--seam", default=os.path.join(REPO, "recomp", "sa1_us_1005", "sdk_entry_points.tsv"))
    ap.add_argument("--out", help="default: recomp/out/overlays/emitter/<name>")
    ap.add_argument("--chunks", type=int, default=4)
    args = ap.parse_args()

    with open(args.config, "rb") as f:
        cfg = tomllib.load(f)
    ov = cfg["sa1_overlay"]
    name = os.path.splitext(os.path.basename(args.config))[0]
    unit = "ovl_" + name
    bin_path = args.bin or os.path.join(os.path.dirname(os.path.abspath(args.config)), cfg["binary"]["path"])
    fn_path = args.functions or os.path.join(REPO, "recomp", "out", "overlays", "dream", name,
                                             "%s.functions.json" % cfg["game"]["id"])
    out = args.out or os.path.join(REPO, "recomp", "out", "overlays", "emitter", name)

    with open(bin_path, "rb") as f:
        data = f.read()
    sha = hashlib.sha256(data).hexdigest()
    if sha != ov["sha256"]:
        sys.exit("%s: sha256 %s is not the configured %s" % (bin_path, sha, ov["sha256"]))
    image = Image(data, ov["slot"])
    code_lo, code_hi = phys(ov["code_lo"]), phys(ov["code_hi"])

    with open(fn_path) as f:
        fl = json.load(f)["functions"]
    functions = []
    for fn in fl:
        e = phys(int(fn["entry"], 16))
        if code_lo <= e < code_hi:
            functions.append((e, min(phys(int(fn["end"], 16)), code_hi), fn.get("origin", "")))
    main_fns = read_main_table(args.main_table)
    seam = read_seam(args.seam)
    print("%s: %d functions in 0x%08x-0x%08x, %d main-image functions, %d seam entries" % (
        name, len(functions), code_lo, code_hi, len(main_fns), len(seam)))

    t0 = time.time()
    prog = Program(image, code_lo, code_hi, functions, seam, sdk_range=BLOCK_B,
                   name_prefix=unit + "_", extern=main_fns)
    prog.build()
    ext, missing = classify_external(prog, main_fns)
    prog.stats.update(ext)
    overlay = {"name": os.path.splitext(os.path.basename(ov["file"]))[0], "table": "sa1_%s_fns" % unit,
               "record": "sa1_" + unit, "code_lo": code_lo, "code_hi": code_hi}
    counts = prog.write_all(out, chunks=args.chunks, unit=unit, overlay=overlay)
    with open(os.path.join(out, "report.txt"), "a", newline="\n") as f:
        slot = [n for n, (lo, _) in SLOTS.items() if lo == phys(ov["slot"])]
        f.write("slot %s 0x%08x\n" % (slot[0] if slot else "?", ov["slot"]))
        for t in missing:
            f.write("note: external call target 0x%08x has no emitted function or seam entry\n" % t)
    print("total %.1fs" % (time.time() - t0))
    for k in sorted(counts):
        print("  %-24s %s" % (k, counts[k]))
    for t in missing:
        print("  external target without a function: 0x%08x" % t)


if __name__ == "__main__":
    main()
