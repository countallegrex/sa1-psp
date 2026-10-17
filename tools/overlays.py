#!/usr/bin/env python3
"""Run the code-overlay pipeline (docs/m1-overlays.md) for every overlay config.

Usage:
  overlays.py new NAME [NAME ...] [--role TEXT] [--code-hi HEX] [--base HEX]
  overlays.py build [NAME ...] [--no-trace] [--no-emit]
  overlays.py list

new    decompresses SONICADV/NAME.PRS into the private overlay directory
       (tools/extract_overlays.py), estimates the code range (find_code_end)
       and writes recomp/sa1_us_1005/overlays/<name>.toml with the header
       entry pointers as seeds.  The slot follows the name: EVnnnn files go to
       the event slot 0x8CB80000, everything else to the area slot 0x8C900000
       unless --base says otherwise (ADV0100/ADV0130 load at 0x8C920000).
       An existing config is never overwritten.
build  for each config (default: all of them): extracts the file if it is
       missing, runs Dream-Recomp discovery restricted to the code range,
       diffs the Flycast trace when there is one and folds new seeds into the
       config, emits C with recomp/emitter/emit_overlay.py, and finally
       regenerates hle/overlay/overlay_registry.c.  Per-overlay numbers go to
       recomp/out/overlays/stats/<name>.json (read by tools/overlay_report.py).
list   prints the configured overlays.

Adding a config is the only step for a new file: `new` writes it, `build`
does the rest, and the CMake tree globs whatever the emitter wrote.

Reads ../private/dump/ only; writes decompressed files only below
../private/overlays/ and generated code only below recomp/out/.
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import extract_overlays as xo  # noqa: E402
from trace_overlays import prs_decompress  # noqa: E402

CONFIGS = os.path.join(REPO, "recomp", "sa1_us_1005", "overlays")
PROJECT = os.path.dirname(REPO)
DATA = os.path.join(PROJECT, "private", "dump", "sa1_us_1005", "data")
BINS = os.path.join(PROJECT, "private", "overlays", "sa1_us_1005")
TRACE = os.path.join(PROJECT, "flycast", "trace-2.7", "sa1trace.txt")
DREAM_DIR = os.path.join(REPO, "recomp", "dream-recomp")
DREAM = os.path.join(DREAM_DIR, "build", "translator",
                     "dream-translate.exe" if os.name == "nt" else "dream-translate")
OUT = os.path.join(REPO, "recomp", "out", "overlays")
AREA, EVENT, AREA2 = 0x8C900000, 0x8CB80000, 0x8C920000
SLOT_NAMES = {AREA: "area", EVENT: "event", AREA2: "second area"}

# What each file is, for the config comment and the docs table.
ROLES = {
    "ADVERTISE": "title screen and menus", "SUMMARY": "file select and result screens",
    "ALERTWND": "menu alert window", "ADV00": "Station Square", "ADV01OBJ": "Egg Carrier objects",
    "ADV0100": "Egg Carrier, flying", "ADV0130": "Egg Carrier, sunk", "ADV02": "Mystic Ruins", "ADV03": "the Past", "STG00": "Hedgehog Hammer",
    "STG01": "Emerald Coast", "STG02": "Windy Valley", "STG03": "Twinkle Park",
    "STG04": "Speed Highway", "STG05": "Red Mountain", "STG06": "Sky Deck",
    "STG07": "Lost World", "STG08": "Ice Cap", "STG09": "Casinopolis", "STG10": "Final Egg",
    "STG12": "Hot Shelter", "B_CHAOS0": "boss Chaos 0", "B_CHAOS2": "boss Chaos 2",
    "B_CHAOS4": "boss Chaos 4", "B_CHAOS6": "boss Chaos 6", "B_CHAOS7": "boss Perfect Chaos",
    "B_EGM1": "boss Egg Hornet", "B_EGM2": "boss Egg Walker", "B_EGM3": "boss Egg Viper",
    "B_E101": "boss E-101 Beta", "B_E101_R": "boss E-101 mkII", "B_ROBO": "boss ZERO",
    "SHOOTING": "Sky Chase", "SBOARD": "Sand Hill", "MINICART": "Twinkle Circuit",
    "AL_MAIN": "Chao garden",
}


def role(stem: str) -> str:
    if stem in ROLES:
        return ROLES[stem]
    if re.fullmatch(r"EV[0-9A-F]{4}", stem):
        return "event %s" % stem[2:]
    return "code overlay"


def configs(names=None) -> list[str]:
    paths = sorted(glob.glob(os.path.join(CONFIGS, "*.toml")))
    if names:
        want = {n.lower() for n in names}
        paths = [p for p in paths if os.path.splitext(os.path.basename(p))[0] in want]
        missing = want - {os.path.splitext(os.path.basename(p))[0] for p in paths}
        if missing:
            sys.exit("overlays: no config for %s" % ", ".join(sorted(missing)))
    return paths


def load(path: str) -> dict:
    with open(path, "rb") as f:
        return tomllib.load(f)


def extract(stem: str) -> bytes:
    src = os.path.join(DATA, "SONICADV", stem + ".PRS")
    with open(src, "rb") as f:
        data = prs_decompress(f.read())
    if not data:
        sys.exit("overlays: %s: PRS stream did not decode" % src)
    os.makedirs(BINS, exist_ok=True)
    with open(os.path.join(BINS, stem + ".bin"), "wb") as f:
        f.write(data)
    return data


TEMPLATE = """\
# Sonic Adventure (USA) v1.005, code overlay SONICADV/{stem}.PRS ({role}),
# for dream-translate (discovery only). Addresses only; nothing from the disc
# is in this file. The game decompresses the file (SEGA PRS) to the {slotname} slot
# at 0x{slot:08X}; tools/overlays.py writes the same bytes to
# ../private/overlays/sa1_us_1005/{stem}.bin. Run with
#   DREAM_CODE_RANGES={lo:08X}:{hi:08X}
# (the code part; data follows it). See docs/m1-overlays.md.
{hdrnote}
[game]
id = "sa1_ovl_{name}"
title = "Sonic Adventure overlay {stem}"
region = "USA"

