#!/usr/bin/env python3
"""Name the console profile samples of tools/psp_run.py --sample-after.

    psp_prof.py [--samples FILE | --pcs FILE] [--log FILE] [--elf FILE] [--top N] [--out FILE]

--pcs FILE: the samples of the game's own sampler (main_psp.c --prof,
backend/psp/prof/sa1prof.c: cop0 EPC from a kernel alarm every ~1 ms,
whichever thread was running; a 16-byte header 'PROF', count, period in
us, missed, then the PCs as little-endian words).  PCs outside the PRX
are the kernel, other modules and the idle thread (the game waiting).
These have no return address, so the callers section is left out.  On
firmware 6.61 thctx (--samples, psp_run.py --sample-after) reads a
context that is not the thread's: use --pcs.

Each sample is the game thread's PC at a moment PSPLink's shell preempted
it (prof_samples.tsv: t, epc, real_epc, ra, sp).  A non-zero real_epc is
the user return address of a syscall the thread was in (waiting for the
GE, a vblank, a file read): those samples are counted as waits under the
function that made the call.  The PRX's load offset comes from the game's
"image: main at 0x..." log line against main's link address; symbols,
sizes and source files from psp-nm -n -S -l on sa1psp_prx.elf (through WSL
on Windows).

Prints: the totals (running, waiting), the flat profile by function, the
same by group (recompiled game code, the HLE by file, the renderer, libc,
soft-float, other), and for the hottest small functions who called them
(the ra of the sample).

Part of sa1-psp (MIT).
"""
from __future__ import annotations

import argparse
import bisect
import collections
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAGE = os.path.join(REPO, "build-psp", "sa1psp", "psplink-run")


def wsl_path(p: str) -> str:
    drive, rest = os.path.splitdrive(os.path.abspath(p))
    return "/mnt/" + drive[0].lower() + rest.replace("\\", "/")


def nm(elf: str, distro: str) -> list[tuple[int, int, str, str]]:
    """(address, size, name, source file) of the text symbols, sorted."""
    if os.name == "nt":
        cmd = ["wsl.exe", "-d", distro, "--exec", os.environ.get("SA1_PSP_WSL_PSPDEV", "/opt/pspdev") + "/bin/psp-nm",
               "-n", "-S", "-l", wsl_path(elf)]
    else:
        cmd = ["psp-nm", "-n", "-S", "-l", elf]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    syms = []
    for line in r.stdout.splitlines():
        f = line.split(None, 4)
        if len(f) >= 4 and f[2] in "tTwW" and len(f[2]) == 1:
            addr, size, name = int(f[0], 16), int(f[1], 16), f[3]
            src = f[4] if len(f) > 4 else ""
            syms.append((addr, size, name, src.split(":")[0]))
        elif len(f) >= 3 and f[1] in "tTwW" and len(f[1]) == 1:      # no size
            src = f[3] if len(f) > 3 else ""
            syms.append((int(f[0], 16), 0, f[2], src.split(":")[0]))
    syms.sort()
    return syms


class Symbols:
    def __init__(self, syms):
        self.syms = syms
        self.addrs = [s[0] for s in syms]

    def find(self, addr: int):
        i = bisect.bisect_right(self.addrs, addr) - 1
        if i < 0:
            return None
        s = self.syms[i]
        if s[1] and addr >= s[0] + s[1] + 64:
            return None
        return s


