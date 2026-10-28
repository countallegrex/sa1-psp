"""Base (d2 walk/bd, a686bed + out_walk) vs bd_div over the 44 survey places: watch traces with --no-pad,
FRAMES each; silent, hidden, 4 runs at a time.  usage: python places_cmp.py [FRAMES] [MODE] [NAME...]
MODE = native (default: base and div, compare) | check (div only, SA1_DIV_MODE=check) | exact (div only, kill switch)."""
import filecmp, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
W = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(W, "..", "s3"))
from survey import PLACES  # noqa: E402
R = os.path.join(W, "wt")
D = os.path.join(os.environ["SA1_ROOT"], r"private/dump/sa1_us_1005/data")
frames = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
mode = sys.argv[2] if len(sys.argv) > 2 else "native"
only = set(sys.argv[3:])
places = [p for p in PLACES if not only or p[0] in only]
L = os.path.join(W, {"native": "pcmp", "check": "pchk", "exact": "pexa"}[mode]); os.makedirs(L, exist_ok=True)
EXE = {"b": os.path.join(W, "..", "d2", "walk", "bd", "backend", "desktop", "sa1psp.exe"),
       "w": os.path.join(W, "bd_div", "backend", "desktop", "sa1psp.exe")}


def run(job):
    (name, spec, gm), b = job
    lv, act = (int(x) for x in spec.split(":"))
    pk = []
    for a, v in ((0x8C7493BA, lv), (0x8C7493BC, act), (0x8C7493BE, lv), (0x8C7493C0, act), (0x8C760970, gm)):
        pk += ["--poke", "12:0x%08X:%d:2" % (a, v)]
    env = dict(os.environ, SDL_AUDIODRIVER="dummy")
    for k in ("SA1_DIV_MODE", "SA1_WALK_MODE", "SA1_DIV_COUNT"):
        env.pop(k, None)
    extra = []
    if b == "w" and mode == "check":
        env["SA1_DIV_MODE"] = "check"
    if b == "w" and mode == "exact":
        extra = ["--math-exact", "8c10ca5c,8c10cb10"]
    subprocess.run([EXE[b], D + "/1ST_READ.BIN", D, "--log", os.path.join(L, "%s.%s.log" % (name, b)), "--no-audio", "--hidden",
                    "--headless", "--no-vmu", "--no-pad", "--frames", str(frames), "--watch-list", R + "/tests/replay/watch.txt",
                    "--watch-trace", os.path.join(L, "%s.%s.tsv" % (name, b))] + pk + extra,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1200, env=env)


bins = ("b", "w") if mode == "native" else ("w",)
jobs = [(p, b) for p in places for b in bins]
with ThreadPoolExecutor(4) as ex:
    list(ex.map(run, jobs))
same = 0
tot = [0, 0, 0, 0]
for name, _, _ in places:
    lw = open(os.path.join(L, "%s.w.log" % name), errors="replace").read()
    calls = [ln.split("SHC divide: ", 1)[1].strip() for ln in lw.splitlines() if "SHC divide: calls" in ln]
    summ = [ln.split("SHC divide summary: ", 1)[1].strip() for ln in lw.splitlines() if "SHC divide summary" in ln]
    if summ:
        f = summ[-1].replace(",", "").split()
        tot[0] += int(f[2]); tot[1] += int(f[4]); tot[2] += int(f[6]); tot[3] += int(f[8])
    trap = "trap at" in lw
    if mode == "native":
        a, c = (os.path.join(L, "%s.%s.tsv" % (name, b)) for b in ("b", "w"))
        ok = os.path.exists(a) and os.path.exists(c) and filecmp.cmp(a, c, shallow=False)
        trap = trap or "trap at" in open(os.path.join(L, "%s.b.log" % name), errors="replace").read()
        same += ok
        print("%-10s %s%s | %s" % (name, "identical" if ok else "DIFFER", " (trap)" if trap else "", calls[-1] if calls else "?"), flush=True)
    else:
        print("%-10s%s | %s | %s" % (name, " (trap)" if trap else "", calls[-1] if calls else "?", summ[-1] if summ else ""), flush=True)
if mode == "native":
    print("identical %d of %d" % (same, len(places)))
if mode == "check":
    print("check total: signed %d checked %d differing; unsigned %d checked %d differing" % tuple(tot))
