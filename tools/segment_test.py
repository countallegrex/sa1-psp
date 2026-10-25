#!/usr/bin/env python3
"""Save-state segments: a change judged scene by scene from the same state.

A vblank-indexed tape drives the same game only while the run under test
keeps the reference's timing: once a change moves the rand() sequence (the
Chaos 0 fight does), the tape's later inputs reach other moments and the
whole-tape compare says nothing.  This driver cuts the tape at its scenes
instead (plan-30fps.md section 8, gate (2) for game-code levers):

  1. replays TAPE with the A options and a watch trace (the reference);
  2. finds the segment starts: each change of a "milestone" watch (game
     mode, level, act) that holds 30 frames, merged when closer than
     --min-len; the first segment starts at boot;
  3. replays TAPE again with a save state at each start (the same build:
     a state belongs to one executable, docs/savestate.md);
  4. replays every segment twice from its state, with A and with A + B,
     to the next start, each with a watch trace;
  5. compares the two with tools/tape_diff.py band (4.0 units, +-2 slip,
     events 30 game frames) --until the game frame before the first
     rng_seed split, or the segment's end when rand() stays in step.

B is the change under test as a runtime switch of the same build (a math
routine on, a lever's option); with no B the driver checks itself (every
segment bit-identical).  Each segment's A run is also compared with the
reference trace bit for bit (the state restores the game exactly).

  SA1_DUMP=..\\private\\dump\\sa1_us_1005 python tools/segment_test.py sonic_to_ec1_start --b=--fmac --b=fused
  python tools/segment_test.py TAPE [--a=OPT]... [--b=OPT]... [--exe EXE | --psp] [--out DIR]
                                    [--starts F,F,...] [--min-len N] [--band B] [--slip N] [--event-slip N]

--psp runs every replay on the console through tools/psp_run.py (the
build in build-psp/sa1psp, PSPLink connected; the PSP's save states hold
the same guarantee, trace-proven 2026-10-25: a load at 4026 replays the
straight run's next 974 frames identically, rng_seed included).  A segment
pass there costs about the segment's length at ~45 fps plus ~15 s a run.

TAPE is a path or a name in tests/replay/.  One line per segment and a
verdict; exit 0 when every segment passes, 1 otherwise, 2 on a setup error.
The traces and states stay in --out (default a temporary directory;
states are ~20-30 MB each).

Part of sa1-psp (MIT).
"""
import argparse
import contextlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import replay_test  # noqa: E402
import tape_diff  # noqa: E402
import tape_expect  # noqa: E402

WATCH = os.path.join(ROOT, "tests", "replay", "watch.txt")
PSP_RUN = os.path.join(ROOT, "build-psp", "sa1psp", "psplink-run")


def segment_starts(trace, watches, settle, min_len, end):
    rows = tape_expect.read_trace(trace, watches)
    starts = sorted({f for f, _, _ in tape_expect.milestones(rows, watches, settle) if f > 1})
    out = []
    for f in starts:
        if f >= end - min_len:
            break
        if f - (out[-1] if out else 1) >= min_len:
            out.append(f)
    return out


def band(ref, run, *args):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = tape_diff.main(["band", ref, run, "--watch", WATCH, *args])
    text = buf.getvalue()
    verdict = next((ln for ln in text.splitlines() if ln.startswith("band:")), "band: ?")
    dev = next((ln for ln in text.splitlines() if ln.startswith("position deviation:")), "")
    return rc, verdict, dev


def rng_split(a, b):
    """(vblank, game frame) of the first row where rng_seed differs, or None."""
    _, ra = tape_diff.read_tsv(a)
    _, rb = tape_diff.read_tsv(b)
    if tape_diff.RNG not in (ra[0] if ra else {}):
        return None
    for x, y in zip(ra, rb):
        if x[tape_diff.RNG] != y[tape_diff.RNG]:
            return int(x["frame"]), int(x[tape_diff.COUNTER])
    return None