[binary]
path = "../../../../private/overlays/sa1_us_1005/{stem}.bin"   # decompressed, {size:,} bytes
load_address = 0x{slot:08X}
link_address = 0x{link:08X}
entry = 0x{entry:08X}

[functions]
pointers = true
sweep = true
# Header entry pointers, then executed call targets discovery missed
# (tools/trace_diff.py --overlay; also in {name}_seeds.txt).
extra = [{extra}]
exclude = []

# Read by recomp/emitter/emit_overlay.py and tools/extract_overlays.py;
# dream-translate ignores this table.
[sa1_overlay]
file = "SONICADV/{stem}.PRS"
slot = 0x{slot:08X}
code_lo = 0x{code_lo:08X}
code_hi = 0x{code_hi:08X}
size = {size}
sha256 = "{sha}"
"""


def cmd_new(a) -> int:
    for stem in a.names:
        stem = stem.upper()
        name = stem.lower()
        path = os.path.join(CONFIGS, name + ".toml")
        if os.path.exists(path):
            print("%s: config exists, left alone" % name)
            continue
        data = extract(stem)
        hdr = xo.header(data)
        start = 0x20 if hdr else 0
        end = int(a.code_hi, 16) if a.code_hi else xo.find_code_end(data, start)
        slot = int(a.base, 16) if a.base else EVENT if re.fullmatch(r"EV[0-9A-F]{4}", stem) else AREA
        link = slot & 0x1FFFFFFF
        extra = [w & 0x1FFFFFFF for w in hdr if start <= (w & 0x1FFFFFFF) - link < end] or [link + start]
        text = TEMPLATE.format(
            stem=stem, name=name, role=a.role or role(stem), slot=slot, link=link,
            slotname=SLOT_NAMES[slot],
            lo=link + start, hi=link + end, size=len(data), entry=(hdr[0] if hdr else slot + start),
            code_lo=slot + start, code_hi=slot + end, sha=hashlib.sha256(data).hexdigest(),
            extra=", ".join("0x%08X" % e for e in extra),
            hdrnote=("# The file starts with a 32-byte header of entry pointers; code starts at +0x20.\n"
                     if hdr else ""))
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("%-10s code +0x%X-+0x%X (%d bytes of %d), %d header entries -> %s" % (
            stem, start, end, end - start, len(data), len(hdr), os.path.relpath(path, REPO)))
    return 0


def seeds_path(name: str) -> str:
    return os.path.join(CONFIGS, name + "_seeds.txt")


def add_seeds(path: str, name: str, new: list[int]) -> None:
    """Append trace seeds to the config's extra list and to <name>_seeds.txt."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"^extra = \[([^\]]*)\]", text, re.M)
    old = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", m.group(1))]
    merged = old + [s for s in new if s not in old]
    text = text[:m.start()] + "extra = [%s]" % ", ".join("0x%08X" % e for e in merged) + text[m.end():]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    sp = seeds_path(name)
    have = []
    if os.path.exists(sp):
        with open(sp, encoding="ascii") as f:
            have = [int(l, 16) for l in f if l.strip()]
    with open(sp, "w", encoding="ascii", newline="\n") as f:
        for s in have + [s for s in new if s not in have]:
            f.write("0x%08X\n" % s)


