# sa1-psp: Sonic Adventure (Dreamcast) on the PSP

A **static recompilation** of Sonic Adventure (Dreamcast, US v1.005) to the Sony
PSP. It is not an emulator. The game's SH-4 executable and its code overlays
are translated ahead of time into C, one C function per SH-4 function, and
compiled with psp-gcc. The Katana SDK layer the game calls into (Shinobi,
Ninja, KAMUI, the sound driver) is replaced by native PSP code: sceGu for
graphics, sceAudio for sound, sceIo for files. No PowerVR, AICA or ARM7 is
emulated.

> **Disclaimer.** This is vibecoded: AI coding agents (Claude) wrote
> practically all of it; the author steered, play-tested on real hardware and
> made the calls. Expect AI-written code: verbose, uneven, sometimes
> over-engineered. Judge it on whether it runs, not on whether it's pretty.
>
> One thing that isn't slop: the testing. Recorded input tapes replay the game
> frame-for-frame, and every change had to keep them identical, plus picture
> comparisons and runs on the real PSP. So whatever the code looks like, the
> game behaves like the original.

The original design document (plan, hardware gap, architecture, milestones,
risks) is [`docs/design.md`](docs/design.md). Code and reports cite it as
"README §N"; those section numbers refer to that file.

## Status (paused 2026-10-28)

What works, on a PSP-3001 with ARK-4 CFW:

- **The whole of Sonic's story runs:** all 44 tested places (levels, acts,
  adventure fields, bosses) start and play.
- **Saving works** (the VMU save becomes a Memory Stick file, `docs/vmu.md`).
- **Audio works:** ADX music and voices, and the sequenced sound effects
  played from the game's own sound banks (`docs/m3-audio-report.md`).
- **Station Square runs at full speed:** ~31.5 ms per game frame against
  the 33.3 ms budget for 30 fps.
- **Heavy stages run at full game speed with frame skip.** Game logic keeps
  real time and pictures are dropped. Emerald Coast needs ~61 ms to draw a
  frame, so it shows about 9 pictures a second.

Known issues:

- **Speed in heavy stages and bosses.** Logic alone is over budget in a few
  places, e.g. Sky Deck 3 (~38.5 ms) and Windy Valley 2 (~39 ms); Chaos 0
  and Casino 2 sit at about 30-31 ms. The per-place numbers are in
  `devkit/surveys/stage-survey.md` and `devkit/surveys/fskip-survey.md`.
- **Texture memory on the card is tighter than under PSPLink.** An EBOOT
  started from the Memory Stick gets about 4 MB less user memory than the
  same code loaded as a PRX through PSPLink. At boot the largest free block
  is 7680 KB against 11776 KB, so the texture pool gets 6 chunks instead of
  9. The rest of the blur and dropped detail comes from this. Best lead:
  ARK-4's high-memory option (`devkit/tx2/capacity-notes.md`). To mimic
  the card under PSPLink, pass `--tex-chunks 6`.
- **Media Engine.** The second CPU runs our code, but every Media Engine
  run hangs the next PSPLink reboot, so it is unused (branch `me-wip`).
  Only probe it with someone at hand to power-cycle the PSP.
- PSP replays are not yet deterministic, so tapes are checked bit-for-bit
  on the desktop only.
- `tools/psp_run.py` stages a PRX only when its size or date differs. A
  newer build of the same size can be skipped silently, so touch the PRX
  before a run. The proper fix is to compare content.

## Game data: bring your own disc

- **There is no SEGA data in this repository**: no disc files, no
  recompiled game code, no SDK material. `tools/check_clean.py` (pre-commit
  hook and CI) refuses anything that looks like game data.
- You need **your own** GD-ROM dump of Sonic Adventure **US v1.005**. Every
  address in `recomp/` is specific to that version.
- **Do not ask for game files** or dumps in issues. Such requests will be
  closed.
- **No prebuilt EBOOTs are provided.** The EBOOT contains the recompiled
  game code, so you build your own from your own dump.

## How to build

The pipeline is a set of scripts run by hand; the one-click launcher
(`launcher/README.md`, milestone M7) was never written. The details are in
the docs named at each step.

1. **Toolchain** (`docs/m-1-toolchain.md`). On Windows: Git, CMake, Ninja,
   LLVM/clang, the VS 2022 Build Tools (C++), and Python 3.11 or later. Also
   PPSSPP for quick tests. The PSP SDK is pspdev v20261001 (psp-gcc 15.2)
   in WSL Ubuntu at `/opt/pspdev`, because pspdev ships no Windows build.
   There is also a Docker image: `docker build --output build-ci-psp .`.
