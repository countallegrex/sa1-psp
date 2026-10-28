# Chaos 0 motion walker: spec for a native, exact replacement

Source: `private/d2/emit_l1/game_005.c` (walker at line 8048, chain 5991-6556 and 7460-7841). The target is the **recompiled** body, not the console. The emitted prologues push `pr` as 0, and the walker's static path then reads those slots as data (see 1.1).

Profile (q0b, Chaos 0, PSP): walker 4.9 %, feb8 2.2, ff90 1.9, 0150 1.7, 00a0 1.2, 0612 1.0, sa1_call 1.6. That is about 14 % in game code, plus push 4.0 / copy 2.8 / pop 1.2 / scale 1.7 in math.c.

## 1. Per-function behaviour

Node = NJS_OBJECT: +0 flags, +4 model, +8..+16 pos (float), +20..+28 ang (int), +32..+40 scale (float), +44 child, +48 sibling. Motion work area: 0x8C88FE40 = mdata cursor, FE44 = p[] base, FE48 = nb[] base, FE4C = frame (float), FE50 = nbFrame, FE54 = element index, FE58 = 0. Keys are 16 bytes: int frame + 3 words.

| Fn | In | Guest writes | Calls | c left behind |
|---|---|---|---|---|
| **0784 walker** (r4 = node) | c->r[4], r8..r15 | pushes 8 words; matrix stack via push/pop | per node: `sdk_8c639c70(0)`, motion cb `*FE24`, draw cb `*0x8C890074` if non-NULL, `fn_8c037098(model)` if model ≠ 0 and !(flags&8), recurse(child), `sdk_8c639b98(1)`; loop over the sibling | r8..r14 and r15 = entry values, t = 1, r0 = pop's RET, r4 = 1, xf = popped; everything else = last writer |
| **0612** (r4 = node) | FE40, FE2C/30/34 | 4 pushes; FE44 = FE40, FE48 = FE40+8, FE54 = 0, then FE40 += 16 | `*FE2C`, `*FE30`, `*FE34`, each with r4 = node, via sa1_call | r1 = 0x8C88FE2C before the first call (feb8 overwrites it if any element is keyed), r2 = 0x8C88FE34 (last set), r12..r15 restored |
| **06a0** | same | same, but FE48 = FE40+12 and FE40 += 24 | same | same. (06e0: +16 / +32) |
| **00a0** translate (keyed) | FE44/48/54, node | 5 pushes + 16 local bytes; FE54++ | p = p[idx]; if p == 0, njTranslate(node+8); else feb8, ff2c, njTranslate(sp+4) | r4 = 0, r5, fr4..6 = xyz, r11..r15 restored; t is **not** written |
| **0150** rotate XYZ (keyed) | same | same | p == 0: node+20 angles; else feb8, ff90; then `sdk_8c639cf4(0,ax,ay,az)` | r4 = 0, r5..r7 = angles |
| 00f8 / 01a8 | — | as 00a0 / 0150 | scale node+32 → `sdk_8c63a69c`; alternative rotation → `sdk_8c639ff8` | — |
| 057a / 0588 / 0596 / 05a4 (static) | node | none (FE54 not incremented) | tail call: translate node+8 / rotate node+20 (cf4) / rotate (ff8) / scale node+32 | r3 = sdk address, r4 = 0, fr4..6 or r5..7 |
| **feb8** search (r4 = keys, r5 = nb, r6 = out) | FE4C | 1 push (r14); `*r6 = lo` | — | r0, r1 = ftrc(frame), r2 = 0, r3, r5, r7, fr3, t, fpul |
| **ff2c** float lerp (r4 keys, r5 idx, r6 nb, r7 out) | FE4C | 3 floats, stored from out+8 down to out+0 | — | r0 = k0, r2 = 0x8C88FE4C, r3 = k0.frame, r4 = k1+12, r5 = k0+12, r7 = out, fr3..fr8, t, fpul |
| **ff90** int lerp (+ tail **0000**) | FE4C | ints at out+0, +4 (ff90) and +8 (0000) | — | r0 = k0, r2, r3 = z, r4 = k1, r5 = d, fr2..fr5, t, fpul |

Behaviour details that must survive verbatim:

