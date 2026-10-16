# recomp/emitter — SH-4 → C emitter

The static recompiler for the game block of `1ST_READ.BIN`. Python 3.10+,
standard library only, so it runs unchanged on Windows, in CI and inside the
launcher. Written from the SH-4 software manual (Renesas REJ09B0318); no
code from any other recompiler. MIT like the rest of the repository.

```
python recomp/emitter/emit_sa1.py \
    --bin ../private/dump/sa1_us_1005/data/1ST_READ.BIN \
    --functions recomp/out/dream/sa1_us_1005.functions.json \
    --out recomp/out/emitter [--update-hle]
python tests/emitter/run_tests.py          # no game data needed
```

Inputs: the user's own binary (never committed), the function list from the
discovery step (`recomp/sa1_us_1005/README.md`, Dream-Recomp run as a
separate process) and the seam table `recomp/sa1_us_1005/sdk_entry_points.tsv`.
Output goes to `recomp/out/emitter/` (git-ignored): `game_NNN.c` chunks,
`game.h`, `game_table.c`, `seam.h`, `seam_stubs.c`, `report.txt`, plus a copy
of `runtime/sh4_rt.[ch]`. `--update-hle` copies `seam.h` and `seam_stubs.c`
to `hle/`; those two contain only addresses and names and are committed.

Measured results are in `docs/m1-emitter-report.md`.

## Layout

| File | Role |
|---|---|
| `sh4emit/isa.py` | the 209 instruction forms: encoding pattern, op id, syntax; builds the 65,536-entry decode table |
| `sh4emit/decode.py` | `Instr`: fields, branch targets, PC-relative addresses, objdump-style text |
| `sh4emit/regs.py` | register numbering, per-instruction use/def sets (depend on FPSCR.SZ/PR), FP bank mapping |
| `sh4emit/cfg.py` | per-function CFG: blocks, delay slots, switch tables, constant propagation, call resolution |
| `sh4emit/analysis.py` | FPSCR state, liveness, dirty sets, call summaries, push/pop idiom |
| `sh4emit/lower.py` | instruction → C statements (the table below) |
| `sh4emit/emit.py` | program driver: entry pruning, summary fixpoint, function/terminator emission, output files |
| `runtime/sh4_rt.h` | the ABI: `sh4_ctx`, RAM macros, arithmetic/FP helper macros |
| `runtime/sh4_rt.c` | function-table dispatch, traps, MMIO and seam-stub logging |
| `emit_sa1.py` | the SA1-specific command line (block ranges, SHA-256 check, seam TSV) |

## ABI

**One C function per SH-4 function**, `void fn_8c0xxxxx(sh4_ctx *c)`, named
by the P1 address of its entry. Between functions the guest register file
lives in `sh4_ctx` (`r[16]`, `fr[16]`, `xf[16]`, `t s q m`, `mach macl pr gbr
fpul fpscr vbr`). Inside a function every register the function touches is a
C local (`uint32_t r0..r15, t, ...; float fr0..fr15, xf0..xf15`). Nothing else
is shared: no cycle counter, no interrupt polling, no resumable functions, no
exceptions.

**Entry:** the function loads from `c` only the registers it may read before
writing (liveness). **Exit (`rts`):** it stores the registers it may have
changed since they were last in sync (the dirty set). **Calls:** before a
direct call it stores `dirty ∩ live_in(callee)`, after it reloads
`live_after ∩ may_write(callee)`. `live_in` and `may_write` are per-function
summaries computed over the whole program to a fixpoint (8 rounds on SA1).
Unknown callees (indirect calls through the function table) use the full
set. Seam (HLE) callees use the SH ABI sets: in `r4–r7, r15, fr4–fr11, gbr`,
out `r0–r7, fr0–fr11, t, mach, macl, fpul`.

**Callee-saved registers.** A function that only writes `r8–r14`/`fr12–fr15`
through pops from `r15` and pushes them somewhere is taken to preserve them
(the SHC prologue/epilogue idiom); callers then neither store nor reload
them around the call. The same ABI trust lets constant propagation keep
`r8–r15` across calls, which is what resolves `jsr @r10` with the pool load
hoisted out of a loop.

**PR.** `jsr`/`bsr` become C calls and `rts` a C return, so PR carries no
value. It is a local initialised to 0 (so the prologue's `sts.l pr,@-r15`
stores a zero; the compiler can use `$zero` on MIPS), set to the return
address after each call only if the function reads it later, and never
stored to or loaded from the context. A routine that returns through a
computed jump to PR would break; SA1's game block has none (see "Non-local
returns").

