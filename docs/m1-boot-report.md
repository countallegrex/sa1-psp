# M1 Desktop boot, no graphics

Exit test (README §8): the recompiled game initialises, loads the title
screen data, then loads Station Square without crashing, visible in the log.

Status: **passed 2026-10-17.** The game initialises, shows the title, walks
the menus on a scripted controller, loads Station Square (`ADV00.PRS`
installed, the field's data read) and runs it, including the first opening
event, until the frame limit of the acceptance run. Everything below was
measured with the desktop build (`build-desktop/backend/desktop/sa1psp.exe`,
clang 23, `-O1`), the US v1.005 dump and the `SONICADV` tree on the host
disk.

## 1. How far the boot gets

Acceptance run (exits by its frame limit after 1.0 s of host time):

```
sa1psp <1ST_READ.BIN> <data root> --frames 3000 \
    --press 500:8:4 --press 900:8:4 --press 1300:4:4 --press 1700:4:4 --press 2100:4:4
```

Frames are guest frames: time is virtual, 1/60 s per frame (§4), so the run
is deterministic and the frame numbers below are the same on every machine.
The host time column is the wall clock of this run.

| Guest frame | Host time | Stage | Evidence in the log |
|---|---|---|---|
| 0 | 0.005 s | System init: `syHwInit`, `syMallocInit(0x8CF00000, 1 MB)`, `njInitSystem(0x38, 0, 1)`, `pdInitPeripheral`, `gdFsInit`, `gdFsChangeDir("SONICADV")` | `syMalloc: heap 0x8cf00000`, `gdFs: init`, `gdFs: cd "SONICADV"` |
| 0–10 | | Sound driver load (`MASARU.DRV`), common data (`S_MOT`, `SONIC`, `SON_EFF`), Sega logo (`SEGALOGO.PRS`) | `gdFs: open ...` |
| 200–202 | 0.009 s | GameMode 1 → 8: intro movie; `MOVIE.BIN` loaded to `0x8CEB0000`, player entry skipped natively, returns -1 | `movie: id 0 skipped` |
| 202 | 0.011 s | GameMode 10/11: `ADVERTISE.PRS` installed in the area slot, title screen data (`MOBILE1.MLT`, `AVA_GTITLE0_E.PRS`) | `overlay: loaded SONICADV/ADVERTISE.PRS`, `overlay slot 0: ADVERTISE` |
| 900 | | Start pressed: title phase 6 → 7, menu textures (`AVA_TITLE_E.PRS`) | `pad: buttons 0x8` |
| 1300–1331 | | A pressed: character select (`AVA_CHSEL_E.PRS`, all seven character models, `SL_60B.BIN`/`PL_60B.BIN`) | |
| 2442–2443 | 0.84 s | A twice more: GameMode 16, `SUMMARY.PRS` (file select) installed | `overlay slot 0: SUMMARY` |
| 2458 | 0.84 s | **GameMode 5 (adventure field): `ADV00.PRS` installed, Station Square data loads** (`STATIONSQUARE.MLT`, `OBJ_SS.PRS`, `SETSS00S`–`05S.BIN`, `CAMSS00S`–`05S.BIN`, `ADVSS03.PRS`, `SS_*`) | `overlay: loaded SONICADV/ADV00.PRS`, `overlay slot 0: ADV00` |
| 2458+ | | Station Square's first logic frames (object table, `SS_MES_S_E.BIN`, `SS_PATH_S.BIN`) | |
| 2465 | 0.86 s | Opening event: `EV0001.PRS` installed in the event slot, `EV_HELI.PRS`, the event script runs as a game task | `overlay slot 1: EV0001`, `tasks: task ... starts fn 0x8c049692(0x0cb80020)` |
| 3000 | 1.00 s | **Acceptance run ends by its frame limit with Station Square still running** (187 distinct seam entries called, 76 of them stubs) | `frame limit 3000 reached` |
| 3105–3122 | | Beyond the run: GameMode 5 → 4, the game loads `B_CHAOS0.PRS` (the Chaos 0 fight, not recompiled) into the area slot and `EV002B.PRS`, then calls `0x8C900000` and traps: `indirect call to an address with no function`. That is the first point outside M1's overlay set. | |

The route is the oracle's (title → new Sonic story → Station Square,
`docs/m0-trace.md` §3) with two differences: the intro movie is skipped
instead of played, and there is no VMU, so the file-select screen takes its
"no memory card" path (20 blocks of `SUMMARY.PRS`, as in the oracle run).

