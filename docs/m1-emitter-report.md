# M1 emitter: report

Date: 2026-10-16. Input: block A of `1ST_READ.BIN` (US v1.005, SHA-256
`45bc5d3c…0367a`), `0x8C010000`–`0x8C111200`, with the 7,206 block-A entries
of Dream-Recomp's M0 function list and the 289-entry seam table. Tool:
`recomp/emitter/` at this commit (own emitter, Python 3.14, see its README
for the ABI). Compilers: clang 23.1.3 (x86-64, MSVC ABI) on Windows and
psp-gcc 15.2.0 (pspdev v20261001) in WSL, both `-Os -fno-strict-aliasing
-ffp-contract=off`, no LTO, 16 parallel jobs.

Question M1 asked of the emitter (README §7): can our own emission land at
**≤ 5x the SH-4 bytes (≤ 4.7 MB of MIPS for block A)** where Dream-Recomp's
LLE-shaped output measured 13.1 MB (13.9x)?

## 1. Result

| Measure | Value |
|---|---|
| SH-4 bytes recompiled (M0 range union, block A) | 941,568 |
| Instructions emitted | 440,850 in 69,989 basic blocks |
| C functions | 5,295 (see §3 for why not 7,206) |
| Emitted C source | 26.8 MB in 32 chunks (758,358 lines) + 0.2 MB of headers and tables |
| clang -Os x86-64 `.text` | **3,354,271 B (3.35 MB), 3.56x** |
| **psp-gcc -Os MIPS `.text`** | **3,655,809 B (3.66 MB), 3.88x** |
| MIPS `.text` per SH-4 instruction | 8.3 B, about 2.1 Allegrex instructions |
| Dream-Recomp, same block, MIPS (M0) | 13,120 KB, 13.9x |
| Function table (`game_table.c`) | 42 KB MIPS (5,295 entries) |
| Seam stubs | 17.5 KB MIPS (replaced by real HLE later) |
| Emitter run time | 58 s (CFG 9.5 s, 8 analysis rounds 44 s, emission 4 s) |
| clang compile, 16 jobs | 4–5 s wall |
| psp-gcc compile, 16 jobs | 9 s wall, 125 s CPU |

**Target met: 3.88x against ≤ 5x, 3.66 MB against ≤ 4.7 MB**, 3.6 times
smaller than Dream-Recomp's output for the same code. The x86 number is
reported for completeness; the budget is MIPS.

Against the PSP-2000 budget of README §7: 3.7 MB of code + 16 MB guest RAM
+ 4–8 MB textures + 3 MB audio + 2 MB runtime = 29–33 MB of 52, before any
dead function is dropped.

## 2. Where the bytes go

The sync traffic between C locals and the `sh4_ctx` struct is the cost of
the "registers are locals" design and is what the summaries keep small:

| Memory operations in the emitted code | Count | Per instruction |
|---|---|---|
| entry loads (live-in registers) | 25,041 | 0.06 |
| exit stores (dirty registers at `rts`) | 33,498 | 0.08 |
| stores before calls (`dirty ∩ live_in(callee)`) | 58,521 | 0.13 |
| reloads after calls (`live ∩ may_write(callee)`) | 48,858 | 0.11 |
| **total sync** | **165,918** | **0.38** |
| guest loads/stores (the game's own) | about 110,000 | 0.25 |
| `mov.l @(disp,pc)` folded to constants | 32,244 | all of them |

Compare Dream-Recomp's 889,831 register accesses through the context struct
(2.0 per instruction) and 227,971 virtual memory calls.

Levers left, in order of expected payoff, none needed for the budget:

1. Trust the SH ABI for `r8–r15` across *every* direct call (today only
   callees that visibly push/pop them are trusted): fewer reloads of frame
   and loop registers after calls.
2. Avoid storing `r15` before every call when the callee only pushes to it
   (treat the stack pointer as an in/out value): about one store per call.
3. Merge the 1,113 orphan entries that the Flycast trace proves dead.

## 3. Structure of the output

| Item | Count | Note |
|---|---|---|
| Entries in the M0 list | 7,206 | block A only |
| merged into their parent function | 3,024 | "branch"-origin entries nothing calls: join points, loop headers, shared tails |
| of which emitted standalone anyway | 1,113 | no emitted function reaches them; may be pointer-table callbacks; trace decides |
| C functions emitted | 5,295 | |
| Direct calls resolved | 14,176 | `bsr`, `jsr`/`bsrf` with a constant register |
| Calls to seam entries | 7,868 + 214 tail calls | direct `sdk_xxxx(c)` calls, prototypes in `hle/seam.h` |
| **Unresolved indirect sites** | **408** | 373 `jsr` through a non-constant register + 35 `jmp`/`braf` without a table; all go through `sa1_call` |
| Tail calls (`bra`/`jmp` to another entry) | 3,413 | plus 1,007 conditional (`bt`/`bf` out of the function) |
| Switch tables | 65 (1,906 cases) | 58 with the case count taken from the guarding compare |
| Pool words written at run time | 300 | none of them is loaded PC-relative in block A, so every pool load folds |
| Invalid words reached | 2 | `.word 0x0900` at `0x8C0B56DC`, `.word 0x0000` at `0x8C0FDC98` (discovery over-ran into data); emitted as traps |
| Illegal delay slots | 6 | the six `.word 0x8c63` Dream-Recomp also reported: a `jsr` whose slot holds the high half of a pool word; treated as `nop` |

Dream-Recomp counted 631 unresolved sites in block A; the difference is
constants held in `r8–r14` across calls (SHC hoists the pool load of a
callee address out of loops), which this emitter follows under the ABI
assumption documented in its README.

## 4. FPSCR, with the assumed-entry policy

Every function assumes PR=0, SZ=0, FR=0 at entry (Katana reset mode).

| Measure | Value |
|---|---|
| FP instructions needing a runtime PR branch | 0 |
| FP instructions needing a runtime SZ branch | 0 |
| Instructions emitted with FR unknown (bank 0 assumed) | 33 |
| Calls made outside the assumed state | 4 |
| Functions returning in a non-entry state | 5, of which unknown 3 |

The 11 "unknown FPSCR effect" functions of M0 reduce to the SHC
rounding-mode idiom (`sts fpscr,rX; and/or <constant>,rX; lds rX,fpscr`,
which leaves PR/SZ/FR untouched and is now tracked) plus one genuinely
unknown point: `lds r0,fpscr` at `0x8C11090C`, inside the Katana-tail
`longjmp` that restores FPSCR from a saved buffer. That function is
unreachable from game code (§5), and the 33 FR-unknown instructions, the 3
unknown-return functions and the 4 non-reset calls are all downstream of
it. The policy holds for the game block: SA1 does not switch precision at
run time. One function (`0x8C0F3EEC`) returns with FR toggled; its callers
inherit FR=1 statically, so no runtime cost.

## 5. Non-local returns: not needed

Block A's Katana tail contains a `setjmp`/`longjmp` pair (`0x8C110954`
saves r8–r15, PR, MACH/L, GBR, FPSCR, FPUL and fr0–fr15 to a buffer;
`0x8C1108E0` restores them and returns 1). The only reference to either in
the whole 6.4 MB image is one pool word in block B (`0x8C668EB0`), i.e. the
SDK's own error/chain handling, which HLE replaces. No game-block function
calls, branches to, or takes the address of either. The reachable game code
contains no `rte`, `trapa`, `sleep` or writes to SPC/SSR; it masks
interrupts through `ldc …,sr` in 19 places, which the lowering ignores
(there are no interrupts). Shinobi's task model is cooperative, so plain C
calls and returns are sufficient; the emitter has no resumable functions
and no exception path. Design for a future game-side `setjmp` user, should
the trace find one: make the pair a seam entry inlined at the call site as
host `setjmp`/`longjmp` on a side table keyed by the guest buffer address.

