#!/usr/bin/env python3
"""Stage 6 of the launcher (launcher/README.md): write the Memory Stick folder.

    convert_assets.py --data DUMP/data --out FOLDER [--eboot EBOOT.PBP]
                      [--args "OPTION ..."] [--tape NAME]... [--dry-run]

FOLDER is the game's folder on the card, e.g. E:/PSP/GAME/SA1PSP or
PPSSPP's memstick/PSP/GAME/SA1PSP.  It receives

  data/1ST_READ.BIN       the game executable (the recompiled code's data
                          and the image hle_init boots from)
  data/SONICADV/...       the disc's game tree, unchanged
  EBOOT.PBP               with --eboot (stage 7's packaging, for now by hand)
  args.txt                with --args: the EBOOT's command line
                          (backend/psp/main_psp.c); --args "" removes it
  NAME.tape, NAME.check   with --tape: tests/replay/ tapes to replay on the
                          device (--replay NAME.tape --check NAME.check)

For M4 this is a straight copy: textures stay PVR and are decoded at load,
as on the desktop (hle/ninja/texture.c), and ADX stays ADX.  Converting
textures offline (downscaled, paletted) is the M6 lever if load time or
memory asks for it; docs/m4-psp-report.md has the measurements.

Files already in FOLDER with the same size and a modification time no older
than the source are skipped, so a re-run copies only what changed (and a
card that lost a file gets it back).  The dump is only read.

Part of sa1-psp (MIT).
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def inside(path: str, parent: str) -> bool:
    path, parent = os.path.normcase(os.path.abspath(path)), os.path.normcase(os.path.abspath(parent))
    return path == parent or path.startswith(parent + os.sep)


def copy_if_changed(src: str, dst: str, dry: bool, stats: dict) -> None:
    st = os.stat(src)
    try:
        dt = os.stat(dst)
        if dt.st_size == st.st_size and dt.st_mtime >= st.st_mtime - 2:   # FAT keeps 2 s
            stats["skipped"] += 1
            return
    except FileNotFoundError:
        pass
    stats["copied"] += 1
    stats["bytes"] += st.st_size
    if dry:
        print(f"copy {src} -> {dst}")
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--data", required=True, help="the dump's data folder (holds 1ST_READ.BIN and SONICADV/)")
    ap.add_argument("--out", required=True, help="the game folder on the Memory Stick (…/PSP/GAME/SA1PSP)")
    ap.add_argument("--eboot", help="EBOOT.PBP to put in the folder")
    ap.add_argument("--args", help="write args.txt with these options (empty: remove it)")
    ap.add_argument("--tape", action="append", default=[], help="copy tests/replay/NAME.tape and NAME.check")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    if inside(a.out, REPO):
        sys.exit("convert_assets: the output must be outside the repository (it holds game data)")
    if inside(a.out, a.data) or inside(a.data, a.out):
        sys.exit("convert_assets: --out and --data must not contain each other (the dump is read-only)")
    bin_src = os.path.join(a.data, "1ST_READ.BIN")
    tree_src = os.path.join(a.data, "SONICADV")
    if not os.path.isfile(bin_src) or not os.path.isdir(tree_src):
        sys.exit(f"convert_assets: {a.data} has no 1ST_READ.BIN and SONICADV/ (tools/verify_dump.py, stage 1-2)")

    stats = {"copied": 0, "skipped": 0, "bytes": 0}
    data_out = os.path.join(a.out, "data")
    copy_if_changed(bin_src, os.path.join(data_out, "1ST_READ.BIN"), a.dry_run, stats)
    for root, _dirs, files in os.walk(tree_src):
        rel = os.path.relpath(root, a.data)
        for name in sorted(files):
            copy_if_changed(os.path.join(root, name), os.path.join(data_out, rel, name), a.dry_run, stats)
    if a.eboot:
        copy_if_changed(a.eboot, os.path.join(a.out, "EBOOT.PBP"), a.dry_run, stats)
    for name in a.tape:
        for ext in (".tape", ".check"):
            src = os.path.join(REPO, "tests", "replay", name + ext)
            if os.path.isfile(src):
                copy_if_changed(src, os.path.join(a.out, name + ext), a.dry_run, stats)
            elif ext == ".tape":
                sys.exit(f"convert_assets: no tape {src}")
    if a.args is not None and not a.dry_run:
        path = os.path.join(a.out, "args.txt")
        if a.args.strip():
            os.makedirs(a.out, exist_ok=True)
            with open(path, "w", newline="\n") as f:
                f.write(a.args.strip().replace(" --", "\n--") + "\n")
        elif os.path.exists(path):
            os.remove(path)
    print(f"convert_assets: {stats['copied']} files copied ({stats['bytes'] / 1e6:.1f} MB), "
          f"{stats['skipped']} already there, into {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