def discover(path: str, cfg: dict, name: str) -> str:
    ov = cfg["sa1_overlay"]
    out = os.path.join(OUT, "dream", name)
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ, DREAM_ASSUME_ENTRY_FPSCR="1",
               DREAM_CODE_RANGES="%08X:%08X" % (ov["code_lo"] & 0x1FFFFFFF, ov["code_hi"] & 0x1FFFFFFF))
    r = subprocess.run([DREAM, "game", "--config", path, "--out-dir", out], cwd=DREAM_DIR, env=env,
                       capture_output=True, text=True)
    with open(os.path.join(out, "translate.log"), "w", encoding="utf-8") as f:
        f.write(r.stdout + r.stderr)
    if r.returncode:
        sys.exit("overlays: dream-translate failed for %s:\n%s" % (name, (r.stdout + r.stderr)[-2000:]))
    return out


def dream_stats(out: str, cfg: dict) -> dict:
    gid = cfg["game"]["id"]
    ov = cfg["sa1_overlay"]
    lo, hi = ov["code_lo"] & 0x1FFFFFFF, ov["code_hi"] & 0x1FFFFFFF
    with open(os.path.join(out, gid + ".functions.json")) as f:
        fns = [x for x in json.load(f)["functions"] if lo <= int(x["entry"], 16) < hi]
    st = {"functions": len(fns)}
    with open(os.path.join(out, "translate.log"), encoding="utf-8") as f:   # dream prints it on stderr
        m = re.search(r"coverage: (\d+) of (\d+) bytes decoded", f.read())
    if m:
        st["decoded"], st["code_bytes_dream"] = int(m.group(1)), int(m.group(2))
    return st


def trace_diff(cfg: dict, name: str, out: str, binp: str) -> dict:
    ov = cfg["sa1_overlay"]
    fj = os.path.join(out, cfg["game"]["id"] + ".functions.json")
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "trace_diff.py"), TRACE, fj,
                        "--overlay", binp, "--base", "%08X" % ov["slot"], "--code-end", "%08X" % ov["code_hi"],
                        "--md", os.path.join(out, "diff.md"), "--seeds", os.path.join(out, "seeds.txt"),
                        "--executed", os.path.join(out, "executed.txt")],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("overlays: trace_diff failed for %s:\n%s" % (name, r.stderr[-2000:]))
    t = r.stdout

    def num(label):
        m = re.search(r"\| \**%s\** \| \**([\d,.]+)" % re.escape(label), t)
        return float(m.group(1).replace(",", "")) if m else 0

    with open(os.path.join(out, "seeds.txt"), encoding="ascii") as f:
        seeds = [int(l, 16) for l in f if l.strip()]
    return {"trace_calls": int(num("Distinct call targets executed (in image)")),
            "trace_found": int(num("of which in the static list")),
            "trace_bytes": int(num("Executed code bytes (in image)")),
            "trace_bytes_in": int(num("of which inside discovered functions")),
            "trace_in_data": int(num("Blocks executed in the data region of the image")),
            "new_seeds": seeds}


