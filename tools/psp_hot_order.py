#!/usr/bin/env python3
"""Write tools/psp_hot_order.txt, the PSP build's hot-code placement, from a console profile.

    psp_hot_order.py --pcs FILE --log FILE --elf FILE [--cover 0.9] [--min-samples 3] [--out FILE]

--pcs/--log/--elf are those of tools/psp_prof.py: the samples of a
main_psp.c --prof run, its game log, and the sa1psp_prx.elf that ran.
The functions that hold the first --cover share of the samples inside
the PRX (at least --min-samples each) are listed, callers next to the
functions they call: each function starts as its own chain, the call
edges (jal/j found in the code, weighted by the smaller of the two
sample counts) are taken heaviest first, and each joins two chains in
the orientation that puts the pair closest; the chains are then written
hottest first.

The list is what backend/psp/CMakeLists.txt (SA1_PSP_HOT_ORDER) turns
into the linker's --section-ordering-file: those functions go first in
.text, contiguous, at the image's start, so code added or removed
elsewhere never moves them in the PSP's 16 KB, 2-way I-cache.  One entry
per line, '#' starts a comment:

    NAME                      the function's section .text.NAME, any object
    FILE.c:NAME               a static function defined in several objects
                              (sa1_off): only FILE.c's copy
    LIB.a:MEMBER.o            a static library's member, whole (pspsdk,
                              newlib: built without -ffunction-sections)

A name the build no longer has is matched by nothing and costs nothing.

Part of sa1-psp (MIT).
"""
from __future__ import annotations

import argparse
import collections
import os
import re
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from psp_prof import wsl_path  # noqa: E402

PSPDEV = os.environ.get("SA1_PSP_WSL_PSPDEV", "/opt/pspdev") if os.name == "nt" else os.environ.get("PSPDEV", "/opt/pspdev")
# The static libraries the game links whose code can be hot.
LIBS = ["psp/sdk/lib/libpspgu.a", "psp/sdk/lib/libpspge.a", "psp/sdk/lib/libpspdisplay.a",
        "psp/lib/libm.a", "psp/lib/libc.a", "lib/gcc/psp/*/libgcc.a"]


def tool(name: str, args: list[str], distro: str) -> str:
    """Run a psp- binutil (through WSL on Windows); the shell globs the library paths."""
    q = ["'%s'" % x if " " in x else x for x in args]
    if os.name == "nt":
        cmd = ["wsl.exe", "-d", distro, "--exec", "bash", "-c", " ".join([PSPDEV + "/bin/" + name] + q)]
    else:
        cmd = ["bash", "-c", " ".join([name] + q)]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600).stdout


def functions(elf: str, distro: str):
    """[(addr, size, name, bind, file)] of the ELF's FUNC symbols; file is the
    STT_FILE that precedes a local symbol in .symtab."""
    out = tool("psp-readelf", ["-sW", wsl_path(elf) if os.name == "nt" else elf], distro)
    fns, cur = [], ""
    for line in out.splitlines():
        f = line.split()
        if len(f) < 8 or not f[0].endswith(":"):
            continue
        typ, bind, name = f[3], f[4], f[7]
        if typ == "FILE":
            cur = name
        elif typ == "FUNC" and f[6] != "UND":
            fns.append((int(f[1], 16), int(f[2]), name, bind, cur if bind == "LOCAL" else ""))
    fns.sort()
    return fns


def text_bytes(elf: str):
    """(address, bytes) of the ELF's .text."""
    d = open(elf, "rb").read()
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    secs = [struct.unpack_from("<10I", d, shoff + i * shentsize) for i in range(shnum)]
    stro = secs[shstrndx][4]
    for s in secs:
        nm = d[stro + s[0]:d.index(b"\0", stro + s[0])].decode()
        if nm == ".text":
            return s[3], d[s[4]:s[4] + s[5]]
    raise SystemExit("psp_hot_order: no .text")


