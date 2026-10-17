#!/usr/bin/env python3
"""Recompile the game block of Sonic Adventure US v1.005 to C.

    python recomp/emitter/emit_sa1.py --bin ../private/dump/sa1_us_1005/data/1ST_READ.BIN \
        --functions recomp/out/dream_seeded/sa1_us_1005.functions.json \
        --out recomp/out/emitter

Inputs: the user's own 1ST_READ.BIN (never committed), the function list
produced by the discovery step (recomp/sa1_us_1005/README.md) and the seam
table recomp/sa1_us_1005/sdk_entry_points.tsv (committed).  Output goes to
recomp/out/ (git-ignored).  Nothing from the binary ends up in the repo:
only the generated seam.h (addresses and names) is copied to hle/ with
--update-hle."""

import argparse
import hashlib
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from sh4emit.image import Image, phys  # noqa: E402
from sh4emit.emit import Program  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
LOAD = 0x8C010000
BLOCK_A = (0x0C010000, 0x0C111200)
BLOCK_B = (0x0C5FA000, 0x0C66C000)
EXPECTED_SHA256 = "45bc5d3c25dcfe2cc2c5701311d46783f33e4daa44a951cd9896724d66f0367a"


def read_seam(path):
    seam = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or line.startswith("address\t") or not line.strip():
                continue
            cols = line.rstrip("\n").split("\t")
            addr = int(cols[0], 16)
            sites = int(cols[1])
            name = cols[4] if len(cols) > 4 and cols[4] not in ("-", "") else None
            if name:
                # keep an identifier-friendly short name
                name = name.split(" (")[0].split(":")[0].strip()
            seam[addr] = (name, sites)
    return seam


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="path to the descrambled 1ST_READ.BIN")
    ap.add_argument("--functions", required=True, help="Dream-Recomp *.functions.json")
    ap.add_argument("--seam", default=os.path.join(REPO, "recomp", "sa1_us_1005", "sdk_entry_points.tsv"))
    ap.add_argument("--out", default=os.path.join(REPO, "recomp", "out", "emitter"))
    ap.add_argument("--chunks", type=int, default=32)
    ap.add_argument("--update-hle", action="store_true", help="copy seam.h / seam_stubs.c into hle/")
    ap.add_argument("--no-sha-check", action="store_true")
    args = ap.parse_args()

    with open(args.bin, "rb") as f:
        data = f.read()
    sha = hashlib.sha256(data).hexdigest()
    if sha != EXPECTED_SHA256 and not args.no_sha_check:
        sys.exit("1ST_READ.BIN sha256 %s is not the US v1.005 dump (%s)" % (sha, EXPECTED_SHA256))
    image = Image(data, LOAD)

    with open(args.functions) as f:
        fl = json.load(open(args.functions))["functions"]
    functions = []
    for fn in fl:
        e = int(fn["entry"], 16)
        if BLOCK_A[0] <= phys(e) < BLOCK_A[1]:
            functions.append((e, int(fn["end"], 16), fn.get("origin", "")))
    seam = read_seam(args.seam)
    print("%d functions in block A, %d seam entries" % (len(functions), len(seam)))

    t0 = time.time()
    prog = Program(image, BLOCK_A[0], BLOCK_A[1], functions, seam, sdk_range=BLOCK_B)
    prog.build()
    counts = prog.write_all(args.out, chunks=args.chunks)
    shutil.copy(os.path.join(HERE, "runtime", "sh4_rt.h"), args.out)
    shutil.copy(os.path.join(HERE, "runtime", "sh4_rt.c"), args.out)
    print("total %.1fs" % (time.time() - t0))
    for k in sorted(counts):
        print("  %-24s %s" % (k, counts[k]))
    if args.update_hle:
        hle = os.path.join(REPO, "hle")
        shutil.copy(os.path.join(args.out, "seam.h"), hle)
        shutil.copy(os.path.join(args.out, "seam_stubs.c"), hle)
        print("updated hle/seam.h and hle/seam_stubs.c")


if __name__ == "__main__":
    main()
