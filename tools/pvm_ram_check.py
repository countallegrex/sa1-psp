"""pvm_ram_check.py - are the texture archives intact in guest RAM?

The game reads a .PRS from disc into a staging buffer and decompresses it
with its own (recompiled) PRS routine; njLoadTexturePvmMemory then decodes
the archive from guest RAM.  With SA1_PVM_SAVE_DIR=<dir> the HLE writes
every archive exactly as it found it in RAM (pvm_<seq>_<address>.bin) and
logs "pvm: saved ... from <file>" with the .PRS the preceding file open
named.  This script decompresses each of those .PRS files on the PC with
tools/prs.py, finds the archive in the result and compares byte for byte.

    python tools/pvm_ram_check.py <log> <save dir> <data root>

Identical for every archive means the extraction and the recompiled
decompressor are clean, so a texture fault is in the decoder; a difference
prints the first offset, which would be an emitter or loader bug.  Part of
sa1-psp (MIT); reads game data, writes nothing into the repo.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prs import prs_decompress  # noqa: E402


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    log, save_dir, root = argv
    pat = re.compile(r'pvm: saved (\S+) \((\d+) bytes at 0x([0-9a-f]+)\) from "([^"]+)"')
    seen = {}
    rows = []
    for line in open(log, errors="replace"):
        m = pat.search(line)
        if m:
            rows.append((m[1], int(m[2]), m[3], m[4]))
    if not rows:
        print("no 'pvm: saved' lines in", log, "(run with SA1_PVM_SAVE_DIR set)")
        return 2
    bad = 0
    cache = {}
    for name, size, addr, prs in rows:
        ram = open(os.path.join(save_dir, name), "rb").read()
        if prs not in cache:
            p = os.path.join(root, *prs.split("/"))
            cache[prs] = prs_decompress(open(p, "rb").read()) if prs.upper().endswith(".PRS") else open(p, "rb").read()
        data = cache[prs]
        key = (prs, size)
        at = data.find(ram[:64])
        if at < 0:
            print("%-28s %-10s %8d bytes  archive head not found in the decompressed file" % (prs, name, size))
            bad += 1
            continue
        ref = data[at:at + len(ram)]
        if ref == ram:
            verdict = "identical"
        else:
            diff = next(i for i in range(min(len(ref), len(ram))) if ref[i] != ram[i]) if len(ref) == len(ram) else min(len(ref), len(ram))
            verdict = "DIFFERS at offset %d (0x%x)" % (diff, diff)
            bad += 1
        dup = " (again)" if key in seen else ""
        seen[key] = 1
        print("%-28s %-10s %8d bytes at 0x%s, file offset %7d: %s%s" % (prs, name, size, addr, at, verdict, dup))
    print("pvm_ram_check: %d archives, %d differ" % (len(rows), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
