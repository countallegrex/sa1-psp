"""card_cp.py SRC DST_ON_MS0 [--mkdir] - copy a PC file to the Memory Stick through the running PSPLink (shell 'cp'),
then copy it back and compare.  Never resets PSPLink, never starts or stops usbhostfs_pc."""
import filecmp
import os
import shutil
import sys
import time

REPO = os.path.join(os.environ["SA1_ROOT"], r"sa1-psp")
sys.path.insert(0, os.path.join(REPO, "tools"))
import psp_run  # noqa: E402

src, dst = sys.argv[1], sys.argv[2]
stage = os.path.join(REPO, "build-psp", "sa1psp", "psplink-run")
name = os.path.basename(src)
shutil.copy2(src, os.path.join(stage, name))
back = os.path.join(stage, name + ".back")
if os.path.exists(back):
    os.remove(back)
if not psp_run.port_open(psp_run.BASE_PORT):
    sys.exit("card_cp: usbhostfs_pc is not running (start a psp_run first)")
log = open(os.path.join(stage, "card_cp.log"), "w")
sess = psp_run.Session(log, os.path.join(stage, "usbhostfs_pc.log"))
sess.connect()
if not psp_run.wait_psplink(sess, 30):
    sys.exit("card_cp: PSPLink did not answer: %s" % sess.lost)
if len(sys.argv) > 3 and sys.argv[3] == "--mkdir":
    d = dst.rsplit("/", 1)[0]
    print("card_cp: mkdir", d, "->", sess.command("mkdir", d, timeout=30))
for args in (("cp", "host0:/psplink-run/" + name, dst), ("cp", dst, "host0:/psplink-run/" + name + ".back")):
    r = sess.command(*args, timeout=60)
    print("card_cp:", " ".join(args), "->", r, "|", " ".join(sess.cmd_lines)[:200])
time.sleep(1)
ok = os.path.exists(back) and filecmp.cmp(src, back, shallow=False)
print("card_cp: read back %s" % ("IDENTICAL" if ok else "DIFFERENT or missing"))
sys.exit(0 if ok else 1)
