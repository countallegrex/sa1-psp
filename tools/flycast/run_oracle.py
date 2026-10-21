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

--shots "name:G,..." pairs screenshots at desktop game frames G: at the
first vblank of each, tools/flycast/oracle_shots.lua writes the frame the
console displays from emulated VRAM (Flycast runs with
rend.EmulateFramebuffer, minimized, and never takes the focus;
--shot-window copies the window from the screen instead, raising it, as
before), the desktop build replays the
tape again in a window with --screenshot at the guest frame where the same
game frame starts, and each pair becomes WORK/shots/name.png, ours left and
the console right, both 480 high.  Montages are for looking, not checks.

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
import shutil
import subprocess
import sys
import tempfile
import time

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


def parse_shots(spec):
    """'name:G,...' -> [(name, G)]."""
    out = []
    for item in (spec or "").split(","):
        if item.strip():
            name, g = item.strip().rsplit(":", 1)
            out.append((name, int(g)))
    return out


def desktop_shots(exe, data, tape, desk, work, shots, a):
    """Desktop screenshots at the guest frame where each game frame starts, then the montages."""
    _, rows = tape_diff.read_tsv(desk)
    start = {}
    for r in rows:
        start.setdefault(int(r["game_frame"]), int(r["frame"]))
    sdir = os.path.join(work, "shots")
    args, frames = ["--replay", tape], []
    for name, g in shots:
        if g not in start:
            print("shots: the desktop run never reached game frame %d (%s)" % (g, name))
            continue
        frames.append(start[g])
        args += ["--screenshot", "%d:%s" % (start[g], os.path.join(sdir, name + "_ours.ppm"))]
    if not frames:
        return
    args += (replay_test.ROUTE_POKES if a.poke_ec else []) + oracle_states.timing_args(a) + ["--frames", str(max(frames) + 2)]
    cmd = [os.path.abspath(exe), os.path.join(data, "1ST_READ.BIN"), data, "--log", os.path.join(work, "desktop_shots.log")] + args
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("shots: no PIL; the pairs are in %s" % sdir)
        return
    for name, g in shots:
        ours, cons = os.path.join(sdir, name + "_ours.ppm"), os.path.join(sdir, name + ".png")
        if not os.path.isfile(cons) and os.path.isfile(os.path.join(sdir, name + ".ppm")):
            Image.open(os.path.join(sdir, name + ".ppm")).save(cons)
        if not (os.path.isfile(ours) and os.path.isfile(cons)):
            print("shots: %s incomplete (ours %s, console %s)" % (name, os.path.isfile(ours), os.path.isfile(cons)))
            continue
        ims = [Image.open(p).convert("RGB") for p in (ours, cons)]
        ims = [im.resize((im.width * 480 // im.height, 480)) for im in ims]
        out = Image.new("RGB", (ims[0].width + ims[1].width + 8, 480 + 24))
        out.paste(ims[0], (0, 0)); out.paste(ims[1], (ims[0].width + 8, 0))
        ImageDraw.Draw(out).text((6, 486), "%s: desktop game frame %d (guest %d), ours left, console right" % (name, g, start.get(g, -1)), fill=(255, 255, 255))
        out.save(os.path.join(sdir, name + "_pair.png"))
        print("shots: %s" % os.path.join(sdir, name + "_pair.png"))


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
    ap.add_argument("--watch", help="watch list for both sides (default tests/replay/watch.txt)")
    ap.add_argument("--shots", metavar="SPEC", help='"name:G,...": console and desktop screenshots at desktop game frame G, montaged')
    ap.add_argument("--shot-window", action="store_true",
                    help="console shots copied from Flycast's window on screen (raises it) instead of read from emulated VRAM")
    oracle_states.add_timing_args(ap)
    a = ap.parse_args(argv)
    global WATCH
    if a.watch:
        WATCH = oracle_states.WATCH = os.path.abspath(a.watch)

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
    # --until G: the desktop need not replay past it (a game frame is about two guest frames;
    # three plus the loads is a safe bound)
    limit = ["--frames", str(min(tape_io.read(tape).end, 3 * a.until + 600))] if a.until is not None else []
    desk, gpad = oracle_states.desktop_trace(exe, data, tape, work, a.poke_ec, oracle_states.timing_args(a) + limit)
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
    lua, shots, capture = "tape_oracle.lua", parse_shots(a.shots), None
    if shots:
        sdir = os.path.join(work, "shots")
        os.makedirs(sdir, exist_ok=True)
        for f in glob.glob(os.path.join(sdir, "*")):
            os.remove(f)
        lua = "oracle_shots.lua"
        shutil.copy2(os.path.join(HERE, lua), os.path.join(os.path.dirname(fexe), lua))
        env.update(SA1_SHOT_SPEC=",".join("%s:%d" % s for s in shots), SA1_SHOTS=sdir)
        if not a.shot_window:
            env.update(SA1_SHOT_FB="1")
            extra.append("config:rend.EmulateFramebuffer=yes")
        else:
            capture = subprocess.Popen(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                        os.path.join(HERE, "capture_window.ps1"), sdir, str(timeout + 60)],
                                       stdout=open(os.path.join(work, "capture.out"), "w"), stderr=subprocess.STDOUT)
    code = oracle_states.run_flycast(fexe, gdi, lua, env, timeout, os.path.join(work, "flycast.out"), extra)
    if capture:
        open(os.path.join(work, "shots", "STOP"), "w").close()
        capture.wait(30)
    if shots:
        desktop_shots(exe, data, tape, desk, work, shots, a)
    if not os.path.isfile(fly):
        sys.exit("Flycast wrote no trace (Lua not loaded? see %s)" % os.path.dirname(fexe))
    print("oracle: traces in %s" % work)
    return tape_diff.main(["diff", desk, fly, "--watch", WATCH])


if __name__ == "__main__":
    sys.exit(main())
