# M4 PSP boot

Exit test (README §8): the same tree built with pspsdk boots to the title
screen, in PPSSPP first, then on the test PSP-3001.

Status: **PPSSPP passed 2026-10-19; hardware pending** (the card is
staged, §8). Everything below was measured with
`build-psp/sa1psp/EBOOT.PBP` (psp-gcc 15.2.0, pspdev v20261001, game code
`-Os`, HLE and backend `-O2`), the US v1.005 dump, PPSSPP 1.20.4 (default
settings: PSP-2000/3000 model, JIT) on Windows 11.

## 1. What runs

| Step | Result |
|---|---|
| hello | M-1's EBOOT still builds (`build-psp/EBOOT.PBP`, CI's artifact) |
| recompiled boot, headless | `--headless --frames 900`: `SEGALOGO.PRS` at frame 10, `ADVERTISE.PRS` installed at 202, the title textures at 332, exit 0 at 900 |
| title rendered | Sega logo, then the title from frame 344: the same picture as the desktop build (screenshot not included in the public repository) |
| `title_menu.tape` | `--replay title_menu.tape --check title_menu.check --no-pace`: **`check: PASS at frame 3000: 0/7 primary failed, 0/25 secondary drifted`**, the desktop's verdict; Station Square installed at 2458 and drawn through its opening event (matches the desktop's frame) |
| hardware | pending (§8) |

The tape replays in strict mode, so the PSP build reproduces the desktop's
run frame for frame: the same game modes at the same frames (1 at 11, 8 at
201, 11 at 203, 16 at 2443, 5 / level 26 / act 3 at 2444), the same
position, rings and lives at every secondary row.

## 2. Memory map and numbers (README §7)

Measured in PPSSPP by the EBOOT's own report (`mem` lines in
`sa1psp.log`; the heap's largest free block is found by halving mallocs
within user RAM). PPSSPP's user memory is larger than the PSP-3001's:
39.4 MB of heap at start, against an estimated 33.8 MB on the console
(M-1 measured 50.7 MB usable beside hello's own 0.84 MB image; less this
EBOOT's 17.0 MB image and the 0.76 MB the kernel keeps outside the heap).
The free column is that estimate less the heap in use; §8 replaces it with
the console's own numbers.

| Region | Size | Where | Note |
|---|---|---|---|
| EBOOT `.text` | 12,030 KB | user RAM at 0x08804000 | recompiled main image 3,862 KB (+ 34 KB function table), **recompiled overlays 7,916 KB (57 files, all linked)**, HLE 163 KB, backend 38 KB, libc/pspsdk the rest |
| EBOOT `.bss` | 4,822 KB | user RAM | display list 2,048 KB, coroutine stacks 1,504 KB (root 256 KB + 39 x 32 KB), vertex batches 240 KB, HLE statics 638 KB |
| Guest RAM | 20,608 KB | heap | 16 MB + the 2 MB TA list area (0x8D000000) + 64 KB slack + the 2 MB null page + 64 KB (`SA1_COMPACT_RAM`, §3) |
| Textures, title | 1,037–1,355 KB (17–21 textures) | heap | 16-bit / CLUT, swizzled, with mip levels |
| Textures, Station Square | 8,834–8,962 KB (972 textures, 50 mirror copies) | heap | the desktop counts 5.99 MB for the same set without mips |
| Frame buffers + Z | 816 KB | EDRAM | 2 x 480x272 RGB565 (stride 512) + 16-bit Z |

| Point | Image | Heap in use | Resident | Free on the console (est.) |
|---|---|---|---|---|
| boot (frame 0) | 17,002 KB | 20,708 KB | 37.8 MB | 13.1 MB |
| **title (frame 1000)** | 17,002 KB | 22,069 KB | **39.1 MB** | **11.7 MB** |
| Station Square (frame 2900) | 17,002 KB | 29,882 KB | 46.9 MB | 3.9 MB |

Against README §7's sum (16 + 3.66 + 0.77 + 6 + 3 + 2 = 31.4 MB) the
differences are the recompiled overlays (7.9 MB of native code for every
overlay, where §7 counted the 0.77 MB of guest bytes one overlay occupies),
the guest RAM's list area and null page (+4.1 MB), and the mip levels
(+~2.9 MB in Station Square). The title fits with 11.7 MB to spare; Station
Square with 3.9 MB, which is the first thing M5 has to win back (§7).

Stack high water on the whole tape: root 2 KB of 256 KB, tasks 1 KB of
32 KB (painted stacks, `coro_psp.c`).

## 3. Memory model

- **Guest RAM** is one 20 MB `memalign` block. The desktop maps the whole
  32 MB window the 25-bit mask can reach, because a few accesses to SH-4
  control registers through a register mask into its top. The PSP cannot
  spend 14 MB on that, so `SA1_COMPACT_RAM` (on for `__psp__` in
  `sh4_rt.h`) allocates RAM + the list area + the null page and sends every
  masked offset past RAM + list area to the null page, where those
  accesses already behave as on the desktop (a read sees the last write).
  The cost is one compare per guest access; constant addresses still fold.
- **Coroutines**: the game's task system runs on our own stacks as on the
  desktop (`backend/psp/coro_psp.c`): the same 40 slots (slot 0 the root),
  one static region at a fixed address, and a context switch in Allegrex
  assembly that saves what the PSP's EABI keeps across calls (s0–s7, fp,
  ra, gp, $f20–$f31). The kernel does not care where a user thread's sp
  points, so no thread state follows the switch. No MMU, so no guard pages:
  the stacks are painted, `sa1_depth_hook` traps 5 KB before a slot's end,
  and the report measures high water.
