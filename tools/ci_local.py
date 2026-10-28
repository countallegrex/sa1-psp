#!/usr/bin/env python3
"""Local CI gate: the jobs of .github/workflows/build.yml, run on this machine.

    python tools/ci_local.py [--commit REV] [--jobs a,b] [--keep] [--force]

Clones REV (default HEAD) of this checkout into fresh directories, so
uncommitted files are not seen: a Windows clone under %TEMP% for the
windows-latest jobs (clang, cmake + Ninja), and a WSL clone on WSL's own disk
for the ubuntu-latest jobs (cc, PSPDEV).  The jobs run in parallel, each with
its log in private/ci-local/logs/<commit>/<job>.log.  One PASS/FAIL/SKIP line
per job is printed, and the summary goes to private/ci-local/<commit>.txt,
whose first line is "PASS <commit>" or "FAIL <commit>" (tools that gate a
landing read that line).  Exit status 0 only when every job passed.

psp-docker is SKIP: Docker is not installed here; the psp job builds with the
same tools/build_psp.sh, against the WSL pspdev install instead of the pinned
image.  An existing PASS file for the commit is reused unless --force.

Part of sa1-psp (MIT).
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NOWIN = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def _main_checkout() -> str:
    """The main checkout when REPO is a linked worktree: its .git is a file
    pointing at a Windows path that WSL git cannot follow, so the clones come
    from the common git directory, and results go next to the main checkout."""
    try:
        common = subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"], cwd=REPO,
                                check=True, capture_output=True, text=True, creationflags=NOWIN).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return REPO
    common = os.path.abspath(common)
    return os.path.dirname(common) if os.path.basename(common) == ".git" else REPO


MAIN = _main_checkout()
OUT = os.path.join(os.path.dirname(MAIN), "private", "ci-local")

# Shared compile lines (build.yml's, per OS).  {cc} is clang or cc; {lm} is
# -lm on Linux; {exe} is .exe on Windows.
LOADER = ("{cc} -std=c11 -D_CRT_SECURE_NO_WARNINGS -Ihle -Ihle/overlay -Irecomp/emitter/runtime "
          "tests/overlay/loader_test.c hle/overlay/overlay_loader.c hle/state.c "
          "recomp/emitter/runtime/sh4_rt.c {lm} -o loader_test{exe}")
PVR = ("{cc} -std=c11 -Wall -D_CRT_SECURE_NO_WARNINGS -Ihle/ninja tests/ninja/pvr_decode_test.c "
       "hle/ninja/pvr_decode.c -o pvr_decode_test{exe}")
TAPE = ("{cc} -std=c11 -Wall -D_CRT_SECURE_NO_WARNINGS -Ihle -Irecomp/emitter/runtime -Ibackend/desktop "
        "tests/replay/tape_format_test.c backend/desktop/input_tape.c backend/desktop/sha256.c "
        "recomp/emitter/runtime/sh4_rt.c {lm} -o tape_format_test{exe}")
SAVE = ("{cc} -std=c11 -Wall -D_CRT_SECURE_NO_WARNINGS -Ihle -Irecomp/emitter/runtime -Ibackend "
        "-Ibackend/desktop tests/savestate/savestate_test.c backend/desktop/platform_desktop.c "
        "backend/desktop/coro_switch.c backend/desktop/savestate.c backend/desktop/sha256.c hle/state.c "
        "recomp/emitter/runtime/sh4_rt.c {nopie} -o savestate_test{exe}")
SOUND = ("{cc} -std=c11 -Wall -Wextra -Wno-unused-parameter -D_CRT_SECURE_NO_WARNINGS -D_USE_MATH_DEFINES "
         "-D_DEFAULT_SOURCE -Ihle/sound tests/sound/snd_test.c hle/sound/snd_core.c hle/sound/adx.c "
         "{lm} -o snd_test{exe}")

WIN = {"cc": "clang", "lm": "", "exe": ".exe", "nopie": "-Xlinker /DYNAMICBASE:NO"}
LIN = {"cc": "cc", "lm": "-lm", "exe": "", "nopie": "-no-pie -lm"}


def steps_for(os_name: str) -> dict[str, list[str]]:
    v = WIN if os_name == "windows" else LIN
    py = "python" if os_name == "windows" else "python3"
    # Explicit .\ on Windows: cmd may not search the current directory
    # (NoDefaultCurrentDirectoryInExePath).
    run = (lambda n: ".\\" + n + ".exe") if os_name == "windows" else (lambda n: "./" + n)
    return {
        f"emitter-tests ({os_name})": [
            f"{py} tests/emitter/run_tests.py",
            LOADER.format(**v), run("loader_test"),
            PVR.format(**v), run("pvr_decode_test")],
        f"replay-harness ({os_name})": [
            TAPE.format(**v), f"{py} tests/replay/run_synthetic.py {run('tape_format_test')}"],
        f"savestate ({os_name})": [
            SAVE.format(**v), f"{py} tests/savestate/run_synthetic.py {run('savestate_test')}"],
        f"sound ({os_name})": [SOUND.format(**v), run("snd_test")],
    }


def linux_jobs() -> dict[str, list[str]]:
    jobs = {"check-clean": ["python3 tools/check_clean.py --all",
                            "python3 tools/gen_overlay_registry.py --check"],
            "psp": ['export PATH="$PATH:$PSPDEV/bin"; sh tools/build_psp.sh build-ci-psp',
                    "test -f build-ci-psp/EBOOT.PBP"]}
    jobs.update(steps_for("linux"))
    return jobs


def windows_jobs() -> dict[str, list[str]]:
    jobs = steps_for("windows")
    jobs["desktop"] = [
        "cmake -S . -B build-ci-desktop -G Ninja -DTARGET=desktop -DCMAKE_C_COMPILER=clang "
        "-DCMAKE_CXX_COMPILER=clang++",
        "cmake --build build-ci-desktop",
        "@hello"]
    return jobs


def git(*args: str, cwd: str = REPO) -> str:
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True,
                          creationflags=NOWIN).stdout.strip()


def wsl_path(win: str) -> str:
    drive, rest = os.path.splitdrive(os.path.abspath(win))
    return "/mnt/" + drive[0].lower() + rest.replace("\\", "/")


def sh_quote(s: str) -> str:
    return "'" + s.replace("'", "'\\''") + "'"


class Runner:
    def __init__(self, sha: str, logdir: str, wintree: str, lintree: str, tmp: str):
        self.sha, self.logdir, self.wintree, self.lintree, self.tmp = sha, logdir, wintree, lintree, tmp

    def log(self, job: str):
        name = job.replace(" ", "_").replace("(", "").replace(")", "")
        return open(os.path.join(self.logdir, name + ".log"), "w", encoding="utf-8", errors="replace")

    def win_job(self, job: str, steps: list[str]) -> tuple[str, str, float]:
        t0 = time.time()
        with self.log(job) as f:
            for step in steps:
                f.write(f"$ {step}\n"); f.flush()
                if step == "@hello":
                    exe = os.path.join(self.wintree, "build-ci-desktop", "backend", "desktop", "sa1psp_hello.exe")
                    r = subprocess.run([exe], cwd=self.wintree, capture_output=True, text=True,
                                       creationflags=NOWIN)
                    f.write(r.stdout + r.stderr)
                    if r.returncode != 0 or "hello from the desktop backend" not in r.stdout:
                        return job, "FAIL", time.time() - t0
                    continue
                r = subprocess.run(step, cwd=self.wintree, shell=True, stdout=f, stderr=subprocess.STDOUT,
                                   creationflags=NOWIN)
                if r.returncode != 0:
                    f.write(f"exit {r.returncode}\n")
                    return job, "FAIL", time.time() - t0
        return job, "PASS", time.time() - t0

    def lin_job(self, job: str, steps: list[str]) -> tuple[str, str, float]:
        t0 = time.time()
        name = job.replace(" ", "_").replace("(", "").replace(")", "")
        script = os.path.join(self.tmp, name + ".sh")
        with open(script, "w", encoding="utf-8", newline="\n") as s:
            s.write("set -e\ncd " + sh_quote(self.lintree) + "\n")
            for step in steps:
                s.write(f"echo {sh_quote('$ ' + step)}\n{step}\n")
        with self.log(job) as f:
            r = subprocess.run(["wsl", "-e", "bash", "-l", wsl_path(script)], stdout=f,
                               stderr=subprocess.STDOUT, creationflags=NOWIN)
            if r.returncode != 0:
                f.write(f"exit {r.returncode}\n")
        return job, "PASS" if r.returncode == 0 else "FAIL", time.time() - t0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--commit", default="HEAD")
    ap.add_argument("--jobs", help="comma-separated substrings of job names to run (default all)")
    ap.add_argument("--keep", action="store_true", help="keep the clones")
    ap.add_argument("--force", action="store_true", help="rerun even if a PASS file exists")
    a = ap.parse_args()

    sha = git("rev-parse", a.commit + "^{commit}")
    os.makedirs(OUT, exist_ok=True)
    result = os.path.join(OUT, sha + ".txt")
    if not a.force and not a.jobs and os.path.isfile(result):
        with open(result, encoding="utf-8") as f:
            if f.readline().startswith("PASS"):
                print(f"ci_local: PASS {sha} (cached in {result})")
                return 0

    lin, win = linux_jobs(), windows_jobs()
    if a.jobs:
        keys = a.jobs.split(",")
        pick = lambda d: {k: v for k, v in d.items() if any(s in k for s in keys)}
        lin, win = pick(lin), pick(win)

    tmp = tempfile.mkdtemp(prefix=f"sa1-ci-{sha[:10]}-")
    wintree = os.path.join(tmp, "win")
    lintree = f"/root/sa1-ci/{sha[:12]}"
    logdir = os.path.join(OUT, "logs", sha)
    os.makedirs(logdir, exist_ok=True)
    t0 = time.time()
    print(f"ci_local: {sha} -> {tmp} + WSL {lintree}", flush=True)

    if win:
        subprocess.run(["git", "clone", "--local", "--quiet", "--no-checkout", MAIN, wintree], check=True,
                       creationflags=NOWIN)
        git("checkout", "--quiet", "--detach", sha, cwd=wintree)
    if lin:
        prep = (f"set -e; rm -rf {lintree}; mkdir -p /root/sa1-ci; "
                f"git -c safe.directory='*' clone --quiet --no-checkout {sh_quote(wsl_path(MAIN))} {lintree}; "
                f"git -C {lintree} checkout --quiet --detach {sha}")
        subprocess.run(["wsl", "-u", "root", "-e", "bash", "-c", prep], check=True, creationflags=NOWIN)

    r = Runner(sha, logdir, wintree, lintree, tmp)
    results = []
    with cf.ThreadPoolExecutor(max_workers=len(lin) + len(win) or 1) as ex:
        futs = [ex.submit(r.lin_job, k, v) for k, v in lin.items()]
        futs += [ex.submit(r.win_job, k, v) for k, v in win.items()]
        for fut in cf.as_completed(futs):
            job, status, secs = fut.result()
            results.append((job, status, secs))
            print(f"{status} {job} ({secs:.0f} s)", flush=True)
    if not a.jobs:
        results.append(("psp-docker", "SKIP", 0.0))
        print("SKIP psp-docker (no Docker here; psp job uses the same build_psp.sh)")

    ok = all(s != "FAIL" for _, s, _ in results)
    verdict = "PASS" if ok else "FAIL"
    if a.jobs:
        print(f"ci_local: {verdict} (partial run: {a.jobs}; no result file written)")
    else:
        with open(result, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"{verdict} {sha}\n")
            f.write(f"date {time.strftime('%Y-%m-%d %H:%M:%S')} wall {time.time() - t0:.0f} s\n")
            for job, status, secs in sorted(results):
                f.write(f"{status} {job} {secs:.0f} s\n")
            f.write(f"logs {logdir}\n")
        print(f"ci_local: {verdict} {sha} ({time.time() - t0:.0f} s) -> {result}")

    if a.keep or not ok:
        print(f"ci_local: clones kept: {tmp} and WSL {lintree}")
    else:
        # git's pack files are read-only, which rmtree cannot delete on Windows.
        def writable(fn, p, _):
            os.chmod(p, 0o700)
            fn(p)
        if sys.version_info >= (3, 12):
            shutil.rmtree(tmp, onexc=writable)
        else:
            shutil.rmtree(tmp, onerror=writable)
        subprocess.run(["wsl", "-u", "root", "-e", "rm", "-rf", lintree], creationflags=NOWIN)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
