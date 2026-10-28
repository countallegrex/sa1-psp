"""PSP gate queue, unattended and resumable (sa1-psp-i gen 20, 2026-10-28).

    python run_gates.py --plan            job list, run count, time estimate (no PSP)
    python run_gates.py                   run every job not done yet, in priority order
    python run_gates.py --only walker     one job (still resumable)
    python run_gates.py --table           rewrite results.md from state.json

Start it only after the orchestrator says "play-test over".  Every finished PSP run is saved in
state.json at once, so after a wedge (power-cycle the PSP, start PSPLink from the XMB) the same
command resumes where it stopped.  A file STOP next to this script stops it before the next run.
The PSPLink host is never stopped or killed by this script (that wedges a connected PSP).

Metrics as private\\qb\\gate.py: T = wall ms per turn over f300-f750 at --no-pace --frame-skip 60,
3 rounds ABBA (forward, reverse, forward); L = (30 T - D) / 29, so dL = 30/29 dT (D cancels).
Pictures: ss/ec --frame-skip 4 f200/f400, Chaos 0 --frame-skip 3 f405/f603, md5 per arm.
"""
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

W = os.path.dirname(os.path.abspath(__file__))
O = os.environ.get("SA1_GATES_OUT") or W                # outputs (a dry run points it elsewhere)
P = os.path.dirname(W)                                  # ...\private
REPO = os.environ.get("SA1_GATES_REPO") or os.path.join(os.path.dirname(P), "sa1-psp")  # the tree usbhostfs_pc serves
RUN = os.path.join(REPO, "build-psp", "sa1psp", "psplink-run")
STATE = os.path.join(O, "state.json")
TABLE = os.path.join(O, "results.md")
PING = os.environ.get("SA1_GATES_PING") or os.path.join(P, "livefix", "tools", "psplink_ping.py")
D2 = os.path.join(P, "d2")
WIN = re.compile(r"\[\s*([0-9.]+) f(\d+)\s*\] draw: (\d+) frames")

# seconds per run, used until state.json has measured ones (desk guess: reset + 14 MB PRX load + game)
GUESS = {"abba": 60.0, "shot": 75.0, "prof": 80.0}

SHOTS = (("ss", "4", ("200", "400")), ("ec", "4", ("200", "400")), ("15:0:4", "3", ("405", "603")))
NAMES = {"ss": "Station Square", "ec": "Emerald Coast", "1:0:4": "EC1", "2:1:4": "WV2", "6:1:4": "Sky Deck 2",
         "6:2:4": "Sky Deck 3", "8:0:4": "Ice Cap 1", "8:2:4": "Ice Cap 3", "9:1:4": "Casino 2", "15:0:4": "Chaos 0"}


def arm(label, path, *args):
    return {"label": label, "dir": path, "args": list(args)}


