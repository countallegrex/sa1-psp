# M0 Execution trace: discovered vs executed

Closes the dynamic half of the M0 exit test (README §8, `docs/m0-report.md`
§6). Static discovery (Dream-Recomp, `docs/m0-report.md` §2) found 9,568
functions; this run records what the game actually executes in Flycast and
compares the two.

## 1. Emulator

| | |
|---|---|
| Flycast | **v2.7** (2026-08-19), `flyinghead/flycast` |
| Release used for the boot check | `flycast-win64-2.7.zip`, 9,885,102 bytes, SHA-256 `7edf489ba6b3053703a18ce8233767eb71a2d89e4bda0421b312c2167a739161` |
| Build used for the trace | tag `v2.7` (commit `5aa091f`) + the local patch below, MSVC 19.44 (VS 2022 Build Tools), Ninja, `Release`, `-DUSE_DX9=OFF -DUSE_BREAKPAD=OFF -DUSE_LUA=OFF -DUSE_DISCORD=OFF` |
| BIOS | none; Flycast's HLE BIOS |
| Disc | the US v1.005 GDI in `../private/dump/sa1_us_1005/` (outside the repo) |

Both live outside the repository in `../flycast/` (`release-2.7/`,
`src-2.7/`, `trace-2.7/`). The release build boots the GDI to the intro
movie unmodified.

## 2. Method

Options considered, cheapest first:

- **Lua scripting** (on in release builds): callbacks per frame and memory
  access only; no instruction or branch hook. Not usable.
- **GDB stub**: `ENABLE_GDB_SERVER` is `OFF` in release builds, so it needs a
  source build anyway, and a breakpoint per function entry costs a round
  trip per hit; it also cannot see addresses outside the static list, which
  is half of what the diff needs.
- **A two-hook logging build** (chosen). Flycast's SH-4 dynarec compiles each
  basic block once, on first execution, and every non-return dynamic jump
  goes through one place in the x64 code generator. Hooking those two points
  records everything the diff needs at near-zero cost: the compile hook runs
  only on cache misses, and the jump hook is one C call and a hash-set lookup
  per `jsr`/`jmp`/`bsrf`/`braf` executed.

The patch (109 lines of diff, kept next to the build as
`../flycast/sa1trace-flycast-2.7.patch`; not committed, since a diff of GPL
code is GPL and this repo is MIT) does the following:

1. `core/hw/sh4/dyna/driver.cpp`: when the environment variable `SA1_TRACE`
   names a file, `compilePC()` appends, after `bm_AddBlock`, one line per
   distinct compiled block: `B vaddr size endtype branch` (start address,
   SH-4 bytes including the delay slot, Flycast `BlockEndType`, and the
   static branch or call target). Lines are deduplicated in memory and the
   file is flushed every 64 new lines, so closing the window loses nothing.
2. `core/rec-x64/rec_x64.cpp`: for blocks ending in `BET_DynamicCall` or
   `BET_DynamicJump`, after the generated code stores the next PC, it calls
   `sa1trace_dyn(site, target)` with the address of the `jsr`/`jmp`/`bsrf`/
   `braf` and the register value it jumps to. Distinct pairs are written as
   `D site target`. `rts` (`BET_DynamicRet`) is not hooked.
3. For blocks that run in main RAM outside the `1ST_READ.BIN` image, the
   compile hook also writes `M vaddr` plus the block's first 32 bytes. This
   is the only game-derived content in the trace and exists so that
   `tools/trace_overlays.py` can tell which disc file the code came from.
   The trace therefore stays outside the repository
   (`../flycast/trace-2.7/sa1trace.txt`); only address lists are committed.

When `SA1_TRACE` is unset the build behaves exactly like upstream (the
dynamic-jump call is not even emitted).

From those two record types `tools/trace_diff.py` derives:

- **call targets executed**: the static target of every executed `bsr`
  block, plus the dynamic target of every executed `jsr`/`bsrf`; only
  targets where a block was actually compiled count;
- **executed bytes**: the union of all compiled blocks;
- **executed discovered functions**: functions whose entry is a call target
  or that contain any executed block;
- **new seeds**: executed call targets that are not a discovered entry
  (inside a discovered range or outside all of them), plus executed `bra`/
  `jmp` targets that fall outside every discovered range (tail calls);
- **never-executed discovered functions**: the rest.

Addresses are normalised to the 29-bit spelling (`0x0Cxxxxxx`) that the
function list uses. Nothing is read from the game binary.

The tool runs as:

```
python -I tools/trace_diff.py ../flycast/trace-2.7/sa1trace.txt \
    recomp/out/dream/sa1_us_1005.functions.json \
    --seeds recomp/sa1_us_1005/trace_seeds.txt \
    --executed recomp/sa1_us_1005/executed_entries.txt
```

