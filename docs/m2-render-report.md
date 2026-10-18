# M2 Desktop graphics

Exit test (README §8): Emerald Coast completed with a controller on
desktop, draw order correct, no desktop-only features.

Status: **renderer done; first exit-test run failed (2026-10-17), the
hang, the aspect ratio and the three Flycast-comparison defects (§8: sky
dome, fog, light records) fixed since; second run pending.** The desktop
build draws every scene on the scripted route from guest RAM, and Emerald
Coast when the stage variables are forced. The maintainer's controller run reached
Emerald Coast and hung after about 80 s (§7: a stray write from the RTC
poll, fixed), with the 4:3 picture stretched (§7: hor+ now). Everything below was measured with
`build-desktop/backend/desktop/sa1psp.exe` (clang 23, game code `-O1`),
the US v1.005 dump and the `SONICADV` tree, on a GeForce GTX 1070 Ti.

*(Screenshots of the running game are not included in the public repository.)*

## 0. Are the bytes right? (texture review, 2026-10-17)

Before more decode work, the question the maintainer asked: are the texture bytes
correct before our decoder touches them? Two checks, both scripted and
repeatable, neither commits game data:

- **The extracted dump against the disc image.** `tools/verify_dump.py
  ..\private\dump\sa1_us_1005` reads the high-density track's ISO9660
  volume straight from `track03.bin` (2352-byte raw sectors, absolute
  LBAs from 45000) and hashes every file against the extracted tree:
  2,832 files, 1,020 MB, all identical, none missing.
- **The archives in guest RAM against an independent decompression.** The
  game reads each `.PRS` into its staging buffer at `0x8CD00000` and
  decompresses it with its own, recompiled, routine; with
  `SA1_PVM_SAVE_DIR=<dir>` the HLE writes every archive exactly as
  `njLoadTexturePvmMemory` found it in RAM, and `tools/pvm_ram_check.py
  <log> <dir> <data root>` decompresses the source file on the PC with
  `tools/prs.py` (a clean-room PRS decoder) and compares byte for byte.
  On the scripted route: 86 archive loads from 66 files (characters,
  menus, Station Square, Emerald Coast, the sky dome's `BG_BEACH.PRS`,
  `OBJ_BEACH.PRS`, ...), all identical. Loose `.PVR` files never pass
  through guest RAM (the HLE reads them from disc itself).

So the extraction, the loader and the recompiled decompressor are clean,
and any texture fault is in the decoder (`hle/ninja/pvr_decode.c`), which
is where the sky-dome fault turned out to be (§8 item 1).

**Review aids, for pointing at broken textures instead of hunting them:**

- `--texdump DIR` writes every texture the HLE decodes as
  `DIR/<index>_<gbix>_<fmt>_<flags>_<w>x<h>_<mips>.ppm` (fmt = 1555, 565,
  4444, yuv, pal4, pal8; flags = tw, twmip, vq, vqmip, svq, svqmip, rect,
  stride, twrect, pal4tw, ...; mips 0/1), and logs
  `texture: dump <index> = <name>` so an index maps back to the texture's
  name and archive.
- `python tools/texsheet.py DIR OUT` tiles a dump directory into labelled
  contact sheets (8x8 tiles of 96 px, label = index, format, flags, size)
  as `OUT/sheet_NNN.png` with an `index.txt`.
- F9 in the window (or `--mark FRAME` from a script) saves
  `DIR/mark_<frame>.ppm` and logs the indices and formats of every texture
  bound in that frame (`mark:` lines), so a broken thing on screen can be
  named by index.

