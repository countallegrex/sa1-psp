"""Stage survey (orders v62): every place of Sonic's story by --stage LEVEL:ACT:MODE, frame skip off.
Per place: idle run (mean + p95 of the windows ending 450/600/750, max, largest free block), a --prof run
(top groups), a run-forward run (stick up from f300).  Resumable: a place with out/NAME.json is skipped.
usage: python survey.py [NAME ...]   (STOP file in this folder ends after the run in flight)"""
import json
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
os.makedirs(OUT, exist_ok=True)

A, F = 4, 5   # game mode: action stage, adventure field
PLACES = [
    ("ec1", "1:0", A), ("ec2", "1:1", A), ("ec3", "1:2", A),
    ("wv1", "2:0", A), ("wv2", "2:1", A), ("wv3", "2:2", A),
    ("casino1", "9:0", A), ("casino2", "9:1", A),
    ("ice1", "8:0", A), ("ice2", "8:1", A), ("ice3", "8:2", A),
    ("tp1", "3:0", A), ("tp2", "3:1", A),
    ("sh1", "4:0", A), ("sh2", "4:1", A), ("sh3", "4:2", A),
    ("rm1", "5:0", A), ("rm2", "5:1", A),
    ("sd1", "6:0", A), ("sd2", "6:1", A), ("sd3", "6:2", A),
    ("lw1", "7:0", A), ("lw2", "7:1", A),
    ("fe1", "10:0", A), ("fe2", "10:1", A), ("fe3", "10:2", A),
    ("chaos0", "15:0", A), ("egghornet", "20:0", A), ("chaos4", "17:0", A),
    ("eggviper", "22:0", A), ("chaos6", "18:0", A), ("skychase1", "36:0", A), ("skychase2", "37:0", A),
    ("ss0", "26:0", F), ("ss1", "26:1", F), ("ss3", "26:3", F), ("ss4", "26:4", F),
    ("mr0", "33:0", F), ("mr1", "33:1", F), ("mr2", "33:2", F),
    ("ecout0", "29:0", F), ("ecout1", "29:1", F), ("ecin0", "32:0", F), ("ecin1", "32:1", F),
]
WIN = re.compile(r"\[\s*[0-9.]+ f(\d+)\s*\] draw: (\d+) frames, frame time avg ([0-9.]+) ms max ([0-9.]+) ms, "
                 r"list replay ([0-9.]+) ms/frame, GE wait ([0-9.]+) ms/frame")
P95 = re.compile(r"draw: frame time p50 ([0-9.]+) ms p95 ([0-9.]+) ms")
FREE = re.compile(r"mem \(exit\):.*largest free block (\d+) KB")
SCORE = (450, 600, 750)


def run(args, log_copy):
    try:
        os.remove(os.path.join(RUN, "sa1psp.log"))
    except OSError:
        pass
    cmd = [sys.executable, os.path.join(REPO, "tools", "psp_run.py"), "--from", B, "--timeout", "300", "--"] + args
    p = subprocess.run(cmd, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    log = os.path.join(RUN, "sa1psp.log")
    text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
    if text:
        shutil.copy2(log, log_copy)
    open(log_copy + ".out", "w", encoding="utf-8").write(p.stdout[-6000:])
    return p.returncode, text


def windows(text):
    lines = text.splitlines()
    res = {}
    for i, l in enumerate(lines):
        m = WIN.search(l)
        if not m:
            continue
        f = int(m.group(1))
        p50 = p95 = None
        for l2 in lines[i + 1:i + 12]:
            q = P95.search(l2)
            if q:
                p50, p95 = float(q.group(1)), float(q.group(2))
                break
        res[f] = dict(n=int(m.group(2)), avg=float(m.group(3)), max=float(m.group(4)), replay=float(m.group(5)), p50=p50, p95=p95)
    return res


def score(ws):
    got = [ws[f] for f in SCORE if f in ws]
    if not got:
        return None
    return dict(mean=sum(w["avg"] for w in got) / len(got), p95=max((w["p95"] or 0) for w in got),
                max=max(w["max"] for w in got), replay=sum(w["replay"] for w in got) / len(got), windows=len(got))


def crash(text, rc):
    for l in text.splitlines():
        if "trap at" in l or "exit: trap" in l or "exception at" in l.lower():
            return l.strip()[:160]
    return "" if rc == 0 else "rc %d" % rc


def main():
    want = set(sys.argv[1:])
    for name, spec, mode in PLACES:
        if want and name not in want:
            continue
        if os.path.exists(os.path.join(W, "STOP")):
            print("STOP"); return
        js = os.path.join(OUT, name + ".json")
        if os.path.exists(js):
            continue
        st = "%s:%d" % (spec, mode)
        r = dict(name=name, stage=st)
        base = ["--stage", st, "--no-pace", "--frame-skip", "off", "--frames", "760", "--stats", "150"]
        rc, text = run(base, os.path.join(OUT, name + "_idle.log"))
        r["idle"] = score(windows(text)); r["idle_crash"] = crash(text, rc)
        m = FREE.findall(text); r["free_kb"] = int(m[-1]) if m else None
        lvl = re.findall(r"level (\d+)[^\n]{0,40}act (\d+)", text)
        r["seen_level"] = lvl[-1] if lvl else None
        if r["idle"] is None:
            json.dump(r, open(js, "w"), indent=1); print(name, "NO WINDOWS", r["idle_crash"]); continue
        prof = "prof_%s.bin" % name
        rc, text = run(base[:-4] + ["--frames", "720", "--prof", "300:700:" + prof], os.path.join(OUT, name + "_prof.log"))
        pb = os.path.join(RUN, prof)
        groups = []
        if os.path.exists(pb):
            shutil.copy2(pb, os.path.join(OUT, prof))
            rep = os.path.join(OUT, name + "_prof.txt")
            subprocess.run([sys.executable, os.path.join(REPO, "tools", "psp_prof.py"), "--pcs", pb, "--log", os.path.join(OUT, name + "_prof.log"),
                            "--elf", os.path.join(B, "sa1psp_prx.elf"), "--top", "40", "--out", rep],
                           cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if os.path.exists(rep):
                t = open(rep, encoding="utf-8", errors="replace").read()
                i = t.find("running, by group:")
                if i >= 0:
                    for l in t[i:].splitlines()[1:6]:
                        g = re.match(r"\s*([0-9.]+) %\s+\d+\s+(.*)", l)
                        if g:
                            groups.append((float(g.group(1)), g.group(2).strip()))
        r["groups"] = groups
        rc, text = run(base + ["--stick", "300:0:-127:460"], os.path.join(OUT, name + "_run.log"))
        r["run"] = score(windows(text)); r["run_crash"] = crash(text, rc)
        json.dump(r, open(js, "w"), indent=1)
        i, u = r["idle"], r["run"] or {}
        print("%-10s %-8s idle %.2f p95 %.1f max %.0f | run %s p95 %s | free %s | %s %s" % (
            name, st, i["mean"], i["p95"] or 0, i["max"], "%.2f" % u["mean"] if u else "-", "%.1f" % (u.get("p95") or 0) if u else "-",
            r["free_kb"], "; ".join("%s %.0f%%" % (g[1], g[0]) for g in groups[:3]), r["idle_crash"] or r["run_crash"]), flush=True)


if __name__ == "__main__":
    main()