RMAIN = arm("main-repin", os.path.join(D2, "repin", "b_rmain"))
# Priority order (orders v77 + m6c gate queue + gen 20 desk results).  "go" is the bar on the first
# candidate arm vs arm 0: {"stage": dL_max} must all hold; "any" = a measured gain (dL < 0, ranges disjoint).
JOBS = [
    {"id": "cardlog", "kind": "shell", "cmd": ["sh", os.path.join(P, "qb", "replay_log.sh")],
     "what": "copy the maintainer's play-test card log off (read-only) and summarise it (m6b)"},
    {"id": "b", "what": "mixer lever (b) e92ad9f (qb-mixer, on 2a47191)",
     "arms": [arm("base-2a47191", os.path.join(P, "qb", "b_base")), arm("qb4", os.path.join(P, "qb", "b_qb4"))],
     "shots": False, "abba": ["9:1:4", "15:0:4"], "go": {"9:1:4": -1.0},
     "after": "GO: rebase qb-mixer onto f95ac27, CI, push (orders v75)"},
    {"id": "walker", "what": "native walker a686bed (d2-walker), main's fresh pins; arm 3 = kill switch",
     "arms": [RMAIN, arm("walker", os.path.join(D2, "repin", "b_rwalkp")),
              arm("walker-exact", os.path.join(D2, "repin", "b_rwalkp"), "--math-exact", "8c040784")],
     "shots": True, "abba": ["15:0:4", "9:1:4", "1:0:4"], "go": {"15:0:4": -1.0},
     "after": "GO needs Chaos 0 <= -1.0 L plus one more motion-heavy place (Casino 2 or EC1); kill = int diffs"},
    {"id": "div", "what": "divide helpers 6039866 (div-helpers, on the walker); same PRX vs its kill switch",
     "arms": [arm("div-exact", os.path.join(P, "d3", "psp", "b_div"), "--math-exact", "8c10ca5c,8c10cb10"),
              arm("div", os.path.join(P, "d3", "psp", "b_div"))],
     "shots": False, "abba": ["2:1:4", "6:2:4", "8:0:4", "8:2:4", "6:1:4"], "go": {"2:1:4": "any"},
     "after": "lands after the walker; Sky Deck 3 is the null control"},
    {"id": "qe", "what": "qe dispatch 2ea59dd (qe-dispatch rebased onto f95ac27); each arm pinned to its own build (cache sets differ)",
     "arms": [RMAIN, arm("qe", os.path.join(D2, "repin", "b_rqe3"))],
     "shots": True, "abba": ["15:0:4", "6:2:4"], "go": {"15:0:4": "any"},
     "after": "its -0.2..-0.35 L was a desk re-estimate; on main's pins it pads +993 KB, so self-pinned: small deltas are layout noise"},
    {"id": "l1", "what": "lever 1 6d1217d (d2-preserved on f95ac27); each arm pinned to its own build (cache sets differ)",
     "arms": [RMAIN, arm("lever1", os.path.join(D2, "repin", "b_rl1"))],
     "shots": True, "abba": ["15:0:4", "9:1:4", "6:2:4", "1:0:4"], "go": {"15:0:4": "any"},
     "after": "unpinned comparison: a delta inside ~1.5 ms is layout noise, see hot job"},
    {"id": "repin", "what": "layout only: main f95ac27 with the old pins vs re-pinned (frees 426 KB)",
     "arms": [arm("main-oldpins", os.path.join(D2, "b_psp_main")), RMAIN],
     "shots": True, "abba": ["ss", "ec", "15:0:4"], "go": {"15:0:4": "any"},
     "after": "neutral is fine to land for the RAM; slower is not"},
    {"id": "hot", "kind": "hot", "what": "hot-order regen from console profiles of main-repin (ss + ec), then bench",
     "base": RMAIN, "stages": ["ss", "ec"], "tree": os.path.join(D2, "wt_fix"),
     "out": os.environ["SA1_ROOT_WSL"] + "/private/d2/out_b",
     "shots": True, "abba": ["ss", "ec", "15:0:4", "6:2:4"], "go": {"15:0:4": "any"},
     "after": "a new list lands only if not slower anywhere; then rebuild every branch against it"},
]


def load():
    try:
        return json.load(open(STATE, encoding="utf-8"))
    except (OSError, ValueError):
        return {"runs": {}, "jobs": {}, "secs": {}}


def save(st):
    tmp = STATE + ".tmp"
    json.dump(st, open(tmp, "w", encoding="utf-8"), indent=1, sort_keys=True)
    os.replace(tmp, STATE)


