"""Frame skip ON check: paced runs, --frame-skip auto; game speed = game frames per wall second / 60 over
the stats windows ending f450..f1050 (timestamps of the 'draw: N frames' lines)."""
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.join(os.environ["SA1_ROOT"], r"sa1-psp")
W = os.path.dirname(os.path.abspath(__file__))
B = os.path.join(W, "b_s3")
RUN = os.path.join(REPO, "build-psp", "sa1psp", "psplink-run")
OUT = os.path.join(W, "out")
PL = [("sd3", "6:2:4"), ("skychase2", "37:0:4"), ("sd1", "6:0:4"), ("mr0", "33:0:5"), ("sd2", "6:1:4"),
      ("chaos0", "15:0:4"), ("chaos4", "17:0:4"), ("ec1", "1:0:4"), ("casino2", "9:1:4"), ("fe1", "10:0:4"), ("ss3", "26:3:5")]
WIN = re.compile(r"\[\s*([0-9.]+) f(\d+)\s*\] draw: (\d+) frames, frame time avg ([0-9.]+) ms")
SKIP = re.compile(r"frame skip: .*?; (\d+) frames drawn, (\d+) pictures skipped")
AUD = re.compile(r"audio: (\d+) waits for the game \(([0-9.]+) s in all\)")
for name, st in PL:
    if os.path.exists(os.path.join(W, "STOP")):
        print("STOP"); break
    log = os.path.join(RUN, "sa1psp.log")
    try:
        os.remove(log)
    except OSError:
        pass
    subprocess.run([sys.executable, os.path.join(REPO, "tools", "psp_run.py"), "--from", B, "--timeout", "300", "--",
                    "--stage", st, "--frame-skip", "auto", "--frames", "1050", "--stats", "150"],
                   cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    if not os.path.exists(log):
        print(name, "no log"); continue
    shutil.copy2(log, os.path.join(OUT, name + "_fskip.log"))
    t = open(log, encoding="utf-8", errors="replace").read()
    ws = {int(m.group(2)): (float(m.group(1)), int(m.group(3))) for m in WIN.finditer(t)}
    if 300 in ws and 1050 in ws:
        dt = ws[1050][0] - ws[300][0]
        drawn = sum(ws[f][1] for f in (450, 600, 750, 900, 1050) if f in ws)
        speed = 750 / dt / 60 * 100
        a = AUD.findall(t)
        print("%-10s %-7s game speed %5.1f %% of real time, %.1f pictures/s drawn (f300-f1050, %.1f s)%s" % (
            name, st, speed, drawn / dt, dt, "; audio waits %s (%s s)" % a[-1] if a else ""), flush=True)
    else:
        print(name, "windows missing", sorted(ws)[:8])
