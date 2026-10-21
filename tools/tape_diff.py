#!/usr/bin/env python3
"""Compare a desktop run with a Flycast run of the same tape, by game frame.

The two runs cannot be compared by vblank: Flycast emulates GD-ROM load
times and real rendering cost, the desktop build loads instantly, so the
same moment of play happens at different vblanks.  Both are aligned on the
game's own frame counter instead (the "game_frame" watch, 0x8C7A64E4,
tests/replay/README.md), which counts game-loop iterations.  The desktop
build also skips whole phases the console runs (the intro movie, the boot
screens in game modes 12 and 13), so the counter alignment is re-anchored
each time the game mode changes: tape_oracle.lua matches Flycast's mode
changes to the desktop run's (the "sync" lines below) and writes the
aligned desktop game frame as an "oracle_g" column.

  tape_diff.py gpad TAPE DESKTOP.tsv -o PADS.txt
      The pad the desktop run presented in each game frame, for
      tools/flycast/tape_oracle.lua (which reads the counter every vblank
      and feeds Flycast that game frame's pad).  DESKTOP.tsv is the
      desktop replay's --watch-trace.

  tape_diff.py diff DESKTOP.tsv FLYCAST.tsv [--watch watch.txt]
      Compares the watched values at the first vblank of each game frame
      both runs reached; reports the first divergent game frame (exit 1)
      or that none diverged (exit 0).  Tolerances and roles come from the
      watch list: "trace" watches are not compared.

Part of sa1-psp (MIT).
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import tape_expect  # noqa: E402
import tape_io  # noqa: E402

COUNTER = "game_frame"
MODE = "game_mode"
RNG = "rng_seed"
ALIGNED = "oracle_g"     # tape_oracle.lua: the desktop game frame this row aligns with, -1 = none


def read_tsv(path):
    with open(path, encoding="utf-8") as f:
        header = f.readline().rstrip("\n").split("\t")
        if not header or header[0] != "frame" or COUNTER not in header:
            sys.exit("%s: not a watch trace with a %s column" % (path, COUNTER))
        rows = []
        for line in f:
            cells = line.rstrip("\n").split("\t")
            if len(cells) == len(header):
                rows.append({h: float(c) for h, c in zip(header, cells)})
    return header, rows


def by_game_frame(rows):
    """game frame -> the row of the last vblank that showed it: the state
    once that game frame's logic has run (where inside a game frame a vblank
    falls differs between the two runs).  An oracle trace is keyed by its
    aligned game frame; its unaligned rows (game modes the desktop run never
    had) are left out."""
    out = {}
    for r in rows:
        g = int(r[ALIGNED] if ALIGNED in r else r[COUNTER])
        if g >= 0:
            out[g] = r
    return out


def offsets(rows):
    """[(vblank, mode, offset)] each time the oracle's alignment changed."""
    out, cur = [], None
    for r in rows:
        if ALIGNED not in r or r[ALIGNED] < 0:
            continue
        off = int(r[COUNTER] - r[ALIGNED])
        if off != cur:
            out.append((int(r["frame"]), int(r.get(MODE, -1)), off))
            cur = off
    return out