def same_rows(ref, run, lo, hi):
    """First vblank in lo..hi where the two traces differ, or None (and the count compared)."""
    _, ra = tape_diff.read_tsv(ref)
    _, rb = tape_diff.read_tsv(run)
    a = {int(r["frame"]): r for r in ra if lo <= r["frame"] <= hi}
    n = 0
    for r in rb:
        f = int(r["frame"])
        if f in a:
            n += 1
            if a[f] != r:
                return f, n
    return None, n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("tape")
    ap.add_argument("--a", action="append", default=[], help="an option of both runs (repeatable; --a=--opt)")
    ap.add_argument("--b", action="append", default=[], help="an option of the run under test only (repeatable)")
    ap.add_argument("--psp", action="store_true",
                    help="run on the PSP over PSPLink (tools/psp_run.py, build-psp/sa1psp; states ~25 MB stay in its psplink-run/)")
    ap.add_argument("--exe", help="sa1psp executable (default as tools/replay_test.py)")
    ap.add_argument("--out", help="directory for traces, states and logs (default: a new temporary one)")
    ap.add_argument("--starts", help="segment starts as vblank frames, comma-separated (default: the milestones)")
    ap.add_argument("--min-len", type=int, default=300, help="shortest segment in frames (default 300)")
    ap.add_argument("--settle", type=int, default=30, help="frames a milestone value must hold (default 30)")
    ap.add_argument("--band", default="4.0")
    ap.add_argument("--slip", default="2")
    ap.add_argument("--event-slip", default="30")
    a = ap.parse_args(argv)

    tape = a.tape if os.path.isfile(a.tape) else os.path.join(ROOT, "tests", "replay", a.tape + ".tape")
    if not os.path.isfile(tape):
        print("error: no tape %s" % a.tape)
        return 2
    if not a.psp:
        dump = os.environ.get("SA1_DUMP")
        if not dump:
            print("skip: SA1_DUMP is not set (tapes need the game data)")
            return 0
        data = os.path.abspath(os.path.join(dump, "data"))
        exe = replay_test.find_exe(a.exe)
        if not exe or not os.path.isfile(exe):
            print("error: no sa1psp executable (--exe or SA1_EXE)")
            return 2
        exe = os.path.abspath(exe)
    out = a.out or tempfile.mkdtemp(prefix="sa1-seg-")
    os.makedirs(out, exist_ok=True)
    j = lambda name: os.path.join(out, name)  # noqa: E731
    end = replay_test.tape_end(tape)
    watches = tape_expect.read_watch(WATCH)
    if a.psp:                             # the console: host0: is build-psp/sa1psp, paths relative to psplink-run/
        os.makedirs(PSP_RUN, exist_ok=True)
        shutil.copyfile(tape, os.path.join(PSP_RUN, os.path.basename(tape)))
        shutil.copyfile(WATCH, os.path.join(PSP_RUN, "watch.txt"))
        st = lambda name: name  # noqa: E731                       # a state as the game names it
        stfile = lambda name: os.path.join(PSP_RUN, name)  # noqa: E731   # and on this machine
    else:
        st = stfile = j

    def go(name, args):
        if not a.psp:
            code, text = replay_test.run(exe, data, ["--replay", tape, "--watch-list", WATCH,
                                                     "--watch-trace", j(name + ".tsv")] + args, j(name + ".log"))
        else:
            frames = int(args[args.index("--frames") + 1]) if "--frames" in args else end
            cmd = [sys.executable, os.path.join(ROOT, "tools", "psp_run.py"), "--timeout", str(int(frames / 25) + 180),
                   "--out", j(name + ".log"), "--", "--headless", "--replay", os.path.basename(tape),
                   "--watch-list", "watch.txt", "--watch-trace", name + ".tsv"] + args
            p = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
                               creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            text = open(j(name + ".log"), encoding="utf-8", errors="replace").read() if os.path.exists(j(name + ".log")) else ""
            m = re.findall(r"\] exit: \d+ frames, [\d.]+ s, code (\d+)", text)
            code = int(m[-1]) if m and p.returncode == 0 else (p.returncode or 99)
            if os.path.isfile(os.path.join(PSP_RUN, name + ".tsv")):
                shutil.move(os.path.join(PSP_RUN, name + ".tsv"), j(name + ".tsv"))
        bad = [w for w in ("trap at 0x", "] watchdog:") if w in text]     # the game's lines, not psp_run's hints
        return code, bad

    print("segments: %s, %d frames, A %s, B %s; files in %s" % (os.path.basename(tape), end, " ".join(a.a) or "-",
                                                                 " ".join(a.b) or "- (self-check)", out))
    code, bad = go("ref", a.a + ["--frames", str(end)])
    if code != 0 or bad:
        print("error: the reference replay failed (exit %d %s); log %s" % (code, " ".join(bad), j("ref.log")))
        return 2
    if a.starts:
        starts = sorted(int(x) for x in a.starts.split(",") if x)
    else:
        starts = segment_starts(j("ref.tsv"), watches, a.settle, a.min_len, end)
    if len(starts) > (16 if a.psp else 64):
        print("error: %d segment starts, more than one run saves (raise --min-len)" % len(starts))
        return 2
    saves = []
    for f in starts:
        saves += ["--save", "%d:%s" % (f, st("seg_%d.state" % f))]
    code, bad = go("saves", a.a + ["--frames", str(end)] + saves)
    missing = [f for f in starts if not os.path.isfile(stfile("seg_%d.state" % f))]
    if code != 0 or bad or missing:
        print("error: the saving replay failed (exit %d %s; missing states %s); log %s"
              % (code, " ".join(bad), missing, j("saves.log")))
        return 2

    bounds = [(1, starts[0] if starts else end)] + [(f, g) for f, g in zip(starts, starts[1:] + [end])]
    failed = empty = 0
    print("%6s %6s  %-6s %-30s %s" % ("from", "to", "", "rand() split (vblank/game frame)", "verdict"))
    for lo, hi in bounds:
        load = [] if lo == 1 else ["--load", st("seg_%d.state" % lo)]
        tag = "s%d" % lo
        ca, bada = go(tag + "_a", a.a + load + ["--frames", str(hi)])
        cb, badb = go(tag + "_b", a.a + a.b + load + ["--frames", str(hi)])
        if ca != 0 or bada:
            failed += 1
            print("%6d %6d  FAIL   the A run failed (exit %d %s); log %s" % (lo, hi, ca, " ".join(bada), j(tag + "_a.log")))
            continue
        diff, n = same_rows(j("ref.tsv"), j(tag + "_a.tsv"), lo, hi)
        if diff is not None:
            failed += 1
            print("%6d %6d  FAIL   the A run from the state leaves the reference at vblank %d" % (lo, hi, diff))
            continue
        if cb != 0 or badb:
            failed += 1
            print("%6d %6d  FAIL   the B run failed (exit %d %s); log %s" % (lo, hi, cb, " ".join(badb), j(tag + "_b.log")))
            continue
        split = rng_split(j(tag + "_a.tsv"), j(tag + "_b.tsv"))
        extra = ["--band", a.band, "--slip", a.slip, "--event-slip", a.event_slip]
        if split:
            extra += ["--until", str(split[1] - 1)]
        rc, verdict, dev = band(j(tag + "_a.tsv"), j(tag + "_b.tsv"), *extra)
        where = "%d/%d" % split if split else "none (whole segment)"
        if rc == 2:                       # rand() split before the first game frame: nothing to judge here
            empty += 1
            print("%6d %6d  %-6s %-30s nothing compared (rand() splits in the first game frame)" % (lo, hi, "-", where))
            continue
        failed += rc != 0
        print("%6d %6d  %-6s %-30s %s" % (lo, hi, "pass" if rc == 0 else "FAIL", where, verdict[6:]))
        if dev:
            print("%15s %s" % ("", dev))
    print("segments: %s: %d of %d segment(s) failed, %d with nothing compared"
          % ("PASS" if not failed else "FAIL", failed, len(bounds), empty))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
