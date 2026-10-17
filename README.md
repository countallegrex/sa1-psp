# sa1-psp — Sonic Adventure (Dreamcast) on PSP

A static-recompilation port of Sonic Adventure (Dreamcast, US v1.005) to the
Sony PSP. This repository contains **only original code and configuration**:
no game data, no recompiled game code, no Sega SDK material. You bring your own
disc dump; the launcher turns it into a PSP EBOOT on your machine.

## 0. End product and legal model

**End product:** a one-click launcher. You point it at a GD-ROM dump of Sonic
Adventure US v1.005 (`.gdi` + tracks). It extracts the executable, recompiles
it, compiles the result with a bundled PSP cross-compiler, links it with the
runtime in this repo, converts the assets, and writes a ready-to-copy
`PSP/GAME/SA1PSP/` folder for the Memory Stick. Nothing game-derived ever
leaves the user's machine, and nothing game-derived is ever in this repo.

This is the only design under which the repository can be public. It means the
launcher is a *build pipeline*, not a pre-built game:

```
dump (.gdi)  ──►  extract 1ST_READ.BIN  ──►  recompile SH-4 → C  ──►  psp-gcc
                                                                        │
assets (.PRS/.PVR/.ADX/.MLT/.AFS)  ──►  convert/downscale  ──►  ms0:/PSP/GAME/SA1PSP/
                                                                        │
                                          runtime + HLE (this repo)  ──►  EBOOT.PBP
```

**Segregation rules (enforced, not just intended):**