- **Tail calls**: psp-gcc 15 has `musttail`, so `SA1_TAIL` keeps the
  guaranteed jumps (a plain `j`, checked at `-O0` and `-Os`).
- **Code**: everything is linked into one ELF EBOOT; the recompiled
  overlays' function tables are installed by the same overlay loader as on
  the desktop.

## 4. The renderer (`backend/psp/draw_gu.c`)

`backend/draw.h` on sceGu, the shape `draw_gl.c` was written to keep:

- One display list per frame (GU_DIRECT, 2 MB, through the uncached alias),
  the GE starting on it while the CPU parses the TA lists;
  `draw_end_frame` waits for the GE and swaps at the next vblank.
- Vertex `(x·z, y·z, −z)` with z = 1/w and a projection whose clip w is z
  again, so the GE divides (perspective-correct texturing), clip z = −z − 2
  for the desktop's depth `1 − 2/z`; the depth range is inverted (65535
  near) so larger is nearer, the PowerVR's sense, and the TA's compare
  codes map one to one. View z is −z, which is what `sceGuFog` measures.
- Textures in main RAM in the PSP formats the HLE already produces
  (5650/5551/4444, CLUT4/CLUT8 kept paletted), swizzled, mip levels built
  at load (box filter; point-sampled indices), at most 512x512.
