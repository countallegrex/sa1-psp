"""D2 census over the survey's 44 places (o1 pokes), FRAMES each, 6 at a time; silent.
No census_NAME.txt = no static-TRS node reached the stack-angle rotate."""
import os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
W = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(W, "..", "s3"))
from survey import PLACES  # noqa: E402
D = os.path.join(os.environ["SA1_ROOT"], r"private/dump/sa1_us_1005/data")
EXE = os.path.join(W, "bd", "backend", "desktop", "sa1psp.exe")
frames = int(sys.argv[1]) if len(sys.argv) > 1 else 1200
L = os.path.join(W, "plogs"); os.makedirs(L, exist_ok=True)


def run(p):
    name, spec, gm = p
    lv, act = (int(x) for x in spec.split(":"))
    pk = []
    for a, v in ((0x8C7493BA, lv), (0x8C7493BC, act), (0x8C7493BE, lv), (0x8C7493C0, act), (0x8C760970, gm)):
        pk += ["--poke", "12:0x%08X:%d:2" % (a, v)]
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", D2_OUT=os.path.join(L, "census_%s.txt" % name))
    try:
        subprocess.run([EXE, D + "/1ST_READ.BIN", D, "--log", os.path.join(L, name + ".log"), "--no-audio", "--hidden",
                        "--headless", "--no-vmu", "--frames", str(frames)] + pk,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600, env=env)
    except subprocess.TimeoutExpired:
        pass
    c = os.path.join(L, "census_%s.txt" % name)
    return name, open(c).readline().strip() if os.path.exists(c) else "0 hits"


with ThreadPoolExecutor(6) as ex:
    for name, r in ex.map(run, PLACES):
        print("%-10s %s" % (name, r), flush=True)
