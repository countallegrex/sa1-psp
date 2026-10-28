"""Add one measured step to the speed tracker (history.json / levers.json in this folder).

    python add_step.py <commit> <ms> "<what>" [--ec <ms>] [--lever-order N --lever-note "text to append"]
                       [--new-lever "name|status|est_ms|measured_ms|note"]
                       [--require-main REF] [--ci-dir DIR] [--no-ci]

<ms> is the Station Square mean ms a frame; --ec stores the Emerald Coast figure for
the same commit as "ec" (omitted when not measured).  Appends the history step and
applies the lever edits (LF, 1-space indent), then prints one line.  Open index.html
(through `python -m http.server`, see devkit/README.md) to see the chart.

Optional checks, both off unless asked for:
  --require-main REF  the commit must be an ancestor of REF (e.g. origin/main)
  --ci-dir DIR        DIR/<full sha>.txt from tools/ci_local.py must start with
                      "PASS <sha>" (also taken from the SA1_CI_DIR environment variable)
--no-ci skips both.  The repository is the one this file lives in (or SA1_REPO).
"""
import argparse, datetime, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get("SA1_REPO") or os.path.dirname(os.path.dirname(HERE))


def run(*args):
    return subprocess.run(args, cwd=REPO, capture_output=True, text=True)


def ci_ok(commit, main_ref, ci_dir):
    if run("git", "rev-parse", "--verify", "-q", commit + "^{commit}").returncode != 0:
        return f"{commit} is not a commit in {REPO}"
    if main_ref and run("git", "merge-base", "--is-ancestor", commit, main_ref).returncode != 0:
        return f"{commit} is not on {main_ref}"
    if ci_dir:
        sha = run("git", "rev-parse", commit).stdout.strip()
        path = os.path.join(ci_dir, sha + ".txt")
        if not os.path.isfile(path):
            return f"no local CI result for {commit} in {ci_dir} (run: python tools/ci_local.py --commit {commit})"
        with open(path, encoding="utf-8") as f:
            first = f.readline().split()
        if first[:2] != ["PASS", sha]:
            return f"local CI for {commit}: {' '.join(first) or 'empty'}"
    return None


def load(name):
    with open(os.path.join(HERE, name), encoding="utf-8") as f:
        return json.load(f)


def save(name, data):
    with open(os.path.join(HERE, name), "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write("\n")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("commit"); p.add_argument("ms", type=float); p.add_argument("what")
    p.add_argument("--ec", type=float, help="Emerald Coast mean ms a frame for this commit")
    p.add_argument("--lever-order", type=int); p.add_argument("--lever-note")
    p.add_argument("--new-lever"); p.add_argument("--no-ci", action="store_true")
    p.add_argument("--require-main"); p.add_argument("--ci-dir", default=os.environ.get("SA1_CI_DIR"))
    a = p.parse_args()
    if not a.no_ci:
        err = ci_ok(a.commit, a.require_main, a.ci_dir)
        if err:
            print("NOT ADDED: " + err); sys.exit(1)
    hist, levers = load("history.json"), load("levers.json")
    if any(h["commit"] == a.commit for h in hist):
        print(f"NOT ADDED: {a.commit} is already step {next(h['step'] for h in hist if h['commit'] == a.commit)}"); sys.exit(1)
    step = hist[-1]["step"] + 1
    hist.append({"step": step, "date": datetime.date.today().isoformat(), "commit": a.commit, "ms": a.ms, "what": a.what})
    if a.ec is not None:
        hist[-1]["ec"] = a.ec
    if a.lever_order and a.lever_note:
        lv = next(l for l in levers if l["order"] == a.lever_order)
        note = lv["note"].rstrip()
        lv["note"] = (note + ("" if not note or note.endswith((".", ";")) else ";") + " " + a.lever_note).strip()
    if a.new_lever:
        name, status, est, meas, note = a.new_lever.split("|", 4)
        levers.append({"order": max(l["order"] for l in levers) + 1, "lever": name, "status": status, "cpu_share": "",
                       "est_ms": float(est) if est else None, "measured_ms": float(meas) if meas else None, "note": note})
    save("history.json", hist); save("levers.json", levers)
    ec = f", ec {a.ec} ms" if a.ec is not None else ""
    print(f"ADDED step {step}: {a.commit} {a.ms} ms{ec}; history {len(hist)} rows, levers {len(levers)} rows")


if __name__ == "__main__":
    main()
