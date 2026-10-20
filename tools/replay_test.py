#!/usr/bin/env python3
"""Replay every input tape in tests/replay/ and check it.

For each NAME.tape with a NAME.check beside it, runs the desktop build
headless with --replay NAME.tape --check NAME.check --expect-frame END and
reports pass/fail: a primary check failing, a trap, the spin watchdog or a
run that stops before the tape's end fails the tape; secondary rows that
drift are reported but pass.  Skips cleanly (exit 0) when SA1_DUMP is unset.

  SA1_DUMP=..\\private\\dump\\sa1_us_1005 python tools/replay_test.py
  python tools/replay_test.py --update title_menu    # regenerate a check file
  python tools/replay_test.py --savestate            # save states: load at 3000, identical at 6000

--savestate records the scripted route (the M1 presses and the Emerald
Coast pokes at 5290) in strict mode with --save 3000 and --save 6000 and
--dump 6000, then replays the tape from the 3000 state to 6000 with the
same --dump and --save: the RAM dumps must be byte-identical, and so must
every chunk of the two 6000 states but the coroutine stacks (dead bytes
between frames differ; docs/savestate.md section 5).

The game data is $SA1_DUMP/data (1ST_READ.BIN and SONICADV/).  The
executable is --exe, $SA1_EXE, or the first of build-desktop/ and
build-tape/ that has one.

Part of sa1-psp (MIT).
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAPES = os.path.join(ROOT, "tests", "replay")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import state_diff  # noqa: E402
import tape_expect  # noqa: E402
import tape_io  # noqa: E402

# The scripted route (docs/m2-render-report.md section 2): title, Start, new
# Sonic story, file select, Station Square, then the stage variables poked to
# Emerald Coast act 1 at frame 5290 when Chaos 0 would load.
ROUTE_PRESSES = ["--press", "500:8:4", "--press", "900:8:4", "--press", "1300:4:4", "--press", "1700:4:4", "--press", "2100:4:4"]
ROUTE_POKES = ["--poke", "5290:8c7493ba:1:2", "--poke", "5290:8c7493bc:0:2", "--poke", "5290:8c7493be:1:2", "--poke", "5290:8c7493c0:0:2"]


def tape_end(path):
    """The last frame a tape defines."""
    return tape_io.read(path).end


def find_exe(arg):
    if arg:
        return arg
    if os.environ.get("SA1_EXE"):
        return os.environ["SA1_EXE"]
    for build in ("build-desktop", "build-tape"):
        for name in ("sa1psp.exe", "sa1psp"):
            p = os.path.join(ROOT, build, "backend", "desktop", name)
            if os.path.isfile(p):
                return p
    return None


GAME_ARGS = []      # --game-arg: options added to every run


def run(exe, data, args, log):
    cmd = [exe, os.path.join(data, "1ST_READ.BIN"), data, "--headless", "--log", log] + args + GAME_ARGS
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
    return p.returncode, text + p.stderr


def verdict(code, log):
    m = re.findall(r"check: (PASS|FAIL) at frame (\d+): (\d+)/(\d+) primary failed, (\d+)/(\d+) secondary drifted", log)
    why = []
    if "watchdog:" in log:
        why.append("watchdog (spin)")
    if re.search(r"trap at 0x", log):
        why.append("trap")
    if code not in (0, 1) and not why:
        why.append("exit code %d" % code)
    fails = [line.split("check: ", 1)[1].strip() for line in log.splitlines()
             if "check: FAIL " in line and "check: FAIL at frame" not in line]
    why += fails[:3] + (["%d more" % (len(fails) - 3)] if len(fails) > 3 else [])
    ok = code == 0 and bool(m) and m[-1][0] == "PASS"
    drift = int(m[-1][4]) if m else 0
    return ok, drift, why


def savestate_check(exe, data, tmp, save_at=3000, end=6000):
    """Straight run with saves at save_at and end, then a run loaded at save_at;
    the dumps at end must be identical.  Returns 0 or 1."""
    j = lambda name: os.path.join(tmp, name)  # noqa: E731
    print("savestate: straight run to %d (save at %d and %d) ..." % (end, save_at, end))
    code, text = run(exe, data, ["--record", j("route.tape"), "--frames", str(end), "--save", "%d:%s" % (save_at, j("a.state")),
                                 "--save", "%d:%s" % (end, j("a_end.state")), "--dump", "%d:%s" % (end, j("a.bin"))] + ROUTE_PRESSES + ROUTE_POKES, j("a.log"))
    if code != 0 or not os.path.isfile(j("a.state")):
        print("savestate: FAIL the straight run exited %d; log %s" % (code, j("a.log")))
        return 1
    print("savestate: run loaded from %d to %d ..." % (save_at, end))
    code, text = run(exe, data, ["--replay", j("route.tape"), "--load", j("a.state"), "--frames", str(end),
                                 "--save", "%d:%s" % (end, j("b_end.state")), "--dump", "%d:%s" % (end, j("b.bin"))] + ROUTE_POKES, j("b.log"))
    if code != 0 or not os.path.isfile(j("b.bin")):
        print("savestate: FAIL the loaded run exited %d; log %s" % (code, j("b.log")))
        return 1
    a, b = open(j("a.bin"), "rb").read(), open(j("b.bin"), "rb").read()
    if a != b:
        first = next(i for i in range(min(len(a), len(b))) if a[i] != b[i])
        print("savestate: FAIL the dumps at %d differ (%d bytes vs %d, first at guest 0x%08x)" % (end, len(a), len(b), 0x8C000000 + first))
        return 1
    ha, ca = state_diff.read_state(j("a_end.state"))
    hb, cb = state_diff.read_state(j("b_end.state"))
    bad = [na for (na, pa), (nb, pb) in zip(ca, cb) if (na != nb or pa != pb) and na not in ("stack", "stack.bytes")]
    if len(ca) != len(cb) or bad:
        print("savestate: FAIL the %d states differ outside the stacks: %s" % (end, ", ".join(bad) or "chunk count"))
        return 1
    print("savestate: pass  dump at %d identical (%d bytes); states at %d identical outside the stacks (%d chunks, %d bytes packed); state at %d is %d bytes"
          % (end, len(a), end, len(ca), ha["packed_len"], save_at, os.path.getsize(j("a.state"))))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", help="sa1psp executable")
    ap.add_argument("--savestate", action="store_true", help="save-state self-check on the scripted route (docs/savestate.md section 5)")
    ap.add_argument("--update", metavar="NAME", action="append", default=[],
                    help="regenerate NAME.check from a traced replay of NAME.tape (repeatable)")
    ap.add_argument("--keep", action="store_true", help="keep the run logs (printed paths)")
    ap.add_argument("--game-arg", action="append", default=[], help="an option for every run (e.g. --game-arg=--ta-direct)")
    ap.add_argument("names", nargs="*", help="tapes to run (default: all)")
    a = ap.parse_args(argv)
    GAME_ARGS[:] = a.game_arg

    dump = os.environ.get("SA1_DUMP")
    if not dump:
        print("skip: SA1_DUMP is not set (tapes need the game data)")
        return 0
    data = os.path.abspath(os.path.join(dump, "data"))
    if not os.path.isfile(os.path.join(data, "1ST_READ.BIN")):
        print("error: no 1ST_READ.BIN in %s" % data)
        return 2
    exe = find_exe(a.exe)
    exe = os.path.abspath(exe) if exe else None
    if not exe or not os.path.isfile(exe):
        print("error: no sa1psp executable (build the desktop target, or pass --exe / set SA1_EXE)")
        return 2

    tmp = tempfile.mkdtemp(prefix="sa1-replay-")
    watch = os.path.join(TAPES, "watch.txt")
    if a.savestate:
        return savestate_check(exe, data, tmp)
    for name in a.update:
        tape = os.path.join(TAPES, name + ".tape")
        trace, log = os.path.join(tmp, name + ".tsv"), os.path.join(tmp, name + "-update.log")
        code, text = run(exe, data, ["--replay", tape, "--watch-list", watch, "--watch-trace", trace], log)
        if code != 0:
            print("update %s: the replay failed (exit %d), check file not written; log %s" % (name, code, log))
            return 1
        tape_expect.main(["--trace", trace, "--watch", watch, "-o", os.path.join(TAPES, name + ".check")])
        print("update %s: wrote tests/replay/%s.check" % (name, name))

    tapes = sorted(glob.glob(os.path.join(TAPES, "*.tape")))
    if a.names:
        tapes = [t for t in tapes if os.path.splitext(os.path.basename(t))[0] in a.names]
    failed = 0
    for tape in tapes:
        name = os.path.splitext(os.path.basename(tape))[0]
        check = os.path.join(TAPES, name + ".check")
        if not os.path.isfile(check):
            print("%-24s skip (no %s.check)" % (name, name))
            continue
        end = tape_end(tape)
        log = os.path.join(tmp, name + ".log")
        code, text = run(exe, data, ["--replay", tape, "--check", check, "--expect-frame", str(end)], log)
        ok, drift, why = verdict(code, text)
        failed += not ok
        status = "pass" if ok else "FAIL"
        extra = []
        if drift:
            extra.append("%d secondary drifted" % drift)
        extra += why
        print("%-24s %s  %d frames%s" % (name, status, end, ("  (" + "; ".join(extra) + ")") if extra else ""))
        if not ok or a.keep:
            print("%-24s log %s" % ("", log))
    print("%d tape(s), %d failed" % (len(tapes), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
