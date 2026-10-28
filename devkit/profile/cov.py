import os
import re, sys
from collections import defaultdict
Q = os.path.join(os.environ["SA1_ROOT"], r"private\q0b\prof_%s_all.txt")
S = os.path.join(os.environ.get("SA1_COV_DIR", "."), "%s_all.txt")   # the ss/ec profile listings
places = [("sd3", Q % "sd3"), ("wv2", Q % "wv2"), ("chaos0", Q % "chaos0"), ("casino2", Q % "casino2"),
          ("EC full", S % "ec"), ("SS full", S % "ss")]
LINE = re.compile(r"^\s*([\d.]+) %\s+(\d+)\s+(.+?)\s+\[(.*)\]\s*$")

def group(name, src):
    m = re.match(r"(?:fn|sub)_(8c[0-9a-f]{6})", name)
    if src.startswith("game code"):
        if m and 0x8c03f000 <= int(m.group(1), 16) <= 0x8c0409ff:
            return "walker 8c03f-8c0409"
        return "game fn_8c0"
    if name.startswith("ovl_"): return "overlay ovl_*"
    if "math.c" in src and "ninja" in src: return "ninja math"
    if "sound" in src or "audio_psp" in src or "adx" in src: return "sound"
    if "renderer" in src or "draw.h" in src or "texpool" in src: return "renderer/GE"
    if "sphere_sweep" in src or "poly_ray" in src: return "collision"
    if "sh4_rt" in src or name in ("sa1_off",) or name.startswith("sa1_call"): return "dispatch sa1_call/seam/off"
    if "hle/ninja" in src: return "ninja kamui/TA/model"
    return "kernel/libc/sys"

res = {}
for pl, f in places:
    rows = []
    for ln in open(f, encoding="utf-8", errors="replace"):
        m = LINE.match(ln)
        if m: rows.append((int(m.group(2)), m.group(3), m.group(4)))
    tot = int(re.search(r"(\d+) samples", open(f).readline()).group(1))
    rows.sort(key=lambda r: -r[0])
    print("==", pl, "functions", len(rows), "samples", tot, "listed", sum(r[0] for r in rows))
    cum = 0; marks = {}; top = []
    for i, (n, nm, src) in enumerate(rows, 1):
        before = cum; cum += n
        if before < 0.65 * tot: top.append((n, nm, src))
        for t in (50, 60, 70, 80):
            if t not in marks and cum >= t / 100 * tot: marks[t] = i
    print("  cover", marks, "top65 count", len(top), "share %.1f" % (100 * sum(r[0] for r in top) / tot))
    g = defaultdict(lambda: [0, 0])
    for n, nm, src in top:
        k = group(nm, src); g[k][0] += n; g[k][1] += 1
    for k, (n, c) in sorted(g.items(), key=lambda x: -x[1][0]):
        print("   %-28s %5.1f %%  %3d fns" % (k, 100 * n / tot, c))
    res[pl] = (tot, {nm: n for n, nm, s in rows}, {nm for n, nm, s in top}, {nm: s for n, nm, s in rows})

def common(keys, label):
    c = set.intersection(*[res[k][2] for k in keys])
    print("== common core", label, len(c))
    for nm in sorted(c, key=lambda x: -sum(res[k][1][x] / res[k][0] for k in keys)):
        print("   %-32s %-22s" % (nm, res[keys[0]][3][nm][:22]), " ".join("%.2f" % (100 * res[k][1][nm] / res[k][0]) for k in keys))
    print("   combined:", " ".join("%s %.1f" % (k, 100 * sum(res[k][1][x] for x in c) / res[k][0]) for k in keys))
common(["sd3", "wv2", "chaos0", "casino2"], "L 4 places")
common(["sd3", "wv2", "chaos0", "casino2", "EC full"], "L4 + EC")