| Lives in this repo (public) | Lives in `..\private\` (never committed) |
|---|---|
| HLE runtime, backends, launcher, asset *converters*, build scripts | The disc dump, archives, extracted files |
| Recompiler configuration: address → name → native/recompile tables. Addresses and function names are facts about the binary, not the binary. | Recompiled C output, compiled objects, EBOOTs |
| Format documentation written by us | SA Tools binaries and the split project (third-party tooling, pulled by the user, not vendored) |
| Tests that run against *user-supplied* data by path | Any Sega SDK material, if ever obtained |

- `.gitignore` blocks the obvious (`*.bin`, `*.gdi`, `*.prs`, `*.pvr`, `*.adx`,
  `*.mlt`, `*.afs`, `*.sfd`, `*.pbp`, `recomp/out/`, `build*/`).
- `tools/check_clean.py` runs as a pre-commit hook and refuses any staged
  file that looks like game data (by extension, by size > 512 KB, or by
  containing Dreamcast/Katana signatures). Install with
  `tools/install_hooks.ps1`.
- GPL tools (Dream-Recomp, Flycast, the pspdev toolchain) are invoked as
  separate processes or downloaded by the launcher; nothing GPL is linked
  into the EBOOT, so this repo can stay MIT. If M0 shows the recompiler's
  emitted code needs GPL runtime headers, we write our own emitter rather
  than relicense.
- Author identity for the project is **CountAllegrex**.

## 1. Thesis

The game logic of Sonic Adventure lives in a 6.4 MB SH-4 executable
(`1ST_READ.BIN`). We do not rewrite it. We translate it once, ahead of time,
into C, and we run that C on the PSP. The one thing we *do* rewrite is the
Sega Katana SDK layer the game calls into (graphics, sound, disc, pad, system).
That layer becomes native PSP code.

The hard constraint is the PSP, not the recompiler. A Dreamcast has 16 MB RAM,
8 MB video RAM, 2 MB sound RAM and a 200 MHz SH-4 with a vector FPU. A
PSP-2000 has 64 MB total and a 333 MHz single-issue MIPS. Every public
Dreamcast recompiler (Dream-Recomp, dcrecomp, DreamcastRecompiled) keeps the
Dreamcast *hardware* emulated underneath the recompiled code: a 16 MB RAM
array, an 8 MB VRAM array, the PowerVR tile accelerator decoded from display
lists, and the sound driver running on an emulated ARM7. That design is why
they work on PC and why it cannot work on PSP. So:

> **The seam is the SDK API, not the hardware.** Recompiled code above the
> seam, native PSP code below it. No PowerVR, no AICA, no ARM7, no VRAM.

This is the same shape as the N64 recompilation ports, which replace libultra
calls with native code rather than emulating the RCP.

## 2. What we are porting (measured, 2026-10-16)

Source: Sonic Adventure, US v1.005, GD-ROM dump, extracted locally to
`..\private\dump\sa1_us_1005\data\` (outside the repo).

| Item | Fact |
|---|---|
| `1ST_READ.BIN` | 6,735,488 bytes, **already descrambled** (starts with the Katana SDK library-handle startup stub, loads at `0x8C010000`). |
| Code overlays | Measured by the M0 trace (`docs/m0-trace.md` §5): area code (`STGnn`, `ADVnn`, `B_*`, `MINI*`, `ADVERTISE`, `SUMMARY` `.PRS`) loads at `0x8C900000`, event code (`EVnnnn.PRS`) at `0x8CB80000`, the movie player (`MOVIE.BIN`) at `0x8CEB0000`. 29 % of the code executed on the Emerald Coast route is in these files, not in `1ST_READ.BIN`. |
| Frame rate | 30 fps on Dreamcast. That is the target, not 60. |
| SDK | Katana SDK (`KATANA` and `Ninja` strings present): Shinobi (`sy*`, `gd*`, `pd*`), Ninja (`nj*`), Katana sound library + Manatee ARM7 driver. |
| Known data layout | `..\private\sa_tools\project\sa_tools_sa1\1st_read_data.ini` (94 KB, from X-Hax SA Tools) gives addresses of models, landtables, object lists, textures, in the executable. |

Disc assets (2,834 files, 973 MB):

| Format | Count | Size | What it is | PSP plan |
|---|---|---|---|---|
| `.ADX` | 109 | 433 MB | BGM and voice streams | Keep. Decode on CPU; it is cheap. Optionally re-encode at lower rate to save card space. |
| `.SFD` | 10 | 175 MB | Sofdec FMV (intro, endings) | Cannot be played natively. Convert offline to PMF, or skip with a still frame. Late milestone. |
| `.AFS` | 2 | 132 MB | Archives (voice banks) | Keep; read entries directly from the card. |
| `.PRS` | 849 | 125 MB | Compressed level geometry, models, textures (PVM inside); about 300 of them (`STG`, `ADV`, `B_`, `MINI`, `EV`) also carry SH-4 code that must be recompiled | Keep compressed on the card. PRS decompression is a tiny LZ routine; the game's own copy runs recompiled, or we HLE it. |
| `.MLT` | 123 | 50 MB | Manatee sound banks (sequenced SFX, tone data) | The hardest asset. See §6 Audio. |
| `.PVR` | 442 | 29 MB | Loose PowerVR textures (many VQ-compressed) | Convert offline to PSP formats (CLUT8 / DXT1 / RGBA4444), downscaled where needed. |
| `.BIN` | 536 | 12 MB | Misc binary (events, camera, etc.) | Keep. |
| `.GIF/.HTM/.JPG` | 727 | 12 MB | Browser manual | Drop. |

## 3. The hardware gap, and what each gap forces

| | Dreamcast | PSP-2000/3000/Go (CFW) | Consequence |
|---|---|---|---|
| CPU | SH-4 200 MHz, 2-way superscalar, 4x4 vector FPU (`ftrv`) | Allegrex 333 MHz, single issue, VFPU | Recompiled code costs 2–4x the original. Net: roughly **half to a third** of the SH-4's throughput for game logic. The per-frame transform/lighting work must leave the recompiled path and go native (§5). |
| Main RAM | 16 MB | 64 MB total, ~52 MB usable | Fits only because VRAM and sound RAM are *not* emulated. **PSP-1000 (32 MB) is out of scope.** |
| Video RAM | 8 MB, PVR textures incl. VQ | 2 MB EDRAM; GE can also texture from main RAM | Framebuffers in EDRAM, textures in main RAM, converted and downscaled offline. |
| GPU | PowerVR2 tile deferred, order-independent transparency, modifier volumes | GE immediate, fixed function, no shaders, Z-buffer | Draw order must be sorted by us (opaque, then punch-through, then translucent back-to-front). Shadows via modifier volumes become stencil or projected quads. |
| Sound | AICA 64 ch + ARM7 running the Manatee driver, 2 MB sound RAM | sceAudio, 8 PCM channels, software mixing on CPU or Media Engine | No ARM7. Sequenced SFX need an HLE sequencer or pre-rendered PCM (§6). |
| Storage | GD-ROM 1.2 GB, ~1.5 MB/s | Memory Stick, ~2–4 MB/s | Faster than the original. Loading is not a problem. |
| Save | VMU | Memory Stick file | Trivial. VMU Chao minigame out of scope. |
| Screen | 640x480 | 480x272 | Render at native PSP res. Half the pixels, which helps the GE. |

## 4. Architecture

```
+--------------------------------------------------------------+
|  Recompiled game code (generated C, one function per SH-4    |
|  function; guest registers are locals; guest RAM is a 16 MB  |
|  byte array)                                                 |
+--------------------------------------------------------------+
|  SDK seam: every call into a Katana SDK entry point is       |
|  redirected by the recompiler to a native function with the  |
|  same signature and the same guest-visible side effects      |
+----------------+----------------+------------------+---------+
| Shinobi HLE    | Ninja HLE      | Sound HLE        | PRS/misc|
| sy* gd* pd*    | nj* (models,   | sd*/Manatee cmds | helpers |
| malloc, files, | matrices,      | ADX streams      |         |
| vsync, timers  | textures, draw)|                  |         |
+----------------+----------------+------------------+---------+
|  Platform backend (one per target)                           |
|   desktop/: SDL + OpenGL fixed-function, stdio, stdout log   |
|   psp/:     sceGu, sceAudio, sceCtrl, sceIo, pspsdk          |
+--------------------------------------------------------------+
```

Three rules that make this work:

1. **One source tree, two backends.** The desktop build exists to debug with a
   real debugger and to iterate in seconds. It must stay *PSP-shaped*: fixed
   function rendering, 32-bit-clean, no feature the GE does not have. If a
   trick works on desktop only, it is a bug.
2. **Guest memory is one flat 16 MB array.** Guest address `0x8Cxxxxxx` maps
   to `ram[addr & 0xFFFFFF]`. No MMU, no VRAM mirror, no store-queue
   emulation. Ninja HLE reads model and texture data *out of that array* with
   the layouts documented by SA Tools.
3. **The game never touches hardware registers.** Anything that did (TA
   writes, PVR registers, AICA memory) was inside the SDK and is now ours.
   If M0 shows the game writes PowerVR vertex data *directly* (bypassing
   Ninja), that path gets a small TA-vertex-to-GE translator, still with no
   VRAM emulation. That is a bounded fallback, not a return to LLE.

### Finding the seam

Katana SDK functions inside `1ST_READ.BIN` have no symbols. Identification
sources, in order of preference:

- Byte-pattern signatures built from Katana SDK static libraries (`.a`
  archives of Shinobi/Ninja/sound), if a matching SDK version is available
  locally. This is the FLIRT approach and gives names for free.
- Cross-reference with `1st_read_data.ini`: the game's data tables point at
  the game-side callers, which bound where SDK code ends.
- Behavioral identification in the emulator: hook known hardware addresses
  (TA FIFO `0x10000000`, PVR regs `0xA05F8000`, AICA `0xA0700000`, GD-ROM,
  Maple) in Flycast and record which SH-4 functions touch them. Those
  functions and their callers up to the first game-side frame are the SDK.
- `doldecomp/sadx` (GameCube) for *game-side* function naming by structure,
  not for the SDK.

The output of this step is `port/recomp/sa1_us_1005.toml` (or whatever the
chosen recompiler uses): address → name → `native` or `recompile`.

### Recompiler

Use **phobos665/Dream-Recomp** (GPLv2, most mature public tool: Crazy Taxi
playable, 80–90 % static function discovery, interpreter fallback for the
rest) for *function discovery and as the first code generator*. Its runtime
is not used on PSP; only its emitted C++ is. Two things to verify in M0,
because either can force us to own the emitter:

- **32-bit cleanliness.** It targets x86-64/ARM64 and C++20. The PSP is
  32-bit MIPS with GCC from pspsdk. Emitted code must not depend on 64-bit
  `size_t`, host pointer width, or x86 intrinsics.
- **Native-call hooks.** We need a per-function "do not recompile, call this
  native symbol instead" mechanism. If it only supports whole-program
  hardware emulation, we patch it.

**Decided at M0 (2026-10-16, `docs/m0-report.md` §5): we write our own
emitter.** Dream-Recomp stays in use for discovery only (function list,
switch tables, coverage report), run as a separate process. Its emitted
code is 32-bit clean and psp-gcc compiles it, but it `#include`s four GPLv2
runtime headers that carry the instruction semantics and the call ABI, has
no native-call hook in the emitter, routes every guest load/store through a
virtual call, and comes out at 13.9x the SH-4 size on MIPS. SH-4 has ~200
instructions, fixed 16-bit encoding; `sp00nznet/dcrecomp` (MIT, plain C11)
is a readable reference and `Dream-Recomp` a second one. This is a known
quantity, not a research problem.

