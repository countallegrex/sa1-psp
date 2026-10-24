#!/usr/bin/env python3
"""Stage 6 of the launcher (launcher/README.md): write the Memory Stick folder.

    convert_assets.py --data DUMP/data --out FOLDER [--eboot EBOOT.PBP] [--pack]
                      [--fsca FILE] [--native] [--args "OPTION ..."] [--tape NAME]... [--dry-run]

FOLDER is the game's folder on the card, e.g. E:/PSP/GAME/SA1PSP or
PPSSPP's memstick/PSP/GAME/SA1PSP.  It receives

  data/1ST_READ.BIN       the game executable (the recompiled code's data
                          and the image hle_init boots from)
  data/SONICADV/...       the disc's game tree, unchanged; or with --pack
  data/SONICADV.PAK       the same files in one file with a sorted index:
                          SONICADV/ holds 2,068 files in one FAT directory
                          and every open on the PSP scans its long-name
                          entries, tens of milliseconds a file; the PSP
                          build reads the pack when it is there
                          (backend/psp/platform_psp.c).  A loose SONICADV/
                          left beside it is no longer read and can go.
  data/fsca_sh4.bin       the console's fsca table (recomp/emitter/runtime/
                          sh4_trig.h), made from the user's own oracle by
                          tools/flycast/fsca_dump.py; --fsca FILE, else the
                          dump's data/fsca_sh4.bin or ../private/ref/.
                          Without it the game uses the host libm (not the
                          console's values) and refuses strict runs.
  data/native/LAND_*.BIN  with --native: Station Square's land tables as the
                          native renderer's meshes (tools/land_convert.py;
                          docs/native-renderer.md)
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
import struct
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


PAK_MAGIC = b"SA1PAK\0\1"
PAK_NAME = 32          # bytes per name in the index
PAK_ALIGN = 2048


def write_pack(tree: str, dst: str, dry: bool, stats: dict) -> None:
    """tree's files (no subfolders) as backend/psp/platform_psp.c reads them:
    magic, count, index bytes, {name[32], offset, size} sorted by the upper-case
    name's bytes, then each file at a 2048-byte boundary.  Rewritten only
    when a source file is newer than the pack or the file set changed."""
    names = sorted(os.listdir(tree), key=lambda n: n.upper().encode("ascii"))
    if any(os.path.isdir(os.path.join(tree, n)) for n in names):
        sys.exit(f"convert_assets: {tree} has subfolders; the pack holds one level")
    for n in names:
        if len(n.encode("ascii")) > PAK_NAME:
            sys.exit(f"convert_assets: {n}: name longer than {PAK_NAME} bytes")
    sizes = [os.path.getsize(os.path.join(tree, n)) for n in names]
    index_bytes = len(names) * (PAK_NAME + 8)
    off = -(-(16 + index_bytes) // PAK_ALIGN) * PAK_ALIGN
    offsets = []
    for s in sizes:
        offsets.append(off)
        off += -(-s // PAK_ALIGN) * PAK_ALIGN
    newest = max(os.stat(os.path.join(tree, n)).st_mtime for n in names)
    try:
        st = os.stat(dst)
        if st.st_size == off and st.st_mtime >= newest - 2:
            stats["skipped"] += len(names)
            return
    except FileNotFoundError:
        pass
    stats["copied"] += len(names)
    stats["bytes"] += sum(sizes)
    if dry:
        print(f"pack {len(names)} files of {tree} -> {dst} ({off / 1e6:.1f} MB)")
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + ".tmp"
    with open(tmp, "wb") as f:
        f.write(PAK_MAGIC + struct.pack("<II", len(names), index_bytes))
        for n, o, s in zip(names, offsets, sizes):
            f.write(n.upper().encode("ascii").ljust(PAK_NAME, b"\0") + struct.pack("<II", o, s))
        for n, o in zip(names, offsets):
            f.write(b"\0" * (o - f.tell()))
            with open(os.path.join(tree, n), "rb") as src:
                shutil.copyfileobj(src, f, 1 << 20)
        f.write(b"\0" * (off - f.tell()))
    os.replace(tmp, dst)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--data", required=True, help="the dump's data folder (holds 1ST_READ.BIN and SONICADV/)")
    ap.add_argument("--out", required=True, help="the game folder on the Memory Stick (…/PSP/GAME/SA1PSP)")
    ap.add_argument("--eboot", help="EBOOT.PBP to put in the folder")
    ap.add_argument("--args", help="write args.txt with these options (empty: remove it)")
    ap.add_argument("--tape", action="append", default=[], help="copy tests/replay/NAME.tape and NAME.check")
    ap.add_argument("--pack", action="store_true", help="SONICADV/ as data/SONICADV.PAK instead of loose files")
    ap.add_argument("--fsca", help="the fsca table (default: DATA/fsca_sh4.bin or ../private/ref/fsca_sh4.bin)")
    ap.add_argument("--native", action="store_true", help="also write data/native/ (tools/land_convert.py)")
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
    if a.pack:
        write_pack(tree_src, os.path.join(data_out, "SONICADV.PAK"), a.dry_run, stats)
        if os.path.isdir(os.path.join(data_out, "SONICADV")):
            print(f"convert_assets: {os.path.join(data_out, 'SONICADV')} is no longer read (the pack is); it can be deleted")
    else:
        for root, _dirs, files in os.walk(tree_src):
            rel = os.path.relpath(root, a.data)
            for name in sorted(files):
                copy_if_changed(os.path.join(root, name), os.path.join(data_out, rel, name), a.dry_run, stats)
    fsca = next((f for f in (a.fsca, os.path.join(a.data, "fsca_sh4.bin"),
                             os.path.join(os.path.dirname(REPO), "private", "ref", "fsca_sh4.bin")) if f and os.path.isfile(f)), None)
    if fsca:
        copy_if_changed(fsca, os.path.join(data_out, "fsca_sh4.bin"), a.dry_run, stats)
    else:
        print("convert_assets: WARNING: no fsca_sh4.bin (tools/flycast/fsca_dump.py): the game will use the host libm's "
              "sine, not the console's, and refuse strict runs")
    if a.native and not a.dry_run:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import land_convert
        land_convert.main(["--data", a.data, "--out", data_out])
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
