# Emitter overhead, Chaos 0 motion cluster (d2, 2026-10-28)

Measured on **q0/b_q0** (the ELF the q0b profile came from). The six functions have the same instruction counts in qb/b_base. Caveat: Allegrex samples land on the *consumer* of a stalled load, so some guard and exit samples are really load latency.

## 1. Counts (static instructions / samples)

| fn | instrs | samples | guard (+cold) | call sync | entry | exit | host spills |
|---|---|---|---|---|---|---|---|
| 8c040784 walker | 366 | 332 | 42(+22)/50 | 88/48 | 9/1 | 9/23 | 16/2 |
| 8c03feb8 search | 95 | 152 | 8(+4)/3 | 0 | 7/25 | 22/33 | 0 |
| 8c03ff90 interp | 178 | 127 | 16(+7)/7 | 9/15 | 4/4 | - | 21/0 |
| 8c040150 | 126 | 115 | 12(+6)/5 | 15/21 | 9/6 | 6/3 | 4/2 |
| 8c0400a0 | 147 | 84 | 20(+10)/13 | 18/8 | 8/6 | 6/7 | 4/0 |
| 8c040612 | 83 | 66 | 0 | 16/9 | 5/7 | 6/5 | 0 |
| cluster | 995 | 876 (12.9 %) | 9.8 %/8.9 % | 14.7 %/11.5 % | 4.2 %/5.6 % | 4.9 %/8.1 % | 4.5 %/0.5 % |

**Guard:** on the hot path it runs `lui 0x3e00`, `and`, a branch and `ext`, which is 3-4 instructions per access. GCC **does** share one masked base across consecutive `@(disp,Rn)` accesses (r14 @4/@44/@48 all use `a3`). It loses that shared base after every call that reloads r14. The `fmov @r4+` triples get three separate guards.

**Walker per node** (flag bit 64 clear, which is the hot path): 83 instructions for a leaf node, 98 for a node with a child. The guard is 13/18 of those (~17 %) and call sync is 32/41 (~40 %). Most of the sync is the `sa1_call(*(0x8c88fe24))` site (12 stores, 7 reloads), plus 7 reloads each after `fn_8c037098` and after the recursive call.

**Root cause:** `preserved_pass` (analysis.py:327) only counts a register as preserved if pops are the *only* thing that writes it. SHC pushes r8-r14 precisely so it can overwrite them, so the rule almost never fires. As a result, callers reload r8-r15 after every direct call and callees store them at every exit. In the emitted C, 41k of the 88k `rN = c->r[N]` lines are r8-r15.

**Most samples are load stalls:** about 45 % of walker samples sit just after node or global loads (node[0] 53, 0x8c890074 22+32, node+4 17).

## 2. Levers (exact)

1. **Match push and pop in `preserved_pass`.** A register counts as preserved when it is pushed at entry, popped from the same r15 slot on every return, the net r15 change is 0, and no other store hits that slot. Add r15 once its balance is proven. This makes the same stack assumption that RAMK and the current rule already make. It removes caller reloads and the guard redone on reloaded bases: about 14 reloads plus 8-9 guard instructions per walker node, and it applies program-wide.
2. **A closed-world ABI for `sa1_call`.** If every function reachable through an address (main image plus overlays) passes lever 1, an indirect call can skip the r8-r15 sync. That removes ~14 of the 19 sync instructions at the hot site. Risk: each overlay unit must publish the flag and the runtime must assert it, and one failing function turns the lever off.
3. **Two-target `jsr @r12`.** Here r12 can only be 0x8c639cf4 or 0x8c639ff8, so the emitter can write an `if` with two direct seam calls instead of a full sync. It is cold in Chaos 0. The global-pointer `sa1_call`s cannot be resolved without a histogram of their targets, which we do not have.
4. **Bitfield area test.** `(a>>25)&31` is the same predicate as `a & 0x3E000000` and compiles to one `ext` instead of `lui`+`and` (4→3 hot instructions). Risk: GCC's combine pass may turn it back into the mask, so check the disassembly.
5. **Fold `@r4+` chains.** Emit them as `RDF(r4,0/4/8); r4+=8`. This is exact under the 16 MB-wrap caveat the header already accepts. Walker guard sites go 12→8, and fn_8c0400a0 gains too.
6. **Skip exit stores of preserved registers whose context value never changed** (needs lever 1; 2-3 stores per call).

Rejected:
- **No guard for RAM-loaded pointers.** Not exact: the null page *is* how null pointers behave.
- **Branch-free `movz` select.** It takes 5 or more instructions, against ~3 plus a filled branch today.
- **HLE float keep-out.** The `c->fr` stores before the sdk calls are their arguments.

## 3. Verdict

Overhead that can be attributed to the emitter is ~35-40 % of the cluster's samples, part of it still stall attribution. Levers 1-5 plausibly remove about half of it: ~20 % of the cluster, ~2.5 % of all samples, under 1 ms of a 32 ms logic turn.

A native walker could not remove the cache misses either. Its extra win would be the per-node context round trips and `sa1_call` dispatch (`sdk_8c639c70`, 269 samples, runs once per node). So it is not needed for the general half. Consider one only if this path must lose more than ~20 %.

---
File: <project-dir>\private\d2\emitter-study.md

1. Cluster 876 samples (12.9 %): guard 9 %, call sync 11.5 %, entry+exit ~14 %; ~45 % of walker samples are load stalls.
2. GCC already shares one masked base per @(disp,Rn) run; every call that reloads r14 breaks it, and so do `@r4+` chains.
3. Root cause: `preserved_pass` needs "written only by pops", so r8-r15 are reloaded after calls and stored at every exit.
4. Exact levers: match push and pop for preserved, closed-world sa1_call ABI, two-target jsr, `ext` area test, fold `@r4+`.
5. Verdict: the general route gets ~half the overhead (~20 % of the cluster, <1 ms); a native walker is not needed for that.