### Interrupts and timing

The game is single-threaded with VBlank and timer interrupts owned by
Shinobi. HLE: the native frame loop sets the "VBlank happened" state the
game polls, drives Ninja's frame counter, and runs the game's registered
VBlank callback (if any) at frame boundaries. No asynchronous preemption of
recompiled code.

## 5. Performance model

Where a Dreamcast frame of SA1 goes, approximately: model transform, lighting,
clipping and TA submission inside Ninja (the biggest CPU consumer), then
collision and physics, then object AI, then the PVR rendering in parallel on
the GPU.

On PSP:

- Ninja's T&L is **native** (VFPU matrix ops, GE does the rest). The
  recompiled code never sees it. This removes the largest cost from the slow
  path.
- Collision, physics, object logic run **recompiled**. Expect 2–4x the SH-4
  cycle count. At 30 fps that is the budget to watch; measure in M2 with
  per-function cycle counters on desktop, then on hardware.
- GE pixel cost at 480x272 is lower than the PVR's at 640x480.

Expectation to state plainly: 30 fps in simple areas, dips in Station Square
and Casinopolis, before optimization. Optimization levers, in order: drop
guest memory high-water mark, `-Os` plus hot-function `-O2`, VFPU in the Ninja
HLE, LOD and draw-distance cuts in the HLE layer (invisible to game logic).