**Guest RAM** is one flat array `sa1_ram` of 16 MB + 64 KB slack; a guest
address maps to `sa1_ram[addr & 0x00FFFFFF]`, so the P0/P1/P2 spellings
(`0x0C…`, `0x8C…`, `0xAC…`) of the same word coincide. Accesses are
`RD32(base, disp)` etc.: the mask is applied to the base and the
displacement added after, which lets the compiler share one masked base
across `@(disp,Rn)` accesses. This differs from hardware only for an access
whose base and base+disp straddle the 16 MB wrap, which nothing does.
Little-endian, natural alignment (SH-4 faults on misaligned access, so the
game never does it). Loads/stores whose address is a compile-time constant
outside RAM go to `sa1_mmio_rd/wr` (the eight TA-writing functions and the
A-tail RTC/DMA code); non-constant addresses are not checked.

**Literal pools.** `mov.l/mov.w @(disp,pc)` and `mova` fold to constants
unless the word is one some instruction stores to through a constant
address (300 such words in block A); those read memory. The whole binary is
still loaded into guest RAM at boot, so tables read through `mova` work.

**Calls by kind.**

| Site | Emitted |
|---|---|
| `bsr`, `jsr @Rn` / `bsrf Rn` with Rn constant, target in block A | `fn_xxxx(c);` with the summary-driven store/reload |
| … target in the seam table | `sdk_xxxx(c);` (prototype in `seam.h`) |
| … target elsewhere in block B | `sa1_trap` (replaced SDK code that is not an entry point) |
| `jsr`/`bsrf` with unknown register | store all dirty, `sa1_call(c, target)`, reload all live |
| `bra`/`jmp` to another function's entry | tail call: store dirty, call, `return;` |
| `jmp @Rn`/`braf Rn` through a recovered table | `switch (rn) { case <entry word>: goto L_…; }` |
| `jmp`/`braf` unresolved | store dirty, `sa1_call(c, target); return;` |

`sa1_call` binary-searches `sa1_fn_table` (every emitted function, sorted by
physical address) and then `sa1_seam_table`, and traps on a miss. Native
code calls back into recompiled code the same way (task callbacks: set
`c->r[4..7]`, call `sa1_call`).

**Floating point.** FP registers are `float` locals. FPSCR.PR, SZ and FR are
tracked statically per instruction from the assumed entry state
PR=0, SZ=0, FR=0 (Katana reset mode): `fschg`/`frchg` toggle, `lds Rn,fpscr`
with a constant or with a register derived from `sts fpscr` by `and`/`or`
with a constant (the SHC rounding-mode idiom) sets, `lds.l @r15+,fpscr` after
an `sts.l fpscr,@-r15` restores, anything else makes the state unknown.
Callees contribute their return state. Where SZ or PR is unknown the two
lowerings are emitted behind a runtime test of the `fpscr` local; where FR
is unknown bank 0 is assumed and counted. FR is a *rename*: under FR=1 the
instruction's `frN` is the `xfN` local, so `frchg` costs nothing. Double
precision (PR=1) uses `DR(hi, lo)`/`DR_SET` over the two float locals
(FR[2n] high word, FR[2n+1] low word, as the manual defines DRn). Pair moves
under SZ=1 transfer FR[2n] from the lower address and FR[2n+1] from the
higher one (register order = address order), which is what makes a
`float[4][4]` matrix load into XMTRX in column-major order; to be confirmed
against a Flycast trace in M2 if any PR=1 code turns up. `fmac` is two
roundings: compile the output with `-ffp-contract=off`. `fsca` uses
`sinf`/`cosf`, `fsrra` is `1/sqrtf`, `ftrc` saturates like the hardware.
Rounding mode: the C code runs in the host's default (round to nearest);
FPSCR.RM writes are tracked but not applied. If the M2 traces show the game
runs in round-to-zero, the backend sets the host FPU mode once at start.

**Flags.** `t`, `s`, `q`, `m` are separate 0/1 locals. `stc sr` rebuilds a
plausible SR from them; `ldc sr` takes them back and ignores the interrupt
mask and mode bits (the game masks interrupts in 19 places; there are no
interrupts). Privileged registers (SSR, SPC, SGR, DBR, banked r0–r7) are
ignored with a comment in the output; none is reached in SA1's game block.

## What is a function