def group_of(name: str, src: str) -> str:
    if name.startswith("fn_"):
        return "game code (recompiled)"
    if re.match(r"__(add|sub|mul|div|neg|extend|trunc|float|fix|eq|ne|lt|le|gt|ge|un|cmp)\w*[sd]f\d", name):
        return "soft-float"
    s = src.replace("\\", "/")
    m = re.search(r"/(hle/[^:]+|backend/[^:]+|recomp/[^:]+)$", s)
    if m:
        path = m.group(1)
        if path.startswith("backend/psp/draw_gu"):
            return "renderer: " + os.path.basename(path)
        return path
    if re.match(r"(mem|str|_?_?(v?s?n?printf|qsort|malloc|free|realloc|calloc))", name):
        return "libc"
    if name.startswith("sce") or name.startswith("_sce"):
        return "pspsdk"
    return "other: " + (os.path.basename(s) if s else "no source")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--samples", default=os.path.join(STAGE, "prof_samples.tsv"))
    ap.add_argument("--pcs", help="the binary samples of main_psp.c --prof (instead of --samples)")
    ap.add_argument("--log", default=os.path.join(STAGE, "sa1psp.log"), help="the run's game log (the 'image: main at' line)")
    ap.add_argument("--elf", default=os.path.join(REPO, "build-psp", "sa1psp", "sa1psp_prx.elf"))
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--wsl-distro", default="Ubuntu")
    ap.add_argument("--out", help="also write the report here")
    a = ap.parse_args()

    main_at = None
    with open(a.log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r"image: main at (0x[0-9a-fA-F]+)", line)
            if m:
                main_at = int(m.group(1), 16)
    if main_at is None:
        print(f"psp_prof: no 'image: main at' line in {a.log}", file=sys.stderr)
        return 1
    syms = Symbols(nm(a.elf, a.wsl_distro))
    link_main = next((s[0] for s in syms.syms if s[2] == "main"), None)
    if link_main is None:
        print("psp_prof: no symbol 'main' in the ELF", file=sys.stderr)
        return 1
    delta = main_at - link_main

    run, wait_by, ra_of = collections.Counter(), collections.Counter(), collections.defaultdict(collections.Counter)
    srcs = {}
    n = outside = 0
    t0 = t1 = None
    def rows():
        if a.pcs:
            import struct
            data = open(a.pcs, "rb").read()
            magic, count, period, missed = struct.unpack_from("<4I", data, 0)
            if magic != 0x464F5250:
                raise SystemExit(f"psp_prof: {a.pcs} is not a --prof file")
            if missed:
                print(f"psp_prof: {missed} samples missed (buffer full)", file=sys.stderr)
            for i, pc in enumerate(struct.unpack_from(f"<{count}I", data, 16)):
                yield "%f %08x 0 0 0" % (i * period / 1e6, pc)
            return
        with open(a.samples, encoding="utf-8") as f:
            yield from f

    for line in rows():
        if line.startswith("#") or not line.strip():
            continue
        t, epc, real, ra, sp = line.split()[:5]
        t = float(t)
        t0 = t if t0 is None else t0
        t1 = t
        n += 1
        epc, real, ra = int(epc, 16), int(real, 16), int(ra, 16)
        if real:
            s = syms.find(real - delta)
            wait_by[s[2] if s else "0x%08x" % real] += 1
            continue
        s = syms.find(epc - delta)
        if not s:
            outside += 1
            run["(outside the PRX: kernel, pspsdk modules) 0x%08x" % (epc & 0xFFFFF000)] += 1
            continue
        run[s[2]] += 1
        srcs[s[2]] = s[3]
        if ra:
            c = syms.find(ra - delta)
            ra_of[s[2]][c[2] if c else "0x%08x" % ra] += 1

    out = []
    def p(text=""):
        out.append(text)
    nrun, nwait = sum(run.values()), sum(wait_by.values())
    p(f"psp_prof: {n} samples over {((t1 or 0) - (t0 or 0)):.1f} s; PRX load offset 0x{delta:08x}")
    if not n:
        print("\n".join(out))
        return 1
    p(f"  running {nrun} ({100.0 * nrun / n:.1f} %), in a syscall (waiting) {nwait} ({100.0 * nwait / n:.1f} %)")
    p()
    p("waiting, by the function that made the syscall:")
    for name, k in wait_by.most_common(12):
        p(f"  {100.0 * k / n:6.2f} %  {k:6d}  {name}")
    p()
    p(f"running, by function (top {a.top}; % of all samples):")
    for name, k in run.most_common(a.top):
        p(f"  {100.0 * k / n:6.2f} %  {k:6d}  {name}  [{group_of(name, srcs.get(name, ''))}]")
    p()
    groups = collections.Counter()
    for name, k in run.items():
        groups[group_of(name, srcs.get(name, ""))] += k
    p("running, by group:")
    for g, k in groups.most_common():
        p(f"  {100.0 * k / n:6.2f} %  {k:6d}  {g}")
    p()
    p("callers (ra) of the hottest non-game functions:" if not a.pcs else "(no callers: --pcs samples carry no ra)")
    shown = 0
    for name, k in (run.most_common() if not a.pcs else []):
        if name.startswith("fn_") or name.startswith("("):
            continue
        callers = ", ".join(f"{c} {v}" for c, v in ra_of[name].most_common(5))
        p(f"  {name} ({k}): {callers}")
        shown += 1
        if shown == 15:
            break
    text = "\n".join(out)
    print(text)
    if a.out:
        with open(a.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
