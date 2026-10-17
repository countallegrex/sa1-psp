#!/usr/bin/env python3
"""CI test of the replay harness on synthetic data (no game data).

  run_synthetic.py PATH/TO/tape_format_test

tape_format_test (tape_format_test.c + backend/desktop/input_tape.c) records
a synthetic run; this checks the tape header and its round trip through
tools/tape_io.py, builds a check file with tools/tape_expect.py from the
run's watch trace, replays the tape against it (pass), against altered
check files (primary fails, secondary only drifts), with an unreachable
--expect-frame and with another title (refused), and runs
tools/replay_test.py without SA1_DUMP (skips).

Part of sa1-psp (MIT).
"""
import hashlib
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import tape_expect  # noqa: E402
import tape_io  # noqa: E402

WATCH = os.path.join(ROOT, "tests", "replay", "watch.txt")
failures = []


def expect(what, cond, detail=""):
    print("%-58s %s" % (what, "ok" if cond else "FAIL " + detail))
    if not cond:
        failures.append(what)


def run(*args):
    p = subprocess.run([str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def main():
    exe = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="sa1-tape-ci-")
    j = lambda name: os.path.join(tmp, name)  # noqa: E731
    with open(j("title.bin"), "wb") as f:
        f.write(b"abc")
    with open(j("other.bin"), "wb") as f:
        f.write(b"abd")

    code, out = run(exe, "record", j("title.bin"), "--record", j("t.tape"), "--epoch", "1000000000",
                    "--watch-list", WATCH, "--watch-trace", j("trace.tsv"))
    expect("record exits 0", code == 0, out)
    t = tape_io.read(j("t.tape"))
    expect("header: title SHA-256", t.header.get("title-sha256") == hashlib.sha256(b"abc").hexdigest(), str(t.header))
    expect("header: epoch", t.header.get("epoch") == "1000000000", str(t.header))
    expect("header: tool version", t.header.get("tool", "").startswith("sa1psp input_tape"), str(t.header))
    expect("tape covers 1000 frames", t.end == 1000, str(t.end))
    expect("Start pressed on frames 10-11 is in the tape", t.pad(10)[0] & 8 and t.pad(11)[0] & 8)
    raw = open(j("t.tape"), encoding="utf-8").read()
    expect("tools/tape_io.py re-encodes the tape byte for byte", tape_io.encode(t) == raw)
    expect("tape is LF-only", "\r" not in open(j("t.tape"), "rb").read().decode())

    tape_expect.main(["--trace", j("trace.tsv"), "--watch", WATCH, "-o", j("good.check")])
    check = open(j("good.check"), encoding="utf-8").read()
    prim = check.split("[secondary]")[0]
    expect("milestones: game mode 1, 11, 5", all(" 8c760970 2 %d - " % v in prim for v in (1, 11, 5)), prim)
    expect("milestones: 2-frame transient (mode 8) ignored", " 8c760970 2 8 - " not in prim, prim)
    expect("milestones: level 26 reached around frame 400", "8c7493ba 2 26 - level@400" in prim, prim)
    expect("secondary rows sampled", "[secondary]" in check and "player_x" in check)

    def replay(*extra, title="title.bin"):
        return run(exe, "replay", j(title), "--replay", j("t.tape"), *extra)

    code, out = replay("--check", j("good.check"), "--expect-frame", "1000")
    expect("replay with its check file passes", code == 0 and "check: PASS" in out and "0 mismatched" in out, out)
    expect("replay uses the tape's epoch", "epoch 1000000000" in out, out)

    with open(j("bad.check"), "w") as f:
        f.write(check.replace(" 8c7493ba 2 26 - ", " 8c7493ba 2 27 - "))
    code, out = replay("--check", j("bad.check"))
    expect("a missed milestone fails (exit 1)", code == 1 and "check: FAIL level@400" in out, out)

    with open(j("drift.check"), "w") as f:
        f.write(check + "600 8c6bad18 f 99 0.1 player_z_wrong\n")     # the file ends in [secondary]
    code, out = replay("--check", j("drift.check"))
    expect("a secondary mismatch only drifts (exit 0)", code == 0 and "check: DRIFT" in out and "check: PASS" in out, out)

    code, out = replay("--check", j("good.check"), "--expect-frame", "1001")
    expect("an unreachable --expect-frame fails", code == 1 and "expected to reach 1001" in out, out)

    code, out = replay(title="other.bin")
    expect("a tape from another title is refused (exit 2)", code == 2 and "another 1ST_READ.BIN" in out, out)

    with open(j("manual.check"), "w") as f:
        f.write("<=500 8c760970 2 11 game_mode_title\n150 8c752be4 s2 3\n200..300 8c6bad14 f -250 0.01\n")
    code, out = replay("--check", j("manual.check"))
    expect("hand-written rows: <=N, exact, M..N with float", code == 0 and "0/3 primary failed" in out, out)

    import tape_diff
    rc = tape_diff.main(["gpad", j("t.tape"), j("trace.tsv"), "-o", j("gpad.txt")])
    gpad = open(j("gpad.txt"), encoding="utf-8").read()
    expect("tape_diff gpad: sync lines and one pad per game frame",
           rc == 0 and "sync 11 50" in gpad and "\n500 " in gpad and "\n501 " not in gpad, gpad[:300])
    expect("tape_diff diff: a trace against itself does not diverge",
           tape_diff.main(["diff", j("trace.tsv"), j("trace.tsv"), "--watch", WATCH]) == 0)
    lines = open(j("trace.tsv"), encoding="utf-8").read().split("\n")
    hdr = lines[0].split("\t")
    shifted = [lines[0]]                    # rings one game frame late from frame 300 on, x off from 700
    for line in lines[1:]:
        if not line:
            continue
        c = line.split("\t")
        fr = int(c[0])
        if fr >= 300:
            c[hdr.index("rings")] = str(((fr - 2) // 50) % 7)
        if fr >= 700:
            c[hdr.index("player_x")] = str(float(c[hdr.index("player_x")]) + 100)
        shifted.append("\t".join(c))
    with open(j("other.tsv"), "w", encoding="utf-8") as f:
        f.write("\n".join(shifted) + "\n")
    rc = tape_diff.main(["diff", j("trace.tsv"), j("other.tsv"), "--watch", WATCH])
    expect("tape_diff diff: a 1-frame slip is not a divergence, a real change is", rc == 1)
    rc = tape_diff.main(["diff", j("trace.tsv"), j("other.tsv"), "--watch", WATCH, "--slip", "0", "--context", "0"])
    expect("tape_diff diff --slip 0: the slip diverges", rc == 1)

    env = dict(os.environ)
    env.pop("SA1_DUMP", None)
    p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "replay_test.py")], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    expect("replay_test.py skips without SA1_DUMP", p.returncode == 0 and p.stdout.startswith("skip"), p.stdout)

    for name in sorted(os.listdir(os.path.join(ROOT, "tests", "replay"))):
        if name.endswith(".tape"):
            path = os.path.join(ROOT, "tests", "replay", name)
            ct = tape_io.read(path)
            expect("committed %s parses and re-encodes" % name, tape_io.encode(ct) == open(path, encoding="utf-8").read())

    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
