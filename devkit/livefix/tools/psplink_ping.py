"""psplink_ping.py - one cheap read-only check: is usbhostfs_pc up, and does PSPLink answer?
No reset, no game start, never stops usbhostfs_pc.  Exit 0 = answers."""
import os
import sys

REPO = os.path.join(os.environ["SA1_ROOT"], r"sa1-psp")
sys.path.insert(0, os.path.join(REPO, "tools"))
import psp_run  # noqa: E402

if not psp_run.port_open(psp_run.BASE_PORT):
    print("ping: usbhostfs_pc is not running")
    sys.exit(2)
stage = os.path.join(REPO, "build-psp", "sa1psp", "psplink-run")
sess = psp_run.Session(open(os.path.join(stage, "ping.log"), "w"), os.path.join(stage, "usbhostfs_pc.log"))
sess.connect()
ok = psp_run.wait_psplink(sess, 6)
print("ping: PSPLink %s" % ("answers" if ok else "does not answer (%s)" % sess.lost))
sys.exit(0 if ok else 1)
