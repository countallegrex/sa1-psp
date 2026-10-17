# M0 Function map: report

Date: 2026-10-16. Input: `1ST_READ.BIN` from the US v1.005 dump, SHA-256
`45bc5d3c25dcfe2cc2c5701311d46783f33e4daa44a951cd9896724d66f0367a`,
6,735,488 bytes, descrambled, loaded at `0x8C010000`. Tool:
`phobos665/Dream-Recomp` at commit `51a148c` (2026-09-21), built with
clang 23.1.3 on Windows, run as a separate process. Compilers for the size
measurements: clang 23.1.3 (x86-64, MSVC ABI) and psp-gcc 15.2.0 from
pspdev v20261001 in WSL.

Exit test (README §8): discovered-vs-executed coverage ≥ 95 %, a named list
of SDK entry points, a measured emitted-code size, and the emitter decision.

**Status: passed (2026-10-16).** The SDK entry-point list
(`docs/seam.md`, `recomp/sa1_us_1005/sdk_entry_points.tsv`), the code sizes
and the decision are below. The dynamic half, a Flycast trace of title →
Station Square → Emerald Coast → Station Square diffed against this list,
is in `docs/m0-trace.md`: 97.76 % of the executed call targets in
`1ST_READ.BIN` were found statically (100 % after adding the 68 seeds the
trace produced). The trace also found that 29 % of the executed code lives
in overlays loaded from disc, which this static run could not see
(`docs/m0-trace.md` §5).

## 1. Binary layout (the fact that changes everything else)

The 6.4 MB executable is **1.45 MB of code and 5.3 MB of data**. Code sits
in two blocks:

| Block | Range | Bytes | Contents |
|---|---|---|---|
| A | `0x8C010000`–`0x8C111200` | 1,053,184 | game code (+ ~21 KB of Katana at the tail) |
| B | `0x8C5FA000`–`0x8C66C000` | 466,944 | Katana SDK code (Shinobi, Ninja, KAMUI, sound) |

Details and evidence in `docs/seam.md`. README §7 assumed "2–3 MB of SH-4
code"; the real figure for what we must recompile is about **0.95 MB** (block
A), because block B is replaced by the HLE layer.

## 2. Discovery

Dream-Recomp's `game` command, seeded at the entry point, pointer pass and
sweep on.

**Out of the box** it is unusable on this title: the pointer pass accepts
any aligned word that points at something that decodes as SH-4, and 5.3 MB
of model data decodes as SH-4. Result after 10 minutes (sweep off): 50,828
functions, 42,323 of them inside the data region, 5.59 MB of "code"; with
the sweep on it had not finished after 25 minutes and was stopped. Fix: a
local change that restricts discovery seeds to the two code blocks
(`DREAM_CODE_RANGES`, described in `recomp/sa1_us_1005/README.md`). With it
the whole translation takes 3.9 s.

Restricted run, numbers from `sa1_us_1005.report.txt` and the function list:

| Measure | Value |
|---|---|
| Functions discovered | **9,568** (block A 7,206, block B 2,362) |
| by origin | call 2,410, pointer 3,046, branch 3,364, sweep 747, seed 1 |
| Instructions emitted | 680,534 |
| Function ranges, union | 1,372,790 B (A 941,568 of 1,053,184 = 89.4 %; B 431,222 of 466,944 = 92.4 %) |
| Bytes decoded as instructions | **1,213,346 of 1,372,790 (88.4 %)** |
| Bytes neither instruction nor literal pool | 98,518, in 2,547 runs ≥ 16 B |
| Unresolved indirect-branch sites | **999** (A 631, B 368): `jsr` through an untraceable register 822, `jmp` 88, `jsr`/`jmp` to an address outside the image 88, `braf` without a table 1 |
| Switch tables | 60 recovered by discovery, 73 lowered by the emitter |
| Direct `jsr`/`bsrf` calls resolved | 20,700; 7,119 call sites go through the function table at run time |
| Instructions not lowered | 13 |
| Pool words the code patches at run time (not folded) | 416 |

The 88.4 % is in line with Dream-Recomp's own 79–90 % across five other
titles. The 11.6 % gap is literal pools the analysis could not attribute
plus functions reached only through the 999 unresolved register jumps
(object-list callbacks, task entries). Closing it is the Flycast trace's
job: every executed address not in the static list is a seed.

## 3. Emitted code size