def emit(path: str, name: str) -> dict:
    r = subprocess.run([sys.executable, os.path.join(REPO, "recomp", "emitter", "emit_overlay.py"), path],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("overlays: emit_overlay failed for %s:\n%s" % (name, (r.stdout + r.stderr)[-3000:]))
    missing = [int(m, 16) for m in re.findall(r"external target without a function: 0x([0-9a-f]+)", r.stdout)]
    st = {"ext_missing": missing}
    with open(os.path.join(OUT, "emitter", name, "report.txt"), encoding="utf-8") as f:
        for line in f:
            m = re.match(r"(\w+) (\d+)$", line.strip())
            if m:
                st["emit_" + m.group(1)] = int(m.group(2))
    return st


def cmd_build(a) -> int:
    trace = os.path.exists(TRACE) and not a.no_trace
    os.makedirs(os.path.join(OUT, "stats"), exist_ok=True)
    allmissing = {}
    for path in configs(a.names):
        name = os.path.splitext(os.path.basename(path))[0]
        cfg = load(path)
        ov = cfg["sa1_overlay"]
        stem = os.path.splitext(os.path.basename(ov["file"]))[0]
        binp = os.path.join(BINS, stem + ".bin")
        data = open(binp, "rb").read() if os.path.exists(binp) else extract(stem)
        if hashlib.sha256(data).hexdigest() != ov["sha256"]:
            sys.exit("overlays: %s does not match the configured SHA-256" % binp)
        out = discover(path, cfg, name)
        st = {"name": name, "file": ov["file"], "role": role(stem), "slot": ov["slot"],
              "code_lo": ov["code_lo"], "code_hi": ov["code_hi"], "size": ov["size"]}
        if trace:
            td = trace_diff(cfg, name, out, binp)
            if td["new_seeds"]:
                add_seeds(path, name, td["new_seeds"])
                cfg = load(path)
                out = discover(path, cfg, name)
                td2 = trace_diff(cfg, name, out, binp)
                td2["seeded_from_trace"] = td["new_seeds"]
                td2["plain_found"] = td["trace_found"]
                td = td2
            else:
                td["plain_found"] = td["trace_found"]
            st.update(td)
        st.update(dream_stats(out, cfg))
        sp = seeds_path(name)
        st["seeds"] = sum(1 for l in open(sp) if l.strip()) if os.path.exists(sp) else 0
        if not a.no_emit:
            st.update(emit(path, name))
            if st["ext_missing"]:
                allmissing[name] = st["ext_missing"]
        with open(os.path.join(OUT, "stats", name + ".json"), "w", encoding="utf-8") as f:
            json.dump(st, f, indent=1)
        line = "%-10s %4d fns, %s/%s bytes decoded" % (name, st["functions"], st.get("decoded", "?"),
                                                       st.get("code_bytes_dream", "?"))
        if trace and st.get("trace_calls"):
            line += ", trace %d/%d calls (plain %d, %d seeds)" % (
                st["trace_found"], st["trace_calls"], st["plain_found"], len(st.get("seeded_from_trace", [])))
        if "emit_functions" in st:
            line += ", %d C fns" % st["emit_functions"]
        print(line, flush=True)
    for name, ms in allmissing.items():
        print("%s: external targets without a function (add to the main config): %s" % (
            name, " ".join("0x%08x" % m for m in ms)))
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "gen_overlay_registry.py")], check=True)
    return 0


def cmd_list(a) -> int:
    for path in configs():
        ov = load(path)["sa1_overlay"]
        print("%-10s %-24s slot 0x%08X code 0x%08X-0x%08X" % (
            os.path.splitext(os.path.basename(path))[0], ov["file"], ov["slot"], ov["code_lo"], ov["code_hi"]))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("new")
    p.add_argument("names", nargs="+")
    p.add_argument("--role")
    p.add_argument("--code-hi", help="end of the code part, as an offset into the file (hex)")
    p.add_argument("--base", help="load address when it is not the slot the name implies (hex), "
                   "e.g. 8C920000 for the Egg Carrier's ADV0100/ADV0130")
    p.set_defaults(fn=cmd_new)
    p = sub.add_parser("build")
    p.add_argument("names", nargs="*")
    p.add_argument("--no-trace", action="store_true")
    p.add_argument("--no-emit", action="store_true")
    p.set_defaults(fn=cmd_build)
    p = sub.add_parser("list")
    p.set_defaults(fn=cmd_list)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
