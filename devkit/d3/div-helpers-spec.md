# SHC division helpers: spec for a native, exact replacement (estimate only)

Source: `private/d2/emit_l1/game_030.c`:
- fn_8c10ca5c: signed 32-bit divide.
- fn_8c10cb10: unsigned 32-bit divide.
- Siblings for v2: fn_8c10cbb8 (signed remainder), fn_8c10cc80 (unsigned remainder), and fn_8c1109bc in game_031.c (16-bit signed divide).

As with the walker, the target is the **recompiled** body (the emitted DIV1/ROTCL/ADDC macros in sh4_rt.h), not the console.

Profile (q0b, PSP):

| Function | WV2 | SD3 | Chaos 0 | Casino 2 |
|---|---|---|---|---|
| 10ca5c | 3.18 % (1.25 ms of L 39.4) | 0.02 % | 0.13 % | 0.12 % |
| 10cb10 | 0.02 % | 0.02 % | 0 | 0 |

The likely WV2 driver is the particle task fn_8c05fed0. It makes three calls per run (60 / k, 0xC000 / (60 / k), 60 / k), with k = `*0x8C1BA1E8`.

There are 387 direct call sites of 10ca5c (4 of them in stg02) and 23 of 10cb10. Neither is tail-called. Both are in game_table.c, so `sa1_call` reaches them too.

## 1. Per-function behaviour

The divisor is in r0 and the dividend in r1. No function reads RAM. None calls anything.

| Fn | In | Guest writes | c left behind (d ≠ 0) | c left behind (d = 0) |
|---|---|---|---|---|
| **10ca5c** signed | r0 = d, r1 = n, r2, r3, r15 | WK32 [r15-4] = r2, [r15-8] = r3; on d = 0 only [r15-4] = r2, plus WR32 0x8C7AC108 = 0x44E | r0 = r1 = Q; r2, r3, r15 = entry; t, q, m (§1.1) | r0 = 0, **r1 = 0x8C7AC108**, r2 = entry, r15 = entry, t = 1; r3, q, m **not written** |
| **10cb10** unsigned | r0 = d, r1 = n, r2, r15 | WK32 [r15-4] = r2; on d = 0 also WR32 0x8C7AC108 = 0x44E | r0 = r1 = Q; r2, r15 = entry; t = 0, m = 0, q = 1 - (Q & 1) | r0 = 0, **r1 = 0x44E**, r2 = entry, r15 = entry, t = 1; q, m not written |
| 10cbb8 signed % (v2) | r0, r1, r2, r3, r4, r15 | 3 pushes (r2, r3, r4) | r0 = remainder, r1 = raw quotient bits, r2/r3/r4 = entry, t, q, m | as 10ca5c, r1 = 0x8C7AC108 |
| 10cc80 unsigned % (v2) | r0, r1, r3, r4, r15 | 2 pushes (r3, r4); on d = 0 only r2 is pushed | r0 = remainder, r1 = quotient bits, r3/r4 = entry, t, q, m | r1 = 0x8C7AC108, r2 = entry |
| 1109bc 16-bit / (v2) | low halves of r0, r1 | 1 push (r2) | r0 = exts.w result, r1 = partial | **r0 = the exts.w divisor (0)**, r1 = 0x44E |

Notes:
- **Entry flags.** Entry t, q and m are dead on d ≠ 0, because div0s/div0u overwrite them.
- **pr.** pr is never stored to c (it is a local in the emitted body). The native must not write c->pr either.
- **The d = 0 paths differ between siblings** (r1 = address in one, the value in another; which registers are pushed). Copy each one exactly from its emitted body. Do not share one zero path between them.
- **Read and write sets.** The native's read set and write set must equal the emitted body's, path by path. Call sites sync only what the callee summary (summaries.json) says the body reads and writes; r15 is stored before only 193 of the 387 calls, for example.
  - Reading a register the original does not read (r4, say) would see a stale c value.
  - Writing one it does not write (r3, q or m on the d = 0 path) breaks the harness and can overwrite a live value the caller did not sync.
- **Callers.** Within 12 lines after the call, the 387 sites reload r0 (all), r15 (354), r3 (192), r1 (112), t (96) and r2 (79). So exact t and r1 matter. q and m are read by no code outside a div0/div1 chain, but full-ctx equality still requires them.

### 1.1 Behaviour that must survive verbatim (candidate closed forms, fixed by the oracle in §5)