The scripted route's dump and sheets are in `..\private\texreview\`
(`dump\`, 2,514 textures; `sheets\`, 40 sheets; `pvm\`, the 86
archives; `marks\`, frames 3000 and 6000 with their bound-texture lists
in the log). To review: look through the sheets, note the index of every
tile that looks wrong, and group the indices by the format and flags in
their labels; each group names one decode path to fix and one synthetic
case to add to `tests/ninja/pvr_decode_test.c`.

**First review (the maintainer, 2026-10-18): 92 of the 2,514 tiles garbled, all
one decode path**, small VQ with mipmaps (`svqmip`), 86 of them 32x32 and
5 16x16, across all three pixel formats. Cause: the decoder sized a small
VQ codebook from the chunk length (`(bytes - index bytes) / 8`), and every
chunk in the game's archives carries 16 bytes of slack, so it took 67
entries where the file has 64 (32x32 with mipmaps), 19 where it has 16
(16x16) and 34 where it has 32 (32x32 without mipmaps), and read the
indices 16 to 24 bytes late. The sizes are now a table by width (16 up to
16x16; 32x32: 32, or 64 with mipmaps; 64x64: 128; above: 256), measured
against Flycast's dumps over every small-VQ texture in the route's
archives (32x32 with mipmaps: 61 of 97 match within rounding with 64
entries, none with 16, 32 or 128; 16x16: 16 entries; the 64x64 values are
the tools' documented ones, not seen on the route). Unit tests cover
16x16 and 32x32, with and without mipmaps, in all three formats, each
chunk carrying the slack. The 92 tiles regenerated with the fix are in
`..\private\texreview\recheck\sheets\sheet_000.png` for the maintainer to
confirm.

## 1. What draws, and how

The design question of M2 was answered by reading the game's renderer
(`docs/formats/ninja.md` §1): SA1 does not use `njDrawModel` for its
levels and characters. Its own code transforms, lights and projects
(recompiled), and writes PowerVR tile-accelerator parameters through the
SH-4 store queues into KAMUI's vertex buffers in main RAM. Those stores
already land in guest RAM under the emitter's RAM macro (the `0xE0000000`
alias masks to the same offset), so the "TA-vertex translator" README §4
allowed is a **parser**: `hle/ninja/kamui.c` owns the render context and
the three list buffers, and at `kmRender` walks the parameters the game
wrote (polygon headers: PCW, ISP/TSP, TSP, TCW; vertices of types 0–8;
sprites) into `backend/draw.h` calls, opaque first, then punch-through
with alpha test, then translucent sorted far to near by each strip's mean
`1/w`. The Ninja draw entries the game does call (sprites, 2D point lists,
textured quads, basic models through `njDrawObject`) write the same
32-byte parameters into the same buffers (`draw2d.c`, `model.c`), so one
path sorts everything.

The backend (`backend/draw.h`, `backend/desktop/draw_gl.c`) is what the
PSP can do: screen-space vertices with `1/w`, 16-bit textures in PSP bit
order (CLUT4/8 declared, expanded on the desktop), PowerVR state codes
(blend factors, depth compare on `1/w`, culling, alpha test, clamp/flip,
filter, shading instruction, fog), one triangle-strip primitive, linear
fog. Depth is `1 − 2/z` from a 4D vertex `(x·z, y·z, −(z+1), z)`, so GL's
own divide gives perspective-correct texturing and `sceGuFog` can use the
same eye distance later. A 16-bit depth buffer is requested.

Textures (`hle/ninja/texture.c`, `docs/formats/pvr.md`): PVM archives
and loose PVR files are decoded once from guest RAM (twiddled, VQ,
mipmapped, palette, rectangle) into textures of the same bit depth, the
`NJS_TEXMEMLIST` entries the game reads are filled, and the TCW's address
field carries the backend handle.

Four findings decided the result:

1. **The strip head is at a fixed address.** The game's 2D writers read
   the compiled header words straight from `0x8C88FBC4` (strip head
   `0x8C88FB34` + `0x90`), so the context cannot live on the heap.
2. **Three writers switch SH-4 register banks** (`0x8C037EE0`,
   `0x8C038516`, `0x8C038802`: `stc sr; and ~0x20000000; ldc sr`), using
   the other bank's r0–r7 as scratch. The emitter lowers `ldc Rn,sr` to the
   T/S/Q/M flags only, so their recompiled bodies clobbered the caller's
   r0–r7 and the strip loop never ended. Their wrappers (`ta_stubs.c`)
   save and restore r0–r7; the hardware semantics is exactly that (only r1
   comes back, equal to the caller's). An emitter-side fix (bank-aware
   lowering) is a follow-up; `recomp/emitter/README.md` should list RB.
3. **The game's own culling reads a clip rectangle** at
   `0x8C88F600`–`0x8C88F60C` (left, top, right, bottom) that only
   `njInitSystem` wrote on the console. Uninitialised, every object (every
   character, every NPC) was culled while landtables drew; initialising it
   to `0, 0, 640, 480` made Sonic appear.
4. **The lists need more than the heap gives.** KAMUI's buffers came from
   the game's 1 MB `syMalloc` heap on the console; here the opaque list
   reaches 990 KB in a frame (the Station Square → stage transition) and
   Station Square itself needs ~350 KB per frame. The three lists (1.5 MB,
   384 KB, 128 KB) live in a 2 MB area above the 16 MB (`sh4_rt.h`:
   guest `0x8D000000`, mask widened to 25 bits). On the desktop the whole
   32 MB window is allocated because a few game accesses to control
   registers go through a register, not a constant, and now mask into its
   top; M4 must either wrap those accessors or restore the 24-bit mask and
   find 1.5 MB inside the console's map (`docs/formats/ninja.md` §1).

## 2. Numbers

Scripted route (the M1 presses, then `--poke 5290:8c7493ba:1:2 …` to make
the stage after Station Square Emerald Coast act 1; frames are guest frames
at 60 Hz, the game renders every second one):

| Scene (frame) | Draw calls | Triangles | Opaque headers | Translucent headers | Textures live | Texture bytes (16-bit) |
|---|---|---|---|---|---|---|
| Title / menu (1000) | 29 | 58 | 1 | 7 | 21 | 0.80 MB |
| Station Square, opening event (2600) | 3,515 | 8,711 | 522 | 113 | 972 | 5.99 MB |
| Station Square (3000) | 2,030 | 5,341 | 342 | 30 | 972 | 5.99 MB |
| Emerald Coast act 1, start (6000) | 4,081 | 11,492 | 470 | 125 | 839 | 5.85 MB |
| Emerald Coast act 1 (7600) | 4,063 | 11,476 | 462 | 133 | 839 | 5.85 MB |

"Draw calls" are backend strips; the game writes short strips (two to
four triangles each), which the PSP backend should merge where the state
does not change. TA buffer high-water marks on the route: opaque 990 KB,
translucent 259 KB, punch-through 0 (nothing on the route uses that list
or modifier volumes).

Frame rate, 480x272, unpaced (`SA1_NOVSYNC=1`):

| Section | Guest frames per second, drawing on | Headless | Rendered frames per second |
|---|---|---|---|
| Station Square (2600–3000) | 662 | 769 | ≈ 330 |
| Emerald Coast (6000–7600) | 655 | 757 | ≈ 330 |

The GL side costs about 15 % of the frame; the recompiled game logic is
the rest. With vsync the window runs at a steady 60 Hz (30 rendered
frames per second, as on the console). Whole route to frame 7600: 10.8 s
drawing, 8.2 s headless.

Guest memory: 16 MB console RAM + 2 MB list area + 64 KB slack (32 MB
reserved on the desktop, §1 item 4). Host texture memory equals the
16-bit totals above plus the GL objects.

## 3. Validation against Flycast

Flycast 2.7 was used side by side for the Sega logo and the attract
demos (character rendering, lighting, HUD) and for the shape of the
menus. Matching: logo, title text, main menu, character select
(textures, blending, layout), Station Square (buildings, street, NPCs,
lamp posts, lighting), Emerald Coast (sky, sea, sand, hotel, palms,
Sonic with his animation, rings, enemies, HUD). Known differences:

- The title screen at frame 700 shows "Press Start Button" and the small
  Sonic Team emblem without the large logo; the menu at frame 1000 shows the
  logo behind the panel. Whether the logo is missing or just late in the
  title's own animation needs a Flycast capture of the title itself,
  which the attract-mode timing defeated this session.
- Offset (specular) colour is added to the base colour only on untextured
  polygons; on textured ones the fixed pipeline has no second colour stage
  (neither does the GE). Specular highlights on textured models are
  therefore missing.
- Fog is linear between the near and far points derived from the game's
  128-entry table; the PowerVR table could be non-linear.
- Translucent sorting is per strip, not per pixel. No visible misorder on
  the route; the water and shadows in Emerald Coast look right.
- Culling: PowerVR "cull negative/positive" maps to GL CCW/CW after the
  y flip; checked on characters (not inside-out) and buildings.

## 4. Entries still stubbed (executed on the route)

Sound: `sd*` driver (`0x8C643xxx`), ADX/CRI (`0x8C644xxx`, `0x8C65xxxx`)
— M3. Debug print family (`0x8C6406AC`, `0x8C64075A`, `0x8C640752`,
`0x8C640922`), `0x8C60D1F2`/`0x8C60D1F8` (work pointer, draw callback),
`0x8C6044E0` (32-bit word copy, harmless), `0x8C6055BA` (a float render
setting). Modifier volumes (list types 1 and 3) are skipped by the parser
and counted; none appeared on the route. `0x8C62159A`/`BA`/`FA` (motion
draws) are SDK-internal and never called by game code.

## 5. Desktop debug aids added

`--headless` (no window; the M1 behaviour), `--scale N`, `--aspect
wide|4:3`, `--screenshot FRAME:FILE.ppm`, `--stats N` (the render counters
every N frames), `--dump FRAME:FILE` (guest RAM), `--poke
FRAME:ADDR:VALUE[:SIZE]`, `--stick FRAME:X:Y[:HOLD]` (scripted analog
stick), `--hwwatch FRAME:ADDR[:LEN]` (x86 debug-register write watch; each
write logs the guest call stack), `SA1_TASK_CHECK=1` (per-frame check of the
task lists' parent/prev links; logs the first broken one with the task's
main), `SA1_HANG_DUMP=FILE` (guest RAM when the watchdog fires),
`SA1_KAMUI_DUMP=<frame>` (every header, pointer hand-off and writer call of
that frame), `SA1_MODEL_DEBUG=1`, `SA1_NOVSYNC=1`, `SA1_REPORT_TOP=N`,
`SA1_TEX_LOG=1` / `SA1_TEX_DUMP_DIR=<dir>` (every texture decode, as PPM;
`--texdump DIR` is the same with the review names of §0, plus F9 /
`--mark FRAME`), `SA1_PVM_SAVE_DIR=<dir>` (every texture archive as found
in guest RAM, for `tools/pvm_ram_check.py`).
Debug builds (`CMAKE_BUILD_TYPE=Debug`, or `-DSA1_RAM_CHECK=ON`) check
every guest access in the flat-RAM macros against the RAM window
(`sh4_rt.h`, §8 item 4): an address outside it is logged once with the
host backtrace (the recompiled functions on the guest call stack), counted,
and listed at exit after the seam report; `SA1_RAM_TRAP=1` aborts at the
first. Release builds compile the bare mask.
The stage variables: `0x8C7493BA` current level, `0x8C7493BC` act,
`0x8C7493BE`/`C0` next level/act (Emerald Coast = 1, Chaos 0 = 15,
Station Square = 26); GameMode at `0x8C760970`.

## 7. The first exit-test run and what it changed

The maintainer's run (controller, title → Station Square → Chaos 0 → Emerald
Coast): four findings, in his order.

**1. Hang in Emerald Coast** (watchdog at frame 28,117, about 5,000 frames
into the stage). The stack was the game's delete-children loop
(`0x8C098E60`) under the pier object's destructor (`O PIER`, STG01
`0x8C903708`), called when the stage was torn down after the orca chase.
Reproduced on the scripted route with a scripted analog stick (`--stick`)
that runs Sonic down the pier: a new per-frame check of the task lists
(`SA1_TASK_CHECK=1`) flagged a plank child task whose parent and previous
links had 4 zero bytes written across them at frame 6,433, and an x86
hardware write watch (`--hwwatch`) on that word caught the writer: the
**real-time-clock poll on the VBlank chain** (`0x8C10BA80` → `0x8C10BD96`
→ `0x8C10BE78`). It reads the AICA RTC through a G2 helper
(`0x8C10D460`) that first suspends the AICA DMA channels through a control
block only the sound driver's init sets up (`0x8C10D5A8`, never run here):
the block pointer is NULL, the channel index is whatever lies at guest
address 0, and the four "suspend" register writes, masked into guest RAM
by the flat-RAM macros, land anywhere. On the console those are hardware
registers. The raw read and the three RTC writers are now HLE entries
(`hle/shinobi/hw.c`, next to the already wrapped `0x8C10BE0C`), so no
recompiled code touches the G2 bus any more. The stale link then made
`0x8C098E60`'s `while (task->child) FreeTask(task->child)` spin, because a
child whose parent field is wrong is never unlinked. Lesson for M4: a
register-based MMIO access that the emitter cannot see at emit time
silently becomes a RAM access; the seam report should count them.

**2. Aspect ratio.** The game projects for 640x480 and the frame is 480x272,
and the first build stretched it. Now (`backend/draw.h`): the HLE keeps the
game's camera and screen distance; in wide mode (default) the vertical field
of view is kept and the frame shows 853x480 of the game's screen space
(hor+), with the clip rectangle at `0x8C88F600` widened so the game's own
culling keeps the objects only the wider frame shows; `--aspect 4:3`
pillarboxes instead. 2D elements (HUD, menus, 2D sprites, textured quads)
keep their 640x480 placement in the centred 4:3 area: the game's 2D writers
and the native 2D entries mark the TA buffer spans they write, the parser
tags those strips `DRAW_LAYER_2D`, and the backend uses the 4:3 projection
for them; a 2D strip spanning the full 640 width (fades, backdrops) covers
the whole frame. The PSP backend gets the same `layer` field.

**3 and 4. Textures, polygons, fog**: see §8 (Flycast comparison).

## 8. Flycast comparison (2026-10-17, second pass)

Method: the replay harness's Flycast oracle (`tools/flycast/tape_oracle.lua`
driven by a tape of the scripted route) plus a small Lua wrapper that skips
the intro movie with Start, rewrites the stage variables to Emerald Coast
act 1 when Chaos 0 would load (what `--poke 5290` does here), and writes a
capture request at the same game frame the desktop screenshot was taken; a
PowerShell loop copies the Flycast window from the screen (pausing the
emulator opens its menu, so captures are live). Frames compared: title,
menu, character select, Station Square (desktop 3000), Emerald Coast
(6000, 7600).

Fixed from it: the title logo (its 2D quads sit at 1/w = 2, in front of
the old near plane: clipped), the offset colour (added after texturing;
visible on the intro layers and characters), mip levels, and a `--no-pad`
switch after an idle pad steered a windowed scripted run off the route.

The three items left open by that pass, and what the third pass (later
the same day) found, in the order of the task list:

**1. Sky dome streaks: the mipmap chain offset.** Flycast's texture dumps
(`texdump/`, outside the repo) gave the ground truth; scoring our decode
of `sonic_sky1` against the matching dump at every candidate offset put
the 256x256 level at `0xAAAC`, not `0xAAB0` (mean absolute error per texel
6.9 against 32.5; a 64x64 ARGB4444 mipmapped texture likewise 13.7 at
`0xAAC` against 116 at `0xAB0`). In a PVR *file* a twiddled 16-bit
mipmap chain starts with one dummy texel (levels at 0x2, 0x4, 0xC, 0x2C,
..., 0xAAAC); the table the decoder used (0x6, 0x8, ..., 0xAAB0) is the
texture-memory layout, four bytes higher. Reading every texel two Morton
positions early swaps the columns of each 2x2 block with the next block's,
which is exactly the vertical dither the sky showed; the previous
"byte-exact offline decode" had used the same table. The decoder is now
its own unit, `hle/ninja/pvr_decode.c`, with a test on synthetic twiddled,
mipmapped, VQ, VQ-mipmapped and rectangle textures
(`tests/ninja/pvr_decode_test.c`, in CI, no game data). VQ mipmap offsets
were already right (no dummy); palettised mipmap chains are untested (none
on the route). After the fix the dome matches the Flycast capture at frames
6000 and 7600. The same class of error, a layout derived from the chunk
length instead of the format's rule, was behind the 92 small-VQ tiles of
the maintainer's review (§0).

**2. Station Square tint: the fog model.** `njSetFogDensity` (`0x8C63FE3C`
→ `0x8C652E38`) is `kmSetFogDensity`: it writes its argument, the
FOG_DENSITY register word, as given (an 8-bit float: mantissa /128 in bits
15..8, signed exponent in bits 7..0). The hardware indexes the 128-entry
table by `z = density / w` clamped to [1, 256): entry `i` covers
`z = (16 + (i & 15)) / 16 * 2^(i >> 4)`, so entry 0 is `w = density` (the
far end) and 127 is `w = density / 248`. `njGenerateFogTable3(table, near,
far)` (`0x8C63FB48`, from its disassembly) sets the density to |far|
(quantised to 8 bits through `frexpf`) and writes a table that is 0 up to
the first entry nearer than |near| and rises linearly in `w` to 1 at |far|;
`njGenerateFogTable` writes the default curve `1/z` (fog = `w / density`);
`njSetFogTable` (`0x8C652DD0`) uploads the floats as bytes. All four are
now real bodies in `kamui.c`; the backend's linear fog takes the `w` where
the table passes 0.02 and 0.98. What the game sets on the route: title and
menus, density `0xFF12` (522,240 units: no visible fog); Station Square and
Emerald Coast, `njGenerateFogTable3(near -7000, far -8000)` with a black
fog colour, i.e. black from 7,111 to 8,000 units (the street's far end),
nothing nearer. The per-polygon fog bits (TSP 23..22) are honoured: the
parser keeps them and the backend enables fog for modes 0 and 3 (table) and
not 1 (per-vertex) or 2 (none); counted over the route, 1,502,391 headers
use table fog, 217,649 none, 0 per-vertex, 0 table mode 2. Station Square
at frame 3000 against the Flycast capture after this: the tone now agrees
within a few percent of luminance in every quarter of the frame (pavement
51/72/90 against 53/77/97 RGB, road 20/42/77 against 20/41/77); what
remains is not a tint but detail (texture filtering, and the frames are
hor+ 960x544 against 4:3 640x480).

**3. Specular on Sonic: no missing term in the HLE, one wrong layout.**
`njSetLightIntensity` / `njSetLightColor` / `njCreateLight` (disassembled)
keep the premultiplied colours as colour x intensity (+0x84) at +0xB4, then
that times ambient, diffuse, specular at +0xC4, +0xD4, +0xE4 (a, r, g, b
each; alpha +0x148 is 0); the M2 body had ambient/diffuse/specular at
+0xB4/+0xC4/+0xD4 with r, g, b, 1, so a reader of the diffuse colour got
the specular-scaled one. Fixed (`light.c`, with +0x80 and +0x88 too), but
it changes nothing on screen: the two lights the route creates (type 0x13
at `0x8C6AEE28` and `0x8C760810`) are only ever passed to the Ninja entries;
no recompiled code reads their records. The game lights its characters
itself and writes the result as packed vertex colours plus offset (specular)
colours: in the frame-6000 opaque list 29,879 vertices carry an offset
colour, 97% of them below 32/255 and the brightest 200/255, all on Sonic.
The renderer adds them in the second, additive pass (`draw_gl.c`
`emit_offset`), and the highlight sits where the capture has it; the
softness that remains is the fixed-function pass itself (per-vertex
colour, no colour clamp), the same thing the GE will do. Noted as a
limitation, not a bug.

**4. Guest addresses outside RAM (the §7 lesson).** The flat-RAM macros
now check every access in Debug builds (§5). The scripted route to frame
7700 reports three addresses:

| Address | Access | Hits | Where | Meaning |
|---|---|---|---|---|
| `0x2D2D2D0A` | read, once, frame 0 | 1 | `fn_8c0100a0` (boot) | a byte read through the word at `0x8C66C3AC`, which is text in the SDK's constant data; the console makes the same read of an unmapped address. Harmless. |
| `0xFF000038` | write, every frame from 2462 | 25,168 | `0x8C09E778` (texture-upload writer, `hle_wraps.tsv` `TA_PASS`) | QACR0 |
| `0xFF00003C` | write, every frame from 2462 | 25,168 | same | QACR1 |

QACR0/1 select the store-queue target area. The writer loads
`0xFF000040` from its literal pool and stores with `mov.l r0,@-r1` twice;
the emitter's constant tracking does not follow the pre-decrement, so the
stores are RAM stores and land at offsets `0x1000038`/`0x100003C`: inside
the opaque TA buffer, overwritten by that frame's parameters before they
are read, which is why nothing broke. Two seam entries to add: the emitter
should carry constants through `@-Rn` and `add #imm` (then these become
`sa1_mmio_wr`, and the hook can ignore the QACR writes as it ignores the
other control registers); until then the pattern is known and harmless.
The run also showed a build hazard: the emitter leaves a copy of
`sh4_rt.h` next to its output, and the desktop target's include order let
it shadow the one in the tree, so the game code compiled against a stale
header; the runtime directory now comes first.

