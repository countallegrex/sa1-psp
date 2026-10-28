"""card_rm.py MS0PATH... - remove files on the Memory Stick through the running PSPLink shell, then list
their folder.  No reset, runs nothing, never touches usbhostfs_pc."""
import os
import sys

REPO = os.path.join(os.environ["SA1_ROOT"], r"sa1-psp")
sys.path.insert(0, os.path.join(REPO, "tools"))
import psp_run  # noqa: E402

stage = os.path.join(REPO, "build-psp", "sa1psp", "psplink-run")
sess = psp_run.Session(open(os.path.join(stage, "card_rm.log"), "w"), os.path.join(stage, "usbhostfs_pc.log"))
sess.connect()
if not psp_run.wait_psplink(sess, 20):
    sys.exit("card_rm: PSPLink did not answer")
for p in sys.argv[1:]:
    print("card_rm: rm", p, "->", sess.command("rm", p, timeout=30))
d = sys.argv[1].rsplit("/", 1)[0]
sess.command("ls", d, timeout=30)
for line in sess.cmd_lines:
    print("   ", line)