## 2. What the runtime does (backend/desktop, hle/)

- **Entry state.** `sa1_ram` is a 16 MB + 64 KB `calloc`; `1ST_READ.BIN`
  goes to `0x8C010000`; r15 = `0x8C00F400` (the loader stub itself fills
  `0x8C00C000`–`0x8C00F400` with its stack pattern), FPSCR = `0x00040001`,
  GBR = 0, VBR = `0x8C000000`. The BIOS vectors `0x8C0000B0/B4/B8/BC/E0`
  hold their own address and `sa1_register_fn` maps each to a native
  syscall handler (`hle/bios/syscalls.c`): sysinfo (ID at `0x8C000068`,
  the five factory bytes `"00110"` the oracle's flash had), flash (erased
  user partition, so the game takes its defaults), GD-ROM drive status
  (paused, GD-ROM present). Then `fn_8c010000(&ctx)`.
- **Frame sync without interrupts.** The game's main loop spins in
  `0x8C051790` until its VBlank handler `0x8C051810` (registered through
  `njSetVSyncFunction` `0x8C605234`) has counted `0x8C754EC0` to zero; the
  adventure field also spins in `0x8C0517F6` until the count drops to a
  limit. Both are wrapped (`recomp/sa1_us_1005/hle_wraps.tsv`): each loop
  turn is one guest frame: the backend hook (input script, limits), the
  KAMUI vblank-in callback the game installs (`0x8C06C1D8`, which samples
  TMU1 and calls the Ninja handler `0x8C604546`), the per-frame callback,
  the syChain handlers (the RTC piece). `njWaitVSync` (`0x8C6050B0`, the
  logo loop) does the same. No asynchronous preemption anywhere.
- **The game's task system.** The game has its own coroutine library at
  `0x8C099A00`–`0x8C099CDA` (36-byte task records, list head `0x8C7A0A08`,
  a guest stack per task); the title, menus and event scripts run as tasks
  that yield once per frame. Yield saves the register context on the guest
  stack and pops the next task's; recompiled code cannot do that (registers
  are C locals). `hle/game/tasks.c` wraps yield, start, kill and
  kill-and-switch: every task is a host coroutine (`sa1_plat_coro_*`:
  Windows fibers, `ucontext` elsewhere, threads with a baton on the PSP),
  the guest register file is saved per task, the list logic stays
  recompiled, and each task keeps its own guest stack region.
- **Code overlays.** The loader hook (`docs/m1-overlays.md` §7) ran for
  real: `ADVERTISE`, `SUMMARY` and `ADV00` were installed by name and their
  tables served every `sa1_call` into the slot. `SUMMARY.PRS` was not in the
  M1 overlay set and was added (file select; the oracle ran 20 blocks of
  it). The movie player `MOVIE.BIN` is not recompiled: `hle/overlay/movie.c`
  registers a native at its entry `0x8CEB0000` that returns -1 (normal end).