- **Search.** lo = 0, hi = nb. While (hi − lo − 1) > 0, compared unsigned: mid = (lo+hi)>>1; if ftrc(frame) ≥ key[mid].frame (unsigned) then lo = mid, else hi = mid. nb = 0 loops forever, as in the original.
- **Next key.** k1 = k0 + 16, or key 0 when idx+1 ≥ nb.
- **Weight.** w = frame − float(k0.frame). If d = k1.frame − k0.frame > 0 (signed), then s = FSRRA(float(d)); s = s·s; w = w·s. If d ≤ 0, w stays unscaled. Never compute w/d.
- **Blend.** Per component: a·(1−w) + b·w as separate fmul, fmul, fadd, with 1−w computed as `1.0f − w`. Rotation keys are ints: float(int), the blend, then `sh4_ftrc` (it saturates).
- **Skips.** njRotateXYZ skips each axis whose low 16 bits are 0. A rotation by 0 is not a no-op for −0.

### 1.1 Stack slots read that the same function did not write

| Reader | Slot | Written by |
|---|---|---|
| 00a0 / 00f8 | sp+0 | feb8 (through r6) |
| 00a0 / 00f8 | sp+4..+12 (floats, read with RDF, not RK) | ff2c (through r7) |
| 0150 / 01a8 | sp+0 | feb8 |
| 0150 / 01a8 | sp+4, +8 | ff90 |
| 0150 / 01a8 | sp+12 | fn_8c040000 (ff90's tail) |
| walker, 0x40 path (0c0407d0) | its own saved pr / r8 / r9 slots, read as the three angles | its prologue: **(0, entry r8, entry r9)** |

feb8, ff2c, ff90 and 0000 write into their caller's frame. On the p == 0 paths the values come from the node, not the stack.

Frame layout (S = walker entry r15):

| Range | Contents |
|---|---|
| S−4..S−32 | r14, r13, r12, r11, r10, r9, r8, pr = 0 |
| S−36..S−48 (0612) | node, flags, r12, 0 |
| S−52..S−68 (element fn) | 0x8C88FE40, old FE40, node, r11 (= 0x20), 0 |
| S−84..S−72 | idx, v0, v1, v2 |
| S−88 | feb8's push of 0x8C88FE40 |

Each element callback reuses the same addresses, so the last keyed one wins. A child walker sits at S−32 and below.

The "r12" pushed at S−44 and stored to c before each motion callback has two values. On the first node of an activation it is the walker's **entry** r12. On every later node it is the draw-cb word read in the previous iteration. The emitted code does not reload r12 after the motion callback; it reloads it only after 0x8C890074, after fn_8c037098 and after the recursion.

## 2. Function-pointer globals

| Global | Set by (fn_8c03fe60, called by 0942/09c0 just before the walk) | Targets |
|---|---|---|
| 0x8C88FE24 motion cb | table 0x8C19ACE0[halfword 0x8C749100]; that halfword is motion+10 & 0xFF3F, written by fn_8c03fe1a | 0612 (MDATA2, 96 %), 06a0 (MDATA3), likely 06e0 (MDATA4) |
| 0x8C88FE2C pos | type bit0 ? 00a0 : 057a | |
| 0x8C88FE30 ang | bit1 ? (root flags&0x20 ? 01a8 : 0150) : (0x20 ? 0596 : 0588) | |
| 0x8C88FE34 scale | bit2 ? 00f8 : 05a4 | |
| 0x8C88FE28 | 0x8C19ACF4 table (fn_8c03fdc0); used only by the sister walker fn_8c040880 (flag 0x80), out of scope | |
| 0x8C890074 draw cb | no stores in emitted code (HLE or overlay sets it) | NULL 99 % |

fn_8c03fe1a also writes FE40 = mdata, FE50, FE4C = frame, FE54 = FE58 = 0. With a NULL motion it leaves FE40 **stale** and still returns 0.

**Existing fidelity gap:** `sdk_8c639ff8`, the alternative rotation used by 01a8, 0596 and the 0x40 path with flag 0x20, is only a weak seam stub (`hle/seam_stubs.c`: it logs and sets r0 = 0). The native must call the same symbol. This work does not fix that.

## 3. Register contract

Callers: only fn_8c040942 and fn_8c0409c0. The walker also calls itself; it has no indirect callers apart from its table entry. After the call, both reload r4 (dead) and r15, then tail-call `sdk_8c605e0a`, which reads no arguments. So the callers rely on **r15** only (plus r8..r14 under the ABI).

For full-ctx equality, the exit state is:

- r8..r14 = entry values, r15 = entry, t = 1;
- r0 = RET of the last pop, r4 = 1, xf = the popped matrix;
- r1, r2, r3, r5..r7, fr0..15, fpul, mach/macl = **last writer** over the last node processed.

Worked example (0612 → 00a0 keyed, 0150 keyed, 05a4):

- r0 = rotation k0 pointer;
- r1 = ftrc(frame);
- r2 = 0x8C88FE34;
- r3 = 0x8C63A69C;
- r4 = 0;
- r5..r7 = angles;
- fr2, fr3 from ff90/0000;
- fr4..6 = scale;
- fr7 = z and fr8 = x of the translation;
- t = (rotation d > 0);
- fpul = ftrc(z);
- FE40 += 16, FE44 = old FE40, FE48 = old FE40 + 8, FE54 = 2.

Inside the walk the walker also stores, before each motion callback, c->r[1] = target, r2 = 0x8C88FE24, r3 = flags, r8 = 0x8C639CF4, r9 = 0x8C639C70, r10 = 0x40, r11 = 0x20, r12, r13 = flags, r14 = node. Callees see these values (and push them). Before fn_8c037098, c->r[12] = the draw-cb word (0), not the node.

## 4. Native design

- **Wrap.** Add 8c040784 to `hle_wraps.tsv` and to the `foreach` lists in **both** `backend/desktop/CMakeLists.txt` (line 75) and `backend/psp/CMakeLists.txt` (line 54), then re-emit. New file `hle/game/motion_walk.c`. Its `fn_8c040784` uses the set_scan pattern: a mode cached from `hle_math_mode_for(0x8c040784)`, then `_orig` (0), native (1) or check (2).
- **Gating.** Under `--strict`, `hle_math_mode` is exact, so tapes would never run the native. Follow that convention in v1. Once the harness passes, make native the default in all modes, with `--math-exact 8c040784` as the kill switch (console-identical lever).
- **Locals.** The node walk becomes a host loop plus recursion. Keep r8..r14 entry values, the flags, FE40 and the frame in locals.
- **Host pointers.** Use them only where `hle_in_ram(a)` has been tested once per record: the node (the root may be NULL from 0942), the motion `p` and `nb` arrays, and the key arrays. If the test fails, fall back to RD32/RDF so the null page behaves as before. FE40 can be stale or 0, so guard it too.
- **Callback dispatch.** Read FE24, FE2C/30/34 and 0x8C890074 again for every node. Compare each against the known addresses and run 0612, 06a0 and 06e0 natively, as one routine with (nbOff, stride) = (8,16), (12,24), (16,32). Run 00a0, 0150, 00f8 and the static four natively too. For any other value: flush the whole ctx, store exactly the `c->r[...]` set the emitted call site stores, call `sa1_call`, then reload the shadow from c.
- **Static 0x40 path.** Do it inline: translate node+8, then rotate with (r4 = 0, r5 = 0, r6 = entry r8, r7 = entry r9) through `sa1_call(flags&0x20 ? 0x8C639FF8 : 0x8C639CF4)`, then scale node+32. Do not call `_orig` for a single node: its fresh frame would hold the current constants, not the entry r8/r9. The alternative is a side-effect-free pre-scan of the subtree that sends the whole call to `_orig`.
- **Flushing.** This is the correctness rule. Keep the chain's outputs in a shadow and write the last-writer set (§3) to c before `fn_8c037098`, the draw callback, any fallback, and the exit. These are about 25 stores per node and cost little. The time saved comes from: no `sa1_call` lookups or depth probes, no `HLE_ENTER`, a single host pointer per record, inlined search and lerp, and no guest round trip for idx/xyz.
- **Math.** Move `m_xform`, `m_translate`, `m_scale`, `m_rot_x/y/z` and push/pop into `hle/ninja/math_inl.h`, included by `math.c` and by the walker. Keep:
  - push's early return at depth ≥ max (RET 0, no copy);
  - both copies through `nj_copy` (the VFPU path is bit-identical);
  - pop's depth check;
  - the RET values in c->r[0].

  The matrix stack in guest RAM must be written exactly as before; it is live RAM, not dead stack.
- **Dead stack.** Mirror the §1.1 frame writes (about 40 host stores per node at the fixed `RAMK` bias). Then tape `--dump` byte-compares and the desktop-vs-PSP test work unchanged, and later uninitialised reads stay identical. Put this behind a flag so it can be dropped once the tapes prove nothing reads it.
- **Float hazards.** Build with `-ffp-contract=off` (sa1_hle has it). Use no `-ffast-math`, no double literals and no reassociation. Keep FSRRA as `1.0f/sqrtf(x)` followed by squaring, and FTRC through `sh4_ftrc`. Keep the fmul/fadd order and the `1.0f*m12` term in translate. This cluster has no `FMAC_F`, so `SH4_FMAC_FUSED` does not matter. x86-64 SSE and the Allegrex FPU have no excess precision.
- **Read order.** Read flags before the push. Read the model after the draw callback, the child after `fn_8c037098`, and the sibling after the pop: callbacks can change the node.

## 5. Desk harness

What exists today:

- desktop save states (`backend/desktop/savestate.c`; `--save N:f` / `--load`);
- `--dump FRAME:FILE`;
- `hle_state_writer/reader` and the per-module `kamui_state` (`hle/state.h`);
- the set_scan / sphere_sweep check-mode skeleton (`ss_check`).

There is no generic whole-RAM A/B harness.

1. **Chain micro-check, PSP and desktop (mode 2).** For each node, snapshot c, FE40..FE5B and xf. Run the orig through `sa1_call(*FE24)` and capture the result. Restore, run the native chain, then compare all of c (r, fr, xf, t, fpul) and FE40..FE5B; stack below r15 is excluded unless mirroring is on. Adopt the orig result. Log like `ss_summary`. This costs a few hundred bytes per node.
2. **Whole-walker check, desktop.** Load a Chaos 0 state (`sonic_to_ec1_start --save 4600:c0.state`). On each sampled call:
   - memcpy `SA1_RAM_ALLOC` into A, plus c, plus the `kamui_state` blob;
   - run `_orig` and capture B, cB and the kamui blob;
   - restore all three, run native;
   - memcmp RAM against B, excluding [entry r15 − 64 KB, entry r15) unless mirrored, and memcmp c and the kamui blob.

   Do not snapshot task state: `hle_task_state` involves the coroutines.
3. **End to end.** Replay `sonic_to_ec1_start` / `sonic_ec1` with `--dump` at about 2000/4000/8000/11000 (the fight). Note that `--math native` is not specific to the walker: it also switches sphere_sweep, poly_ray and set_scan, which are only "within a band". So compare a plain strict replay (baseline) against `--math native --math-exact 8c029b00,8c029400,8c091380`, which pins every other gated routine to exact. The dumps must be byte-identical, which needs dead-stack mirroring. Also run `tools/replay_test.py`. The same caveat applies to tier 2 run under `--math check`.

## 6. Size and risks

About **550 lines**:

| Part | Lines |
|---|---|
| Walker plus static path | 130 |
| Generic motion chain and element fns | 150 |
| Search and lerps | 70 |
| Shadow/flush and stack mirror | 80 |
| Mode switch and check mode | 120 |

Plus about 40 lines to move the math.c helpers into a header.

Risks:

- **Last-writer bookkeeping** across varying targets and NULL `p` paths: 00a0 leaves t untouched, the static callbacks leave FE54 untouched, and a NULL `p` leaves r0 to r3 alone. This is the main source of ctx mismatches, and tier 1 catches it.
- **Unknown callback targets** in other stages. The fallback must copy each call site's write set exactly.
- **The `sdk_8c639ff8` stub**: an existing gap, kept as it is.
- **Stale FE40 / NULL root**: keep the guarded access.
- **Host recursion depth** grows with skeleton depth (shallow; the probe that `sa1_call` used to run is lost, so add `sa1_depth_probe()` per node).
- **Gating under `--strict`** (see §4).