Dream-Recomp emits one C++ translation unit. Two variants were measured:

- **v1, as emitted.** Every one of the 9,568 functions is "entered from
  outside" as far as the FPSCR analysis can tell (no static path from the
  entry point reaches the bulk of the code), so the emitter writes both the
  single- and double-precision lowering behind a runtime test at **111,691**
  FP instructions.
- **v2, assumed entry FPSCR.** A one-line local change keeps Katana's reset
  mode (PR = 0, SZ = 0) at uncalled entries. FP runtime branches drop to 160.
  This is the policy our own emitter would use; it is correct unless SA1
  switches precision at run time, which the `fschg`/`lds fpscr` summaries
  (11 functions with an unknown effect) will tell in M1.

Source size and object size (`.text` summed over 48 chunks of the unit,
`-Os`, no LTO):

| | v1 (as emitted) | v2 (assumed FPSCR) |
|---|---|---|
| Emitted C++ | 82.2 MB (+0.55 MB header) | 72.5 MB |
| clang -Os x86-64 `.text` | 18.30 MB | 12.71 MB |
| **psp-gcc -Os MIPS `.text`** | **23.54 MB** | **18.67 MB** |
| of which block A (game) | 16.37 MB | **13.12 MB** |
| of which block B (SDK, replaced) | 6.85 MB | 5.24 MB |

Ratios against the SH-4 bytes they came from (range union): block A
**13.9× (v2) to 17.4× (v1)** on MIPS; block B 12.2× to 15.9×. README §7
guessed "4–8× for naive emission"; Dream-Recomp's output is twice that.

Against the PSP-2000 budget (README §7, ~52 MB usable): 13.1 MB of game code
+ 16 MB guest RAM + 4–8 MB textures + 3 MB audio + 2 MB runtime = 38–42 MB.
It fits on paper with no margin, before the never-executed functions are
dropped. It does not fit if the code must also be in RAM while a level loads
its data. Either way the size is the symptom, not the problem; see §5.

Why the code is this big (counts from the v2 unit):

| Emitted construct | Count | Cost |
|---|---|---|
| Guest register accesses as `c.r[n]` through a `Ctx&` | 889,831 | every access is a load or store to memory; no register allocation across guest instructions |
| Guest loads/stores as `m.read*/write*` | 227,971 | `Memory` is an abstract class: each is an indirect call through a vtable, never inlined |
| `c.cycles += k` block accounting | 137,574 | needed by the LLE scheduler; unused by us |
| Interrupt polls `if (c.cycles >= c.next_event) deliver_irq` | 12,271 | at every entry and back-edge |
| Resume `switch (resume_pc)` prologue | 9,568 | one per function, for non-local returns |
| `nonlocal_return` check on `rts` | 6,255 | compares PR with entry PR at every return |
| `call_indirect` through the function table | 7,119 | |
| `goto` between basic blocks | 147,172 | fine |
| FP runtime branches (v1) | 111,691 | halved by v2 |

## 4. 32-bit cleanliness and portability audit

Greps over the 72.5 MB unit, then the psp-gcc build:

| Check | Result |
|---|---|
| `size_t`, `uintptr_t`, `intptr_t`, `long long`, `__int128` | 0 occurrences |
| `uint64_t`/`int64_t` in emitted code | 0 (the runtime's `Ctx::cycles` is 64-bit; fine on MIPS) |
| x86 intrinsics (`_mm_`, `__builtin`) | 0 |
| `reinterpret_cast`, `template`, `auto`, `constexpr` | 0 |
| C++ features used | `static_cast`, `goto`, `switch`, `[[maybe_unused]]`, one `try` in the runtime header |
| psp-g++ 15.2 `-std=c++20 -Os -c` | **compiles**, 48/48 chunks, no errors |
| psp-g++ with `-fno-exceptions -fno-rtti` | fails: `dream/runtime/memory.h` throws from `BareMemory::idx`; the ABI (`NonLocalReturn`) is exception-based |

Verdict: the emitted C++ itself is 32-bit clean and psp-gcc accepts it. The
problems are in what it depends on, not in the lowering.

**GPL coupling.** Every unit starts with
`#include "dream/runtime/memory.h"`, `sh4/abi.h`, `sh4/ctx.h`, `sh4/ops.h`.
Those headers are GPLv2 and are not thin: `ops.h` holds the semantics of
`div1`, `mac.l/w`, `addc/subc`, `shad/shld`, `fsca` (a table), `fipr`,
`ftrv`, FPSCR bank swaps, and the generated code calls them on every such
instruction. `abi.h` defines the call-table, non-local-return and interrupt
contract. Linking an EBOOT against them makes the EBOOT GPL, which README §0
rules out. Re-implementing the same header API under our licence would be a
clean-room exercise over 30-odd helpers whose reference is the GPL file we
just read; defensible, but it buys only the lowering, which is the cheap part.

**Native-call hook.** The config format has `[hle]` and `[hooks]` tables, but
the translator does not read them into the emitter (`grep` finds the parser
only). Calls to a known function are emitted as direct C++ calls, so the only
way to divert one is `[functions] exclude`, which routes its call sites
through the run-time function table where the runtime could register a
native entry. There is no "emit a call to native symbol X" path. Adding one
is a small patch, but it is a patch to a GPL program we would then ship as
part of the pipeline.

## 5. Decision: write our own emitter

Keep Dream-Recomp for what it is good at and stop there:

- **Keep:** function discovery (entry, pointer and sweep passes, switch-table
  recovery, delay-slot handling, the coverage report), run as a separate
  process; its `*.functions.json` and `discover` JSON are the input to our
  pipeline. Its decoder is verified against binutils for all 65,536 opcodes
  and its emitter is the second reference for instruction semantics (the
  first is `sp00nznet/dcrecomp`, MIT, C11).
- **Write:** the SH-4 → C emitter, the register/memory model and the runtime
  ABI, under this repo's licence.

Reasons, in numbers:

1. **13.1 MB of MIPS for 0.94 MB of SH-4** (13.9×) with the LLE-shaped
   runtime, where the design in README §4 (flat 16 MB array, registers in
   locals, no cycle accounting, no resumable functions) removes the four
   largest items in the §3 table: 890 k register memory accesses, 228 k
   virtual memory calls, 138 k cycle updates, 9.6 k resume switches. A
   target of **≤ 5× (≤ 4.7 MB for block A)** is the M1 measurement; N64 and
   Xbox 360 recompilers with register-to-local lowering land in 3–5×.
2. **227,971 virtual calls** on the hot path is a performance ceiling as well
   as a size one; README §5 budgets 2–4× the SH-4 cycle count for recompiled
   logic, and an indirect call per load/store on a single-issue in-order CPU
   is outside that.
3. **GPL headers in every unit** (§4) make the as-is output unshippable in
   an MIT EBOOT regardless of size.
4. **No native-call hook** in the emitter (§4); the seam is 289 entries that
   must become direct native calls, and the exclude-then-table-lookup
   workaround costs an indirect call at each of the 7,738 sites.
5. The emitter is the known quantity README §4 says it is: 680,534
   instructions across ~190 opcodes, fixed 16-bit encoding, and two public
   references for the semantics. The discovery and coverage tooling, which is
   the hard and mature part of Dream-Recomp, stays in use.

What transfers from the M0 run into M1 directly: the function list (9,568
entries with extents), the 60 switch tables, the 416 run-time-patched pool
words (must never be folded), the 11 functions with unknown FPSCR effect,
the 13 unlowered instructions (to look at), and the 289 seam addresses.

## 6. Open items carried to M1

- ~~Flycast trace and diff~~: done, `docs/m0-trace.md`. Follow-ups from it:
  discovery and emission for the code overlays (`ADVERTISE.PRS`,
  `ADV00.PRS` and the opening `EV*.PRS` first, for M1's exit test), one
  function table per overlay slot, and switching the emitter's input to the
  seeded function list (`recomp/out/dream_seeded/`).
- Byte-pattern signatures for block B if Katana static libraries turn out to
  be available locally (ask the maintainer once; do not search for them).
- Decide the three `njRotate` axes, and name the two hot Shinobi entries at
  `0x8C605628` and `0x8C608CCC`.
- Look at the 8 game functions that write the TA FIFO directly and the one
  that touches `0xA0720009` (`docs/seam.md` §5).

## 7. Reproduction

Everything under `recomp/out/` (ignored) came from the commands in
`recomp/sa1_us_1005/README.md`. The unit was split into 48 chunks at
function boundaries for parallel compilation; chunk sizes were summed with
`llvm-size` and `psp-size`. The literal-pool scans are 150 lines of Python
that read the binary by path; they will be committed under `tools/` when the
seam table gets its first consumer (M1).
