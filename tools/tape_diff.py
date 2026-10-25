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

  tape_diff.py quantize TAPE DESKTOP.tsv -o OUT.tape [--from FRAME]
      TAPE with every vblank given the pad of the first vblank of its game
      frame (by DESKTOP.tsv, a --watch-trace of TAPE), so the pad changes
      only where a game frame starts.  The adventure field runs two logic
      steps per game frame, each reading the pad after one vblank of the
      frame's wait (0x8C0517F6); when the console runs behind it does both
      in one vblank period, so both read the same latched pad and a pad
      change between the two cannot reach it as it reached the desktop.
      Replay the output once more and check that it reports no change: a
      different input can move a game-frame boundary.

  tape_diff.py slow TAPE DESKTOP.tsv FLYCAST.tsv -o OUT.tape [--column substeps]
      TAPE with a "slow-substep F" line for each game frame where the
      console ran a logic sub-step more than the desktop.  The game's
      catch-up loop (0x8C04EA4A) adds a third sub-step to a game frame when
      the profiling timer TMU1 shows >= 1.85 vblanks after the second;
      on the console a long GD-ROM load does that, the desktop's loads
      take no time, so the console runs one 1/60 s step ahead from then on
      (seen as a camera a sub-step ahead, then a route off once the
      camera-relative stick moves Sonic).  The line makes the desktop's
      TMU1 reads in vblank F run two vblanks late (hle.h
      hle_tmu_slow_frames).  Both traces need the sub-step counter
      0x8C752BEC as a watch column ("8c752bec 4 substeps trace"); a game
      frame G where console minus desktop grows, and stays grown for the
      next --persist aligned game frames, gives F = the desktop's last
      vblank before G.  (Where a vblank falls inside a game frame differs
      between the runs, so the difference also shows one-frame blips that
      are no extra step.)  Replay the output and run this again on the
      new desktop trace: it must report no new frame.

  tape_diff.py diff DESKTOP.tsv FLYCAST.tsv [--watch watch.txt]
      Compares the watched values at the first vblank of each game frame
      both runs reached; reports the first divergent game frame (exit 1)
      or that none diverged (exit 0).  Tolerances and roles come from the
      watch list: "trace" watches are not compared.

  tape_diff.py band REF.tsv RUN.tsv [--watch watch.txt] [--band B] [--slip N]
                    [--event-slip N] [--until G] [--exact]
      The tolerance comparer: does RUN play the same game as REF?  Both are
      --watch-traces of the same tape (desktop before/after a change,
      desktop vs PSP; a Flycast trace is aligned by its oracle_g column).
      RNG and float bit-exactness are not required (2026-10-25); RUN passes
      when, by game frame:
        route      the changes of the "milestone" watches (game mode, level,
                   act) come in the same order, each within the check
                   files' slack (tape_expect.py: 300 + 2 % of the frame,
                   counted here in game frames);
        events     every integer "exact" watch (rings, lives: a lives change
                   is a death or a 1-up) changes through the same values in
                   the same order, each within --event-slip game frames;
        positions  the "exact" float watches, taken as one vector (the
                   player's X, Y, Z), stay within --band (default: the watch
                   list's largest tolerance) of REF's, or of REF's within
                   --slip game frames; game frames near a milestone change
                   (a teleport) are not compared;
        end        RUN reaches REF's last game frame, within --event-slip
                   (no trap, no soft-lock).  --until G stops at game frame G.
      The largest position deviation and its game frame are printed either
      way (the band a native routine actually holds).  --exact is the
      bit-exact compare: every column but the frame (rng_seed and the other
      "trace" watches too) equal on every frame, by vblank when neither
      trace has oracle_g; for debugging, off by default.  One verdict line
      "band: PASS|FAIL ..."; exit 0 pass, 1 fail, 2 nothing to compare.

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


def cmd_quantize(a):
    tape = tape_io.read(a.tape)
    _, rows = read_tsv(a.trace)
    first, changed = {}, 0
    for r in rows:
        f, g = int(r["frame"]), int(r[COUNTER])
        if g not in first:
            first[g] = f
        elif f >= a.start and tape.pad(f) != tape.pad(first[g]):
            tape.pads[f] = tape.pad(first[g])
            changed += 1
    tape_io.write(a.out, tape)
    print("quantize: %d vblanks changed" % changed, file=sys.stderr)
    return 0


def cmd_slow(a):
    tape = tape_io.read(a.tape)
    hd, drows = read_tsv(a.desktop)
    hf, frows = read_tsv(a.flycast)
    for path, h in ((a.desktop, hd), (a.flycast, hf)):
        if a.column not in h:
            sys.exit("%s: no %s column (watch 8c752bec 4 %s trace)" % (path, a.column, a.column))
    last_vb = {}                   # desktop game frame -> its last vblank
    for r in drows:
        last_vb[int(r[COUNTER])] = int(r["frame"])
    d, f = by_game_frame(drows), by_game_frame(frows)
    gs = sorted(set(d) & set(f))
    deltas = [int(f[g][a.column]) - int(d[g][a.column]) for g in gs]
    prev, added = None, 0
    for i, (g, delta) in enumerate(zip(gs, deltas)):
        if (prev is not None and delta > prev and g - 1 in last_vb
                and len(deltas) > i + a.persist and min(deltas[i:i + a.persist + 1]) >= delta):
            vb = last_vb[g - 1]
            if vb not in tape.slow:
                tape.slow.append(vb)
                added += 1
            print("slow: game frame %d: console +%d sub-step(s) -> slow-substep %d%s"
                  % (g, delta - prev, vb, "" if delta - prev == 1 else " (more than one: check by hand)"),
                  file=sys.stderr)
        prev = delta
    tape_io.write(a.out, tape)
    print("slow: %d frame(s) added" % added, file=sys.stderr)
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


def segment_of(row, names):
    return tuple(row[n] for n in names)


def changes(rows, gs, name):
    """[(game frame, value)] each time `name` changes, first value included."""
    out, cur = [], None
    for g in gs:
        v = rows[g][name]
        if v != cur:
            out.append((g, v))
            cur = v
    return out


def cmd_band_exact(href, rref, hrun, rrun):
    cols = [h for h in href if h in hrun and h not in ("frame", ALIGNED)]
    if ALIGNED in href or ALIGNED in hrun:
        a, b, key = by_game_frame(rref), by_game_frame(rrun), "game frame"
    else:
        a = {int(r["frame"]): r for r in rref}
        b = {int(r["frame"]): r for r in rrun}
        key = "frame"
    common = sorted(set(a) & set(b))
    if not common:
        print("band: no %s in common" % key)
        return 2
    for k in common:
        bad = [c for c in cols if a[k][c] != b[k][c]]
        if bad:
            print("band: FAIL exact at %s %d: %s" % (key, k, ", ".join(
                "%s %.9g != %.9g" % (c, a[k][c], b[k][c]) for c in bad)))
            return 1
    if max(b) < max(a):
        print("band: FAIL exact: the run stops at %s %d, the reference at %d" % (key, max(b), max(a)))
        return 1
    print("band: PASS exact: %d %ss identical in %d columns (%s)" % (len(common), key, len(cols), ", ".join(cols)))
    return 0


def cmd_band(a):
    href, rref = read_tsv(a.ref)
    hrun, rrun = read_tsv(a.run)
    if a.exact:
        return cmd_band_exact(href, rref, hrun, rrun)
    watches = [w for w in tape_expect.read_watch(a.watch) if w.name in href and w.name in hrun]
    mile = [w for w in watches if w.role == "milestone"]
    events = [w for w in watches if w.role == "exact" and not w.is_float()]
    pos = [w for w in watches if w.role == "exact" and w.is_float()]
    band = a.band if a.band is not None else max([w.tol for w in pos] or [0.0])
    R, C = by_game_frame(rref), by_game_frame(rrun)
    if a.until is not None:
        R = {g: r for g, r in R.items() if g <= a.until}
        C = {g: r for g, r in C.items() if g <= a.until}
    if not R or not C:
        print("band: nothing to compare (reference %d, run %d game frames)" % (len(R), len(C)))
        return 2
    gr, gc = sorted(R), sorted(C)
    end = min(gr[-1], gc[-1])
    fails = []

    # route: milestone changes in order, within the check files' slack
    for w in mile:
        cr, cc = changes(R, gr, w.name)[1:], changes(C, gc, w.name)[1:]
        n = min(len(cr), len(cc))         # unmatched changes near the end of the shorter run are not
        cr = cr[:n] + [(g, v) for g, v in cr[n:] if g <= end - a.slack]     # compared ("end" covers a short run)
        cc = cc[:n] + [(g, v) for g, v in cc[n:] if g <= end - a.slack]
        for i, (g, v) in enumerate(cr):
            if i >= len(cc):
                fails.append("route: %s never becomes %g (reference at game frame %d)" % (w.name, v, g))
                break
            g2, v2 = cc[i]
            s = a.slack + int(g * a.slack_pct / 100)
            if v2 != v:
                fails.append("route: %s change %d is %g at game frame %d, the reference's %g at %d"
                             % (w.name, i + 1, v2, g2, v, g))
                break
            if abs(g2 - g) > s:
                fails.append("route: %s becomes %g at game frame %d, the reference at %d (slack %d)"
                             % (w.name, v, g2, g, s))
                break
        else:
            extra = cc[len(cr):]
            if extra:
                fails.append("route: %s becomes %g at game frame %d, which the reference never does"
                             % (w.name, extra[0][1], extra[0][0]))

    # events: the same values in the same order, each within --event-slip
    nev = 0
    for w in events:
        cr, cc = changes(R, gr, w.name), changes(C, gc, w.name)
        n = min(len(cr), len(cc))
        cr = cr[:n] + [(g, v) for g, v in cr[n:] if g <= end - a.event_slip]
        cc = cc[:n] + [(g, v) for g, v in cc[n:] if g <= end - a.event_slip]
        nev += max(0, len(cr) - 1)
        for i in range(max(len(cr), len(cc))):
            if i >= len(cr) or i >= len(cc):
                g, v = (cc[i] if i < len(cc) else cr[i])
                fails.append("events: %s %g at game frame %d in only the %s" % (
                    w.name, v, g, "run" if i < len(cc) else "reference"))
                break
            (g, v), (g2, v2) = cr[i], cc[i]
            if v != v2:
                fails.append("events: %s change %d is %g at game frame %d, the reference's %g at %d"
                             % (w.name, i, v2, g2, v, g))
                break
            if abs(g2 - g) > a.event_slip:
                fails.append("events: %s becomes %g at game frame %d, the reference at %d (slip %d)"
                             % (w.name, v, g2, g, a.event_slip))
                break

    # positions: within the band of the reference's, or of its own +-slip game frames
    worst, worst_g, raw_worst, compared, outside = 0.0, None, 0.0, 0, 0
    first_out = None
    if pos:
        names = [w.name for w in mile]
        near = set()                       # game frames next to a milestone change in either run
        for rows, gs in ((R, gr), (C, gc)):
            for w in mile:
                for g, _ in changes(rows, gs, w.name)[1:]:
                    near.update(range(g - a.settle, g + a.settle + 1))

        def dist(r1, r2):
            return sum((r1[w.name] - r2[w.name]) ** 2 for w in pos) ** 0.5

        for g in gr:
            if g not in C or g in near or segment_of(R[g], names) != segment_of(C[g], names):
                continue
            d0 = dist(R[g], C[g])
            raw_worst = max(raw_worst, d0)
            d = d0
            if d > band:
                for k in range(1, a.slip + 1):
                    for h in (g - k, g + k):
                        if h in R and segment_of(R[h], names) == segment_of(C[g], names):
                            d = min(d, dist(R[h], C[g]))
            compared += 1
            if d > worst:
                worst, worst_g = d, g
            if d > band:
                outside += 1
                if first_out is None:
                    first_out = (g, d, [(w.name, R[g][w.name], C[g][w.name]) for w in pos])
        if first_out:
            g, d, vals = first_out
            fails.append("positions: %d game frame(s) outside the band %g, first at game frame %d (%.4g: %s)"
                         % (outside, band, g, d, ", ".join("%s %.6g/%.6g" % t for t in vals)))

    # end: the run reaches the reference's last game frame
    if gc[-1] < gr[-1] - a.event_slip:      # a lag frame more or less is not a stop
        fails.append("end: the run stops at game frame %d, the reference reaches %d (trap, soft-lock or short run)"
                     % (gc[-1], gr[-1]))

    print("compared game frames %d..%d (reference), %d..%d (run): %d route changes, %d events, "
          "%d position frames" % (gr[0], gr[-1], gc[0], gc[-1],
                                  sum(len(changes(R, gr, w.name)) - 1 for w in mile), nev, compared))
    where = "game frame %d" % worst_g if worst_g is not None else "no game frame"
    print("position deviation: max %.6g at %s (%.6g without the slip), band %g" % (worst, where, raw_worst, band))
    for f in fails:
        print("  " + f)
    if fails:
        print("band: FAIL %s" % fails[0])
        return 1
    print("band: PASS (band %g, max %.4g at %s; route, events and end match)" % (band, worst, where))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gpad", help="game-frame pad table for tape_oracle.lua")
    g.add_argument("tape")
    g.add_argument("trace", help="desktop --watch-trace of a replay of TAPE")
    g.add_argument("-o", "--out")
    q = sub.add_parser("quantize", help="pad changes moved to the start of their game frame")
    q.add_argument("tape")
    q.add_argument("trace", help="desktop --watch-trace of a replay of TAPE")
    q.add_argument("-o", "--out", required=True)
    q.add_argument("--from", dest="start", type=int, default=0, help="leave tape frames before this one")
    s = sub.add_parser("slow", help="slow-substep lines where the console ran an extra logic sub-step")
    s.add_argument("tape")
    s.add_argument("desktop", help="desktop --watch-trace of a replay of TAPE")
    s.add_argument("flycast", help="oracle trace of the same tape")
    s.add_argument("-o", "--out", required=True)
    s.add_argument("--persist", type=int, default=8,
                   help="aligned game frames the larger difference must last (default 8)")
    s.add_argument("--column", default="substeps", help="the 0x8C752BEC watch column (default substeps)")
    d = sub.add_parser("diff", help="first divergent game frame")
    d.add_argument("desktop")
    d.add_argument("flycast")
    d.add_argument("--watch", default=tape_expect.DEFAULT_WATCH)
    d.add_argument("--start", type=int, default=0, help="ignore game frames before this one")
    d.add_argument("--context", type=int, default=8, help="game frames to show after the first divergence")
    d.add_argument("--slip", type=int, default=2,
                   help="a value Flycast has this many game frames earlier or later is a slip, not a divergence "
                        "(default 2; 0 = frame-exact)")
    b = sub.add_parser("band", help="tolerance compare: same route and events, positions within a band")
    b.add_argument("ref", help="reference --watch-trace")
    b.add_argument("run", help="--watch-trace of the run under test (same tape)")
    b.add_argument("--watch", default=tape_expect.DEFAULT_WATCH)
    b.add_argument("--band", type=float, help="position band (default: the watch list's largest float tolerance)")
    b.add_argument("--slip", type=int, default=2,
                   help="a position the reference has this many game frames earlier or later counts (default 2)")
    b.add_argument("--event-slip", type=int, default=30,
                   help="game frames an event (rings, lives) may move (default 30)")
    b.add_argument("--slack", type=int, default=300, help="milestone slack, game frames (default 300)")
    b.add_argument("--slack-pct", type=float, default=2.0, help="plus this percent of the game frame (default 2)")
    b.add_argument("--settle", type=int, default=30,
                   help="game frames either side of a milestone change left out of the band (default 30)")
    b.add_argument("--until", type=int, help="compare game frames up to this one only")
    b.add_argument("--exact", action="store_true", help="bit-exact compare of every column (debugging)")
    a = ap.parse_args(argv)
    return {"gpad": cmd_gpad, "quantize": cmd_quantize, "slow": cmd_slow,
            "band": cmd_band}.get(a.cmd, cmd_diff)(a)


if __name__ == "__main__":
    sys.exit(main())
