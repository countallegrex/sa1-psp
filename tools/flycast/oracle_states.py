#!/usr/bin/env python3
"""A library of Flycast save states keyed by tape frame, for the oracle.

  oracle_states.py build TAPE [--frames NAME:FRAME,...] [--poke-ec]
  oracle_states.py list
  oracle_states.py check TAPE          # exit 1 when the library is stale or missing

A comparison against the console at tape frame N normally replays the
whole tape in Flycast from boot (tools/flycast/run_oracle.py); with the
library, `run_oracle.py TAPE --from-state N` starts Flycast from the
nearest earlier state instead and feeds the tape from there.

The library lives outside the repository, in ../flycast/states/ next to the
oracle (the states hold the console's RAM: game data), as
`<tape stem>__<name>.state` plus `manifest.json`, which names for every
state the tape it was made from, the tape's SHA-256, the tape (guest)
frame and desktop game frame it stands for, and the oracle script's
alignment variables at the moment of the save (tape_oracle.lua
SA1_ORACLE_RESUME).  A state is built by running the tape once with
SA1_ORACLE_SAVE: Flycast saves into a slot when the aligned desktop game
frame reaches the requested one (from the UI thread; the state is a few
vblanks past the request, and the manifest records where exactly), and
the slot files are moved here.  `run_oracle.py --from-state` rebuilds the
library of a tape automatically when the tape's hash no longer matches.

Default frames when --frames is not given: `start:120`, then every 3000
frames; `tests/replay/sonic_to_ec1_start.tape` gets the route's scenes
(title, Station Square arrival, Chaos 0 cutscene start, Chaos 0 fight,
Emerald Coast start, the frame before the fourth run's crash).  A frame
the console never reaches on that tape (the console loses the Chaos 0
fight on the maintainer's inputs, so nothing after it) is reported and left out.

Part of sa1-psp (MIT).
"""
import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import replay_test  # noqa: E402
import tape_diff  # noqa: E402
import tape_io  # noqa: E402

WATCH = os.path.join(ROOT, "tests", "replay", "watch.txt")
FLYCAST_HOME = os.path.join(os.path.dirname(ROOT), "flycast")
LIBRARY = os.path.join(FLYCAST_HOME, "states")
MANIFEST = "manifest.json"
LOAD_SLOT = 9          # the slot a from-state run loads; the library's files are copied into it

