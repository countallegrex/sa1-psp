#!/usr/bin/env python3
"""Dump the oracle's fsca table: Flycast 2.7 runs fsca for every 16-bit angle
in the guest (tools/flycast/fsca_dump.lua) and the result goes to
../private/ref/fsca_sh4.bin, which the runtime loads (sh4_trig.h).

  fsca_dump.py [--tape FILE] [--exe SA1PSP] [--flycast DIR] [--gdi FILE] [--out FILE] [--check FILE]

The game calls njSin/njCos (where the hook sits) only in gameplay, so a tape
(default tests/replay/title_menu.tape, which reaches Station Square) drives
the oracle there, as run_oracle.py does (the desktop replay makes its game-
frame pad table; --exe as run_oracle).

The file is 65536 x (sin, cos), little-endian float bit patterns, 512 KB.
It is output of the user's own oracle setup, not part of the repository
(like the game data, it stays in private/).  --check only checks an
existing file.  The checks: every value in [-1, 1]; sin(0) = 0 and
cos(0) = 1; within 2^-20 of the true value; and how much of the table the
quarter-turn symmetries reproduce (reported, not required).

The routine fsca_dump.lua writes (at 0x8C008100; njSin at 0x8C63A9B8 is
pointed at it):

  00 D10D  mov.l @(lit_ctl),r1    12 4608  shll2 r6        26 E3FF  mov #-1,r3
  02 6212  mov.l @r1,r2           14 425A  lds r2,FPUL     28 2132  mov.l r3,@r1
  04 E3FF  mov #-1,r3             16 F0FD  fsca FPUL,DR0   2a 5011  mov.l @(4,r1),r0  (call count)
  06 3320  cmp/eq r2,r3           18 F50A  fmov.s fr0,@r5  2c 7001  add #1,r0
  08 890F  bt done                1a 7504  add #4,r5       2e 1101  mov.l r0,@(4,r1)
  0a D50C  mov.l @(lit_buf),r5    1c F51A  fmov.s fr1,@r5  30 644D  extu.w r4,r4
  0c E601  mov #1,r6              1e 7504  add #4,r5       32 445A  lds r4,FPUL
  0e 4618  shll8 r6               20 7201  add #1,r2       34 000B  rts
  10 4608  shll2 r6               22 4610  dt r6           36 F0FD  fsca FPUL,DR0
                                  24 8BF6  bf loop         38/3c lit_ctl, lit_buf

Part of sa1-psp (MIT)."""
import argparse
import glob
import math
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import oracle_states  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
DEFAULT_OUT = os.path.join(os.path.dirname(ROOT), "private", "ref", "fsca_sh4.bin")


def f32(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def check(path):
    data = open(path, "rb").read()
    if len(data) != 65536 * 8:
        print("fsca_dump: %s is %d bytes, want %d" % (path, len(data), 65536 * 8))
        return False
    w = struct.unpack("<131072I", data)
    s = [f32(w[2 * i]) for i in range(65536)]
    c = [f32(w[2 * i + 1]) for i in range(65536)]
    ok = True
    if any(not (-1.0 <= v <= 1.0) for v in s + c):
        print("fsca_dump: values outside [-1, 1]"); ok = False
    if s[0] != 0.0 or c[0] != 1.0:
        print("fsca_dump: sin(0) %r cos(0) %r" % (s[0], c[0])); ok = False
    err = max(max(abs(s[i] - math.sin(2 * math.pi * i / 65536)), abs(c[i] - math.cos(2 * math.pi * i / 65536))) for i in range(65536))
    if err > 2.0 ** -20:
        print("fsca_dump: max error %.3g > 2^-20" % err); ok = False
    shift = sum(1 for i in range(65536) if w[2 * i + 1] == w[2 * ((i + 0x4000) & 0xFFFF)])
    odd = sum(1 for i in range(65536) if w[2 * ((-i) & 0xFFFF)] == (w[2 * i] ^ 0x80000000) or (i == 0))
    half = sum(1 for i in range(65536) if w[2 * ((i + 0x8000) & 0xFFFF)] == (w[2 * i] ^ 0x80000000) or w[2 * i] == 0)
    print("fsca_dump: %s: max error vs true %.3g (2^%.1f); cos(i) == sin(i + 0x4000) for %d, sin(-i) == -sin(i) for %d, "
          "sin(i + 0x8000) == -sin(i) for %d of 65536" % (path, err, math.log2(err) if err else -999, shift, odd, half))
    return ok


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--flycast", default=os.path.join(oracle_states.FLYCAST_HOME, "oracle-2.7"), help="Flycast directory")
    ap.add_argument("--gdi", help="GD-ROM image (default: the .gdi in $SA1_DUMP)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output (default ../private/ref/fsca_sh4.bin)")
    ap.add_argument("--check", metavar="FILE", help="only check FILE")
    ap.add_argument("--tape", default=os.path.join(ROOT, "tests", "replay", "title_menu.tape"), help="tape that reaches gameplay")
    ap.add_argument("--exe", help="desktop sa1psp executable (for the tape's pad table)")
    ap.add_argument("--timeout", type=int, default=600)
    a = ap.parse_args(argv)
    if a.check:
        return 0 if check(a.check) else 1
    dump = os.environ.get("SA1_DUMP", "")
    gdi = a.gdi or next(iter(sorted(glob.glob(os.path.join(dump, "*.gdi")))), None)
    if not gdi:
        print("fsca_dump: no GD-ROM image (--gdi or SA1_DUMP)")
        return 2
    fexe = oracle_states.oracle_dir(a.flycast)
    shutil.copy2(os.path.join(HERE, "fsca_dump.lua"), os.path.join(os.path.dirname(fexe), "fsca_dump.lua"))
    tmp = a.out + ".tmp"
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    env = dict(os.environ, SA1_FSCA_OUT=os.path.abspath(tmp))
    exe = a.exe or os.environ.get("SA1_EXE") or next((p for p in (os.path.join(ROOT, d, "backend", "desktop", "sa1psp.exe") for d in ("build-desktop", "build-tape", "build-desk-g")) if os.path.isfile(p)), None)
    if not exe:
        print("fsca_dump: no desktop build (--exe or SA1_EXE)")
        return 2
    work = os.path.abspath(a.out + ".work")
    os.makedirs(work, exist_ok=True)
    _, gpad = oracle_states.desktop_trace(exe, os.path.join(dump, "data"), a.tape, work)
    env.update(SA1_ORACLE_GPAD=gpad, SA1_ORACLE_WATCH=oracle_states.WATCH, SA1_ORACLE_OUT=os.path.join(work, "flycast.tsv"),
               SA1_ORACLE_MAX="36000")
    log = os.path.abspath(a.out + ".flycast.log")
    oracle_states.run_flycast(fexe, gdi, "fsca_dump.lua", env, a.timeout, log, ["config:Dynarec.Enabled=no"])
    if not os.path.isfile(tmp) or not check(tmp):
        print("fsca_dump: no good dump (Flycast log %s)" % log)
        return 1
    os.replace(tmp, a.out)
    print("fsca_dump: wrote %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