## 6. Audio

Two different problems:

- **Streams (BGM, voices, `.ADX`, `.AFS`):** ADX is 4-bit ADPCM at ~1/4 the
  cost of MP3 to decode. Decode on the main CPU in a 2-channel ring buffer to
  `sceAudio`. Straightforward.
- **Sequenced SFX (`.MLT` banks):** the game sends commands to the Manatee
  driver on the ARM7 ("play tone X from bank Y at pitch/pan"). Options:
  1. **HLE sequencer:** implement the subset of Manatee commands SA1 uses
     and a small PCM/ADPCM mixer. Correct and compact, more work.
  2. **Pre-rendered SFX:** capture each sound id from an emulator run to a
     PCM clip, ship clips on the card, map command → clip. Fast to ship,
     misses pitch/effects variation.
  Start with 2 to get sound in M3, migrate to 1 if quality demands it.

## 7. Memory budget on PSP-2000 (to be measured, not assumed)

| Region | Budget | Note |
|---|---|---|
| Guest RAM | 16 MB | Can shrink to the measured high-water mark of the game heap. |
| Recompiled code | **3.66 MB measured (M1, own emitter), 3.88x the SH-4 bytes; target was ≤ 4.7 MB** | Measured at M1 (`docs/m1-emitter-report.md`): block A (0.94 MB of SH-4; the SDK block is replaced) compiles to 3,655,809 B of MIPS `.text` at `-Os` with `recomp/emitter/`, against 13.1 MB (13.9x) for Dream-Recomp's LLE-shaped output at M0. Registers are C locals, RAM is one array, and call sites sync only the registers the callee reads or writes (0.38 memory operations per instruction). Further levers: drop never-executed functions found by tracing, trust the SH ABI for r8–r15 at every call, per-level code paging. |
| Textures (main RAM) | 4–8 MB | Offline downscale; CLUT8 or DXT1. |
| Audio buffers + SFX | 2–3 MB | |
| Runtime, stacks, I/O | 2 MB | |
| EDRAM | 0.75 of 2 MB | Two 480x272 16-bit framebuffers + Z. |