DEFAULT_FRAMES = {
    "sonic_to_ec1_start": [("title", 300), ("station_square", 1300), ("chaos0_cutscene", 4100),
                           ("chaos0_fight", 7420), ("emerald_coast", 15600), ("before_crash", 18400)],
    "ec_scripted": [("title", 300), ("station_square", 1300), ("emerald_coast", 5350)],
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def tape_key(tape):
    """How the manifest names a tape: repository-relative when inside it."""
    tape = os.path.abspath(tape)
    rel = os.path.relpath(tape, ROOT)
    return rel.replace("\\", "/") if not rel.startswith("..") else tape


def read_manifest():
    p = os.path.join(LIBRARY, MANIFEST)
    if not os.path.isfile(p):
        return {"states": []}
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def write_manifest(m):
    os.makedirs(LIBRARY, exist_ok=True)
    with open(os.path.join(LIBRARY, MANIFEST), "w", encoding="utf-8") as f:
        json.dump(m, f, indent=1)
        f.write("\n")


def states_of(m, tape):
    key = tape_key(tape)
    return [s for s in m["states"] if s["tape"] == key]


def is_fresh(m, tape):
    """True when the library holds states of this tape made from its current bytes."""
    ss = states_of(m, tape)
    return bool(ss) and all(s["tape_sha256"] == sha256(tape) for s in ss)


def nearest(m, tape, frame):
    """The state of this tape with the largest frame <= frame, or None."""
    cands = [s for s in states_of(m, tape) if s["frame"] <= frame and os.path.isfile(os.path.join(LIBRARY, s["file"]))]
    return max(cands, key=lambda s: s["frame"]) if cands else None


# ---- running Flycast ---------------------------------------------------------
def oracle_dir(path=None):
    """A Flycast copy of our own: portable config (emu.cfg beside the exe), no VMU saves."""
    path = path or os.path.join(FLYCAST_HOME, "oracle-2.7")
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


def game_basename(gdi):
    return os.path.splitext(os.path.basename(gdi))[0]


def slot_file(fexe, gdi, slot):
    """Where Flycast keeps slot N of this game (oslib getSavestatePath: data/<game>[_N].state)."""
    name = game_basename(gdi) + ("_%d" % slot if slot > 0 else "") + ".state"
    return os.path.join(os.path.dirname(fexe), "data", name)


def quiet_window():
    """Windows: start Flycast minimized without activating it (SW_SHOWMINNOACTIVE through
    STARTUPINFO, which the first ShowWindow of the process takes), so an oracle run never
    takes the focus or covers the screen; it keeps emulating while minimized."""
    if os.name != "nt":
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 7    # SW_SHOWMINNOACTIVE
    return si


def run_flycast(fexe, gdi, lua, env, timeout, log, extra_config=()):
    """Runs Flycast with the oracle script and the given environment; returns the exit code."""
    shutil.copy2(os.path.join(HERE, "tape_oracle.lua"), os.path.join(os.path.dirname(fexe), "tape_oracle.lua"))
    cfg = "config:LuaFileName=%s,input:device1.1=10,input:device1.2=10" % lua
    # SA1_FLYCAST_CONFIG: more SECTION:KEY=VALUE pairs, comma-separated, for experiments
    # (config:Dynarec.Enabled=no makes guest code patched from Lua take effect at once)
    extra_config = list(extra_config) + [kv for kv in os.environ.get("SA1_FLYCAST_CONFIG", "").split(",") if kv]
    for kv in extra_config:
        cfg += "," + kv
    cmd = [fexe, "-config", cfg, os.path.abspath(gdi)]
    print("oracle: running Flycast (up to %d s): %s" % (timeout, " ".join(cmd)), flush=True)
    t0 = time.time()
    with open(log, "w") as out:
        try:
            p = subprocess.run(cmd, env=env, cwd=os.path.dirname(fexe), timeout=timeout, stdout=out, stderr=subprocess.STDOUT,
                               startupinfo=quiet_window())
            code = p.returncode
        except subprocess.TimeoutExpired:
            print("oracle: Flycast did not exit in %d s" % timeout)
            code = -1
    print("oracle: Flycast done in %.0f s (exit %d), output %s" % (time.time() - t0, code, log))
    return code


def add_timing_args(ap):
    """--fpu-round / --sound-timing: override the tape's header lines on the desktop replay, so
    a tape recorded before those lines replays like the console (zero, real)."""
    ap.add_argument("--fpu-round", choices=("zero", "nearest"), help="desktop FPU rounding (default: the tape's fpu-round line)")
    ap.add_argument("--sound-timing", choices=("real", "none"), help="desktop sound timing (default: the tape's sound-timing line)")
    ap.add_argument("--aspect", choices=("4:3", "wide"),
                    help="desktop aspect (default: the tape's aspect line, absent = wide; 4:3 is the console's: "
                         "wide widens the game's own culling rectangle, which game logic reads)")
    ap.add_argument("--stick-gate", choices=("round", "square"),
                    help="desktop stick range (default: the tape's stick-gate line, absent = square; round is the "
                         "console's: its controller scales a stick vector longer than 128 back onto that circle)")


def timing_args(a):
    """The desktop flags for add_timing_args' options."""
    out = []
    if getattr(a, "fpu_round", None):
        out += ["--fpu-round", a.fpu_round]
    if getattr(a, "sound_timing", None):
        out += ["--sound-timing", a.sound_timing]
    if getattr(a, "aspect", None):
        out += ["--aspect", a.aspect]
    if getattr(a, "stick_gate", None):
        out += ["--stick-gate", a.stick_gate]
    return out


def desktop_trace(exe, data, tape, work, poke_ec=False, extra=()):
    """Replays the tape on the desktop build with the watch trace; returns (trace, gpad) paths.
    poke_ec: the scripted route's stage pokes (tools/replay_test.py ROUTE_POKES), which the
    console side mirrors with SA1_ORACLE_POKE_EC.  extra: more desktop flags (timing_args)."""
    desk, gpad = os.path.join(work, "desktop.tsv"), os.path.join(work, "gpad.txt")
    args = ["--replay", os.path.abspath(tape), "--watch-list", WATCH, "--watch-trace", desk] + (replay_test.ROUTE_POKES if poke_ec else []) + list(extra)
    code, _ = replay_test.run(os.path.abspath(exe), data, args, os.path.join(work, "desktop.log"))
    if code != 0:
        sys.exit("desktop replay failed (exit %d), log %s" % (code, os.path.join(work, "desktop.log")))
    tape_diff.main(["gpad", os.path.abspath(tape), desk, "-o", gpad])
    return desk, gpad


def game_frame_at(desk, frame):
    """The desktop game frame counter at guest frame `frame` of the trace (the last row at or before it)."""
    header, rows = tape_diff.read_tsv(desk)
    g = 0
    for r in rows:
        if r["frame"] > frame:
            break
        g = int(r[tape_diff.COUNTER])
    return g


def default_frames(tape, end):
    stem = os.path.splitext(os.path.basename(tape))[0]
    if stem in DEFAULT_FRAMES:
        return [(n, f) for n, f in DEFAULT_FRAMES[stem] if f <= end]
    return [("start", 120)] + [("f%d" % f, f) for f in range(3000, end, 3000)]


def build(tape, frames, exe, data, gdi, flycast=None, poke_ec=False, work=None, timeout=0, extra=()):
    """Builds (or rebuilds) the library entries of one tape.  Returns the entries made."""
    import tempfile
    tape = os.path.abspath(tape)
    work = work or tempfile.mkdtemp(prefix="sa1-states-")
    os.makedirs(work, exist_ok=True)
    desk, gpad = desktop_trace(exe, data, tape, work, poke_ec, extra)
    fexe = oracle_dir(flycast)
    frames = sorted(frames, key=lambda nf: nf[1])
    plan = []
    for k, (name, frame) in enumerate(frames):
        plan.append({"slot": k + 1, "name": name, "frame": frame, "game_frame": game_frame_at(desk, frame)})
    for p in plan:
        old = slot_file(fexe, gdi, p["slot"])
        if os.path.isfile(old):
            os.remove(old)
    state_out = os.path.join(work, "states.txt")
    if os.path.isfile(state_out):
        os.remove(state_out)
    nframes = tape_io.read(tape).end
    env = dict(os.environ, SA1_ORACLE_GPAD=gpad, SA1_ORACLE_WATCH=WATCH, SA1_ORACLE_OUT=os.path.join(work, "flycast.tsv"),
               SA1_ORACLE_MAX=str((max(p["game_frame"] for p in plan) if plan else nframes // 2) * 2 + 10000), SA1_ORACLE_STATE_OUT=state_out,   # the intro movie alone is 6,900 console vblanks
               SA1_ORACLE_SAVE=",".join("%d:%d" % (p["slot"], p["game_frame"]) for p in plan),
               SA1_ORACLE_STOP=str(max(p["game_frame"] for p in plan) + 4) if plan else "-1")
    if poke_ec:
        env["SA1_ORACLE_POKE_EC"] = "1"
    run_flycast(fexe, gdi, "tape_oracle.lua", env, timeout or nframes * 3 // 60 + 120, os.path.join(work, "flycast.out"))
    saved = {}
    if os.path.isfile(state_out):
        for line in open(state_out, encoding="utf-8"):
            v = line.split()
            if len(v) == 10:
                saved[int(v[0])] = {"vblank": int(v[2]), "console_game_frame": int(v[3]), "aligned_game_frame": int(v[4]),
                                    "resume": " ".join(v[2:3] + v[5:10])}
    # A state is only as good as the alignment at the time: the console's game
    # mode, level and act at the save must be the desktop's at that game frame
    # (the console's Chaos 0 fight runs longer than ours, and until a mode
    # change re-anchors the offset an "Emerald Coast" save is still the fight).
    fly_rows = {}
    fly_trace = os.path.join(work, "flycast.tsv")
    if os.path.isfile(fly_trace):
        for r in tape_diff.read_tsv(fly_trace)[1]:
            fly_rows[int(r["frame"])] = r
    desk_rows = {}
    for r in tape_diff.read_tsv(desk)[1]:
        desk_rows.setdefault(int(r[tape_diff.COUNTER]), r)
    for p in plan:
        if p["slot"] not in saved:
            continue
        fr, dr = fly_rows.get(saved[p["slot"]]["vblank"]), desk_rows.get(p["game_frame"])
        if fr is None or dr is None:
            continue
        where = {k: int(fr[k]) for k in ("game_mode", "level", "act") if k in fr}
        saved[p["slot"]]["console"] = where
        bad = [k for k in where if int(dr[k]) != where[k]]
        if bad:
            print("states: %s (frame %d = game frame %d) dropped: the console is at mode %d level %d act %d, the desktop at mode %d level %d act %d (%s differ)"
                  % (p["name"], p["frame"], p["game_frame"], where["game_mode"], where["level"], where["act"],
                     int(dr["game_mode"]), int(dr["level"]), int(dr["act"]), ", ".join(bad)))
            del saved[p["slot"]]
    m = read_manifest()
    key = tape_key(tape)
    m["states"] = [s for s in m["states"] if s["tape"] != key]
    for s in [s for s in read_manifest()["states"] if s["tape"] == key]:
        f = os.path.join(LIBRARY, s["file"])
        if os.path.isfile(f):
            os.remove(f)
    os.makedirs(LIBRARY, exist_ok=True)
    stem = os.path.splitext(os.path.basename(tape))[0]
    made = []
    digest = sha256(tape)
    for p in plan:
        src = slot_file(fexe, gdi, p["slot"])
        if p["slot"] not in saved or not os.path.isfile(src):
            if p["slot"] not in saved and os.path.isfile(src):
                os.remove(src)
            elif p["slot"] not in saved:
                print("states: %s (frame %d = game frame %d) not reached by the console on this tape; left out" % (p["name"], p["frame"], p["game_frame"]))
            continue
        fname = "%s__%s.state" % (stem, p["name"])
        shutil.move(src, os.path.join(LIBRARY, fname))
        entry = {"tape": key, "tape_sha256": digest, "name": p["name"], "frame": p["frame"], "game_frame": p["game_frame"],
                 "file": fname, "poke_ec": bool(poke_ec), "made": time.strftime("%Y-%m-%d %H:%M:%S")}
        entry.update(saved[p["slot"]])
        m["states"].append(entry)
        made.append(entry)
        print("states: %s <- tape frame %d (game frame %d; state at console vblank %d, aligned game frame %d)"
              % (fname, p["frame"], p["game_frame"], entry["vblank"], entry["aligned_game_frame"]))
    m["states"].sort(key=lambda s: (s["tape"], s["frame"]))
    write_manifest(m)
    print("states: manifest %s, %d state(s) for %s" % (os.path.join(LIBRARY, MANIFEST), len(made), key))
    return made


def stage(entry, fexe, gdi):
    """Copies a library state into the load slot; returns the environment that makes
    tape_oracle.lua load it first thing and continue the alignment."""
    dst = slot_file(fexe, gdi, LOAD_SLOT)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(os.path.join(LIBRARY, entry["file"]), dst)
    return {"SA1_ORACLE_LOAD_SLOT": str(LOAD_SLOT), "SA1_ORACLE_RESUME": entry["resume"]}


def parse_frames(spec):
    out = []
    for item in spec.split(","):
        name, frame = item.split(":")
        out.append((name.strip(), int(frame)))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="build (or rebuild) the states of one tape")
    b.add_argument("tape")
    b.add_argument("--frames", help="NAME:FRAME,... tape frames to keep (default: the tape's scenes, or every 3000 frames)")
    b.add_argument("--poke-ec", action="store_true", help="the scripted route: the desktop replay gets its --poke 5290 and the console the same rewrite when Chaos 0 would load")
    b.add_argument("--exe", help="desktop sa1psp executable")
    b.add_argument("--flycast", help="Flycast directory (default ../flycast/oracle-2.7)")
    b.add_argument("--gdi", help="GD-ROM image (default: the .gdi in $SA1_DUMP)")
    b.add_argument("--work", help="working directory for the traces")
    b.add_argument("--timeout", type=int, default=0)
    add_timing_args(b)
    sub.add_parser("list", help="the library's manifest")
    c = sub.add_parser("check", help="is the library of this tape current?")
    c.add_argument("tape")
    a = ap.parse_args(argv)

    if a.cmd == "list":
        m = read_manifest()
        if not m["states"]:
            print("no states in %s" % LIBRARY)
            return 0
        for s in m["states"]:
            ok = "ok" if os.path.isfile(os.path.join(LIBRARY, s["file"])) else "MISSING"
            fresh = "current" if os.path.isfile(os.path.join(ROOT, s["tape"])) and sha256(os.path.join(ROOT, s["tape"])) == s["tape_sha256"] else "stale"
            print("%-32s %-18s frame %6d game %5d  %s %s  (%s)" % (s["tape"], s["name"], s["frame"], s["game_frame"], ok, fresh, s["file"]))
        return 0
    if a.cmd == "check":
        m = read_manifest()
        fresh = is_fresh(m, a.tape)
        print("%s: %s" % (tape_key(a.tape), "current (%d states)" % len(states_of(m, a.tape)) if fresh else "stale or missing"))
        return 0 if fresh else 1

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
    end = tape_io.read(a.tape).end
    frames = parse_frames(a.frames) if a.frames else default_frames(a.tape, end)
    build(a.tape, frames, exe, data, gdi, a.flycast, a.poke_ec, a.work, a.timeout, timing_args(a))
    return 0


if __name__ == "__main__":
    sys.exit(main())