## 6. Correctness so far

`tests/emitter/run_tests.py`: 129 encodings checked against the manual's
opcode listing, 51 snippet cases (arithmetic with carry/overflow, all shift
forms including `shad`/`shld`, every load/store addressing mode, GBR forms,
pool folding and run-time-patched words, `div0u`/`div1` division, `mac.l`,
`mac.w` saturation, `dmuls`/`dmulu`, delay slots that write the branch's
own operand, `bsr`/`jsr`/`bsrf`/`braf`, indirect calls through the function
table, tail and conditional tail calls, both switch-table shapes, seam
calls, the callee-saved contract, single and double arithmetic, `fschg`
pair moves, `frchg` bank renaming, `fsca`/`fsrra`/`fipr`/`ftrv`, FPSCR
save/restore), all passing on clang/Windows; the CI job added with them runs them on
gcc/Linux and clang/Windows (first run pending at the time of writing). The harness found one real emitter bug before anything shipped
(`bf` emitted with the sense of `bt`); the rest of its failures were
mistakes in the snippets.

What the tests do not cover: the whole-program analyses (entry pruning,
summaries at scale) are only exercised on the game itself, where "it
compiles" is the sole check until M1's boot test. Open items are in §7.

## 7. Open items carried into the rest of M1

- Run the game block: `hle/seam_stubs.c` logs and returns 0; the Shinobi
  HLE that M1's exit test needs (memory, files, pad, vsync, timers) is the
  next step, together with a `main` that loads `1ST_READ.BIN` into guest
  RAM and calls `fn_8c010000`.
- Flycast executed-function trace (sibling task): decides which of the
  1,113 orphan entries are dead, and whether any of the 408 indirect sites
  reach an address outside the emitted set.
- Rounding mode: the hardware resets to round-to-zero (FPSCR 0x00040001);
  whether Katana's startup changes it is unknown. C code runs in
  round-to-nearest. Decide from the trace in M2.
- The 8 TA-writing game functions and `0x8C053B60` (`0xA0720009`) now go
  through `sa1_mmio_rd/wr`, which logs; their translator is M2 work.
- Confirm the SZ=1 pair-move word order and the DRn high/low convention
  against hardware traces if any PR=1 code is ever executed (none is
  reachable today).

## 8. Reproduction

```
python recomp/emitter/emit_sa1.py --bin ../private/dump/sa1_us_1005/data/1ST_READ.BIN \
    --functions recomp/out/dream/sa1_us_1005.functions.json --out recomp/out/emitter
# in recomp/out/emitter (scripts written by this session, not committed):
#   clang  -Os -std=c11 -fno-strict-aliasing -ffp-contract=off -c game_*.c ; llvm-size
#   psp-gcc -Os -G0 -std=gnu11 -fno-strict-aliasing -ffp-contract=off -c game_*.c ; psp-size
```

Sizes are the sum of `.text` over the 32 `game_NNN.o` objects (BSD `size`
output, so `.rodata` such as switch jump tables is included); the function
table, runtime and seam stubs are listed separately above.