Launch with `../flycast/trace-2.7/run_trace.bat`, which sets `SA1_TRACE`
and opens the GDI. Flycast's default keyboard map (stick on I/J/K/L, A = X,
B = C, Start = Enter) or any XInput pad works.

Overlays are identified with:

```
python -I tools/trace_overlays.py ../flycast/trace-2.7/sa1trace.txt \
    ../private/dump/sa1_us_1005/data --prs
```

It votes each out-of-image cluster to a (file, load address) pair from
sampled `M` records, then checks every record against that file; `--prs`
also tries every `*.PRS` file decompressed (SEGA PRS). About 80 s.

## 3. Run

2026-10-16, 21:18–21:29 (11 minutes), the maintainer on the controller: boot,
intro movie, title, new Sonic story, opening events and the Chaos 0 fight in
Station Square, Emerald Coast (act 1 completed), back to Station Square.
Dynarec on, everything else at defaults.

The trace has 38,387 block records (37,340 distinct start addresses;
26,325 inside the image), 5,325 distinct dynamic edges from 3,667 dynamic
call/jump sites, and 12,047 out-of-image byte records; 1.6 MB.

## 4. Results against the M0 function list

`tools/trace_diff.py` against `recomp/out/dream/sa1_us_1005.functions.json`
(9,568 functions):

| Measure | Value |
|---|---|
| Distinct call targets executed inside the image | 2,631 |
| of which in the static list | 2,572 |
| **Coverage: executed call targets that static discovery found** | **97.76 %** |
| Executed code bytes inside the image | 382,660 |
| of which inside discovered functions | 381,532 (**99.71 %**) |
| Discovered functions executed (called, or any executed block inside) | 3,653 of 9,568 (A 2,569, B 1,084) |
| Discovered functions never executed | **5,915** (A 4,637, B 1,278) |
| Bytes of never-executed functions not shared with an executed one | A 438,452, B 242,242 |
| Executed call targets inside a discovered function but not at its entry | 3 |
| Executed call targets outside every discovered function | 56 |
| Executed `bra`/`jmp` targets outside every discovered function | 9 |
| **New seeds** (the three rows above) | **68** (62 in block A, 6 in block B) |
| Executed runs outside discovered functions | 62 (1,128 bytes), 7 with no seed in them |
| Blocks executed in the image's data region | 0 |

**The M0 exit criterion (≥ 95 %) is met for `1ST_READ.BIN`: 97.76 % of the
executed call targets were already in the static list, and 99.71 % of the
executed bytes were inside discovered functions.**

### With the seeds

The 68 seeds are in `recomp/sa1_us_1005/trace_seeds.txt` and in
`[functions] extra` of `sa1_us_1005.toml`. Re-running discovery with them
(same command as `recomp/sa1_us_1005/README.md`, into
`recomp/out/dream_seeded/`, 3.4 s):

| Measure | M0 list | Seeded list |
|---|---|---|
| Functions | 9,568 | 9,542 (+82, −108) |
| Executed call targets found statically | 97.76 % | **100.00 %** (2,631 of 2,631) |
| Executed bytes inside discovered functions | 99.71 % | **99.99 %** |
| Discovered functions executed | 3,653 | 3,661 (A 2,575, B 1,086) |
| Discovered functions never executed | 5,915 | 5,881 (A 4,620, B 1,261) |
| Executed runs outside discovered functions | 62 (1,128 B) | 4 (52 B) |

The 108 entries that disappear are all mis-splits the seeds correct: every
one now lies inside a discovered function. 71 came from the branch pass,
21 from the pointer pass, 16 from the sweep. Several were 2 or 4 bytes past the real
entry (`0x0C01937E` for `0x0C01937C`, `0x0C020AE4` for `0x0C020AE0`). The
4 runs still outside (`0x0C019360`, `0x0C04A480`, `0x0C0A6696`,
`0x0C107AF6`, 6–24 bytes each) are not call or tail-jump targets; they are
reached by fall-through or an exception path.

`recomp/sa1_us_1005/executed_entries.txt` lists the 3,661 executed function
entries of the seeded list (addresses only), for the code-size cut and as a
regression check for later traces.

### Never executed

5,881 discovered functions (61.6 %) did not run on this route; in block A
that is 4,620 functions and 438 KB of SH-4 that no executed function
shares, 47 % of block A's discovered bytes. This is **not** dead code yet:
one 11-minute route covers one character, one stage, one boss and the first
events. It is the upper bound for the code-size cut and shrinks with every
route traced; a cut needs traces of all six stories, the Chao Garden and
the menus first.

