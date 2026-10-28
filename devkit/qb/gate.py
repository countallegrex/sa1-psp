"""qb PSP gate (copy of private\qa\gate.py) (from private\\qc\\gate.py). Usage: gate.py BASE CAND [noshots] [noabba] [extra game args after --].
(1) pictures on drawn turns: ss/ec --frame-skip 4 f200/f400, Chaos 0 --frame-skip 3 f405/f603, md5 compare.
(2) L ABBA x3 same batch: --no-pace --frame-skip 60, T = wall ms per turn over f300-f750, L = (30 T - D) / 29,
D from speed/fskip-survey.md. Builds are named by folder under private\\qa (or an absolute path)."""
import hashlib
import os
import re
import shutil
import statistics
import subprocess
import sys

REPO = os.path.join(os.environ["SA1_ROOT"], r"sa1-psp")
W = os.path.dirname(os.path.abspath(__file__))
RUN = os.path.join(REPO, "build-psp", "sa1psp", "psplink-run")
argv = sys.argv[1:]
extra = []
if "--" in argv:
    i = argv.index("--"); extra = argv[i + 1:]; argv = argv[:i]
names = [a for a in argv if a not in ("noshots", "noabba")][:2]
# a name may carry its own game args: "name@arg1,arg2"
B, A = {}, {}
for n in names:
    path, _, args = n.partition("@")
    B[n] = path if os.path.isabs(path) else os.path.join(W, path)
    A[n] = args.split(",") if args else []
WIN = re.compile(r"\[\s*([0-9.]+) f(\d+)\s*\] draw: (\d+) frames")
D = {"6:2:4": 99.1, "15:0:4": 65.6, "9:1:4": 59.07}
STAGES = [s for s in os.environ.get("QA_STAGES", "9:1:4,15:0:4").split(",") if s]


def run(b, args):
    log = os.path.join(RUN, "sa1psp.log")
    try:
        os.remove(log)
    except OSError:
        pass
    subprocess.run([sys.executable, os.path.join(REPO, "tools", "psp_run.py"), "--from", B[b], "--timeout", "300", "--"]
                   + args + A[b] + extra, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    t = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
    return t


def stop():
    return os.path.exists(os.path.join(W, "STOP"))


def tag(b):
    return re.sub(r"[^A-Za-z0-9]+", "_", b)


if "noshots" not in sys.argv:
    for st, fs, fr in (("ss", "4", ("200", "400")), ("ec", "4", ("200", "400")), ("15:0:4", "3", ("405", "603"))):
        h = {}
        for b in names:
            if stop():
                sys.exit("STOP")
            args = ["--stage", st, "--no-pace", "--frame-skip", fs, "--frames", str(int(fr[1]) + 10)]
            for f in fr:
                args += ["--screenshot", "%s:s%s.ppm" % (f, f)]
            t = run(b, args)
            open(os.path.join(W, "shotlog_%s_%s.txt" % (tag(b), st.replace(":", "_"))), "w", encoding="utf-8").write(t)
            for f in fr:
                src = os.path.join(RUN, "s%s.ppm" % f)
                dst = os.path.join(W, "shot_%s_%s_s%s.ppm" % (tag(b), st.replace(":", "_"), f))
                if os.path.exists(src):
                    shutil.move(src, dst)
                    h[(b, f)] = hashlib.md5(open(dst, "rb").read()).hexdigest()[:12]
        for f in fr:
            a, c = h.get((names[0], f)), h.get((names[1], f))
            print("shot %-7s s%s %s %s %s" % (st, f, "identical" if a and a == c else "DIFFER", a, c), flush=True)

if "noabba" not in sys.argv:
    for st in STAGES:
        T = {b: [] for b in names}
        for r in range(3):
            for b in (names if r % 2 == 0 else names[::-1]):
                if stop():
                    sys.exit("STOP")
                t = run(b, ["--stage", st, "--no-pace", "--frame-skip", "60", "--frames", "750", "--stats", "150"])
                ws = {}
                for m in WIN.finditer(t):
                    ws.setdefault(int(m.group(2)), float(m.group(1)) * 1000.0)
                if 300 in ws and 750 in ws:
                    T[b].append((ws[750] - ws[300]) / 225)
                if r == 0:
                    open(os.path.join(W, "abbalog_%s_%s.txt" % (tag(b), st.replace(":", "_"))), "w", encoding="utf-8").write(t)
                print("  %s run %d %s T %s" % (st, r, b, "%.2f" % T[b][-1] if T[b] else "-"), flush=True)
        ma, mb = statistics.median(T[names[0]]), statistics.median(T[names[1]])
        la, lb = (30 * ma - D[st]) / 29, (30 * mb - D[st]) / 29
        print("L %-7s T %s %.2f [%s] %s %.2f [%s] -> L %.2f -> %.2f delta %+.2f ms" % (
            st, names[0], ma, " ".join("%.2f" % x for x in T[names[0]]), names[1], mb,
            " ".join("%.2f" % x for x in T[names[1]]), la, lb, lb - la), flush=True)