If this does not fit in 52 MB after measurement, the next step is per-level
code paging (load only the functions a level needs), which SA1's level
structure makes tractable.

## 8. Milestones

Each one has a single exit test. Do not start the next before the test passes.

- [x] **M-1 Toolchain.** Windows: git, CMake, Ninja, LLVM/clang or MSVC,
      Python deps. PSP: pspsdk (`pspdev`), PPSSPP for iteration. Exit: a
      hello-world builds for both desktop and PSP from one CMake tree.
      *Passed 2026-10-16; see `docs/m-1-toolchain.md`.*
- [x] **M0 Function map.** Run Dream-Recomp discovery on `1ST_READ.BIN`;
      trace a Flycast playthrough (title → Station Square → Emerald Coast) to
      list every executed function; identify SDK entry points. Exit: a
      report with discovered-vs-executed coverage ≥ 95 %, a named list of SDK
      entry points, and a measured emitted-code size. Decide here whether to
      keep Dream-Recomp's emitter or write ours.
      *Passed 2026-10-16. Static: 9,568 functions, 88.4 % of code bytes
      decoded, 289 SDK entry points, 13.1 MB MIPS measured, decision: own
      emitter (`docs/m0-report.md`, `docs/seam.md`). Dynamic: Flycast 2.7
      trace of title → Station Square → Emerald Coast act 1 → Station
      Square; 97.76 % of the 2,631 executed call targets in `1ST_READ.BIN`
      were found statically (100 % with the 68 trace seeds now in the
      config); 5,881 discovered functions never ran on that route
      (`docs/m0-trace.md`). New scope found by the trace: 29 % of the
      executed code is in disc overlays (`STG`/`ADV`/`B_`/`EV*.PRS`,
      `MOVIE.BIN`) that M1 must recompile too.*
- [ ] **M1 Desktop boot, no graphics.** Recompiled code + Shinobi HLE
      (memory, files from the card layout, pad, vsync, timers) + Ninja stubs
      that log. Exit: the game initialises, loads the title screen data, then
      loads Station Square without crashing, visible in the log.
      *In progress (2026-10-16): the emitter is done (`recomp/emitter/`,
      `docs/m1-emitter-report.md`): block A becomes 5,295 C functions, 3.66 MB
      of MIPS at `-Os` (3.88x, target ≤ 5x), compiles with clang and psp-gcc,
      51 snippet tests pass locally and run in CI, 408 indirect sites go through the function
      table, no non-local returns needed. `hle/seam.h` holds the 289 seam
      prototypes with logging stubs. Still to do: the Shinobi HLE bodies, a
      boot `main`, and discovery + emission for the overlays the title
      screen and Station Square load (`ADVERTISE.PRS`, `ADV00.PRS`, the
      opening `EV*.PRS`; `docs/m0-trace.md` §5).*
