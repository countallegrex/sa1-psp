import re, sys, glob, json, os
base = os.path.join(os.environ["SA1_ROOT"], r"private\d2")
files = glob.glob(base + r"\emit_l1\game_0*.c") + glob.glob(base + r"\ovl_l1\stg06\ovl_stg06_0*.c") + glob.glob(base + r"\ovl_l1\stg02\ovl_stg02_0*.c")
fdef = re.compile(r"^void ((?:fn|ovl_stg0[26])_[0-9a-zA-Z_]+)\(sh4_ctx \*c\) \{")
call = re.compile(r"\b((?:fn|sdk|ovl_stg0[26])_[0-9a-f]{8}[a-z_]*)\(c\)")
funcs = {}
cur = None
for f in files:
    for line in open(f, encoding="utf-8", errors="replace"):
        m = fdef.match(line)
        if m:
            cur = m.group(1); funcs[cur] = dict(file=os.path.basename(f), calls={}, ins=0, rd=0, wr=0, cld=0, cst=0, sa1=0, lines=0)
            continue
        if cur is None: continue
        d = funcs[cur]; d['lines'] += 1
        if line.startswith("}"):
            cur = None; continue
        if "/* 0c" in line: d['ins'] += 1
        d['rd'] += len(re.findall(r"\bRD(?:32|16S|16|8S|8|F|D)?\(", line))
        d['wr'] += len(re.findall(r"\bWR(?:32|16|8|F|D)?\(", line))
        d['cld'] += len(re.findall(r"= c->(?:r|fr|xf)\[", line)) + len(re.findall(r"= c->(?:t|fpul|gbr|mach|macl)\b", line))
        d['cst'] += len(re.findall(r"c->(?:r|fr|xf)\[\d+\] =", line)) + len(re.findall(r"c->(?:t|fpul|gbr|mach|macl) =", line))
        d['sa1'] += line.count("sa1_call(")
        for cm in call.finditer(line):
            n = cm.group(1); d['calls'][n] = d['calls'].get(n, 0) + 1
json.dump(funcs, open(sys.argv[1], "w"))
print(len(funcs))