## 6. Open items

- **The exit test.** Needs a controller run through the real route, recorded:
  `build-desktop\backend\desktop\sa1psp.exe ..\private\dump\sa1_us_1005\data\1ST_READ.BIN ..\private\dump\sa1_us_1005\data --record tests\replay\sonic_ec1.tape`
  (title → Sonic → Station Square → Chaos 0 win → Emerald Coast act 1
  complete; `tests/replay/README.md`). The Chaos 0 fight must be won (the
  overlays doc says the scripted pad cannot). M2 is ticked only after that
  run passes.
- Palettised mipmapped textures: whether their chain starts with a dummy
  texel like the 16-bit one is unverified (none on the route; §8 item 1).
- Emitter: constants through `@-Rn` / `add #imm` so register-based MMIO
  stores become `sa1_mmio_wr` (the QACR writes, §8 item 4); register-bank
  switches (`SR.RB`) should be lowered, or at least detected and reported.
- Fog: a table is reduced to linear near/far; the per-vertex fog mode (1)
  is not implemented (unused on the route).
- `hle_wraps.tsv` still lists the eight non-writers (three collision
  functions, five flag tests) as wrapped; they run unchanged through the
  wrappers. Dropping the rows means a main-image re-emit.
- PSP shape to keep for M4: 32 MB desktop window vs 18 MB on the PSP
  (§1 item 4); CLUT textures stay paletted there; strips should be merged
  per state change; the 16-bit depth buffer with `1 − 2/z` loses precision
  far away (no artefact seen at 480x272); the offset-colour pass is a second
  draw of every textured strip that has one.
- `njDrawPolygon2D` fans are approximated as strips; no fan on the route.
