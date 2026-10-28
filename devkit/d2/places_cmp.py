"""Base vs lever-1 over the 44 survey places: watch traces with --no-pad, FRAMES each; silent."""
import filecmp, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
W = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(W, "..", "s3"))
from survey import PLACES  # noqa: E402
R = os.path.join(os.environ["SA1_ROOT"], r"sa1-psp")
D = os.path.join(os.environ["SA1_ROOT"], r"private/dump/sa1_us_1005/data")
frames = int(sys.argv[1]) if len(sys.argv) > 1 else 1200
L = os.path.join(W, "pcmp"); os.makedirs(L, exist_ok=True)


def run(job):
    (name, spec, gm), b = job
    lv, act = (int(x) for x in spec.split(":"))
    pk = []
    for a, v in ((0x8C7493BA, lv), (0x8C7493BC, act), (0x8C7493BE, lv), (0x8C7493C0, act), (0x8C760970, gm)):
        pk += ["--poke", "12:0x%08X:%d:2" % (a, v)]
    exe = os.path.join(W, "bd_" + b, "backend", "desktop", "sa1psp.exe")
    env = dict(os.environ, SDL_AUDIODRIVER="dummy")
    subprocess.run([exe, D + "/1ST_READ.BIN", D, "--log", os.path.join(L, "%s.%s.log" % (name, b)), "--no-audio", "--hidden",
                    "--headless", "--no-vmu", "--no-pad", "--frames", str(frames), "--watch-list", R + "/tests/replay/watch.txt",
                    "--watch-trace", os.path.join(L, "%s.%s.tsv" % (name, b))] + pk,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600, env=env)


jobs = [(p, b) for p in PLACES for b in ("b", "l1")]
with ThreadPoolExecutor(8) as ex:
    list(ex.map(run, jobs))
same = 0
for name, _, _ in PLACES:
    a, c = (os.path.join(L, "%s.%s.tsv" % (name, b)) for b in ("b", "l1"))
    ok = os.path.exists(a) and os.path.exists(c) and filecmp.cmp(a, c, shallow=False)
    same += ok
    trap = any("trap at" in open(os.path.join(L, "%s.%s.log" % (name, b)), errors="replace").read() for b in ("b", "l1"))
    print("%-10s %s%s" % (name, "identical" if ok else "DIFFER", " (trap)" if trap else ""), flush=True)
print("identical %d of %d" % (same, len(PLACES)))