def say(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    open(os.path.join(O, "run_gates.log"), "a", encoding="utf-8").write(line + "\n")


def stopped():
    return os.path.exists(os.path.join(O, "STOP"))


def ping():
    r = subprocess.run([sys.executable, PING], capture_output=True, text=True)
    return r.returncode == 0, (r.stdout + r.stderr).strip().splitlines()[-1:] or [""]


class Wedged(Exception):
    pass


def psp(st, key, kind, a, game_args, keep=()):
    """One PSP run, cached in state.json under key.  Returns the saved record."""
    if key in st["runs"]:
        return st["runs"][key]
    if stopped():
        raise SystemExit("STOP file present")
    for attempt in (1, 2):
        log = os.path.join(RUN, "sa1psp.log")
        try:
            os.remove(log)
        except OSError:
            pass
        os.utime(os.path.join(a["dir"], "sa1psp.prx"))   # psp_run.py stages only a newer or resized PRX: force it
        t0 = time.time()
        rc = subprocess.run([sys.executable, os.path.join(REPO, "tools", "psp_run.py"), "--from", a["dir"],
                             "--timeout", "300", "--quiet", "60", "--"] + game_args + a["args"],
                            cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT).returncode
        secs = time.time() - t0
        text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
        ok = rc == 0 and "] exit: " in text
        if ok or rc == 2:
            break
        alive, msg = ping()
        say("  %s: psp_run rc %d, no exit line; ping: %s" % (key, rc, msg[0]))
        if not alive:
            raise Wedged("%s (rc %d): PSPLink does not answer" % (key, rc))
    rec = {"rc": rc, "ok": ok, "secs": round(secs, 1), "t": time.strftime("%Y-%m-%d %H:%M:%S"),
           "nomem": text.count("no memory"), "decodefail": text.count("decode failed")}
    if kind == "abba":
        ws = {}
        for m in WIN.finditer(text):
            ws.setdefault(int(m.group(2)), float(m.group(1)) * 1000.0)
        rec["T"] = round((ws[750] - ws[300]) / 225, 3) if 300 in ws and 750 in ws else None
    logs = os.path.join(O, "logs")
    os.makedirs(logs, exist_ok=True)
    open(os.path.join(logs, re.sub(r"[^A-Za-z0-9.]+", "_", key) + ".log"), "w", encoding="utf-8").write(text)
    for src, dst in keep:                     # (file in RUN, absolute destination)
        if os.path.exists(os.path.join(RUN, src)):
            shutil.copyfile(os.path.join(RUN, src), dst)
            rec.setdefault("kept", []).append(dst)
    st["runs"][key] = rec
    st["secs"].setdefault(kind, []).append(rec["secs"])
    save(st)
    say("  %s: rc %d %s%s" % (key, rc, "T %.2f" % rec["T"] if rec.get("T") else "", " (%.0f s)" % secs))
    return rec


def do_shots(st, job, arms):
    for stg, fs, fr in SHOTS:
        for a in arms[:2]:
            args = ["--stage", stg, "--no-pace", "--frame-skip", fs, "--frames", str(int(fr[1]) + 10)]
            keep = []
            for f in fr:
                name = "shot_%s_%s_%s_s%s.ppm" % (job["id"], a["label"], stg.replace(":", "_"), f)
                args += ["--screenshot", "%s:s%s.ppm" % (f, f)]
                keep.append(("s%s.ppm" % f, os.path.join(O, "shots", name)))
            os.makedirs(os.path.join(O, "shots"), exist_ok=True)
            psp(st, "%s/shot/%s/%s" % (job["id"], stg, a["label"]), "shot", a, args, keep)


def do_abba(st, job, arms):
    for stg in job["abba"]:
        for r in range(3):
            for a in (arms if r % 2 == 0 else arms[::-1]):
                psp(st, "%s/abba/%s/%d/%s" % (job["id"], stg, r, a["label"]), "abba", a,
                    ["--stage", stg, "--no-pace", "--frame-skip", "60", "--frames", "750", "--stats", "150"])


def do_hot(st, job):
    """Profiles on the PSP, then the desk part of tools/psp_hot_regen.sh (list, unpinned build, pin, build)."""
    h = os.path.join(O, "hot")
    os.makedirs(h, exist_ok=True)
    args = []
    for stg in job["stages"]:
        p = "prof_hot_%s" % stg
        psp(st, "hot/prof/%s" % stg, "prof", job["base"],
            ["--stage", stg, "--no-pace", "--frames", "720", "--frame-skip", "off", "--prof", "200:700:%s.bin" % p],
            [("%s.bin" % p, os.path.join(h, p + ".bin")), ("sa1psp.log", os.path.join(h, p + ".log"))])
        args += ["--pcs", os.path.join(h, p + ".bin"), "--log", os.path.join(h, p + ".log")]
    new = os.path.join(h, "b_hot", "sa1psp.prx")
    if not os.path.exists(new):
        tree = job["tree"]
        u, pinned = os.path.join(h, "hot_u.txt"), os.path.join(h, "hot_pinned.txt")
        wl = os.path.join(W, "withlist.sh")
        wh = h
        steps = [[sys.executable, "tools/psp_hot_order.py"] + args + ["--elf", os.path.join(job["base"]["dir"], "sa1psp_prx.elf"), "--out", u],
                 ["sh", wl, tree, "hot_u", job["out"], h, u],
                 [sys.executable, "tools/psp_hot_order.py", "--pin", os.path.join(h, "b_hot_u", "sa1psp_prx.elf"), "--list", u, "--out", pinned],
                 ["sh", wl, tree, "hot", job["out"], h, pinned]]
        for s in steps:
            r = subprocess.run(s, cwd=tree, capture_output=True, text=True)
            open(os.path.join(h, "desk.log"), "a", encoding="utf-8").write(" ".join(s) + "\n" + r.stdout + r.stderr)
            if r.returncode:
                raise SystemExit("hot: desk step failed: %s (see %s)" % (s[1], os.path.join(h, "desk.log")))
        say("  hot: new list built (%s); kept in private\\gates\\hot, the tree's list is unchanged" % wh)
    return [job["base"], arm("hot-regen", os.path.join(h, "b_hot"))]


def runs_of(job):
    if job.get("kind") == "shell":
        return {}
    n = len(job["arms"]) if "arms" in job else 2
    c = {"abba": 3 * n * len(job["abba"])}
    if job.get("shots"):
        c["shot"] = 2 * len(SHOTS)
    if job.get("kind") == "hot":
        c["prof"] = len(job["stages"])
    return c


def per_run(st, kind):
    s = st["secs"].get(kind)
    return statistics.median(s) if s else GUESS[kind]


def plan(st):
    tot = left = 0.0
    for j in JOBS:
        c = runs_of(j)
        secs = sum(per_run(st, k) * v for k, v in c.items())
        done = sum(1 for k in st["runs"] if k.split("/")[0] == j["id"])
        n = sum(c.values())
        rem = secs * max(0, n - done) / n if n else 0
        tot += secs
        left += rem
        print("%-8s %3d runs (%s) ~%4.0f min, done %d  %s" % (j["id"], n, ", ".join("%d %s" % (v, k) for k, v in c.items()) or "shell",
                                                             secs / 60, done, j["what"]))
    print("total ~%.1f h, left ~%.1f h (per run: abba %.0f s, shot %.0f s, prof %.0f s%s)" % (
        tot / 3600, left / 3600, per_run(st, "abba"), per_run(st, "shot"), per_run(st, "prof"),
        "" if st["secs"] else "; desk guesses, measured ones replace them"))


def table(st):
    out = ["# PSP gate results", "", "Written by private\\gates\\run_gates.py, %s. dL = 30/29 dT vs arm 0 (ms per logic turn); "
           "'disjoint' = every candidate T below every arm-0 T (or above, for a loss)." % time.strftime("%Y-%m-%d %H:%M"), ""]
    for j in JOBS:
        if j.get("kind") == "shell":
            r = st["jobs"].get(j["id"], {})
            out += ["## %s: %s" % (j["id"], j["what"]), "", "```", r.get("out", "not run"), "```", ""]
            continue
        arms = j.get("arms") or [j["base"], arm("hot-regen", "")]
        out += ["## %s: %s" % (j["id"], j["what"]), ""]
        if j.get("shots"):
            rows = []
            for stg, _, fr in SHOTS:
                for f in fr:
                    hs = []
                    for a in arms[:2]:
                        p = os.path.join(O, "shots", "shot_%s_%s_%s_s%s.ppm" % (j["id"], a["label"], stg.replace(":", "_"), f))
                        hs.append(hashlib.md5(open(p, "rb").read()).hexdigest()[:12] if os.path.exists(p) else None)
                    rows.append("%s f%s %s" % (stg, f, "-" if None in hs else ("identical" if hs[0] == hs[1] else "DIFFER %s %s" % tuple(hs))))
            out += ["Pictures (%s vs %s): %s" % (arms[0]["label"], arms[1]["label"], "; ".join(rows)), ""]
        out += ["| place | " + " | ".join("%s T (3 runs)" % a["label"] for a in arms) + " | dL | disjoint | bar | verdict |",
                "|" + "---|" * (len(arms) + 5)]
        verdicts = []
        for stg in j["abba"]:
            T = {a["label"]: [st["runs"].get("%s/abba/%s/%d/%s" % (j["id"], stg, r, a["label"]), {}).get("T") for r in range(3)]
                 for a in arms}
            cells, med = [], {}
            for a in arms:
                v = [x for x in T[a["label"]] if x is not None]
                med[a["label"]] = statistics.median(v) if len(v) == 3 else None
                cells.append("%s [%s]" % ("%.2f" % med[a["label"]] if med[a["label"]] else "-", " ".join("%.2f" % x for x in v)))
            b, c = arms[0]["label"], arms[1]["label"]
            if med[b] is None or med[c] is None:
                out.append("| %s | %s | - | - | - | not run |" % (NAMES.get(stg, stg), " | ".join(cells)))
                continue
            dl = 30.0 / 29.0 * (med[c] - med[b])
            disj = max(T[c]) < min(T[b]) or min(T[c]) > max(T[b])
            bar = j["go"].get(stg)
            if bar is None:
                v = "info"
            elif bar == "any":
                v = "GO" if dl < 0 and disj else ("NO-GO" if dl > 0 and disj else "neutral")
            else:
                v = "GO" if dl <= bar and disj else "NO-GO"
            if bar is not None:
                verdicts.append(v)
            out.append("| %s | %s | %+.2f | %s | %s | %s |" % (NAMES.get(stg, stg), " | ".join(cells), dl, "yes" if disj else "no",
                                                             "-" if bar is None else bar, v))
        bad = [k for k, r in st["runs"].items() if k.split("/")[0] == j["id"] and (r.get("nomem") or r.get("decodefail") or not r.get("ok"))]
        out += ["", "Job verdict: %s. %s" % ("/".join(verdicts) or "-", j.get("after", ""))]
        if bad:
            out.append("Runs with problems: " + ", ".join("%s (rc %s, no memory %s, decode failed %s)" % (
                k, st["runs"][k]["rc"], st["runs"][k]["nomem"], st["runs"][k]["decodefail"]) for k in bad))
        out.append("")
    open(TABLE, "w", encoding="utf-8").write("\n".join(out))


def main():
    st = load()
    if "--plan" in sys.argv:
        plan(st)
        return 0
    if "--table" in sys.argv:
        table(st)
        print(TABLE)
        return 0
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
    alive, msg = ping()
    if not alive and "not running" not in msg[0]:      # no host yet: psp_run.py starts it on the first run
        say("PSPLink does not answer (%s): start PSPLink from the XMB, then run again" % msg[0])
        return 3
    plan(st)
    try:
        for j in JOBS:
            if only and j["id"] != only:
                continue
            if st["jobs"].get(j["id"], {}).get("done"):
                continue
            say("job %s: %s" % (j["id"], j["what"]))
            if j.get("kind") == "shell":
                r = subprocess.run(j["cmd"], capture_output=True, text=True)
                st["jobs"][j["id"]] = {"done": True, "out": (r.stdout + r.stderr).strip()[-3000:]}
            else:
                arms = do_hot(st, j) if j.get("kind") == "hot" else j["arms"]
                if j.get("shots"):
                    do_shots(st, j, arms)
                do_abba(st, j, arms)
                st["jobs"][j["id"]] = {"done": True}
            save(st)
            table(st)
    except Wedged as e:
        st["wedged"] = {"at": time.strftime("%Y-%m-%d %H:%M:%S"), "why": str(e)}
        save(st)
        table(st)
        say("WEDGED: %s. Power-cycle the PSP (hold POWER), start PSPLink from the XMB, run the same command again" % e)
        return 5
    st.pop("wedged", None)
    save(st)
    table(st)
    say("all done: %s" % TABLE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