- **Shinobi HLE (`hle/shinobi/`).** Memory: `syMalloc` over the game's
  1 MB heap at `0x8CF00000`, carving from the top as the SDK does (the game
  stages whole files at the heap's bottom). Files: `gdFs` over the data
  root with the SDK's 84-byte guest-RAM handles; reads complete at once, so
  `gdFsGetStat` and `gdFsExecServer`, which the game polls, see "complete".
  Pad: 24 records of 52 bytes at `0x8C88E9A4`; the controller must carry
  Maple function `0x01000000` and support bits containing `0x000C000E`, or
  the game treats the port as empty. Timer: `syTmrGetCount` from the
  virtual clock (1.28 counts per µs). Config: stereo, English, no flash
  settings block, no VMU (`bu*` entries answer "no medium"). Chain: a table
  of handlers run per frame. Hardware registers game code touches directly
  (TMU1 profiling timer, PDTRA cable pins, G2 FIFO status, cache control)
  are served by `hle/shinobi/hw.c` through the runtime's MMIO hook.
- **Ninja math (`hle/ninja/math.c`).** Not graphics: matrix stack (with
  the SDK's guest globals, because the game has an inlined `njPushMatrix`
  and reads the stack pointer directly), rotations, translate/scale,
  CalcPoint/CalcVector, Unit/Set/Get/Multi/InvertMatrix, sin/cos/tan/sqrt,
  the distance family, `njSetCamera` and the camera helpers, and the screen
  globals (`njSetScreen`, `njSetPerspective`, `njClipZ`) that game code reads
  for culling. Drawing, textures, lights, fog, KAMUI frame calls and all
  sound entries stay logging stubs (five sound stubs write the output the
  game reads: dummy handles, `ADXT_GetTime`, `ADXT_Create` = NULL so ADX is
  off).
- **Stubs.** Every unimplemented seam entry logs once with its arguments
  and counts hits; the exit report lists the 30 busiest callees. A stub or
  MMIO read repeated two million times within one frame traps; a pure
  guest loop that makes no HLE call is caught by a 10 s watchdog thread
  that walks the main thread's stack (`backend/desktop/backtrace_win.c`).

## 3. Seam entries: real bodies and stubs

Of the 291 seam entries, 234 execute on the oracle route. Bodies this
session:

| Class (`seam_executed.tsv`) | Executed on the route | Real body | Still a stub |
|---|---|---|---|
| Shinobi (system, files, pad, timer, config, VMU, cache; includes the Ninja core entries in `0x8C604500`–`0x8C611100`) | 104 | 53 | 51 (all Ninja render-state/texture entries, plus `syCacheICI` and friends that need nothing) |
| Ninja (math, camera, draw, light, fog, KAMUI frame, chain, sd, ADX glue) | 117 | 55 | 62 (draw, sprite, 2D, light, fog, debug print, KAMUI, sd commands) |
| Sound (ADXCRS/ADXSTM/SJ) | 13 | 4 | 9 |
| **Total** | **234** | **112** | **122** |

113 entries have real bodies (one, `syCfgExit`, is not on the route).
The lists are the `sdk_*` definitions under `hle/shinobi`, `hle/ninja`,
`hle/sound`; the generated `hle/seam_stubs.c` is weak, so a definition
anywhere replaces its stub at link time. Game functions wrapped
(`recomp/sa1_us_1005/hle_wraps.tsv`): the PRS loaders (overlay hook), the
two frame waits, the RTC read, and the five task-system entries.

## 4. Decisions worth knowing

- **Virtual time.** Guest time advances 1/60 s per frame plus the host time
  elapsed inside the frame (capped below a frame). Runs are deterministic
  frame for frame at any host speed (the title walk runs at about 3,000
  frames per second here), and the game's fades and timers behave as at
  60 Hz.
- **Input script.** `--press FRAME:BUTTONS[:HOLD]` sets the controller for
  HOLD frames; the pad server copies it into the guest records. The route
  above needs Start at 900 and A at 1300, 1700 and 2100 (the Start at 500
  is consumed by the title intro).
- **Debug aids.** `--watch ADDR[:OFF...]` logs changes of a guest word (a
  pointer chain is followed, e.g. `0x8c9610cc:44:4` is the title phase);
  `--trace-funcs FILE` with `-DSA1_TRACE_FUNCS=ON` dumps every executed
  host function with the frame it first ran in, which diffed against the
  Flycast trace's first-execution order found the title's missing path;
  the trap hook prints a host backtrace (guest call stack, since functions
  are named by address).
- **Oracle use.** The Flycast trace (`docs/m0-trace.md`) decided the boot
  order of SDK entries, which block-A functions the SDK calls back, the
  movie player's return convention, that `SUMMARY.PRS` is on the route, and
  which ADVERTISE functions the title must run. The register/memory dump
  patch to Flycast was not needed this session.

## 5. Emitter bugs found and fixed

Each has a case in `tests/emitter/run_tests.py` (55 cases now, all pass;
each new case fails on the previous emitter):

| Case | Bug | Where it bit |
|---|---|---|
| `callee_saved_across_indirect` | A register the caller reloads after a call must be stored before it when dirty: `may_write` is a may-set, and a callee containing an unknown indirect call "may write" everything. | SHC's `sprintf` lost its stream pointer in r10 after `putc` through a function pointer; file names came out as garbage. |
| `passthrough_arg` | A callee's live-in registers are live-in of the caller even when the caller never touches them. | The file-loader wrappers passed r4 through untouched and their callers never stored it. |
| `switch_far_target` | A table whose size a compare proves keeps every entry, even beyond the function's discovery end (a tail call then). | The 53-entry character-select mode switch had 4 entries. |
| `branch_entry_in_table` | A "branch"-origin entry stays a function when its address is a 32-bit word anywhere in the image, or when the trace shows a `jsr`/`jmp` through a register reaching it (`recomp/sa1_us_1005/trace_call_targets.txt`, 655 targets, 24 of them branch-origin). | Station Square's object table (in `ADV00.PRS`'s data, so no image scan can see it) pointed at a merged entry, `0x8C060460`. Keeping every *executed* branch entry instead was wrong: a switch head kept standalone loses the compare that bounds its table. |

