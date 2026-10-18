#!/usr/bin/env python3
"""CI test of the save-state machinery on synthetic data (no game data).

  run_synthetic.py PATH/TO/savestate_test

savestate_test (savestate_test.c with backend/desktop/savestate.c,
platform_desktop.c, coro_switch.c and hle/state.c) runs coroutines whose
state lives in their stack locals, in guest RAM and in a module blob.  This
saves a state in one process, resumes it in another and compares the
checksum at the end with a straight run's; then it checks that a state from
another build (the hash altered), a truncated file and a file with a chunk
renamed are refused, that tools/state_diff.py reads the file, and that the
byte-run packer round-trips.

Part of sa1-psp (MIT).
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import state_diff  # noqa: E402

failures = []


def expect(what, cond, detail=""):
    print("%-58s %s" % (what, "ok" if cond else "FAIL " + detail))
    if not cond:
        failures.append(what)


def run(*args):
    p = subprocess.run([str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def checksum(out):
    m = re.search(r"step (\d+) checksum ([0-9a-f]{8})", out)
    return (int(m.group(1)), m.group(2)) if m else None


def main():
    exe = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="sa1-state-ci-")
    j = lambda name: os.path.join(tmp, name)  # noqa: E731

    code, out = run(exe, "pack")
    expect("byte-run packer round trip", code == 0 and "round trip ok" in out and "rejected" in out, out)

    code, straight = run(exe, "run", 0, 120, j("none.state"))
    expect("straight run to step 120", code == 0 and checksum(straight) is not None, straight)
    code, saved = run(exe, "run", 50, 120, j("s50.state"))
    expect("run with a save at step 50 reaches the same checksum", code == 0 and checksum(saved) == checksum(straight), saved)
    expect("state file written", os.path.isfile(j("s50.state")) and os.path.getsize(j("s50.state")) > 1000)

    code, loaded = run(exe, "load", j("s50.state"), 120)
    expect("a process resumed from step 50 reaches the same checksum", code == 0 and checksum(loaded) == checksum(straight),
           "%s\nvs straight %s" % (loaded.strip(), checksum(straight)))
    code, loaded2 = run(exe, "load", j("s50.state"), 51)
    expect("resumed run stops one step later with another checksum", code == 0 and checksum(loaded2) and
           checksum(loaded2)[0] == 51 and checksum(loaded2)[1] != checksum(straight)[1], loaded2)

    hdr, chunks = state_diff.read_state(j("s50.state"))
    names = [n for n, _ in chunks]
    expect("state_diff reads the file: frame 50, RAM then the module then stacks",
           hdr["frame"] == 50 and names[:4] == ["RAM", "test.step", "test.counter", "test.coros"] and "STACKS" in names and names[-1] == "CURRENT", str(names[:8]))
    expect("five coroutine stacks saved (root + 4 workers)", names.count("stack") == 5, str(names.count("stack")))
    expect("flags and epoch from the meta hook", hdr["flags"] == 1 and hdr["epoch"] == 12345, str(hdr))

    data = bytearray(open(j("s50.state"), "rb").read())
    other = bytearray(data)
    other[16] ^= 0xFF                      # first byte of the executable's hash
    open(j("other-build.state"), "wb").write(other)
    code, out = run(exe, "load", j("other-build.state"), 120)
    expect("a state from another build is refused", code == 5 and "another build" in out, out)

    open(j("short.state"), "wb").write(data[:len(data) - 100])
    code, out = run(exe, "load", j("short.state"), 120)
    expect("a truncated state is refused", code == 5 and "truncated" in out, out)

    body_off = data.find(b"test.step\0")
    renamed = bytearray(data)
    # the body is packed; rename inside the packed stream only works if the
    # name is stored literally, which the byte-run packer does for short runs
    if body_off > 0:
        renamed[body_off:body_off + 9] = b"test.stop"
        open(j("renamed.state"), "wb").write(renamed)
        code, out = run(exe, "load", j("renamed.state"), 120)
        expect("a state whose chunk names differ from this build is refused", code == 5 and "expects test.step" in out, out)
    else:
        expect("chunk name found literally in the packed stream", False)

    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