def lib_members(distro: str) -> dict[str, str]:
    """name -> 'libX.a:member.o' of the static libraries' text symbols."""
    out = tool("psp-nm", ["-A", "--defined-only"] + [PSPDEV + "/" + l for l in LIBS], distro)
    m = {}
    for line in out.splitlines():
        g = re.match(r"(\S+\.a):(\S+\.o):\S+ [TW] (\S+)$", line)
        if g:
            m.setdefault(g.group(3), os.path.basename(g.group(1)) + ":" + g.group(2))
    return m


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pcs", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--cover", type=float, default=0.90)
    ap.add_argument("--min-samples", type=int, default=3)
    ap.add_argument("--out", default=os.path.join(REPO, "tools", "psp_hot_order.txt"))
    ap.add_argument("--wsl-distro", default="Ubuntu")
    a = ap.parse_args()

    main_at = None
    for line in open(a.log, encoding="utf-8", errors="replace"):
        m = re.search(r"image: main at (0x[0-9a-fA-F]+)", line)
        if m:
            main_at = int(m.group(1), 16)
    fns = functions(a.elf, a.wsl_distro)
    link_main = next((f[0] for f in fns if f[2] == "main"), None)
    if main_at is None or link_main is None:
        raise SystemExit("psp_hot_order: no 'image: main at' in the log or no main in the ELF")
    delta = main_at - link_main
    starts = [f[0] for f in fns]
    import bisect

    def find(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr < fns[i][0] + max(fns[i][1], 4):
            return i
        return None

    data = open(a.pcs, "rb").read()
    magic, count, _period, _missed = struct.unpack_from("<4I", data, 0)
    if magic != 0x464F5250:
        raise SystemExit(f"psp_hot_order: {a.pcs} is not a --prof file")
    hits = collections.Counter()
    inside = 0
    for pc in struct.unpack_from(f"<{count}I", data, 16):
        i = find(pc - delta)
        if i is not None:
            hits[i] += 1
            inside += 1

    hot, acc = [], 0
    for i, k in sorted(hits.items(), key=lambda x: (-x[1], fns[x[0]][2], x[0])):
        if acc >= a.cover * inside or k < a.min_samples:
            break
        hot.append(i)
        acc += k
    hotset = set(hot)

    # Call edges between hot functions (jal/j to a hot function's start).
    base, text = text_bytes(a.elf)
    edges = collections.Counter()
    for i in hot:
        ad, sz = fns[i][0], fns[i][1]
        for off in range(ad - base, ad - base + sz, 4):
            w, = struct.unpack_from("<I", text, off)
            if (w >> 26) in (2, 3):
                j = find(((w & 0x3FFFFFF) << 2) | ((base + off) & 0xF0000000))
                if j is not None and j in hotset and j != i:
                    edges[tuple(sorted((i, j)))] = min(hits[i], hits[j])
    chain = {i: [i] for i in hot}
    for (i, j), _w in sorted(edges.items(), key=lambda e: (-e[1], e[0])):
        ci, cj = chain[i], chain[j]
        if ci is cj:
            continue
        best = None
        for x in (ci, ci[::-1]):
            for y in (cj, cj[::-1]):
                c = x + y
                dist = abs(c.index(i) - c.index(j))
                if best is None or dist < best[0]:
                    best = (dist, c)
        for k in best[1]:
            chain[k] = best[1]
    chains, seen = [], set()
    for i in hot:
        c = chain[i]
        if id(c) not in seen:
            seen.add(id(c))
            chains.append(c)
    chains.sort(key=lambda c: -sum(hits[k] for k in c))

    libs = lib_members(a.wsl_distro)
    names = collections.Counter(f[2] for f in fns)
    lines, size, done = [], 0, set()
    for c in chains:
        for i in c:
            ad, sz, name, bind, src = fns[i]
            mem = libs.get(name)
            stem = os.path.splitext(src)[0]
            if mem and re.sub(r"^lib\w+_a-", "", mem.split(":")[1])[:-2] == stem or (mem and not src):
                entry = mem
            elif bind == "LOCAL" and names[name] > 1 and src:
                entry = src + ":" + name
            else:
                entry = name
            if entry in done:
                continue
            done.add(entry)
            size += sz
            lines.append("%-40s # %5.2f %%  %6d B" % (entry, 100.0 * hits[i] / inside, sz))
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The PSP build's hot code, placed first in .text in this order (SA1_PSP_HOT_ORDER,\n"
                "# backend/psp/CMakeLists.txt).  Written by tools/psp_hot_order.py from a console\n"
                "# profile: %s, %d functions, %.1f %% of the samples inside the PRX, %d bytes.\n"
                "# Share of samples and size are of the profiled build.\n"
                % (os.path.basename(a.pcs), len(lines), 100.0 * acc / inside, size))
        f.write("\n".join(lines) + "\n")
    print(f"psp_hot_order: {len(lines)} functions, {100.0 * acc / inside:.1f} % of {inside} samples, {size} bytes -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