- [ ] **M2 Desktop graphics.** Ninja HLE on OpenGL fixed function, reading
      models and textures from guest RAM. Exit: Emerald Coast completed with
      a controller, draw order correct, no desktop-only features.
- [ ] **M3 Desktop audio.** ADX streaming plus pre-rendered SFX. Exit: BGM
      and jump/ring sounds in Emerald Coast.
- [ ] **M4 PSP boot.** Same tree built with pspsdk: 16 MB guest array,
      sceGu Ninja backend, sceCtrl, sceIo from `ms0:/`. Run in PPSSPP first.
      Exit: title screen on PPSSPP, then on hardware.
- [ ] **M5 PSP playable.** Exit: Emerald Coast completed on real hardware;
      profile recorded (fps, frame time split, RAM high-water mark).
- [ ] **M6 Complete.** All stories, saves, Chao Garden, cutscenes (PMF or
      stills), texture downscale pass, framerate pass. Exit: a full Sonic
      story on hardware without a crash.
- [ ] **M7 Launcher.** One executable: pick dump → EBOOT + Memory Stick
      folder, with toolchain and recompiler fetched on first run and every
      step resumable. Exit: a fresh Windows machine with only the launcher
      and a dump produces a working `PSP/GAME/SA1PSP/`. The GitHub repo stays
      **private** until this passes `tools/check_clean.py --all`, and it is
      made public only after the maintainer explicitly says so at that point.

## 9. Risks, ranked

1. **Emitted code size on PSP.** Could exceed RAM outright. Measured in M0;
   mitigations in §7.
2. **The SDK seam is leaky.** The game may call Ninja internals or write TA
   data directly in places (custom effects, Chao). Mitigation: TA-vertex
   translator for those paths; keep it bounded.
3. **Recompiled game logic too slow for 30 fps.** Mitigations in §5.
   Floor: a stable 20 fps is still a playable port.
4. **Sequenced SFX fidelity.** Mitigation: pre-rendered clips first.
5. **Recompiler is not 32-bit clean / lacks native hooks.** Mitigation: own
   emitter (§4).
6. **FPU semantics.** SH-4 `fsca`, `fsrra`, `ftrv`, FPSCR precision mode.
   Implement in C with explicit precision; validate against Flycast traces
   on desktop before PSP.
7. **FMV.** Sofdec is not decodable on PSP in the time budget. Skip or
   convert offline. Cosmetic.

## 10. Layout

```
sa1-psp/                      this repo (public)
  CMakeLists.txt              one tree, options: -DTARGET=desktop|psp
  recomp/
    sa1_us_1005/              recompiler config, seam table (committed)
    out/                      generated game code (ignored, never committed)
  hle/                        platform-agnostic SDK HLE: shinobi/, ninja/, sound/, prs/
  backend/
    desktop/                  SDL + OpenGL fixed function, stdio
    psp/                      sceGu, sceAudio, sceCtrl, sceIo, EBOOT packaging
  launcher/                   the one-click pipeline (GUI + steps), downloads
                              toolchain/recompiler on first run
  tools/                      check_clean, trace parser, signature builder,
                              texture converter, SFX pre-render, PMF conversion
  docs/                       milestone reports, format notes, seam findings
  tests/                      run against user-supplied data by path only

../private/                   never committed
  dump/sa1_us_1005/           extracted GD-ROM (data/1ST_READ.BIN etc.)
  archives/                   original zip, rar, 7z, skeleton EBOOT
  sa_tools/                   X-Hax SA Tools binaries + split project
```

## 11. Working rules