2. **Dump.** Put the `.gdi` and its tracks in a folder (outside the
   checkout; the repo's convention is `../private/dump/sa1_us_1005/`).
   Extract the data track's file tree into `data/` next to them with a GDI
   extractor of your choice. Then run
   `python tools/verify_dump.py <dump dir>`, which hashes every file
   against the image. `data/1ST_READ.BIN` must match the SHA-256 in
   `recomp/sa1_us_1005/README.md`.
3. **Function discovery** (`recomp/sa1_us_1005/README.md`). Build
   Dream-Recomp (GPLv2; used as a separate process, never linked) with the
   two small local changes described there. Run it on `1ST_READ.BIN` into
   `recomp/out/dream_seeded/`.
4. **Recompile** (`recomp/emitter/README.md`, `docs/overlays-sonic.md`):
   ```
   python recomp/emitter/emit_sa1.py --bin <dump>/data/1ST_READ.BIN \
       --functions recomp/out/dream_seeded/sa1_us_1005.functions.json --out recomp/out/emitter
   python -I tools/overlays.py build          # every code overlay, ~100 s
   python tests/emitter/run_tests.py          # emitter self-test, no game data
   ```
5. **Compile.** For the desktop debug build (SDL2 + OpenGL fixed function):
   `cmake -S . -B build-desktop -G Ninja -DTARGET=desktop
   -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`, then
   `cmake --build build-desktop`. For the PSP, inside WSL from the checkout:
   `sh tools/psp_wsl_build.sh`. It builds on WSL's own disk and copies
   `EBOOT.PBP` and the PRXs to `build-psp/sa1psp/`.
6. **Memory Stick** (`tools/convert_assets.py`):
   ```
   python tools/convert_assets.py --data <dump>/data --out E:/PSP/GAME/SA1PSP \
       --eboot build-psp/sa1psp/EBOOT.PBP --pack [--fsca <fsca_sh4.bin>]
   ```
   This writes `data/1ST_READ.BIN`, `data/SONICADV.PAK` (the disc tree in
   one indexed file) and the EBOOT. The console's exact `fsca`
   sine/cosine table comes from your own Flycast setup
   (`tools/flycast/fsca_dump.py`). Without it the game uses the host libm.
   It still runs, but strict replays are refused.
   You need a PSP-2000 or later (the EBOOT uses `MEMSIZE=1`) with custom
   firmware. The PSP-1000 is out of scope.

For development on a PSP over USB, see `tools/psplink/README.md` and
`tools/psp_run.py`. Desktop replays and their checks are in
`tests/replay/README.md`.

## Unlanded branches

Each branch passed the local CI. None has passed its PSP timing gate. The
gate is a timed comparison of base and candidate on the console, run by
`devkit/gates/run_gates.py` (see `devkit/README.md`). Before landing a branch:
rebase it on `main`, rebuild it against the current `tools/psp_hot_order.txt`,
check the code padding, and rerun the tapes with `--exact` and the CI.

| Branch | What | Desk status | PSP gate (bar to land) |
|---|---|---|---|
| `qb-mixer` | Sound mixer inner loop in registers, ADPCM decode inline | Bit-exact (sound test bit-equal, tapes `--exact`) | Casino 2 + Chaos 0; GO at -1.0 ms per logic turn in Casino 2 |
| `qe-dispatch` | Indirect calls and repeated seam entries without a call frame | 4 tapes byte-identical to its base | Chaos 0 + Sky Deck 3; any measured gain. Each arm is pinned to its own build, so deltas under ~1.5 ms are layout noise |
| `d2-preserved` | Emitter: push/pop-matched r8-r14 count as preserved (fewer register reloads, 26,672 -> 19,052) | 4 tapes + 44 places identical | Any gain; same layout-noise caveat |
| `d2-walker` | Native Chaos 0 motion walker (`fn_8c040784`); kill switch `--math-exact 8c040784` | Exact over 4 tapes and 44 places; desktop Chaos 0 -4.6 % | -1.0 ms in Chaos 0 plus one more motion-heavy place |
| `div-helpers` | Native SHC integer divide helpers; kill switch `--math-exact 8c10ca5c,8c10cb10` (stacked on `d2-walker`) | 6.43 G oracle cases, 0 differences; desktop Windy Valley 2 -4.85 % | Any measured gain in Windy Valley 2; lands after the walker |
| `me-wip` | Media Engine mixer probes and a work-in-progress `snd_core` | Committed as-is at the pause, not tested as a whole | Do not land; take parts from it |

## Where to start (for a successor)

In order of value:

1. **Card memory.** Find the ~4 MB the card EBOOT loses against PSPLink
   (ARK-4 high memory is the lead). Three more texture chunks hold the
   whole Station Square / Emerald Coast set.
2. **Run the PSP gates** for the five speed branches above
   (`devkit/gates/`, ~3 h, resumable after a wedge). Land each one that
   passes. Fix the `psp_run.py` staging check first, and distrust older
   single-run A/B numbers.
3. **Soak test.** Run a long poked level chain on the PSP at card capacity
   (`devkit/tx2/`). Make PSP replays deterministic.
4. **Media Engine.** Moving the audio mixer to the second CPU is a real
   lever. Solve the reboot hang first.
5. **Bigger speed work.** A native model-draw path for Emerald Coast: about
   48 % of its frame is in ~18 functions (`devkit/surveys/`,
   `devkit/profile/`). Also a native Ninja matrix stack.

Rules that kept the port sane: measure before optimizing, and keep
console-identical changes first. Behaviour-changing ones go last, behind a
toggle and off by default. Never stop a PSPLink host while a PSP is
connected, because it wedges the console until a power-cycle.

## Developer kit

[`devkit/`](devkit/README.md) holds the working tools from development: the
PSP gate runner, the speed tracker (with a chart page), the stage survey,
the branch harnesses, the chain-test recipe, profiles, and extra input
tapes. They assume a `private/` work folder next to the checkout.

## License

MIT (`LICENSE`). GPL tools (Dream-Recomp, Flycast, pspdev) are only run as
separate processes. Sonic Adventure is © SEGA; this project is not affiliated
with or endorsed by SEGA.

## Maintainer wanted

The project is paused and looking for someone to take it over.
Open an issue if you are interested.