**10cb10** (div0u sets q = m = t = 0):
- r1 is rotated 33 times. The first rotate inserts the entry t = 0, and that same bit leaves at the 33rd rotate. So the final **t = 0**.
- Each DIV1 leaves t = the next quotient digit, with t = (q == m). So the final **q = 1 - (Q & 1)** and **m = 0**.
- Q = n / d, unsigned.

**10ca5c**:
- **sd** = (n >> 31) ^ (d >> 31). It is the t produced by the second div0s, and it is the bit that the 33rd rotate pushes back out. **m** = d >> 31.
- **raw** = Q - sd (mod 2^32): the r1 value before the closing addc.
- **t** = the carry of raw + sd, i.e. (sd && Q == 0).
- **q** = (raw & 1) ? m : !m.
- Expected Q is C truncation, computed on magnitudes in uint32 (|n| / |d|, then the sign applied). This needs no signed host `/`, so INT_MIN / -1 is neither undefined nor a trap, and it gives 0x80000000.
- Whether the bit-serial result matches for INT_MIN, for d = INT_MIN and for all mixed-sign pairs is for the oracle to decide. **Any class that differs goes to `_orig`.**

**Desk pre-check done.** `d3/divsim.py` is a Python model of the sh4_rt.h macros (DIV0S/DIV0U/SUBC/DIV1/ROTCL/ADDC) run on the emitted step sequence. Over 371 K pairs, the closed forms above match it in r0/r1, t, q and m for both functions, with **0 mismatches**. The pairs:
- every edge-value pair (0, ±1, INT_MIN, INT_MAX, 2^k, 2^k - 1, -2^k, 60, 0xC000), INT_MIN / -1 included;
- all n, d in [-200, 200];
- 200 K random pairs.

This only checks the model of the macros, not the compiled emitted C, so the §5 oracle is still required. But no `_orig` fallback class is expected.

