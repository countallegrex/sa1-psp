import os
import re, json, sys
P = os.path.join(os.environ["SA1_ROOT"], r"private\q0b\prof_%s_all.txt")
cg = json.load(open("cg.json"))
def prof(s):
    d = {}; tot = None
    for l in open(P % s, encoding="utf-8", errors="replace"):
        m = re.match(r"\s+([\d.]+) %\s+(\d+)\s+(\S+)\s+\[", l)
        if m and m.group(3) not in d: d[m.group(3)] = (float(m.group(1)), int(m.group(2)))
    return d
sd, wv = prof("sd3"), prof("wv2")
callers = {}
for f, d in cg.items():
    for k in d['calls']: callers.setdefault(k, set()).add(f)
names = set(k for k in list(sd)+list(wv) if (k.startswith("fn_") or k.startswith("ovl_")))
rows = []
for n in names:
    a = sd.get(n, (0,0))[0]; b = wv.get(n, (0,0))[0]
    rows.append((a+b, n, a, b))
rows.sort(reverse=True)
for s, n, a, b in rows[:int(sys.argv[1])]:
    d = cg.get(n) or cg.get(n.replace("_orig",""))
    if not d: print(n, a, b, "NOCG"); continue
    cs = [k for k in d['calls'] if k.startswith(("fn_","ovl_"))]
    print(f"{n:22s} sd {a:4.2f} wv {b:4.2f} ins {d['ins']:4d} rd {d['rd']:3d} wr {d['wr']:3d} cld {d['cld']:3d} cst {d['cst']:3d} sa1 {d['sa1']:2d} {d['file']}")
    print("    callers:", " ".join(sorted(callers.get(n.replace('_orig',''), set())))[:300])
    print("    callees:", " ".join(f"{k}x{v}" for k, v in d['calls'].items())[:300])