Cost of the fixes on the main image: `stores_at_calls` 58,514 → 78,703
(+35 %), source 26.68 → 26.94 MB (+1 %); switch entries 1,906 → 2,223;
clang `-Os` x86-64 `.text` 3,354,271 → 3,464,312 B (+3.3 %; compiled in
5 s on 16 jobs). The function count is 4,262 (from 5,343): the seeded
discovery list resolved most of the former 1,113 orphan entries into their
parents, and 22 branch-origin entries are kept because the trace called
them. The MIPS figure is to be re-measured with psp-gcc (§6).

## 6. Open items

- **Next overlay: `B_CHAOS0.PRS`.** The run ends at frame 3122 when the
  game loads the Chaos 0 boss file and calls into it. The same recipe as
  `SUMMARY` (extract, discover, `trace_diff --overlay`, emit, registry)
  applies; `STG01.PRS` (Emerald Coast) follows for M2's exit test.
- **No graphics, so no visual check.** The 32 direct-TA writers are no-ops
  and every Ninja draw entry is a stub; M2 replaces both with the GE backend
  and the TA-vertex translator. The matrix stack and math entries are
  native already (`hle/ninja/math.c`) and should be checked against a
  Flycast register dump once M2 draws something wrong.
- **Sound is off by construction** (`ADXT_Create` returns NULL, `sd*`
  stubs); M3.
- **Flycast register/memory dumps** were not needed; `--watch`,
  `--trace-funcs` and the trace's first-execution order were enough. The
  patch stays as described in `docs/m0-trace.md`.
- **VMU.** All `bu*` entries answer "no medium"; saving is M6 work. The game
  took the no-VMU path of the file select without complaint.
- **Emitted size.** The two call-sync fixes add stores (+35 % stores at
  calls); the MIPS `.text` of the main image should be re-measured with
  psp-gcc before M4 (the x86-64 `-Os` figure is in §5).
- **PSP shape to keep in mind for M4:** the task coroutines need a
  thread-based `sa1_plat_coro_*`; the watchdog, backtraces and
  `--trace-funcs` are desktop-only debug aids; everything else in `hle/` is
  plain C11.
- **Cosmetic.** After a trap the watchdog still fires 10 s later with an
  unsymbolised stack; the trap path should exit at once.
