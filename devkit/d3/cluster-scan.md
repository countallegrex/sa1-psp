# D3 cluster scan: next native candidate in SD3 and WV2 (desk only, 2026-10-28)

Sources: q0b PSP profiles (`private/q0b/prof_*_all.txt`, b_q0 6e7242e), emitted C in `private/d2/emit_l1` (lever-1 emit) and `private/d2/ovl_l1/stg06|stg02`. Call graph: `d3/cg.py` parses the emitted C into `d3/cg.json` (`python -I cg.py cg.json`), and `d3/hot.py N` joins it with the profiles (run it from d3).

**Places.** SD3 is `--stage 6:2:4`: Sky Deck act 3 (stg06 overlay), not Speed Highway. The qc commit message calls it "Sky Deck 3" too. WV2 is `--stage 2:1:4` (stg02 overlay).

**ms rule.** ms per turn = sample share x q0b L (SD3 40.5, WV2 39.4). I did not rescale for qa, qf or qc. Those levers cut math and HLE draw work, not these game functions, so the absolute cost per turn is unchanged.

**Excluded.**
- The walker set (fn_8c040784 and its motion chain 0612/06a0/feb8/ff2c/ff90/0000/00a0/0150).
- Code that is already native but profiles under its game name: 02d2e0 (2.64 % SD3), 028d0a, 028ec2, 091380 (set_scan), 0376d0/037350 (ta_stubs).

## 1. Ranked clusters (self samples, % of all PSP samples)

| | Cluster (callers -> callees) | SD3 | WV2 | Chaos 0 | Casino 2 | Kind | Removable | Gain SD3 ms | Gain WV2 ms |
|---|---|---|---|---|---|---|---|---|---|
| a | Model draw entry: 036f12/036bc0 (static tree walker) -> 037098/03700c/037108 -> 03718c (bounding-sphere clip test) | 3.36 | 5.40 | 3.03 | 1.25 | ctx/dispatch, compute leaf | 0.20-0.35 | 0.27-0.48 | 0.43-0.75 |
| d | **SHC integer-division helpers** 10ca5c (+cb10/cbb8/cc80) | 0.12 | **3.28** | 0.13 | 0.25 | algorithmic, compute-bound | **0.85-0.92** | ~0.01 | **0.9-1.2** |
| d' | its WV2 driver 05fed0/06003e/06000c (particle task, 3 divides per call) | 0 | 1.49 | 0 | 0 | ordinary game code | (not counted) | - | - |
| e | Collision primitives 10e6a0 -> 10ef84 -> 10fbb8/10e7b8/10fb94; 10d0a8 (under the native 02d2e0) | 0.79 | 3.87 | 0.27 | 1.21 | ctx/call | 0.20-0.35 | 0.06-0.11 | 0.30-0.53 |
| g | Character mesh: 07a55c -> 056712 -> 056738 (recursive); 055ece/0bdc80 -> 05646c, 056000 | 2.70 | 3.40 | 0.72 | 4.32 | loop compute, low ctx | 0.10-0.20 | 0.11-0.22 | 0.13-0.27 |
| c | Object range test: 0919cc -> 0919e8 -> 3x 09111a (many object callers) | 1.60 | 2.66 | 0.24 | 0.48 | small leaves, full ctx per call | 0.30-0.50 | 0.20-0.33 | 0.31-0.52 |
| f | WV2 drawer: 09c3a0 -> 09c1c0 -> 03700c, sdk matrix | 0 | 2.54 | 0 | 0 | ctx/sdk calls | 0.25-0.40 | - | 0.25-0.40 |
| i | Player collision: 033f00 -> 033a10/0335d4 -> 03357a -> 03339e, 0332a0 | 1.76 | 2.24 | 0.29 | 0.51 | call-heavy (13 callees) | 0.30-0.45 | 0.21-0.32 | 0.26-0.40 |
| b | Character motion walker: 0583a0 -> 057bc0 (recursive) -> 055d4e | 1.90 | 2.03 | 1.73 | 2.19 | walker class | 0.25-0.40 | 0.19-0.31 | 0.20-0.32 |
| h | stg06 overlay: 8c900680 -> 8c901100; 8c90a5de -> 8c90a540 | 1.94 | 0 | 0 | 0 | ctx/sdk calls | 0.25-0.40 | 0.20-0.31 | - |
| j | 02f63c -> 02f5e0 (many callers) | 1.08 | 0.87 | 0.16 | 0.40 | small | 0.20-0.30 | 0.09-0.13 | 0.07-0.10 |

**Shared value.**
- (a), (b), (g), (c) and (i) are hot in both SD3 and WV2.
- (b) is the only cluster at a flat 1.7-2.2 % in all four places.
- (d), (e) and (f) are WV2 levers.

## 2. Overhead counts for the top clusters (emitted C, per call)

| Function | Guest instrs | Guarded RD/WR | ctx loads / stores | Dispatch | Real work |
|---|---|---|---|---|---|
| 03718c clip test | 99 | 24 / 6 | 26 (16 of them xf for FTRV) / 16 per exit | none | FTRV (16 fmul + 12 fadd), ~25 float ops, ~8 loads |
| 037098 draw entry | 40 | 14 / 12 | 15 / 18 | 5 direct calls (3 of them HLE natives) | flag tests, the model fields |
| 0919e8 range test | 70 | 11 / 1 | 12 in, 13 out, + 3x(7 out, 3 in) around 09111a | 3 direct calls | 3 positions, 3 pointer tests |
| 09111a | 18 | 3 / 0 | 5 / 11 | none | 3 fsub, 1 fmul, 2 FMAC, 3 compares |
| 10ca5c divide | 102 | 0 / 0 (1 WR on /0) | 5 / 8 | none | **one 32-bit divide, done as 32 DIV1 + 33 ROTCL** |

