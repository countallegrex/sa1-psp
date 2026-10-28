"""Emit every overlay with a given emitter copy.  python emit_ovl.py EMITTER_DIR MAIN_TABLE OUT_DIR"""
import glob, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
REPO = os.path.join(os.environ["SA1_ROOT"], r"sa1-psp")
em, table, out = sys.argv[1:4]


def one(cfg):
    name = os.path.splitext(os.path.basename(cfg))[0]
    fn = glob.glob(os.path.join(REPO, "recomp", "out", "overlays", "dream", name, "*.functions.json"))[0]
    r = subprocess.run([sys.executable, os.path.join(em, "emit_overlay.py"), cfg, "--functions", fn, "--main-table", table,
                        "--seam", os.path.join(REPO, "recomp", "sa1_us_1005", "sdk_entry_points.tsv"),
                        "--out", os.path.join(out, name)], capture_output=True, text=True)
    return name, r.returncode, (r.stdout + r.stderr)[-300:] if r.returncode else ""


with ThreadPoolExecutor(8) as ex:
    for name, rc, msg in ex.map(one, sorted(glob.glob(os.path.join(REPO, "recomp", "sa1_us_1005", "overlays", "*.toml")))):
        print(name, rc, msg, flush=True)