Every entry in the discovery list is a candidate. The CFG of a function is
everything reachable from its entry inside block A, stopping at other
entries (tail calls). Two adjustments from measurement on SA1:

- 3,024 "branch"-origin entries (discovery registered them because some
  branch landed outside the function ranges it knew at the time) are never
  called. They are join points, loop headers and shared tails of real
  functions, so they are local code of whoever branches to them (one C
  function fewer, no tail-call sync). The 1,113 of them that no emitted
  function reaches are emitted as standalone functions anyway, in case a
  pointer table refers to them; the Flycast trace will say which are dead.
- Call targets that are not in the list get a function of their own
  (0 in SA1: discovery had already found them all).

Switch tables are recovered at `jmp @Rn`/`braf Rn` from the SHC/GCC idiom
`mova tbl,r0; mov.{b,w,l} @(r0,Ri),Rj [; add Rb,Rj]; jmp/braf`. The entry
count comes from the guarding `cmp/hi`/`cmp/hs` against a constant in the
predecessor block when there is one (58 of 65 tables), else entries are
accepted while they are even, inside the function's neighbourhood and
before the first case body. The emitted `switch` keys on the table *word*
in the register, not on a reconstructed index, so an over-long table only
adds unreachable cases and a short one traps with the address.

## Non-local returns