- PowerVR state: blend factors (`OTHER_COLOR` is the GE's src/dst colour,
  ZERO/ONE and the destination-alpha factors are `GU_FIX`: the frame buffer
  has no alpha, as the desktop's GL window), depth compare and write, cull
  (the GL convention flipped for the GE's y-down screen; checked on
  Station Square), alpha test ≥ 0x80 for punch-through, the shading
  instructions as `sceGuTexFunc` (DECAL = REPLACE, DECAL_ALPHA = DECAL,
  MODULATE(_ALPHA) = MODULATE, `GU_TCC_RGB` when the texture alpha is
  ignored), fog enable, flat/Gouraud, `sceGuDepthOffset(4)` for translucent
  strips.
- Strips of one state and projection merge into one GE strip through
  degenerate joints (winding parity kept). The title's 2,009 depth-sorted
  translucent strips change state 1,527 times, so they stay ~1,530 draws;
  Station Square's busiest frame is 7,763 strips in 1,983 GE draws.
- The offset (specular) colour is a second, additive, untextured draw at
  equal depth as on the desktop, collected per state run in its own batch
  of 16-byte vertices. Drawing it per strip filled the 2 MB list in
  Station Square (8,709 draws, ~350 strips a frame dropped); batched, the
  list peaks at 1.8 MB with nothing dropped.

Input: cross A, circle B, square X, triangle Y, Start, the d-pad, the nub
as the analog stick (dead zone 28), L/R as the triggers (0 or 255). Files:
sceIo under `ms0:/PSP/GAME/SA1PSP/data`, six descriptors kept open,
`sceIoReadAsync` + wait. Timing: 333 MHz (`scePowerSetClockFrequency`;
the PSP boots at 222), guest frame N waits for vblank N, so 60 Hz with the
game drawing every second frame. Log: buffered to `sa1psp.log` next to the
EBOOT and copied to stdout (psplink, PPSSPP's log).

## 5. Frame time

| Scene | PPSSPP | Hardware |
|---|---|---|
| Title (frames 360–700) | **33.4 ms** per drawn frame (two vblanks: the console's 30 fps); 222 MHz gave 50 ms | pending |
| Station Square, opening event (2458–3000) | 78–100 ms (10–13 fps), 5,100–7,800 strips, up to 16,037 triangles | pending |

GE wait at the end of the frame is ~0.01 ms in every scene: the frame is
CPU-bound (recompiled game logic, the HLE's TA parse and the list build).
PPSSPP times the CPU from its own model of the Allegrex, so the hardware
column is the one that counts.

Time to title: in PPSSPP, `main()` to game mode 11 (title) 6.7 s with the
tape's 2.4 s hash of `1ST_READ.BIN`; the title's textures are in by frame
344. Hardware pending.

## 6. What differs from the desktop

- Guest RAM 20 MB (`SA1_COMPACT_RAM`) instead of the 34 MB window; the
  out-of-window register accesses land in the null page.
- Stacks 256 KB / 32 KB instead of 1 MB, painted instead of guarded; no
  save states on the PSP (`--save`/`--load` are desktop options).
- Rendering: a constant depth offset (the GE has no slope term); mirrored
  repeat through mirrored texture copies (50 in Station Square); textures
  over 512 lose their top level (none on the route); mip levels built on
  the CPU; the offset-colour pass ordered per state run, not per strip.
- The GE drops a primitive with a vertex outside its 4096-pixel window
  instead of clipping it. The backend counts such strips (~250 a frame in
  Station Square, mostly ground and walls running far past the screen
  edge); PPSSPP draws them anyway, so whether they vanish on the console is
  the first thing to look at on hardware.
- `--screenshot` reads the frame from EDRAM after the GE finishes; PPSSPP
  does not copy its frame buffer back to emulated EDRAM, so it writes black
  there (captures come from the window instead).
- Crash handling: a user-mode EBOOT cannot install an exception handler on
  the PSP (`pspDebugInstallErrorHandler` needs a kernel export, and
  importing it stops the EBOOT from loading). Traps and the stack probe
  print a guest backtrace from a scan of the coroutine stack, mapped
  through the function tables (`crash_psp.c`); a watchdog thread logs and
  flushes a hang; hardware faults show in PPSSPP's log or psplink, and
  `tools/psp_addr2line.sh` names the addresses from the unstripped
  `build-psp/sa1psp/sa1psp.elf`.
- Desktop-only debug options not carried: `--poke`, `--dump`, `--watch`,
  `--goto`, `--texdump`, `--trace-funcs`, `--hwwatch`.

## 7. Stubbed

- Sound: `hle/sound/` stubs as on the desktop (M3 has not landed a
  `backend/audio.h`; when it does, `backend/psp/` gets a stub of it).
- FMV skipped (as on the desktop). VMU and saves as on the desktop.

## 8. Hardware

The card (the test PSP-3001, ARK CFW, `E:\PSP\GAME\SA1PSP\`) holds the
EBOOT, `data/` (1ST_READ.BIN and SONICADV, 981 MB) and `args.txt`, which
plays with the pad and logs frame time and memory every 300 frames
(`--stats 300 --mem 400`) to `sa1psp.log`. A second boot can replay the
tape instead (the commented line in `args.txt`). Results pending.

## 9. Open items for M5

1. **Frame time** in Station Square (CPU-bound): profile on hardware,
   then the README §5 levers (hot functions at `-O2`, VFPU in the Ninja
   HLE, the TA parse, fewer GE state commands per state change).
2. **Memory**: page the recompiled overlays (7.9 MB of native code for
   57 overlays, of which at most ~0.4 MB run at once: `ADV00` is 343 KB),
   for example one PRX per overlay loaded with the guest file; shrink the
   stacks (2 KB / 1 KB used); textures without mip levels for the 2D layer
   or downscaled (M6's pass).
3. **GE guard band**: clip strips that run past the GE's window on the CPU,
   if hardware shows them missing.
4. Display list double-buffering, once the GE is the bottleneck anywhere
   (GE wait is ~0 today); list peak 1.8 MB of 2 MB.
5. Depth: a per-frame depth range fitted to the 1/w the lists use (M2
   report §6).
6. A kernel-side exception handler (ARK's kubridge loads kernel PRXs) for
   hardware crashes with registers.
7. Overlap file reads with frames (`sceIoReadAsync` is in place; gdFs would
   report "busy" until the read completes).
