#!/usr/bin/env python3
"""Run a tape on the desktop build and in Flycast, then diff them by game frame.

  run_oracle.py tests/replay/title_menu.tape [--flycast DIR] [--gdi FILE]

1. Replays the tape on the desktop build with --watch-trace (desktop.tsv).
2. tools/tape_diff.py gpad: the pad of each game frame (gpad.txt).
3. Runs Flycast 2.7 with tools/flycast/tape_oracle.lua, which feeds those
   pads by the game's frame counter and traces the same watches
   (flycast.tsv).  Flycast runs from its own copy of the release
   (../flycast/oracle-2.7 by default, made from ../flycast/release-2.7 on
   first use) with the VMUs unplugged, as the desktop build has none.
4. tools/tape_diff.py diff desktop.tsv flycast.tsv.

Needs SA1_DUMP (the data and the GD-ROM image) and a desktop build
(--exe, $SA1_EXE, build-desktop/ or build-tape/).  Outputs go to --work
(default a temporary directory, printed).  Flycast is GPL and stays
outside the repository; so do the traces (they are game state).

Part of sa1-psp (MIT).
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import replay_test  # noqa: E402
import tape_diff  # noqa: E402
import tape_io  # noqa: E402

WATCH = os.path.join(ROOT, "tests", "replay", "watch.txt")
FLYCAST_HOME = os.path.join(os.path.dirname(ROOT), "flycast")


def oracle_dir(path):
    """A Flycast copy of our own: portable config (emu.cfg beside the exe), no VMU saves."""
    exe = os.path.join(path, "flycast.exe")
    if os.path.isfile(exe):
        return exe
    release = os.path.join(FLYCAST_HOME, "release-2.7", "flycast.exe")
    if not os.path.isfile(release):
        sys.exit("no Flycast: %s or %s" % (exe, release))
    os.makedirs(os.path.join(path, "data"), exist_ok=True)
    shutil.copy2(release, exe)
    open(os.path.join(path, "emu.cfg"), "w").close()
    print("oracle: made %s from %s" % (path, os.path.dirname(release)))
    return exe


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("tape")
    ap.add_argument("--exe", help="desktop sa1psp executable")
    ap.add_argument("--flycast", default=os.path.join(FLYCAST_HOME, "oracle-2.7"), help="Flycast directory")
    ap.add_argument("--gdi", help="GD-ROM image (default: the .gdi in $SA1_DUMP)")
    ap.add_argument("--work", help="output directory")
    ap.add_argument("--timeout", type=int, default=0, help="seconds to wait for Flycast (default: 3x the tape at 60 Hz + 120)")
    a = ap.parse_args(argv)

    dump = os.environ.get("SA1_DUMP")
    if not dump:
        print("skip: SA1_DUMP is not set")
        return 0
    data = os.path.abspath(os.path.join(dump, "data"))
    gdi = a.gdi or next(iter(sorted(glob.glob(os.path.join(dump, "*.gdi")))), None)
    if not gdi:
        sys.exit("no .gdi in %s (pass --gdi)" % dump)
    exe = replay_test.find_exe(a.exe)
    if not exe:
        sys.exit("no desktop executable")
    work = os.path.abspath(a.work or tempfile.mkdtemp(prefix="sa1-oracle-"))
    os.makedirs(work, exist_ok=True)
    tape = os.path.abspath(a.tape)
    desk, gpad, fly = (os.path.join(work, n) for n in ("desktop.tsv", "gpad.txt", "flycast.tsv"))

    code, _ = replay_test.run(os.path.abspath(exe), data, ["--replay", tape, "--watch-list", WATCH, "--watch-trace", desk],
                              os.path.join(work, "desktop.log"))
    if code != 0:
        sys.exit("desktop replay failed (exit %d), log %s" % (code, os.path.join(work, "desktop.log")))
    tape_diff.main(["gpad", tape, desk, "-o", gpad])

    fexe = oracle_dir(a.flycast)
    shutil.copy2(os.path.join(HERE, "tape_oracle.lua"), os.path.join(os.path.dirname(fexe), "tape_oracle.lua"))
    env = dict(os.environ, SA1_ORACLE_GPAD=gpad, SA1_ORACLE_WATCH=WATCH, SA1_ORACLE_OUT=fly)
    frames = tape_io.read(tape).end
    timeout = a.timeout or frames * 3 // 60 + 120
    env["SA1_ORACLE_MAX"] = str(frames * 4 + 3600)      # loads take vblanks the desktop does not
    cmd = [fexe, "-config", "config:LuaFileName=tape_oracle.lua,input:device1.1=10,input:device1.2=10", os.path.abspath(gdi)]
    print("oracle: running Flycast (up to %d s): %s" % (timeout, " ".join(cmd)))
    try:
        subprocess.run(cmd, env=env, cwd=os.path.dirname(fexe), timeout=timeout)
    except subprocess.TimeoutExpired:
        print("oracle: Flycast did not exit in %d s; diffing what it traced" % timeout)
    if not os.path.isfile(fly):
        sys.exit("Flycast wrote no trace (Lua not loaded? see %s)" % os.path.dirname(fexe))
    print("oracle: traces in %s" % work)
    return tape_diff.main(["diff", desk, fly, "--watch", WATCH])


if __name__ == "__main__":
    sys.exit(main())