**(a).** The ctx share is ~35-45 % of static ops. But 03718c is a compute leaf, and its FTRV and compares stay in a native version. The flush rule also keeps the exit stores, because the HLE natives 037350/0376d0 read c. Hence 0.20-0.35.

**(c).** This one is the most overhead-heavy. Every 09111a call costs a full ctx round trip for 9 float ops. But it is only 0.65 / 1.05 ms in total.

**(d).** The method's ctx counts score it near zero. Its cost is in the emitted algorithm instead:
- **Emitted cost.** On the PSP, `fn_8c10ca5c` is 0xEA4 bytes (937 MIPS instructions; b_q0 disassembly). One DIV1 step is ~14 instructions with two data-dependent branches, about half of them taken to out-of-line paths. Each call runs ~480 instructions, ~550-650 cycles.
- **Native cost.** One host `div` (~36 cycles), a closed form for t/q, and 5 loads + 8 stores of ctx: ~70-80 cycles. That leaves 8-15 %, so 85-92 % is removable.
- **Attribution.** The function makes no RAM loads, so load-stall misattribution does not inflate its share.

## 3. Calibration

- **Desktop only.** The walker's only realised number is from the desktop: -4.6 % in Chaos 0 on a cluster of ~13 % of PSP samples, a ratio of ~0.35. Its PSP PRX (b_psp_walk) is built but not run, so the PSP ratio is unmeasured. I use 0.25-0.40 for walker-class clusters, lower for compute leaves (a, g) and higher for small call-bound leaves (c, i).
- **Not used for (d).** (d) is priced from instruction counts, not from this ratio.

## 4. Choice and gain

**Candidate: the SHC division helpers, fn_8c10ca5c (signed /) and fn_8c10cb10 (unsigned /) in v1.** Spec: `private/d3/div-helpers-spec.md`.

| Place | Share | ms now | Gain (low-high) |
|---|---|---|---|
| WV2 | 3.18 + 0.02 % | 1.26 | **0.9-1.2 ms L** |
| SD3 | 0.04 % | 0.016 | ~0.01 (below 0.5: no SD3 value) |
| Chaos 0 / Casino 2 | 0.13 / 0.12 % | 0.04 | ~0.03-0.04 each |

Assumptions:
- q0b shares hold on the current main (no change has touched these functions).
- Native cost is 8-15 % of emitted.
- Call-site syncs in the callers stay as they are (not counted as gain).
- The WV2 share comes from the q0b spot: a 720-turn stationary run early in act 2. Particle density varies along the act, so the gain is for that spot. The check-mode call counter over the places run will show the spread.
- The WV2 driver is likely fn_8c05fed0, a particle task with three divides by `*0x8C1BA1E8` per call (1.01 % + 06003e 0.38 %). That fits ~250 objects x 3 divides per turn. A call counter in check mode would confirm it.

**Against the 0.5 L bar:**
- Only (d) clears it with confidence, and only in WV2.
- (a) clears it in WV2 only at the top of an unmeasured range (0.43-0.75), and in SD3 not at all (0.27-0.48). Park (a) until the walker's PSP gate gives a real PSP ratio. (a) is also the only cluster with a plausible SD3 gain near 0.5.
- (b), (c), (e), (f), (g), (i), (h) and (j) are each below 0.5 in every place: no.

## 5. Risks of the choice

- **Closed form for t/q.** It must match the bit-serial result for every (dividend, divisor) pair, including INT_MIN / -1 and divide by zero. The pure-function oracle (spec §5) decides. Any input class that differs falls back to `_orig`.
  - A Python model of the macros (`d3/divsim.py`) already agrees on 371 K pairs, with 0 mismatches, edge strata included. That checks the model only, not the compiled emitted C.
- **Host division traps.** INT_MIN / -1 traps on x86 and is undefined on MIPS. Special-case divisor -1.
- **Kill switch.** It must work under `--strict` like the walker (`hle_math_mode_exact_ok` is on the unlanded d2-walker branch).
- **Little value outside WV2.** Do not sell it as a general L lever.

## 6. Side flags (out of scope, one line each)

- **TA_PASS bodies.** `fn_8c052518_orig` / `fn_8c0522c0_orig` are 2.1 % of SD3 (0.85 ms) and 1.5 % of Casino 2. They are TA_PASS in ta_stubs.c, so the full bodies run on skipped turns too. Check whether a qc-style skip applies (a separate lever, not a rewrite).
- **(b) is a second walker.** It has a 16-byte key search, an fdiv/FMAC blend and sdk push/rotate/translate. `math_inl.h` from the walker would make it cheaper, but it is below 0.5 per place. Consider it only as a four-place bundle (~0.2-0.3 each).
- **(c) is an emitter idea.** Inline small leaves such as 09111a (18 instructions behind a full ctx round trip), as an emitter-wide lever.
- **The rest of the division family.** fn_8c10cbb8 (signed %, 240 call sites), fn_8c10cc80 (unsigned %) and fn_8c1109bc (16-bit /) total ≤ 0.1 % anywhere. Add them in v2 with the same wrap and oracle.
- **The general route.** An emitter idiom pass that collapses `rotcl/div1` chains would also cover overlays. Defer it.
