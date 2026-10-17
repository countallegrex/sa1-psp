#!/usr/bin/env python3
"""Replay every input tape in tests/replay/ and check it.

For each NAME.tape with a NAME.check beside it, runs the desktop build
headless with --replay NAME.tape --check NAME.check --expect-frame END and
reports pass/fail: a primary check failing, a trap, the spin watchdog or a
run that stops before the tape's end fails the tape; secondary rows that
drift are reported but pass.  Skips cleanly (exit 0) when SA1_DUMP is unset.

  SA1_DUMP=..\\private\\dump\\sa1_us_1005 python tools/replay_test.py
  python tools/replay_test.py --update title_menu    # regenerate a check file

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
import tape_expect  # noqa: E402
import tape_io  # noqa: E402


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


def run(exe, data, args, log):
    cmd = [exe, os.path.join(data, "1ST_READ.BIN"), data, "--headless", "--log", log] + args
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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", help="sa1psp executable")
    ap.add_argument("--update", metavar="NAME", action="append", default=[],
                    help="regenerate NAME.check from a traced replay of NAME.tape (repeatable)")
    ap.add_argument("--keep", action="store_true", help="keep the run logs (printed paths)")
    ap.add_argument("names", nargs="*", help="tapes to run (default: all)")
    a = ap.parse_args(argv)

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
