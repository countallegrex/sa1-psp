"""o2 GO check: paced runs, --frame-skip auto, f300-f1050 (750 vblanks = 375 turns at 2 vblanks).
game speed = 750 vblanks / wall s / 60; pictures/s = pictures drawn in the windows / wall s;
drawn % = pictures / 375 turns.  usage: python fskip2.py BUILD_DIR TAG NAME:STAGE ..."""
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.join(os.environ["SA1_ROOT"], r"sa1-psp")
W = os.path.dirname(os.path.abspath(__file__))
RUN = os.path.join(REPO, "build-psp", "sa1psp", "psplink-run")
WIN = re.compile(r"\[\s*([0-9.]+) f(\d+)\s*\] draw: (\d+) frames, frame time avg ([0-9.]+) ms")
AUD = re.compile(r"audio: (\d+) waits for the game \(([0-9.]+) s in all\)")
B, TAG = sys.argv[1], sys.argv[2]
for spec in sys.argv[3:]:
    name, st = spec.split(":", 1)
    log = os.path.join(RUN, "sa1psp.log")
    try:
        os.remove(log)
    except OSError:
        pass
    subprocess.run([sys.executable, os.path.join(REPO, "tools", "psp_run.py"), "--from", B, "--timeout", "300", "--",
                    "--stage", st, "--frame-skip", "auto", "--frames", "1050", "--stats", "150"],
                   cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    if not os.path.exists(log):
        print(TAG, name, "no log"); continue
    shutil.copy2(log, os.path.join(W, "%s_%s.log" % (TAG, name)))
    t = open(log, encoding="utf-8", errors="replace").read()
    ws = {}
    for m in WIN.finditer(t):
        ws.setdefault(int(m.group(2)), (float(m.group(1)), int(m.group(3))))   # the first line per frame (the exit summary repeats the last)
    if 300 in ws and 1050 in ws:
        dt = ws[1050][0] - ws[300][0]
        drawn = sum(ws[f][1] for f in (450, 600, 750, 900, 1050) if f in ws)
        a = AUD.findall(t)
        print("%-5s %-8s %-7s game speed %5.1f %%, %4.1f pictures/s, drawn %5.1f %% of turns (f300-f1050, %.2f s)%s" % (
            TAG, name, st, 750 / dt / 60 * 100, drawn / dt, drawn / 375 * 100, dt,
            "; audio waits %s (%s s)" % a[-1] if a else ""), flush=True)
    else:
        print(TAG, name, "windows missing", sorted(ws)[:8])
