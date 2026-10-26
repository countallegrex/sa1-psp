#!/usr/bin/env python3
"""Write tools/psp_hot_order.txt, the PSP build's hot-code placement, from a console profile.

    psp_hot_order.py --pcs FILE --log FILE [--pcs FILE --log FILE ...] --elf FILE
                     [--cover 0.9] [--warm-cover 0.99] [--min-samples 3] [--out FILE]

--pcs/--log/--elf are those of tools/psp_prof.py: the samples of a
main_psp.c --prof run, its game log, and the sa1psp_prx.elf that ran;
several profiles of the same ELF (one per stage) are summed.
The functions that hold the first --cover share of the samples inside
the PRX (at least --min-samples each) are listed, callers next to the
functions they call: each function starts as its own chain, the call
edges (jal/j found in the code, weighted by the smaller of the two
sample counts) are taken heaviest first, and each joins two chains in
the orientation that puts the pair closest; the chains are then written
hottest first.  The functions after those, up to --warm-cover, follow
hottest first ("warm"), then a "=CHUNK.c" line per game and overlay
object in link order; the list comes out unpinned: build it, then --pin
it to that build (tools/psp_hot_regen.sh does both).

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

    psp_hot_order.py --pin ELF [--list FILE] [--out FILE]

rewrites the list with an "@ADDR" line before every entry and one after
the last: ADDR is where that function, and the code after the list,
start modulo 16 KB in ELF, a build of the same list (pinning that build's
own list changes nothing in it).  With every entry pinned, a lever that
grows or shrinks one function moves no other listed function's cache
sets, nor the libraries and game chunks after the list: only the
unlisted HLE and backend code still moves.  Entries without a symbol in
ELF (functions the lever adds) go unpinned, after the entry before them.

Lines "=CHUNK.c @ADDR" after the list place the game and overlay objects
one by one after the libraries, each whole and pinned at its start (the
linker symbol sa1_obj_<CHUNK_c> the build defines there), and "=@ADDR"
pins the code after them: a lever that changes one chunk moves no other.
--pin rewrites their addresses too; a "=CHUNK.c" line without one (a new
chunk) goes unpinned.

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


def markers(elf: str, distro: str) -> dict[str, int]:
    """name -> address of the sa1_obj_* symbols (the game objects' starts)."""
    out = tool("psp-readelf", ["-sW", wsl_path(elf) if os.name == "nt" else elf], distro)
    m = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 8 and f[0].endswith(":") and f[7].startswith("sa1_obj_"):
            m[f[7]] = int(f[1], 16)
    return m


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
    ap.add_argument("--pcs", action="append", help="repeatable, one per profile (with its --log)")
    ap.add_argument("--log", action="append")
    ap.add_argument("--elf")
    ap.add_argument("--pin", metavar="ELF", help="pin every entry of --list to its address in ELF instead")
    ap.add_argument("--list", default=os.path.join(REPO, "tools", "psp_hot_order.txt"))
    ap.add_argument("--cover", type=float, default=0.90)
    ap.add_argument("--warm-cover", type=float, default=0.99,
                    help="the functions up to this share go after the hot list, hottest first, unchained")
    ap.add_argument("--min-samples", type=int, default=3)
    ap.add_argument("--out", default=os.path.join(REPO, "tools", "psp_hot_order.txt"))
    ap.add_argument("--wsl-distro", default="Ubuntu")
    a = ap.parse_args()
    if a.pin:
        return pin(a)
    if not (a.pcs and a.log and a.elf) or len(a.pcs) != len(a.log):
        ap.error("--pcs, --log (one per --pcs) and --elf are required")

    fns = functions(a.elf, a.wsl_distro)
    link_main = next((f[0] for f in fns if f[2] == "main"), None)
    if link_main is None:
        raise SystemExit("psp_hot_order: no main in the ELF")
    starts = [f[0] for f in fns]
    import bisect

    def find(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr < fns[i][0] + max(fns[i][1], 4):
            return i
        return None

    # The profiles' samples are summed as they are: a stage with longer
    # frames weighs more (the same frame range gives it more samples).
    hits = collections.Counter()
    inside = 0
    for pcs, log in zip(a.pcs, a.log):
        main_at = None
        for line in open(log, encoding="utf-8", errors="replace"):
            m = re.search(r"image: main at (0x[0-9a-fA-F]+)", line)
            if m:
                main_at = int(m.group(1), 16)
        if main_at is None:
            raise SystemExit(f"psp_hot_order: no 'image: main at' in {log}")
        delta = main_at - link_main
        data = open(pcs, "rb").read()
        magic, count, _period, _missed = struct.unpack_from("<4I", data, 0)
        if magic != 0x464F5250:
            raise SystemExit(f"psp_hot_order: {pcs} is not a --prof file")
        for pc in struct.unpack_from(f"<{count}I", data, 16):
            i = find(pc - delta)
            if i is not None:
                hits[i] += 1
                inside += 1

    # The game and overlay objects in link order (their sa1_obj_* markers):
    # the warm list leaves their code in them, whole.
    mk = markers(a.elf, a.wsl_distro)
    objs = sorted((ad, n) for n, ad in mk.items() if n != "sa1_obj_end")
    chunks = ["=" + n[len("sa1_obj_"):-2] + ".c" for _ad, n in objs if n.endswith("_c")]
    game_lo = objs[0][0] if objs else 0
    game_hi = mk.get("sa1_obj_end", game_lo)

    hot, warm, acc, wacc = [], [], 0, 0
    for i, k in sorted(hits.items(), key=lambda x: (-x[1], fns[x[0]][2], x[0])):
        if k < a.min_samples:
            break
        if acc < a.cover * inside:
            hot.append(i)
            acc += k
        elif acc + wacc < a.warm_cover * inside:
            if not game_lo <= fns[i][0] < game_hi:
                warm.append(i)
            wacc += k
        else:
            break
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
    done = set()

    def emit(order, tag=""):
        lines, size = [], 0
        for i in order:
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
            lines.append(("%-40s # %5.2f %%  %6d B" + tag) % (entry, 100.0 * hits[i] / inside, sz))
        return lines, size

    lines, size = emit([i for c in chains for i in c])
    wlines, wsize = emit(warm, "  warm")
    profs = " + ".join(os.path.basename(p) for p in a.pcs)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The PSP build's hot code, placed first in .text in this order (SA1_PSP_HOT_ORDER,\n"
                "# backend/psp/CMakeLists.txt).  Written by tools/psp_hot_order.py from console\n"
                "# profiles: %s, %d functions, %.1f %% of the samples inside the PRX, %d bytes.\n"
                "# Share of samples and size are of the profiled build.\n"
                % (profs, len(lines), 100.0 * acc / inside, size))
        f.write("\n".join(lines) + "\n")
        if wlines:
            f.write("# Warm HLE, backend and library code (up to %.0f %% of the samples, %d bytes) after\n"
                    "# the hot list: outside it, any edit to the code before it would move it.\n"
                    % (100.0 * a.warm_cover, wsize))
            f.write("\n".join(wlines) + "\n")
        if chunks:
            f.write("\n# The game and overlay objects, whole, after the libraries, in link order: each pinned\n"
                    "# at its start, so a lever that changes one chunk moves none after it (--pin).\n")
            f.write("\n".join(chunks) + "\n")
    print(f"psp_hot_order: {len(lines)} hot + {len(wlines)} warm functions, {100.0 * acc / inside:.1f} + "
          f"{100.0 * wacc / inside:.1f} % of {inside} samples, {size} + {wsize} bytes, {len(chunks)} objects -> {a.out}")
    return 0


def pin(a) -> int:
    """Rewrite --list with an "@ADDR" line before every entry and one after the
    last: each listed function, and the code after the list, at its address
    modulo 16 KB in --pin's ELF (a build of that list)."""
    fns = functions(a.pin, a.wsl_distro)
    by_name = collections.defaultdict(list)
    for f in fns:
        by_name[f[2]].append(f)
    members = collections.defaultdict(list)
    for name, mem in lib_members(a.wsl_distro).items():
        members[mem].append(name)

    def span(entry):
        if re.match(r"^[^:]+\.a:[^:]+$", entry):
            fs = [f for n in members.get(entry, []) for f in by_name.get(n, []) if f[3] != "LOCAL"]
        elif ":" in entry:
            src, name = entry.split(":", 1)
            fs = [f for f in by_name.get(name, []) if f[4] == src]
        else:
            fs = by_name.get(entry, [])
            fs = [f for f in fs if f[3] != "LOCAL"] or fs
        if not fs:
            return None
        return min(f[0] for f in fs), max(f[0] + f[1] for f in fs)

    objs = markers(a.pin, a.wsl_distro)

    def list_end():
        if end is not None:
            out.append("@0x%-38x # the code after the list" % (end & 0x3FFF))

    out, pinned, end, in_objs = [], 0, None, False
    for line in open(a.list, encoding="utf-8"):
        line = line.rstrip("\n")
        entry = line.split("#")[0].strip()
        if entry.startswith("@") or entry.startswith("=@"):
            continue                      # the old pins: every entry gets a new one
        if entry.startswith("="):
            if not in_objs:
                list_end()
                in_objs = True
            name = entry[1:].split()[0]
            addr = objs.get("sa1_obj_" + re.sub(r"\W", "_", name))
            if addr is not None:
                line = "=%s @0x%x" % (name, addr & 0x3FFF)
                pinned += 1
            else:
                line = "=" + name
        elif entry:
            s = span(entry)
            if s:
                out.append("@0x%x" % (s[0] & 0x3FFF))
                pinned += 1
                end = s[1]
            else:
                end = None                # a new function: where it ends is unknown
        out.append(line)
    if not in_objs:
        list_end()
    elif "sa1_obj_end" in objs:
        out.append("=@0x%-37x # the code after the game objects" % (objs["sa1_obj_end"] & 0x3FFF))
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print(f"psp_hot_order: {pinned} entries pinned to {os.path.basename(a.pin)} -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
