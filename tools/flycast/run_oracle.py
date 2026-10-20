#!/usr/bin/env python3
"""Run a tape on the desktop build and in Flycast, then diff them by game frame.

  run_oracle.py tests/replay/title_menu.tape [--flycast DIR] [--gdi FILE]
  run_oracle.py TAPE --from-state N [--until G] [--poke-ec]

1. Replays the tape on the desktop build with --watch-trace (desktop.tsv).
2. tools/tape_diff.py gpad: the pad of each game frame (gpad.txt).
3. Runs Flycast 2.7 with tools/flycast/tape_oracle.lua, which feeds those
   pads by the game's frame counter and traces the same watches
   (flycast.tsv).  Flycast runs from its own copy of the release
   (../flycast/oracle-2.7 by default, made from ../flycast/release-2.7 on
   first use) with the VMUs unplugged, as the desktop build has none.
4. tools/tape_diff.py diff desktop.tsv flycast.tsv.

--from-state N starts Flycast from the state library's nearest state at or
before tape frame N instead of from boot (tools/flycast/oracle_states.py:
the states live in ../flycast/states/ with a manifest; the library of the
tape is rebuilt first when the tape's hash no longer matches it), and the
comparison covers the game frames from there.  --until G stops the console
once the aligned desktop game frame reaches G.  --poke-ec applies the
scripted route's Emerald Coast poke on the console side.

Needs SA1_DUMP (the data and the GD-ROM image) and a desktop build
(--exe, $SA1_EXE, build-desktop/ or build-tape/).  Outputs go to --work
(default a temporary directory, printed).  Flycast is GPL and stays
outside the repository; so do the traces and the states (they are game
state).

Part of sa1-psp (MIT).
"""
import argparse
import glob
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)
import oracle_states  # noqa: E402
import replay_test  # noqa: E402
import tape_diff  # noqa: E402
import tape_io  # noqa: E402

WATCH = oracle_states.WATCH
FLYCAST_HOME = oracle_states.FLYCAST_HOME


def oracle_dir(path):
    return oracle_states.oracle_dir(path)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("tape")
    ap.add_argument("--exe", help="desktop sa1psp executable")
    ap.add_argument("--flycast", default=os.path.join(FLYCAST_HOME, "oracle-2.7"), help="Flycast directory")
    ap.add_argument("--gdi", help="GD-ROM image (default: the .gdi in $SA1_DUMP)")
    ap.add_argument("--work", help="output directory")
    ap.add_argument("--timeout", type=int, default=0, help="seconds to wait for Flycast (default: 3x the tape at 60 Hz + 120)")
    ap.add_argument("--from-state", type=int, metavar="N", help="start the console from the library state nearest before tape frame N")
    ap.add_argument("--until", type=int, metavar="G", help="stop the console at desktop game frame G")
    ap.add_argument("--poke-ec", action="store_true", help="the scripted route: --poke 5290 on the desktop replay, the same rewrite on the console when Chaos 0 would load")
    oracle_states.add_timing_args(ap)
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
    desk, gpad = oracle_states.desktop_trace(exe, data, tape, work, a.poke_ec, oracle_states.timing_args(a))
    fly = os.path.join(work, "flycast.tsv")

    fexe = oracle_dir(a.flycast)
    env = dict(os.environ, SA1_ORACLE_GPAD=gpad, SA1_ORACLE_WATCH=WATCH, SA1_ORACLE_OUT=fly)
    frames = tape_io.read(tape).end
    timeout = a.timeout or frames * 3 // 60 + 120
    env["SA1_ORACLE_MAX"] = str(frames * 4 + 3600)      # loads take vblanks the desktop does not
    if a.until is not None:
        env["SA1_ORACLE_STOP"] = str(a.until)
    if a.poke_ec:
        env["SA1_ORACLE_POKE_EC"] = "1"
    extra = []
    if a.from_state is not None:
        m = oracle_states.read_manifest()
        if not oracle_states.is_fresh(m, tape):
            print("oracle: the state library of %s is stale or missing; building it" % oracle_states.tape_key(tape))
            end = tape_io.read(tape).end
            oracle_states.build(tape, oracle_states.default_frames(tape, end), exe, data, gdi, a.flycast, a.poke_ec,
                                os.path.join(work, "states"), a.timeout)
            m = oracle_states.read_manifest()
        entry = oracle_states.nearest(m, tape, a.from_state)
        if not entry:
            sys.exit("no library state of this tape at or before frame %d" % a.from_state)
        env.update(oracle_states.stage(entry, fexe, gdi))
        print("oracle: starting the console from state %s (tape frame %d = game frame %d, console vblank %d)"
              % (entry["name"], entry["frame"], entry["game_frame"], entry["vblank"]))
    code = oracle_states.run_flycast(fexe, gdi, "tape_oracle.lua", env, timeout, os.path.join(work, "flycast.out"), extra)
    if not os.path.isfile(fly):
        sys.exit("Flycast wrote no trace (Lua not loaded? see %s)" % os.path.dirname(fexe))
    print("oracle: traces in %s" % work)
    return tape_diff.main(["diff", desk, fly, "--watch", WATCH])


if __name__ == "__main__":
    sys.exit(main())