## 5. Code loaded at run time (overlays)

Static discovery only looked at `1ST_READ.BIN`. The trace shows the game
also executes code it loads from disc. Every out-of-image block but five
(48 bytes in the BIOS/syscall area) matched a file
(`tools/trace_overlays.py`):

| File | Load address | Size (decompressed) | Blocks executed | Bytes executed |
|---|---|---|---|---|
| `IP.BIN` | `0x8C008000` | 32,768 | 317 | 5,342 |
| `SONICADV/ADVERTISE.PRS` (title, menus) | `0x8C900000` | 937,008 | 1,341 | 19,172 |
| `SONICADV/ADV00.PRS` (Station Square) | `0x8C900000` | 2,328,347 | 1,070 | 12,258 |
| `SONICADV/B_CHAOS0.PRS` (Chaos 0) | `0x8C900000` | 392,812 | 921 | 13,670 |
| `SONICADV/STG01.PRS` (Emerald Coast) | `0x8C900000` | 1,954,056 | 2,076 | 33,484 |
| `SONICADV/SUMMARY.PRS` | `0x8C900000` | 695,821 | 20 | 270 |
| `SONICADV/EV0001.PRS` | `0x8CB80000` | 81,596 | 395 | 5,496 |
| `SONICADV/EV0002.PRS` | `0x8CB80000` | 157,824 | 191 | 3,070 |
| `SONICADV/EV0003.PRS` | `0x8CB80000` | 87,496 | 258 | 3,416 |
| `SONICADV/EV0006.PRS` | `0x8CB80000` | 31,640 | 266 | 2,614 |
| `SONICADV/EV002B.PRS` | `0x8CB80000` | 325,560 | 798 | 11,834 |
| `SONICADV/MOVIE.BIN` (movie player) | `0x8CEB0000` | 184,592 | 3,356 | 49,270 |

So there are three overlay slots:

- **`0x8C900000`, area code.** One stage (`STGnn`), adventure field
  (`ADVnn`), boss (`B_*`), mini-game (`MINI*`) or menu (`ADVERTISE`,
  `SUMMARY`) file at a time; PRS-compressed, code and data together. The
  disc has 12 `STG`, 31 `ADV*`, 14 `B_*` and 7 `MINI*` PRS files (29.7 MB
  compressed).
- **`0x8CB80000`, event (cutscene) code.** `EVnnnn.PRS`, 236 files
  (13.7 MB compressed).
- **`0x8CEB0000`, the movie player.** `MOVIE.BIN`, uncompressed.
- `IP.BIN` at `0x8C008000` is the boot sector and its syscall glue, which
  the HLE layer replaces anyway.

Measured share: on this route **154,554 bytes executed in overlays**
(summed per file, `IP.BIN` excluded) against 382,660 in the image, so
**29 % of the executed game code is not in `1ST_READ.BIN`**. Overlay call
targets: 1,099 distinct addresses (a lower bound: files sharing a slot share
addresses, and the trace deduplicates by address). Counting them as
undiscovered, call-target coverage over image + overlays is 68.95 %
(70.54 % with the seeds).

Consequences, carried to M1 (`docs/m0-report.md` §6):

1. **Every overlay needs its own discovery and emission**, at its slot
   address, from the decompressed file. Dream-Recomp's `[[relocations]]`
   only handles copies out of the main image, so this needs one config per
   overlay with `[binary]` pointing at a decompressed copy under
   `../private/`, or the same in our own emitter's driver.
2. **Overlays share addresses.** `0x8C900000` hosts a different file per
   area, so the run time needs one function table per overlay, switched
   when the game loads the file (hook the loader, not the address).
3. A PRS decompressor exists in `tools/trace_overlays.py`; the launcher
   (README §0) needs one for the same reason.
4. M1's exit test (title screen, then Station Square) already needs
   `ADVERTISE.PRS` and `ADV00.PRS` recompiled, plus the `EV*` files for the
   opening events or a way to skip them.

Done for those seven files in `docs/m1-overlays.md`: per-overlay discovery
and emission, 100 % of the 406 executed overlay call targets found with 12
seeds. The four unexplained runs in §4 turned out to be main-image entries
that only overlays call.

## 6. Exit test verdict

- Discovered vs executed, `1ST_READ.BIN`: **97.76 %** of executed call
  targets (99.71 % of executed bytes). **≥ 95 %: met.** 100 % with the 68
  seeds.
- Run-time-loaded overlays are a scope item that M0's static discovery could
  not see: 29 % of executed code on this route, three slots, about 300 files.
  Recorded above and carried to M1.