def cmd_gpad(a):
    tape = tape_io.read(a.tape)
    _, rows = read_tsv(a.trace)
    pads, firsts = {}, {}
    for r in rows:                 # the last vblank of a game frame wins: that is when the game samples
        g = int(r[COUNTER])
        pads[g] = tape.pad(int(r["frame"]))
        firsts.setdefault(g, pads[g])   # and the first one (tape_oracle.lua presents it a vblank early)
    if not pads:
        sys.exit("%s: empty trace" % a.trace)
    lines = ["# game-frame pads from %s and %s (tools/tape_diff.py gpad)" % (os.path.basename(a.tape), os.path.basename(a.trace)),
             "# sync MODE GAME_FRAME [SEED]: the desktop run entered game mode MODE at GAME_FRAME,"
             " with the rand() seed SEED (hex; when the trace has an rng_seed column)",
             "# GAME_FRAME BUTTONS STICK_X STICK_Y LTRIG RTRIG: the pad at the game frame's last vblank",
             "# first GAME_FRAME BUTTONS ...: the pad at its first vblank"]
    mode, prev = None, None
    for r in rows:
        if MODE in r and r[MODE] != mode:
            mode = r[MODE]
            g = int(r[COUNTER])    # the first count after the last one seen in the old mode (tape_oracle.lua)
            at = prev + 1 if prev is not None and g > prev + 1 else g
            seed = " %x" % (int(r[RNG]) & 0xFFFFFFFF) if RNG in r else ""
            lines.append("sync %d %d%s" % (mode, at, seed))
        prev = int(r[COUNTER])
    last = (0, 0, 0, 0, 0)
    for g in range(min(pads), max(pads) + 1):
        last = pads.get(g, last)   # a lag frame skips a count: keep the pad
        lines.append("%d %x %d %d %d %d" % ((g,) + last))
        lines.append("first %d %x %d %d %d %d" % ((g,) + firsts.get(g, last)))
    text = "\n".join(lines) + "\n"
    if a.out:
        with open(a.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    print("gpad: game frames %d..%d" % (min(pads), max(pads)), file=sys.stderr)
    return 0


def cmd_diff(a):
    watches = [w for w in tape_expect.read_watch(a.watch) if w.role != "trace" and w.name != COUNTER]
    hd, rd = read_tsv(a.desktop)
    hf, rf = read_tsv(a.flycast)
    watches = [w for w in watches if w.name in hd and w.name in hf]
    d, f = by_game_frame(rd), by_game_frame(rf)
    common = sorted(set(d) & set(f))
    if a.start:
        common = [g for g in common if g >= a.start]
    if not common:
        print("no game frame in common (desktop %s, flycast %s)" % (
            "%d..%d" % (min(d), max(d)) if d else "none", "%d..%d" % (min(f), max(f)) if f else "none"))
        return 2
    print("comparing game frames %d..%d (%d in common; desktop reached %d, flycast %d)"
          % (common[0], common[-1], len(common), max(d), max(f)))
    for vb, mode, off in offsets(rf):
        print("  flycast vblank %d, game mode %d: aligned with desktop game frame + %d" % (vb, mode, off))
    skipped = sum(1 for r in rf if ALIGNED in r and r[ALIGNED] < 0)
    if skipped:
        print("  %d flycast vblanks in game modes the desktop run never had (not compared)" % skipped)
    def close(w, g, k):
        return g + k in f and abs(d[g][w.name] - f[g + k][w.name]) <= w.tol

    first = None
    diverged, slips = {}, []
    for g in common:
        for w in watches:
            if close(w, g, 0):
                continue
            k = next((k for s in range(1, a.slip + 1) for k in (-s, s) if close(w, g, k)), None)
            if k is not None:                 # the same value one or two game frames away
                if not slips or slips[-1][1] != w.name or slips[-1][0] < g - 1:
                    slips.append((g, w.name, k))
                continue
            diverged.setdefault(w.name, g)
            if first is None:
                first = g
        if first is not None and g >= first + a.context:
            break
    for g, name, k in slips[:20]:
        print("  slip: %s at game frame %d matches flycast %+d game frame(s)" % (name, g, k))
    if len(slips) > 20:
        print("  slip: %d more" % (len(slips) - 20))
    if first is None:
        print("no divergence in %d game frames (%d slip(s) within %d game frames)" % (len(common), len(slips), a.slip))
        return 0
    print("first divergent game frame: %d (desktop vblank %d, flycast vblank %d)"
          % (first, d[first]["frame"], f[first]["frame"]))
    for name, g in sorted(diverged.items(), key=lambda t: t[1]):
        print("  %-12s from game frame %d" % (name, g))
    names = [w.name for w in watches]
    print("\n%8s  %s" % ("game", "  ".join("%22s" % n for n in names)))
    for g in [x for x in common if first - 3 <= x <= first + a.context]:
        cells = []
        for w in watches:
            dv, fv = d[g][w.name], f[g][w.name]
            mark = "*" if abs(dv - fv) > w.tol else " "
            cells.append("%10.6g/%-10.6g%s" % (dv, fv, mark))
        print("%8d  %s" % (g, "  ".join(cells)))
    print("(desktop/flycast, * = outside tolerance)")
    return 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gpad", help="game-frame pad table for tape_oracle.lua")
    g.add_argument("tape")
    g.add_argument("trace", help="desktop --watch-trace of a replay of TAPE")
    g.add_argument("-o", "--out")
    d = sub.add_parser("diff", help="first divergent game frame")
    d.add_argument("desktop")
    d.add_argument("flycast")
    d.add_argument("--watch", default=tape_expect.DEFAULT_WATCH)
    d.add_argument("--start", type=int, default=0, help="ignore game frames before this one")
    d.add_argument("--context", type=int, default=8, help="game frames to show after the first divergence")
    d.add_argument("--slip", type=int, default=2,
                   help="a value Flycast has this many game frames earlier or later is a slip, not a divergence "
                        "(default 2; 0 = frame-exact)")
    a = ap.parse_args(argv)
    return cmd_gpad(a) if a.cmd == "gpad" else cmd_diff(a)


if __name__ == "__main__":
    sys.exit(main())
