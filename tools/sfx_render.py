#!/usr/bin/env python3
"""Render sound-effect programs from the game's banks to WAV.

  python tools/sfx_render.py STATIONSQUARE.MLT [--bank B] [--program P] [--key K] [--hold S]
  python tools/sfx_render.py --route          # the banks the maintainer's route loads

The game plays its effects as MIDI notes on programs of the banks in the
multi-unit files (docs/formats/sound.md section 5), and the audio core
(hle/sound/snd_core.c) synthesises them at run time from the tone data, so
the port ships no pre-rendered clips and the launcher has nothing to render.
This tool renders programs through that same core, for listening and for
comparisons with console captures: every program of the file's program
banks, played as the game plays an effect (key 60, velocity 127), held
--hold seconds and released.

Clips are game-derived, so they go under ..\\private\\sfx\\<FILE>\\ (outside
the repository), with index.tsv (bank, program, seconds, peak).  The data
comes from $SA1_DUMP/data/SONICADV.  The renderer (tools/sfx_render/
sfx_render.c) is compiled on first use with clang or cc into
..\\private\\sfx\\bin\\.

Known programs: bank 0 program 7 is the ring, bank 0 program 17 the jump
(every level file carries bank 0).

Part of sa1-psp (MIT).
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(os.path.dirname(ROOT), "private", "sfx")
ROUTE = ["MOBILE1.MLT", "STATIONSQUARE.MLT", "CHAOS0.MLT", "EMERALDCOAST.MLT", "P_SONICTAILS.MLT",
         "V_SONICTAILS_E.MLT", "V_SONICTAILS.MLT", "E_0001.MLT", "E_0003.MLT"]


def build():
    exe = os.path.join(OUT, "bin", "sfx_render" + (".exe" if os.name == "nt" else ""))
    src = [os.path.join(ROOT, "tools", "sfx_render", "sfx_render.c"),
           os.path.join(ROOT, "hle", "sound", "snd_core.c"), os.path.join(ROOT, "hle", "sound", "adx.c")]
    if os.path.isfile(exe) and all(os.path.getmtime(exe) >= os.path.getmtime(s) for s in src):
        return exe
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        sys.exit("no C compiler (clang, cc or gcc) on PATH")
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    cmd = [cc, "-std=c11", "-O2", "-D_CRT_SECURE_NO_WARNINGS", "-I" + os.path.join(ROOT, "hle", "sound"), *src, "-o", exe]
    if os.name != "nt":
        cmd.append("-lm")
    subprocess.run(cmd, check=True)
    return exe


def render(exe, mlt, args):
    name = os.path.splitext(os.path.basename(mlt))[0]
    out = os.path.join(OUT, name)
    os.makedirs(out, exist_ok=True)
    cmd = [exe, mlt, out]
    if args.bank is not None:
        cmd.append(str(args.bank))
        if args.program is not None:
            cmd.append(str(args.program))
    cmd += ["--key", str(args.key), "--hold", str(args.hold)]
    p = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True)
    rows = [l.split("\t") for l in p.stdout.splitlines() if l.strip()]
    with open(os.path.join(out, "index.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("bank\tprogram\tseconds\tpeak\n")
        for r in rows:
            f.write("\t".join(r) + "\n")
    print("%s: %d clips in %s" % (name, len(rows), out))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("mlt", nargs="*", help="multi-unit file(s), by path or by name in $SA1_DUMP/data/SONICADV")
    ap.add_argument("--route", action="store_true", help="the files the M3 route loads")
    ap.add_argument("--bank", type=int)
    ap.add_argument("--program", type=int)
    ap.add_argument("--key", type=int, default=60)
    ap.add_argument("--hold", type=float, default=1.0)
    a = ap.parse_args(argv)
    names = list(a.mlt) + (ROUTE if a.route else [])
    if not names:
        ap.error("name a file or pass --route")
    dump = os.environ.get("SA1_DUMP", os.path.join(os.path.dirname(ROOT), "private", "dump", "sa1_us_1005"))
    exe = build()
    for n in names:
        path = n if os.path.isfile(n) else os.path.join(dump, "data", "SONICADV", n.upper())
        if not os.path.isfile(path):
            print("skip: %s not found" % n)
            continue
        render(exe, path, a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