Checked, not needed. Block A contains an SHC-style `setjmp`/`longjmp` pair
at the Katana tail (`0x8C110954` saves r8–r15, PR, MACH/L, GBR, FPSCR, FPUL,
fr0–fr15 to a buffer; `0x8C1108E0` restores them and returns 1), but the
only reference to either is one pool word inside block B (`0x8C668EB0`, the
SDK's own error/chain handling, replaced by HLE). No game-block function
calls, branches to or stores the address of either; the reachable game code
has no `rte`, no `trapa`, no `sleep`, and no `ldc …,spc/ssr`. Shinobi's task
model is cooperative (callbacks run to completion from the frame loop), so
the recompiled code needs neither resumable functions nor a longjmp-style
task switch. If a later milestone finds a game-side `setjmp` user, the plan
is to make the pair a seam entry inlined at the call site as host
`setjmp`/`longjmp` on a side table keyed by the guest buffer address.

## Per-instruction lowering

Generated from `LOWERING_DOC` in `sh4emit/lower.py`.

| SH-4 | C |
|---|---|
| mov #imm,Rn / mov Rm,Rn | `rn = imm;` / `rn = rm;` |
| mov.l @(disp,pc),Rn | folded: `rn = <pool word>;` unless the word is run-time patched, then `rn = RD32(addr, 0);` |
| mov.w @(disp,pc),Rn | folded sign-extended word, same rule |
| mova @(disp,pc),r0 | `r0 = <address>;` |
| mov.{b,w,l} loads | `rn = RD8S/RD16S/RD32(base, disp);` (byte/word sign-extend); `@Rm+` adds the size after (not when n == m) |
| mov.{b,w,l} stores | `WR8/16/32(base, disp, rm);`; `@-Rn` decrements first |
| constant address outside RAM | `sa1_mmio_rd(addr, size)` / `sa1_mmio_wr(addr, size, v)` (hardware registers) |
| swap.b / swap.w / xtrct | shift-and-mask expressions |
| add / sub / and / or / xor / not / neg | C operators on uint32_t |
| addc / subc / negc | `ADDC/SUBC/NEGC` macros, carry in `t` |
| addv / subv | `ADDV/SUBV` macros, overflow in `t` |
| cmp/* | `t = <relation>;` with `(int32_t)` casts for the signed forms |
| cmp/str | `CMPSTR` macro (any equal byte) |
| div0u / div0s / div1 | `DIV0U/DIV0S/DIV1` macros over the `q`, `m`, `t` locals, manual's restoring step |
| mul.l / muls.w / mulu.w | `macl = ...;` |
| dmuls.l / dmulu.l | 64-bit product into `mach:macl` |
| mac.l / mac.w | `MACL_STEP/MACW_STEP` macros (S-bit saturation included) |
| dt | `rn -= 1; t = rn == 0;` |
| exts/extu | casts and masks |
| shll/shlr/shal/shar/rotl/rotr | shift with `t` = shifted-out bit |
| rotcl / rotcr | `ROTCL/ROTCR` macros through `t` |
| shad / shld | `SHAD/SHLD` macros (sign of rm picks direction; -32 special case) |
| shll2/8/16, shlr2/8/16 | plain shifts |
| tst / tst #imm / tas.b | `t = (a & b) == 0;`; tas.b reads, tests, sets bit 7 |
| and/or/xor/tst.b #imm,@(r0,gbr) | read-modify-write through `gbr` |
| movt / clrt / sett / clrs / sets / clrmac | flag and MAC assignments |
| ldc/stc sr | only T, S, Q, M are modelled; `SR_FROM_FLAGS()` rebuilds a plausible SR |
| ldc/stc gbr, vbr | locals `gbr`, `vbr` |
| ldc/stc ssr, spc, sgr, dbr, Rn_bank | privileged: ignored (store) / zero (load), commented in the output |
| lds/sts mach, macl, pr, fpul, fpscr (+.l forms) | locals; the `.l` forms go through RD32/WR32 and move the pointer |
| pref / ocbi / ocbp / ocbwb / ldtlb / nop | no code (comment) |
| movca.l r0,@Rn | plain 32-bit store |
| trapa / sleep / rte / invalid word | `sa1_trap(c, pc, text)` |
| bt / bf (+/s) | `if (t) goto L;` with the slot before the test; T captured first when the slot writes it |
| bra / jmp @Rn (constant) | `goto L;` locally, else tail call: store dirty registers, call, `return;` |
| bsr / jsr @Rn / bsrf (constant target) | store live-in-of-callee ∩ dirty, `fn_xxxx(c);` or `sdk_xxxx(c);`, reload live ∩ may-write-of-callee |
| jsr / bsrf (unknown target) | store all dirty, `sa1_call(c, target);`, reload all live |
| jmp / braf through a recovered table | `switch (rn) { case <entry>: goto L; ... default: sa1_trap }` |
| jmp / braf unresolved | store all dirty, `sa1_call(c, target); return;` |
| rts | store dirty registers, `return;` (PR is not consulted) |
| fmov FRm,FRn | `frn = frm;`; under SZ=1 the pair (`DRn`/`XDn` by field low bit) |
| fmov.s loads/stores | `RDF/WRF`; under SZ=1 two words, register order = address order (see README) |
| fldi0 / fldi1 / flds / fsts | assignments, `F2U/U2F` for the bit moves |
| fadd / fsub / fmul / fdiv | float operators; under PR=1 `DR_SET(hi, lo, DR(..) op DR(..))` |
| fmac | `frn = fr0 * frm + frn;` (two roundings: compile with -ffp-contract=off) |
| fabs / fneg | sign-bit operations (`FABS_F/FNEG_F`), `fabs()` / `-` under PR=1 |
| fsqrt / fsrra | `sqrtf` / `1.0f / sqrtf` |
| fcmp/eq / fcmp/gt | `t = a == b;` / `t = b > a;` (frn > frm) |
| float / ftrc | `(float)(int32_t)fpul` / `FTRC()` saturating round-toward-zero |
| fcnvsd / fcnvds | double <-> single through FPUL |
| fipr FVm,FVn | `FIPR` macro: 4-term dot product into fr[4n+3] |
| ftrv xmtrx,FVn | `FTRV` macro: XMTRX (xf bank, column-major) times the vector |
| fsca fpul,DRn | `FSCA` macro: sinf/cosf of (fpul & 0xFFFF) / 65536 turns |
| fschg / frchg | `fpscr ^= bit`; SZ and FR are also tracked statically so fmov and register naming need no runtime test when the state is known |

## Report fields

`recomp/out/emitter/report.txt` (and the emitter's stdout) lists the counts
the milestone report quotes: functions, instructions, blocks, calls by kind,
tail calls, switch tables and bounded tables, indirect jumps, folded and
unfolded pool words, patched words, stores/reloads at calls, entry loads
and exit stores, runtime FP branches (`rt_pr`, `rt_sz`), FR-unknown
instructions, calls made outside the assumed FPSCR state, functions whose
return FPSCR state is not the entry state, merged/orphan/added entries, and
every note (switch tables, illegal delay slots, unknown FPSCR points) with
its address.

## Tests

`tests/emitter/run_tests.py` assembles hand-written snippets with
`tests/emitter/sh4asm.py` (driven by the same table as the decoder, guarded
by 129 encodings checked against the manual's listing), runs them through
`Program` exactly like the game block, compiles the result with the host C
compiler together with `runtime/sh4_rt.c`, executes it, and compares
registers and memory with expected values. It needs no game data and runs
in CI on Linux (gcc) and Windows (clang).
