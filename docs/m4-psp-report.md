# M4 PSP boot

Exit test (README §8): the same tree built with pspsdk boots to the title
screen, in PPSSPP first, then on the test PSP-3001.

Status: **passed 2026-10-19.** PPSSPP first, then the test PSP-3001 (ARK
CFW): the title, the menus, character select, Station Square and its
opening event, the Chaos 0 fight to its result screen, 11,829 guest
frames and 997 s without a crash, quit with HOME (§8). Two faults showed
only on the console and are fixed (§8). Everything below was measured with
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
| hardware | the title at frame 344, then the maintainer's controller through character select, Station Square (opening event at 1227) and the Chaos 0 fight to its result screen (11,705); HOME at 11,829 (§8) |

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
| EBOOT `.bss` | 3,318 KB | user RAM | display list 2,048 KB, vertex batches 240 KB, HLE statics 638 KB |
| Main thread stack | 2,048 KB | kernel-allocated | the coroutine stacks, 1,504 KB (root 256 KB + 39 x 32 KB), are carved from it (§3, §8) |
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

These are build 1's figures in PPSSPP; build 3 moved the 1.5 MB of
coroutine stacks from `.bss` to the main thread's stack (image 15,501 KB +
2 MB of stack, the same total), and §8 has the console's own numbers.

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
  one block carved from the PSP main thread's own 2 MB stack (a region in
  `.bss` powered the console off at the first switch, §8), and a context switch in Allegrex
  assembly that saves what the PSP's EABI keeps across calls (s0–s7, fp,
  ra, gp, $f20–$f31). The kernel does care where a user thread's sp points
  (it must stay inside the thread's stack), which is why the block comes
  from the main thread's stack; no other thread state follows the switch.
  A self-test at start-up switches into a coroutine, makes a syscall and
  writes a log line from it before anything else runs. No MMU, so no guard pages:
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
| Title (frames 360–700) | **33.4 ms** per drawn frame (two vblanks: the console's 30 fps); 222 MHz gave 50 ms | **76.3 ms** (13 fps), GE wait 5.5 ms |
| Menus, character select | 26–33 ms | 26 ms (menus), 63–73 ms (character select), GE wait 6–8 ms |
| Station Square, opening event | 78–100 ms (10–13 fps), 5,100–7,800 strips, up to 16,037 triangles | **217–388 ms** (2.6–4.6 fps), GE wait ~0.02 ms |
| Station Square, walking (the maintainer's run, 2700–3900) | | 51–279 ms by view (148 ms typical) |
| Chaos 0 fight (4200–9900) | | 124–277 ms (typically 125–200 ms) |

On the console the 3D scenes are CPU-bound (GE wait ~0.02 ms at the end
of a Station Square frame); the title's 2,009 translucent strips are the
one place the GE shows (5.5 ms). The console runs 2.3x (title) to 3–4x
(Station Square) slower than PPSSPP's model of the Allegrex predicted:
memory latency, which PPSSPP does not charge, is the likeliest difference,
and the guest-RAM traffic of the recompiled code and the HLE's TA parse
are where M5 starts (§9). Frame times include a 0.1 s hitch every 300
frames from the memory probe of `--stats`.

Time to title, on the console (build 3, write-through log): `main()` to
game mode 11 in 10.9 s, the title's textures in at 14.4 s (frame 332),
the title on screen from frame 344; about 3 s of that is the log itself
(~115 lines written and closed one by one, ~30 ms each). Loading from the
XMB to `main()` (the 12.5 MB EBOOT) is not in the log. In PPSSPP: game
mode 11 at 6.7 s including the tape's 2.4 s hash of `1ST_READ.BIN`.

## 6. What differs from the desktop

- Guest RAM 20 MB (`SA1_COMPACT_RAM`) instead of the 34 MB window; the
  out-of-window register accesses land in the null page.
- Stacks 256 KB / 32 KB instead of 1 MB, painted instead of guarded.
  Save states (`--save`/`--load`) came to the PSP later, streamed and tied
  to the build (section 8.1).
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
- Desktop-only debug options not carried (`--poke` came later with
  `--stage`, section 8.1): `--dump`, `--watch`,
  `--goto`, `--texdump`, `--trace-funcs`, `--hwwatch`.

## 7. Stubbed

- Sound: `hle/sound/` stubs as on the desktop (M3 has not landed a
  `backend/audio.h`; when it does, `backend/psp/` gets a stub of it).
- FMV skipped (as on the desktop). VMU and saves as on the desktop.

## 8. Hardware

The card (the test PSP-3001, ARK CFW, `E:\PSP\GAME\SA1PSP\`) holds the
EBOOT, `sa1exc.prx`, `data/` (1ST_READ.BIN and SONICADV, 981 MB) and
`args.txt` (`--stats 300 --mem 400 --sync-log`: play with the pad, frame
time and memory every 300 frames, every log line written through to the
card). Four boots:

| Build | Result | Cause | Fix |
|---|---|---|---|
| 1 (dd24662) | black screen, console off; `sa1psp.log` created, empty | (the log was buffered) | the log writes through to the card until frame 300 or with `--sync-log`, a `step:` line before each init call (404e644) |
| probe 1 (404e644) | the same; the log ends at `step: root coroutine` | the first switch onto a coroutine stack in `.bss` (or the first syscall from it): the kernel wants a thread's sp inside its stack; PPSSPP does not care | the coroutine stacks are carved from the main thread's stack, 2 MB, and a self-test runs first (4df2e01) |
| probe 2 (4df2e01) | **the title on the console**, Start, the menus; off while character select loaded the character models (after `E102.PRS`, frame 1745) | the PSP starts threads with FPU traps on (FCSR 0x00000E00: invalid, divide by zero, overflow); the game runs on the SH-4 with all of them masked, and a float 0/0 or overflow in the recompiled code trapped; PPSSPP raises no FPU exceptions | FCSR 0x01000000: traps masked, flush to zero (the SH-4's DN) (5d3e4f9) |
| build 3 (5d3e4f9) | title, menus, character select, Station Square with its opening event, the Chaos 0 fight to the result screen, 11,829 frames, 997 s; HOME | | |

Build 3 also carries `sa1exc.prx`, a kernel-mode crash screen the EBOOT
loads through ARK's kubridge (`backend/psp/exc/sa1exc.c`): an exception
then paints EPC, BadVAddr, Cause and the registers on the screen instead
of powering the console off. It loaded on the console (`sa1exc.prx load
0x04341129, start 0x04341129`); nothing has crashed since.

Memory on the console (heap in use / largest free block; the EBOOT image
is 15,501 KB and the main thread's stack 2 MB besides):

| Point | Heap in use | Largest free block |
|---|---|---|
| start | 0 KB | 33,856 KB |
| boot (guest RAM allocated) | 20,708 KB | 13,120 KB |
| **title (frame 400)** | 22,359 KB | **11,456 KB** |
| character select (900) | 24,206 KB | 8,576 KB |
| Station Square (1500–3900) | 29,855–30,142 KB | **3,520–3,904 KB** |
| Chaos 0 (4200–9900) | 25,184–25,592 KB | 4,096 KB |

So resident at the title is 15.1 MB of image + 2 MB of stack + 21.8 MB of
heap = 38.9 MB, with 11.2 MB still free; Station Square leaves 3.4 MB, as
§2 estimated.

The GE's guard band: the backend counted 19,000–56,000 strips per 150
frames in Station Square with a vertex past the GE's 4096-pixel window
(~130–380 a frame). The maintainer did not report missing ground or walls; a
side-by-side with PPSSPP at the same frame is open (§9).

### 8.1 The console over USB: PSPLink

PSPLink (pspdev/psplinkusb v3.2.1, built in `tools-psplink/` next to the
repository; the PSP side in `ms0:/PSP/GAME/PSPLINK`, WinUSB bound to "PSP
Type B" 054C:01C9 with Zadig) and `tools/psp_run.py` run the game from the
PC without touching the card: usbhostfs_pc serves `build-psp/sa1psp/` as
`host0:`, the script stages the game and its `args.txt` in
`host0:/psplink-run/`, resets PSPLink, starts the game, streams its log,
and names an exception's EPC and RA against the ELF.  The 1 GB of game
data stays on the card (`--data ms0:/...`); the log, screenshots and save
states land on the PC.

- ARK-4 refuses `ldstart` of the EBOOT's static ELF (0x80020148), so the
  build links the game a second time as a relocatable PRX
  (`sa1psp_prx` target, `sa1psp.prx`, symbols in `sa1psp_prx.elf`; the
  game logs its load address, `image: main at`, and the script subtracts
  it) (e0815c8).
- PSPLink's replies carry no tag: a `ver` resent while the PSP was still
  connecting was answered late and taken for the next command's reply.
  The script counts replies against commands and resends `ver` every 15 s
  only (e0815c8).
- `--no-crash-screen`: `sa1exc.prx` would take the default exception
  handler from PSPLink and stop the console in exception context, which
  takes the USB link down; with PSPLink's handler the thread sleeps and
  the registers come back over USB.
- Never kill usbhostfs_pc while the PSP is connected.  PSPLink's
  `usbhostfs.prx` reads the host's hello only in its USB attach handler,
  a blocking read with no timeout; after a connected host dies, the next
  one's hello is never read (usbhostfs_pc `-v`: `Bulk Write returned -7`,
  a timeout, then a port reset, once a second, even with `-t 8000`), and
  only restarting PSPLink on the console recovers it.  `psp_run.py --stop`
  did exactly that twice on 2026-10-19; it now refuses without `--force`.

A run takes about 30 s including the reset.  Test checkpoints, so a
console test reaches its scene in seconds (4754f0d):

- `--stage ss|ec|chaos0|LEVEL[:ACT[:MODE]]` sets the level, act and game
  mode at guest frame 12 and lets the game load the level itself: Station
  Square by frame 13 with Sonic facing the camera, Emerald Coast act 1 by
  frame 26.
- `--save FRAME:FILE` / `--load FILE` (`backend/psp/savestate_psp.c`): the
  desktop's design with the chunks streamed (20 MB of guest RAM does not
  fit beside a 30 MB state in the console's free memory); 25-30 MB, about
  6 s.  A state holds native stacks, so it loads only into the build that
  saved it (image hash): re-record after each rebuild, for example
  `psp_run.py --timeout 1400 -- --replay sonic_ec1.tape --no-pace --frames 7000 --save 6990:c6990.state`
  (the Chaos 0 arrival, about 11 min).
- `--screenshot FRAME:FILE.ppm` reads the frame from EDRAM after the GE
  finishes, before the swap.  Desktop references come from the same tape
  or `--stage` with `build-desktop`'s `--screenshot`.

### 8.2 Console-only renderer faults

None of these shows in PPSSPP, which compares depth exactly, draws past
the GE's window and does not model the display's scan-out.

| Build | Seen on the console | Cause | Fix |
|---|---|---|---|
| 5d3e4f9 (build 3) | ground and walls missing at the edges of the view | the GE rejects a whole primitive with a vertex outside its 4096-pixel window | strips past a guard rectangle are cut into triangles and clipped on the CPU in screen space, attributes through w (694b9e8) |
| 5d3e4f9 | character select's text gone ("Select a character", the name box, File/Clear) | the UI quads and their text sit at the same 1/w a unit above the far end of the 16-bit buffer; with depth writes the GE's rounding decided the ties | translucent 2D strips do not write depth (sorted far to near, they still test against the 3D) (694b9e8, kept) |
| 5d3e4f9 | the title's water wobble ran past the 4:3 art | the menus' 3D drawn with the wide projection | `DRAW_LAYER_3D_43`: the game-mode-11 3D in the 4:3 area, both backends (4754f0d) |
| 694b9e8 | z-fighting everywhere, HUD and text boxes flickering, Sonic's back through his front | that commit also fitted the depth range to the previous frame's nearest vertex (the mapping changed every frame, nearer geometry collapsed to the front) and turned depth writes off for all translucent strips (the characters are mostly translucent) | both reverted (11dea0d) |
| 11dea0d | the mono-eye: a white band across Sonic's face at z ~70 | one 16-bit range over z 1..inf steps 0.075 units at z = 70; face and eye-white strips a few hundredths apart lost by rounding; a nearer start of the range clamps the floor in front of the camera instead | two passes split at z = 16: the far pass (16 bits over w 0..1/16, 16x finer), a depth clear, the near pass; strips spanning the split are cut there on the CPU (ca5c25f) |
| ca5c25f | character select: Sonic doubled, an afterimage as he turns | pspsdk swaps with `PSP_DISPLAY_SETBUF_IMMEDIATE` unless told otherwise, so the display changed buffer mid-scan: at 4-14 fps every swap showed one refresh torn between two frames. EDRAM screenshots are taken before the swap and never showed it | `guSwapBuffersBehaviour(PSP_DISPLAY_SETBUF_NEXTFRAME)`; the frame's clears move to the start of the replay, behind a wait for the vblank after the last swap (e4ebe77; console check pending) |

Left open: the eye texture is darker than on the desktop at a distance (the
GE picks one mip level per primitive, a coarse one for a small far face;
`--gu-max-mip 1` shows the eye but the floor shimmers; kept as is until
the maintainer judges it in motion).

### 8.3 The load stall

Leaving character select for Station Square (and Chaos 0 for Station
Square) holds one frame for 5.7-6.5 s on the console, the faded-out frame
on screen, which the maintainer saw as a black-out.  The maintainer's PSPLink run of
ca5c25f, frame 1246: 6.5 s from the level change to the first frame, of
which about 4.5 s in the 25 `njLoadTexturePvm` calls (`ADVSS03.PRS`, 264
textures, 1.6 s; `OBJ_SS.PRS`, 177 textures, 0.7 s) and about 55 ms per
file open even for files of a few hundred bytes (40-odd files: the
`SETSS`/`CAMSS` sets, `SL_`/`PL_` and the PVMs).  The same load in PPSSPP
split the texture time into the PowerVR decode (a third) and the
backend's mip levels and swizzle (two thirds); the box filter unpacked
and repacked every texel with some twenty integer divides, the decode
computed each twiddled index with two divisions and a bit loop.  With a
packed-field box filter and a twiddle table the PPSSPP texture time of
that load fell from 3.2 s to 0.6 s.  The log now prints each PVM's time
split (decode, create), and `--stats` the file opens and reads; the
console numbers are pending (PSPLink was down when this was written).

## 9. Open items for M5

1. **Frame time**: 76 ms at the title, 125–390 ms in Station Square and
   the Chaos 0 fight on the console, CPU-bound. Profile on the console
   (PSPLink), then the README §5 levers (hot functions at `-O2`, VFPU in
   the Ninja HLE, the TA parse, fewer GE state commands per state change,
   the guest-access compare of `SA1_COMPACT_RAM`).
2. **Memory**: page the recompiled overlays (7.9 MB of native code for
   57 overlays, of which at most ~0.4 MB run at once: `ADV00` is 343 KB),
   for example one PRX per overlay loaded with the guest file; shrink the
   stacks (2 KB / 1 KB used); textures without mip levels for the 2D layer
   or downscaled (M6's pass).
3. **GE guard band**: compare a console frame of Station Square with
   PPSSPP's at the same frame (PSPLink and `--screenshot`, which reads
   EDRAM on the console); clip strips that run past the GE's window on the
   CPU if they are missing.
4. Display list double-buffering, once the GE is the bottleneck anywhere
   (GE wait is ~0 today); list peak 1.8 MB of 2 MB.
5. Depth: a per-frame depth range fitted to the 1/w the lists use (M2
   report §6).
6. Memory: Station Square leaves 3.4 MB on the console; Emerald Coast's
   textures are about the same size, so item 2 comes before M5's exit run.
7. Overlap file reads with frames (`sceIoReadAsync` is in place; gdFs would
   report "busy" until the read completes).