Other rules:
- **No float, no FMAC, no rounding questions.** The only arithmetic is 32-bit integer.
- **Dead stack.** The pushes land below the returned r15 and are never read across functions. Mirror them anyway, which is 1-2 host stores at the fixed RAMK bias. Then `--dump` and the desktop-vs-PSP compare stay byte-identical, and later uninitialised reads stay identical. Unlike the walker, no flag is needed: it costs ~2 cycles.
- **Zero path.** On d = 0, the write to 0x8C7AC108 (the SHC library's error word; 0x44E) goes through the same WR32 (constant address).

## 2. Globals

| Global | Access | Notes |
|---|---|---|
| 0x8C7AC108 | write 0x44E on d = 0 only | the run-time library's error word. The emitted main image and overlays only ever write it (from 10b9c2, the 10d0xx library functions and this family); no literal-address read exists. Keep the write exact anyway |

There are no function pointers, no callbacks and no stack slots read across functions.

## 3. Register contract

Shown for 10ca5c with d ≠ 0. The full exit state for full-ctx equality is:
- r0 = r1 = Q;
- r2, r3, r15 = entry values;
- t, q, m as in §1.1;
- everything else (r4..r14, fr*, xf*, fpul, mach/macl, gbr, pr) untouched.

The d = 0 rows in §1 are the other exit. No caller reads anything else.

## 4. Native design

- **Wrap.** Add 8c10ca5c and 8c10cb10 to `recomp/sa1_us_1005/hle_wraps.tsv`. Add both to the `foreach` lists in **both** `backend/desktop/CMakeLists.txt` and `backend/psp/CMakeLists.txt`, then re-emit. The image changes only by the `_orig` rename.
  - Overlays call `fn_8c10ca5c` by symbol (4 stg02 sites), so they get the native without an overlay re-emit. Confirm that the overlay link uses the main symbols.
  - New file `hle/game/sh_div.c` (MIT, our own code).
- **Mode.** Use the set_scan pattern: a mode cached from `hle_math_mode_for(0x8c10ca5c)`, then `_orig` (0), native (1) or check (2). As for the walker, make native the default in every mode, including `--strict`, with `--math-exact 8c10ca5c,8c10cb10` as the kill switch (a console-identical lever). This needs `hle_math_listed` / `hle_math_mode_exact_ok` from d2-walker a686bed (not landed): either land the walker first or port those two helpers.
- **Body** (10ca5c, d ≠ 0):
  1. Load r0, r1, r2, r3 and r15 from c. Store the two dead-stack words.
  2. Take un = |n| and ud = |d| as uint32. Q = un / ud, negated when sd.
  3. Derive t and q (§1.1), and m = d >> 31.
  4. Store r0, r1, r2, r3, r15, t, q and m.

  That is ~70-80 cycles on the Allegrex (`div` is ~36 of them), against ~550-650 for the emitted body. Classes the oracle does not prove go to `fn_8c10ca5c_orig(c)`, which is exact by construction.
- **Build.** The `teq` divide-by-zero trap that GCC adds after `div` is harmless (d = 0 branches first). Do not add `-mno-check-zero-division` globally. No floats are involved, so the `-ffp-contract` flags do not matter.
- **Not in v1:** inlining at the call sites (the caller-side c syncs stay); the emitter idiom pass for rotcl/div1 chains (the general route, which would also cover overlays and the siblings).

## 5. Desk harness

This is a **pure function** of (r0, r1, r2, r3, r15). It reads no RAM, reads no globals, calls nothing, and writes only 2 dead-stack words and one constant-address word. So a savestate A/B adds nothing: an oracle over the input space covers every state the game can reach.

1. **Oracle, desktop (new test, e.g. `tests/sh_div_oracle.c`).** Link `fn_8c10ca5c_orig` / `fn_8c10cb10_orig` and the natives against a test `sa1_ram`.
   - Per case: fill c at random, including the entry t, q and m (this proves they are dead), and set r15 to a P1 stack address. Run `_orig` and native on identical copies. memcmp the whole `sh4_ctx`, the 8 bytes below r15 and the word at 0x8C7AC108.
   - Input sets:
     - (a) **exhaustive** sign-extended 16-bit x 16-bit (2^32 pairs; ~100 ns per `_orig`, so a few minutes over 8 threads);
     - (b) every d in [-4096, 4096] x 64 K stratified n (0, ±1, INT_MIN, INT_MAX, 2^k ± 1, random);
     - (c) every 2^k ± {0, 1} divisor against random n;
     - (d) 10^9 random 32-bit pairs;
     - (e) d = 0 with random n, d = -1 and d = INT_MIN.
   - Report any differing class. Each one goes to `_orig` in the native, and the oracle is re-run until 0 differences.
2. **Check mode in situ (mode 2, desktop).** Per call: snapshot c, the 8 stack bytes and the error word. Run `_orig` and capture the result. Restore, run native, compare, then adopt the `_orig` result. Count calls per turn, which confirms the WV2 driver and the ~700 calls per turn.
   - Run it on the 44 places (`places.py`, 3000 frames, including wv2 = `--stage 2:1:4` and sd3 = `--stage 6:2:4`) and on the 4 tapes.
   - Expect 0 differing words. The tapes do not contain WV2, so the places run is the live coverage.
3. **End to end.**
   - 4 tapes byte-identical in exact, native and check modes.
   - 44 places identical (`places_cmp.py`).
   - `--dump` on a WV2 run at frames 1000/3000/6000: native against the same exe with `--math-exact 8c10ca5c,8c10cb10`, byte-identical including dead stack. Pin the other gated routines exact, as in the walker spec §5.3.
   - Local CI.
4. **PSP gate (after the maintainer's play-test).** One PRX, WV2 L ABBA x3 against the same PRX with the kill switch. Expect -0.9..-1.2 ms. SD3 is the null control (expect ~0).

## 6. Size and risks

About **120 lines**:

| Part | Lines |
|---|---|
| 2 natives | 45 |
| Mode, check mode and counters | 50 |
| Wiring | 5 |
| Oracle test (separate) | ~120 |

Each v2 sibling adds ~25 lines and the same oracle sets.

Risks:
- **Closed form for t/q** on mixed-sign and INT_MIN classes. The oracle decides, and `_orig` is the fallback, so the risk is only to the gain, never to exactness.
- **Host divide edge cases:** INT_MIN / -1 (avoided by the unsigned-magnitude form), d = 0 (branched first).
- **Kill switch under `--strict`.** It depends on the walker's settings helpers (§4).
- **Gain is WV2-only** (~1 ms). Elsewhere it is ≤ 0.04 ms. If the PSP gate shows less than 0.5 ms, the cause is likely call-site sync in the callers (unchanged here). The emitter idiom pass or call-site inlining is then the next step.
- **Code layout.** The walker exe was 1-4 % slower on the desktop even with the walker set exact. The kill-switch arm of the same PRX controls for that in the PSP gate.
