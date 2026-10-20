# M5 PSP performance (in progress)

Goal (README §8): full speed on the test PSP-3001. This report covers
the console work from 2026-10-19 on, measured on the console over
PSPLink (`tools/psp_run.py`, M4 report §8.1) with `--no-pace`. PPSSPP
models no caches and is not a speed reference here.

## 1. A profiler that works on the console

PSPLink's `thctx` (the first sampler, `psp_run.py --sample-after`) gives
no usable PCs on the 3001's firmware 6.61: the context it prints is not
the thread's (strings and `0xDEADBEEF` fill where the registers should
be, "EPC" values like `0x3f800000`), and at ~500 replies a second it
slowed Station Square from 140 to 530 ms a frame.

`backend/psp/prof/sa1prof.c` replaces it: a kernel module, loaded through
kubridge like the crash screen, that sets a kernel alarm (6.60 NIDs,
`libpspthreadman_kernel_660`) every ~1 ms, jittered, and stores cop0 EPC
(the PC the interrupt stopped) into a buffer the game owns.

    psp_run.py -- --stage ss --no-pace --frames 720 --prof 200:700:prof.bin
    psp_prof.py --pcs build-psp/sa1psp/psplink-run/prof.bin [--lines FUNC]

36,142 samples over frames 200-700 at 1 kHz, frame time unchanged.
`--lines FUNC` breaks a function down by source line (psp-addr2line).
Samples are whatever ran, so PCs outside the PRX are the kernel
(syscalls), other modules or the idle thread. The samples carry no
return address (the interrupted `ra` is not reachable from the alarm
handler), so callers of small helpers are not known.

`psp_run.py --from DIR` runs another checkout's PRX while the running
usbhostfs_pc keeps serving `--build` (the PRX is staged into its
`psplink-run/`): a second worktree builds without the other sessions'
uncommitted edits, and usbhostfs_pc is never restarted.

## 2. Station Square, step by step

`--stage ss --no-pace`, frames 300-600, ms per frame:

| Commit | Change | Frame | TA parse and record |
|---|---|---|---|
| e8c1790 | (start) | 141-146 | 82-87 |
| 448786f | `texture_from_tcw` by a handle map: kamui looked up every textured strip's TCW by walking the 2048 texture slots, **27.5 %** of all samples | 98-101 | 40-42 |
| 68ca26e | (the M3 audio in the build) | 102-106 | 41-43 |
| d5d3e10, e978392 | kamui: one walk cuts each TA buffer into runs of one list, each parse visits its own runs and reads through one host pointer (the three parses walked every buffer whole, reading each vertex word through the guest-address helpers: two thirds of `parse_region`); renderer: one 1/w per vertex (it was divided twice, three times with an offset colour, plus three integer divides by 255) | 88-90 | 28-30 |
| 5c16b92 | the game thread's FPU rounds toward zero, as the Dreamcast (correctness; the baseline below) | 88.33 | 28.4 |
| d4d245a | the hottest game chunks (game_003, game_005) at `-O2`, so `sa1_off` is inlined there (+117 KB) | 84.56 | 28.8 |
| 4fc6352 | `stack_probe` keeps the current coroutine's stack limit instead of looking it up per call | 86.13 (alone) | 28.3 |
| d4d245a + 4fc6352 | both (title_menu.tape PASS on the console) | **82.45-82.49** | 28.9 |

From 5c16b92 on, the frame column is the mean of the three `--stats 150`
windows ending at frames 300, 450 and 600 (two runs each agree within
0.05 ms). Tried and dropped: replaying the display list without pspsdk's
per-draw `sceGeListUpdateStallAddr` syscall measured 1.1 ms worse.

The whole `sonic_ec1` tape to frame 7000 took 679 s at the start, 501 s
after 448786f; the Station Square streets went 150 -> 127 ms, the
Chaos 0 fight 163 -> 139 ms after e978392.

Profile at 88-90 ms (% of all samples): `sa1_off` 11.8, `strip_emit` 9.1,
`parse_region` 7.0, the game's TA writers (`fn_8c037990`, `fn_8c037294`,
`fn_8c037350`, `fn_8c037ccc`, `fn_8c037ee0`) 12, `clip_plane` 3.7,
`memcpy` 3.4 (batch to display list, through the list's uncached alias),
kernel 4 (mostly `sceGeListUpdateStallAddr`, below), the HLE matrix
helpers 3.8, `sa1_seam_hit` 1.6, `stack_probe` 1.4, audio 1.2.

`sa1_off` is the guest-address helper of `sh4_rt.h`: `static inline`,
but the `-Os` game code calls it out of line at every guest access.
Inlining it everywhere would add ~6 MB (222,901 call sites, ~7
instructions each); the chunks holding the hottest game functions can
take `-O2` instead (`SA1_PSP_HOT_CHUNKS`, game_003 and game_005: +117 KB).
The real fix belongs to the emitter (bases known to be RAM, register
allocation).

## 3. Console-only renderer fault: the rooftop's blue wedge

The Station Square opening (`sonic_ec1` ~3180-3260: Sonic on a rooftop
before the night skyline) showed a blue wedge over the roof on the
console only. Bisected on the console with `roof.state`:

| Variant | Wedge |
|---|---|
| default, `--gu-split 0`, `--gu-debug 1` (mip 0), `2` (no mips) | yes |
| `--gu-debug 32` (skip 3D strips that reach the near plane) or `64` (skip guard-band strips) | no |
| skip translucent near-plane strips only / opaque only | no / yes |
| the water's w forced to the far end | yes (darker: fog) |

A probe of the GE draws over a wedge pixel found three: the sky, the
skyline-and-roof backdrop (`station3`, a translucent **2D** quad at 1/w
0.001) and the sea (`sswater01`, from a near-plane vertex, z ~1400
there). Since 694b9e8 translucent 2D strips wrote no depth (for the
character select's text), so the sea behind the backdrop drew over it.
48c2e3a: only the 2D strips at either end of the buffer (1/w < 1e-4,
the text at 0.00002, and the HUD at 1) skip the write.

## 4. Open

- `sa1_off` for good: a guest-memory base register, the emitter knowing
  which bases are RAM (r15, GBR, literal pools); more hot chunks at `-O2`
  meanwhile, chosen by a per-chunk profile.
- The display-list copy through the uncached alias (`memcpy` 3.4 %).
- The game's TA writers and the HLE model path: the GE hardware
  transform lever (README §5, orchestrator backlog).
- The load stalls (loose files): 4 s character select -> Station Square,
  8.5 s at the Chaos 0 transition; `SONICADV.PAK` on the card is
  the maintainer's to deploy (USB mass storage ends PSPLink).
