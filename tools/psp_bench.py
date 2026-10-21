#!/usr/bin/env python3
"""A/B timing batches on the PSP: N interleaved runs per variant, medians.

    psp_bench.py [--runs N] [--frames F] [--stats S] [--stage NAME] [--windows LIST]
                 [--stop-file PATH] [--tsv FILE] [--build DIR] VARIANT...

A VARIANT is NAME=DIR[:FLAGS]: the build folder whose PRX runs (as
psp_run.py --from DIR; '.' for --build itself) and extra game options for
it, e.g.  base=build-psp/sa1psp  novtex=build-psp/sa1psp:--no-vtex.  Two
variants of one build folder are a same-binary A/B, the only one that code
placement cannot bias (a build's code layout moved Station Square by up to
5 ms a frame on the PSP-3001, 2026-10-21).

Every round runs each variant once, the order reversed every other round
(ABBA), so a slow drift of the console hits both sides alike.  A run is one
psp_run.py call with --stage NAME --no-pace --frames F --stats S; its score
is the mean of the 'frame time avg' of the --stats windows ending at
--windows (default 300,450,600: the figure the speed tracker uses).  The
first run after an idle console is ~2.5 ms slow only in its first window
(f150, never scored), so no warm-up run is spent.  The summary gives each
variant's median, its spread, and the other variants' median delta against
the first.  Every run's figures also go to --tsv.

--stop-file PATH: checked before every run; if it exists the batch ends
after the run in flight (never killing the PSPLink host: a killed host
wedges a connected PSP until it is power-cycled) and prints what it has.

Part of sa1-psp (MIT).
"""
import argparse
import os
import re
import statistics
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIN = re.compile(r"\[\s*[0-9.]+ f(\d+)\s*\] draw: \d+ frames, frame time avg ([0-9.]+) ms.*?"
                 r"list replay ([0-9.]+) ms/frame, GE wait ([0-9.]+) ms/frame")
TA = re.compile(r"\[\s*[0-9.]+ f(\d+)\s*\] render: TA parse and record ([0-9.]+) ms/frame")


def parse_variant(s: str, build: str):
    if "=" not in s:
        raise SystemExit(f"psp_bench: variant {s!r} is not NAME=DIR[:FLAGS]")
    name, rest = s.split("=", 1)
    # a Windows drive letter's colon is not the flags separator
    cut = rest.find(":", 2 if re.match(r"^[A-Za-z]:[\\/]", rest) else 0)
    d, flags = (rest, "") if cut < 0 else (rest[:cut], rest[cut + 1:])
    d = build if d in (".", "") else d
    return name, os.path.abspath(d), flags.split()


def score(log: str, windows):
    try:
        t = open(log, encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    w = {int(m.group(1)): (float(m.group(2)), float(m.group(3)), float(m.group(4))) for m in WIN.finditer(t)}
    ta = {int(m.group(1)): float(m.group(2)) for m in TA.finditer(t)}
    if not all(f in w for f in windows):
        return None
    mean = lambda xs: sum(xs) / len(xs)
    return {"ms": mean([w[f][0] for f in windows]), "replay": mean([w[f][1] for f in windows]),
            "gewait": mean([w[f][2] for f in windows]),
            "ta": mean([ta[f] for f in windows]) if all(f in ta for f in windows) else float("nan")}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("variants", nargs="+", help="NAME=DIR[:FLAGS]")
    ap.add_argument("--runs", type=int, default=3, help="runs per variant (default 3)")
    ap.add_argument("--frames", type=int, default=620)
    ap.add_argument("--stats", type=int, default=150)
    ap.add_argument("--stage", default="ss", help="--stage checkpoint (default ss: Station Square)")
    ap.add_argument("--windows", default="300,450,600", help="the --stats windows scored (default 300,450,600)")
    ap.add_argument("--timeout", type=float, default=240, help="psp_run.py --timeout per run")
    ap.add_argument("--build", default=os.path.join(REPO, "build-psp", "sa1psp"), help="psp_run.py --build (host0:)")
    ap.add_argument("--stop-file", help="end the batch before the next run when this file exists")
    ap.add_argument("--tsv", help="append every run's figures here")
    ap.add_argument("--game", nargs=argparse.REMAINDER, default=[], help="game options for every variant (last)")
    a = ap.parse_args()
    windows = [int(x) for x in a.windows.split(",")]
    variants = [parse_variant(v, a.build) for v in a.variants]
    log = os.path.join(a.build, "psplink-run", "sa1psp.log")
    results = {v[0]: [] for v in variants}
    t_batch = time.monotonic()
    stopped = False
    for r in range(a.runs):
        order = variants if r % 2 == 0 else variants[::-1]
        for name, d, flags in order:
            if a.stop_file and os.path.exists(a.stop_file):
                print(f"psp_bench: {a.stop_file} exists: batch ended", flush=True)
                stopped = True
                break
            cmd = [sys.executable, os.path.join(REPO, "tools", "psp_run.py"), "--build", a.build, "--from", d,
                   "--timeout", str(a.timeout), "--", "--stage", a.stage, "--no-pace", "--frames", str(a.frames),
                   "--stats", str(a.stats)] + flags + a.game
            try:
                os.remove(log)
            except OSError:
                pass
            t0 = time.monotonic()
            p = subprocess.run(cmd, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
            dt = time.monotonic() - t0
            s = score(log, windows) if p.returncode == 0 else None
            if s is None:
                tail = "\n".join(p.stdout.splitlines()[-8:])
                print(f"psp_bench: run {r} {name} failed (psp_run rc={p.returncode}, {dt:.0f} s):\n{tail}", flush=True)
                return 1
            results[name].append(s)
            print(f"run {r} {name:10s} {s['ms']:7.2f} ms  (TA {s['ta']:.2f}, replay {s['replay']:.2f}, "
                  f"GE wait {s['gewait']:.2f})  {dt:.0f} s", flush=True)
            if a.tsv:
                with open(a.tsv, "a", encoding="utf-8") as f:
                    f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')}\t{name}\t{d}\t{' '.join(flags)}\t"
                            f"{s['ms']:.3f}\t{s['ta']:.3f}\t{s['replay']:.3f}\t{s['gewait']:.3f}\n")
        if stopped:
            break
    print(f"psp_bench: {sum(len(v) for v in results.values())} runs in {time.monotonic() - t_batch:.0f} s; "
          f"score = mean of f{'/f'.join(map(str, windows))}, median over runs")
    base = None
    for name, _, _ in variants:
        ms = [s["ms"] for s in results[name]]
        if not ms:
            continue
        med = statistics.median(ms)
        line = f"{name:10s} median {med:7.2f} ms  range {min(ms):.2f}-{max(ms):.2f}  n={len(ms)}"
        if base is None:
            base = med
        else:
            line += f"  delta {med - base:+.2f} ms"
        print(line)
    return 2 if stopped else 0


if __name__ == "__main__":
    sys.exit(main())