- Version-locked to **US v1.005**. Addresses in `recomp/` are specific to this
  dump. Nothing from SARecomp's PAL v1.003 build transfers by address.
- Desktop build is a debugging tool, not a product. Every feature must be
  expressible in the PSP backend before it is merged.
- Measure before optimizing: every milestone report carries numbers
  (coverage, code size, frame time, RAM high-water).
- Scratch and generated files never go in `port/`; disc data never leaves the
  dump folder.
- CI (`.github/workflows/build.yml`) builds the PSP target in the pinned
  pspdev image and the desktop target on Windows, and runs
  `tools/check_clean.py --all` on every push and pull request. Keep it green.
  No CI job may ever need game data. Local Docker build:
  `docker build --output build-ci-psp .` (see `docs/m-1-toolchain.md`).

## 12. Reference points (checked 2026-10-16)

| Project | Use to us | Caveat |
|---|---|---|
| `phobos665/Dream-Recomp` (GPLv2) | Function discovery, first emitter, BIOS/GD HLE design | LLE runtime (Vulkan PVR2, emulated ARM7) is not reusable on PSP. |
| `sp00nznet/dcrecomp` (MIT) | Clean C11 SH-4 → C reference if we write our own emitter | Very early; AICA stub, basic triangles. |
| `sonicfreak1337/SARecomp` | Proof SA1 recompiles and runs; Sonic-side runtime design | Recompiler (KatanaRecomp) is **not public**; repo consumes a private binary SDK. PAL v1.003 only. Dev on hold since 2026-09-24. |
| `X-Hax/sa_tools` (local copy) | SA1 data formats and addresses | Addresses are for this exact version; good. |
| `doldecomp/sadx` | Game-side function naming by structure | GameCube; not an address source. |
| `flyinghead/flycast` | Ground truth: traces, hardware-touch logging, audio capture | GPLv2, used as a tool only. |

## 13. Local state (2026-10-16)

- Dump extracted at `..\private\dump\sa1_us_1005\`, `1ST_READ.BIN` descrambled.
- `..\private\archives\EBOOT_skeleton.PBP` is a 176 KB skeleton titled
  "SA1 PSP", no game code.
- Installed on Windows: Git 2.55, GitHub CLI 2.102, Python 3.14,
  CMake 4.4.4, Ninja 1.13.2, LLVM 23.1.3, VS 2022 Build Tools 17.14 (C++),
  PPSSPP 1.20.4, WSL 2.7.13.
- Installed in WSL (Ubuntu 26.04.1, everything as root): pspdev v20261001
  at `/opt/pspdev` (psp-gcc 15.2.0), cmake 4.2.3, ninja 1.13.2. The PSP build
  runs in WSL because pspdev ships no Windows build.
- M-1 passed: desktop hello runs; `build-psp/EBOOT.PBP` (858 KB, unstripped)
  boots in PPSSPP, and its 16 MB malloc succeeds. Build commands are in
  `docs/m-1-toolchain.md`.
- M0 static work done: Dream-Recomp cloned and built in
  `recomp/dream-recomp/` (ignored), its output in `recomp/out/` (ignored);
  see `recomp/sa1_us_1005/README.md` for the two local patches it needs.
- Flycast 2.7 in `..\flycast\`: the official release in `release-2.7\`,
  and a trace build (v2.7 + a local logging patch, MSVC) in `trace-2.7\`,
  started with `run_trace.bat`. The M0 trace is
  `..\flycast\trace-2.7\sa1trace.txt`; method in `docs/m0-trace.md`.
- M1 emitter done: `python recomp/emitter/emit_sa1.py --bin <1ST_READ.BIN>
  --functions recomp/out/dream/sa1_us_1005.functions.json` writes
  `recomp/out/emitter/` (26.8 MB of C, 32 chunks) in about a minute;
  `python tests/emitter/run_tests.py` is the emitter's test suite.
