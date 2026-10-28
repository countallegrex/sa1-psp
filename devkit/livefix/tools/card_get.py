"""card_get.py OUTDIR [ls DIR]... [get MS0PATH]... - copy files OFF the Memory Stick through the running PSPLink
(shell 'cp' to host0:), twice, and compare the two copies.  Runs nothing on the PSP, never resets PSPLink,
never starts or stops usbhostfs_pc."""
import filecmp
import os
import shutil
import sys
import time

REPO = os.path.join(os.environ["SA1_ROOT"], r"sa1-psp")
sys.path.insert(0, os.path.join(REPO, "tools"))
import psp_run  # noqa: E402

out = sys.argv[1]
ops = sys.argv[2:]
os.makedirs(out, exist_ok=True)
stage = os.path.join(REPO, "build-psp", "sa1psp", "psplink-run")
if not psp_run.port_open(psp_run.BASE_PORT):
    sys.exit("card_get: usbhostfs_pc is not running")
log = open(os.path.join(stage, "card_get.log"), "w")
sess = psp_run.Session(log, os.path.join(stage, "usbhostfs_pc.log"))
sess.connect()
if not psp_run.wait_psplink(sess, 30):
    sys.exit("card_get: PSPLink did not answer: %s" % sess.lost)
bad = 0
for i in range(0, len(ops), 2):
    op, arg = ops[i], ops[i + 1]
    if op == "ls":
        r = sess.command("ls", arg, timeout=30)
        print("card_get: ls", arg, "->", r)
        for line in sess.cmd_lines:
            print("   ", line)
        continue
    name = arg.rsplit("/", 1)[-1]
    copies = []
    for k in (1, 2):
        dst = os.path.join(stage, "%s.get%d" % (name, k))
        if os.path.exists(dst):
            os.remove(dst)
        r = sess.command("cp", arg, "host0:/psplink-run/%s.get%d" % (name, k), timeout=120)
        print("card_get: cp", arg, k, "->", r, "|", " ".join(sess.cmd_lines)[:160])
        copies.append(dst)
    time.sleep(1)
    ok = all(os.path.exists(c) for c in copies) and filecmp.cmp(copies[0], copies[1], shallow=False)
    if ok:
        shutil.copy2(copies[0], os.path.join(out, name))
        print("card_get: %s %d bytes, two reads IDENTICAL -> %s" % (name, os.path.getsize(copies[0]), out))
    else:
        bad += 1
        print("card_get: %s: reads DIFFERENT or missing" % name)
sys.exit(1 if bad else 0)
