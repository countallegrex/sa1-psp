# M2 Desktop graphics

Exit test (README §8): Emerald Coast completed with a controller on
desktop, draw order correct, no desktop-only features.

Status: **renderer done; first exit-test run failed (2026-10-17), the
hang, the aspect ratio and the three Flycast-comparison defects (§8: sky
dome, fog, light records) fixed since; second run (2026-10-18) crashed in
Emerald Coast, its seven findings fixed (§9); third run pending.** The desktop
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
  `DIR/<index>_<gbix>_<fmt>_<flags>_<w>x<h>_<mips>.pam` (PAM, RGB plus
  alpha; fmt = 1555, 565,
  4444, yuv, pal4, pal8; flags = tw, twmip, vq, vqmip, svq, svqmip, rect,
  stride, twrect, pal4tw, ...; mips 0/1), and logs
  `texture: dump <index> = <name>` so an index maps back to the texture's
  name and archive.
- `python tools/texsheet.py DIR OUT` tiles a dump directory into labelled
  contact sheets (8x8 tiles of 96 px, label = index, format, flags, size,
  alpha composited over a grey checkerboard) as `OUT/sheet_NNN.png` with
  an `index.txt`.
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
`..\private\texreview\recheck\sheets\sheet_000.png`; the maintainer confirmed
90 and queried two plain green gradients (1292 and 1539, both
`kanbandaigreen`, ARGB4444): decoded with alpha they are one texture, a
uniform green whose alpha ramps row by row from 0 at the top to 255 at the
bottom, which is an alpha gradient, not a decode fault (a wrong codebook or
index base scrambles such a ramp). The dumps carry alpha since, and the
sheets composite it over a checkerboard, so transparent textures are
judgeable; the full set with alpha is in `..\private\texreview\sheets_alpha\`.

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
`SA1_KAMUI_DUMP=<frame>[,<frame>...]` (every header, pointer hand-off, writer
and sprite call of those frames, with each strip's screen extent and an
`audit:` line decoding its TA state: list, depth compare and Z write, cull,
blend factors, use-alpha and ignore-texture-alpha, fog mode, clamp/flip,
colour type and face colour, shading instruction, and the texture's format,
name and whether its alpha channel is used; §11. `SA1_KAMUI_DUMP_VERTS=1`
adds the vertices of the odd strips; `SA1_TA_BT=<address>` the writer call
behind one strip; §10), `SA1_MODEL_DEBUG=1`, `SA1_NOVSYNC=1`, `SA1_REPORT_TOP=N`,
`SA1_TEX_LOG=1` / `SA1_TEX_DUMP_DIR=<dir>` (every texture decode, as PPM;
`--texdump DIR` is the same with the review names of §0, plus F9 /
`--mark FRAME`), `SA1_PVM_SAVE_DIR=<dir>` (every texture archive as found
in guest RAM, for `tools/pvm_ram_check.py`).
The window title shows the guest frame (`sa1-psp (desktop)  f<N>`, refreshed every 10 rendered frames) so screen captures carry it; `SA1_NO_TITLE_FRAME=1` keeps the plain title.
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

## 9. The second exit-test run and what it changed (2026-10-18)

The maintainer's second run (keyboard, no controller detected; title → Station
Square → Chaos 0, won after two deaths → Emerald Coast) ended in a host
stack overflow at guest frame 23,274, 5,474 frames into Emerald Coast, and
its seven screenshots showed wedge-shaped rectangles, a boss title drawn as
blurred squares, cutscene cameras under the police cars, and a game that
ran too fast. The run's tape (`--record`) was 4 KB of a 23,000-frame run.
Everything below was reproduced by replaying that tape (its 17,645 recorded
frames) on the desktop build, with the Flycast oracle run on the same tape
for reference frames (`tools/flycast/tape_oracle.lua` plus a wrapper that
requests a screen capture at a desktop game frame), and with a scripted
route into Emerald Coast for the crash. In the maintainer's order:

**1. The crash: a NULL-pointer write over the BIOS vectors.** The
exception (0xC00000FD) left no trace: the crash handler needs stack to
walk one. What the log did have, 4,800 frames earlier at frame 18,460,
was 64 "guest write outside RAM" reports for addresses `0x00000000` to
`0x000000F8`, all from `fn_8c0402a4` under the motion-callback walker
`0x8C040880` of the Emerald Coast dolphins (`O DOLPHIN`, STG01
`0x8C901800`): the dolphin's per-node callback writes a 256-byte block
through a NULL pointer. On the console that lands in the boot ROM and
vanishes. Under the flat-RAM mask it landed on RAM offset 0, where the
HLE keeps the BIOS syscall vectors (`0x8C0000B0`–`BC`, `E0`), and the
first call through a clobbered vector ran a random function until the
host stack was gone. `0x8C040880` is the game's `njDrawObject` with
callbacks (`+44` child, `+48` sibling, four indirect calls per node);
its normal depth is small (below).
The fix is in the runtime: `sh4_rt.h` `sa1_off()` sends area-0 accesses
(guest `0x00000000`–`0x01FFFFFF` and the P1 alias) to a 2 MB *null page*
above the RAM window, one test and a conditional move per access (folded
away for constant addresses; store queues and RAM keep the mask; the P2
alias is not caught, the game never forms it). Reproduced on the scripted
route (`--poke 5290` into Emerald Coast, `--goto 5400:4053:366` towards
the pier): the dolphin's 64 writes now land on the null page and the
vectors, watched with `--watch`, stay intact. The crash itself did not
reproduce before the fix because the route never reached the vector call;
the third run is the test.
Two aids came out of it. A host-stack probe (`sa1_depth_hook`, sampled at
every indirect call and HLE entry) reports per-stack high-water marks at
exit and traps with the guest call stack 160 KB before an overflow:
**measured on the whole route (title to Emerald Coast), main 12 KB, task
coroutines 4 KB of their 1 MB**, so the PSP can give its task threads
32 KB each (1 MB for the 32-task table; README §7 budgets 2 MB for
runtime and stacks). And the crash handler now survives a stack overflow
(64 KB guaranteed on the main thread and every fiber,
`SetThreadStackGuarantee`), walks the backtrace from the exception
context and ends the tape.

**2. The tape.** `--record` buffered through stdio, so the crash lost all
but the first 4 KB. The recorder now rewrites the open run-length record
in place and flushes after every frame: a crash or a kill keeps the whole
run, and only the `end` line is missing (the loader tolerates that). The
truncated `tests/replay/sonic_ec1.tape` is untracked; the third run's
`--record` overwrites it.

**3. Speed.** Pacing was vsync alone, one swap per *rendered* frame, and
the game renders every second guest frame: on the 60 Hz display it ran at
double speed. Guest frames are now paced to a fixed 60 Hz clock
(`draw_gl_pace`: sleep, then spin to the deadline; re-based if the run
falls a quarter second behind) whatever the refresh rate; vsync stays on
against tearing, `SA1_NOVSYNC=1` runs unpaced, headless and
`--strict`/replay semantics are unchanged. The refresh rate is logged at
startup. Checked: 900 guest frames in 15.5 s windowed.

**4. The wedges: `njDrawPolygon2D`'s attributes.** The SDK body
(`0x8C63EBC8`, disassembled) treats `attr & 0x20` as "the points are the
polygon's outline in ring order" and reorders them 0, n−1, 1, n−2, … into
a strip (`0x8C63E834`), and `attr & 0x40` as translucent (the header
writer `0x8C63E258` takes it). The M2 body had 0x20 as translucent and
0x40 as a fan whose approximation dropped the fourth point, so every
4-point outline (the event scripts' letterbox bars, fades and subtitle
boxes, `attr 0x60`) came out as a wedge. No PVR sprite polygons are
involved: the parser's sprite path (type 5) is never reached on the
route. Fixed in `draw2d.c`; and the `tex` entries of a textured 2D point
list are 16-bit u, v pairs in 1/256 (the writer `0x8C63E8FC`), not
floats.

**5. The boss title and the subtitles.** Two texture-loading faults.
The title's font is `SONICADV/B32ASCII.PVR` (256x256 ARGB4444, the
32-pixel ASCII sheet), named by the game *with* its extension; the HLE
appended `.PVR` again and looked for `B32ASCII.PVR.PVR`, so the title
quads bound whatever texture was current (a 16x16 HUD element,
magnified: the blurred squares). Names that already end in `.PVR` are now
used as they are, and "Chaos 0" renders as on the console. The subtitles
go through the message renderer (`0x8C055C4C`, every frame): it composes
the line into a 512x64 ARGB1555 image in RAM with the game's own font
and loads it as a *texinfo* memory texture (`njSetTextureInfo`,
`njSetTextureName`, `njLoadTexture`, attr `0x40800000`: the name's
pointer is an NJS_TEXINFO whose texaddr is bare texels, size and format
from the texinfo; SDK loader `0x8C64D1A6`). The HLE's `njSetTextureName`
stored the global index where the attribute goes, and the loader knew
only PVR images; both are fixed (`texture.c`), and the quad's UVs were
the 16-bit pairs of item 4. "Surrender yourself!" and the opening's lines
now show in their boxes. A failed `njLoadTexture` entry is logged with
its pointer and attribute.

**6. Fog: not inverted.** Flycast's `fog_mode2` indexes the 128-entry
table by `z = density / w` clamped to [1, 256): entry 0 is `w = density`,
the *far* end, entry 127 the near end (§8 item 2). `njGenerateFogTable3`
(disassembled) writes 1.0 at entry 0 and 0 from the first entry nearer
than |near|; the log line, which printed `table[0] 1.000`, meant the far
entry and now says so. Against the Flycast captures of the same tape
(the Chaos 0 cutscene and fight in Station Square at night; Emerald
Coast from the earlier pass) the distances match: the route's fog is
black from 7,111 to 8,000 units and nothing nearer, on both. What was
wrong in those frames was item 7's camera, not the fog. The per-vertex
fog mode (TSP mode 1) is still unimplemented and unused on the route.

**7. The cutscene camera: `njCameraDirFromBase` pitches.** `--aspect 4:3`
gave the same wrong cameras, so the widened clip rectangle was cleared.
The event scripts place every cutscene camera with
`njCameraPosFromBase`, `njCameraDirFromBase`, `njCameraRollFromBase`,
`njCameraYaw`, `njSetCamera` (390 calls each over the Chaos 0 arrival;
gameplay uses none of the four "FromBase" entries, which is why gameplay
cameras were fine). The SDK's `njCameraDirFromBase` (`0x8C64041C`)
rotates the base direction about X, in y/z, the same pitch as
`njCameraPitch`; the M2 body yawed it about Y, so a camera meant to look
down from a pole looked sideways and ended up under the police cars and
inside buildings. `njSetCamera` and `njCameraYaw` were checked against
their disassembly and match. After the fix the arrival cutscene's shots
(street level with the searchlight, the top-down shot over the officers,
Chaos 0 walking in) agree with the Flycast captures of the same tape.
The maintainer's screenshot 4 (the close-up on Sonic's legs in the Emerald Coast
hotel) could not be replayed: his inputs after frame 17,645 were in the
lost part of the tape; the third run will tell.

**9. Collision ("seemed off too"): not reproduced.** Two recorded tapes
through the Flycast oracle (`tools/flycast/run_oracle.py`; for the second
a wrapper that applies the route's `--poke 5290` on the console side
when Chaos 0 would load), comparing the watched player position and
ground height by game frame. Station Square, 2,222 game frames of the
opening (the event moves Sonic between shots; the diff's one-frame slips
are those cuts): x, z identical, y within 0.012 units. Emerald Coast,
955 game frames of which 250 moving at full speed, 1,100 units along the
beach under a scripted stick: never outside the harness tolerance
(4 units), ground height (y) within 0.39 units throughout, rings and
lives identical; x and z differ by up to 6.6 units for about a hundred
frames while running (one game frame of motion: the console's stage
switch lands one frame off the desktop's poke) and settle to 0.0003
units in x and 2.9 in z at the end. What the maintainer saw was the camera of
item 7, which makes any collision look wrong. Checked alongside: the
eight collision and flag-test functions once stubbed as TA writers
(`docs/overlays-sonic.md`) run their recompiled bodies through the
pass-through wrappers (`ta_stubs.c` `TA_PASS`; the rows are still in
`hle_wraps.tsv`, dropping them means a re-emit); SR.RB bank switching
occurs only in the interrupt and `longjmp` code of the Katana tail, not
in game code; and every FPSCR write in the recompiled code is in that
tail too, so on the console the game runs in the boot rounding mode
(round to zero, FPSCR 0x00040001) while the host rounds to nearest, and
`fsrra`/`fsca` are `1/sqrtf` and `sinf`/`cosf`. That is the likeliest
source of the sub-unit y differences; it does not reach collision.

**Aids added:** `--goto FRAME:X:Z[:RADIUS]` (a waypoint autopilot for
scripted runs; the stick-to-world mapping is calibrated from the movement
it produces, a wall swings the heading and taps A); the host-stack probe
and its exit line; `SA1_KAMUI_DUMP` also logs every
`njDrawPolygon2D/3D` call of the frame with texture, colour and UVs;
`tape_oracle.lua` exports the aligned game frame for capture wrappers.
Emerald Coast act 1's object placements (`SET0100S.BIN`, little-endian
32-byte entries; the STG01 object list at `0x8CAA888C`, 20-byte entries)
gave the pier's coordinates for the route: planks from (4053, 366) to
(5053, 1096).

## 10. The third and fourth exit-test runs and what they changed (2026-10-18, evening)

The maintainer's third run (controller; title → Sonic → Station Square at night →
Chaos 0 arrival and fight, won → Station Square by day → Emerald Coast,
stopped a few thousand frames in) came with four screenshots and three
complaints: the subtitles flicker, the polygon problems around Chaos
remain (a cyan slab over Sonic in the "Ah, yeah!" shot, a cyan prism from
Chaos's head at the fight's title), and the fog looks black. His fourth
run, the same route again, crashed in Emerald Coast at guest frame 18,403
with nothing in the log; the fifth item below is that crash. Its tape
(the recorder writes in place, so the third run's tape was overwritten by
the fourth's) is now `tests/replay/sonic_to_ec1_start.tape`, 18,402 frames,
with a check file (title → Station Square at 1184 → Chaos 0 at 4026 →
Station Square act 4 at 12826 → Emerald Coast at 15559; rings, lives and
position every 600 frames); `sonic_ec1` stays reserved for the full route.
Every fix below was verified by replaying that tape against its check
file, which passes with every change in (0/16 primary, 0/155 secondary).

Method as in §8 and §9: the tape replayed on the desktop build with
screenshots, and in Flycast 2.7 through the oracle (`tape_oracle.lua`
under a wrapper that requests a screen capture at a desktop game frame),
then side-by-side montages, ours left, console right, in
`..\private\exit-test-3\compare\` (not committed: console captures):

| Montage | Frames (ours guest = game; console game) | Verdict |
|---|---|---|
| `shot8_f5400.png` | 5400 = 2697; 2697 | Composition, tint, subtitle and letterbox identical. Different: the console has rain (thin vertical streaks); ours had none, and instead one wedge from the top of the frame down to Chaos's head, the maintainer's "cone" (item 4). |
| `night_f5350.png` | 5350 = 2672; 2672 | Night tint identical within a few percent (sky 54/58/60 against 55/59/60, mid-frame 52/85/94 against 53/82/89 RGB). Different before the fixes: the near pavement was black in ours (item 4); the console's subtitle is already up (its cutscene runs about 30 game frames behind ours, see below). |
| `night_f6080.png` | 6080 = 3037; 3037 | Identical in tint and framing (left quarter 63/57/59 against 63/57/58, ground 37/41/39 against 48/52/52); the console shows rain and its subtitle, ours not yet. |
| `shot10_f6680.png` | 6680 = 3337; 3370 | Same shot of the cutscene (the console is taken 33 game frames later, where its camera is at the same point). Different before: the cyan slab over Sonic; the console has Chaos standing in the middle of the plaza. Fixed (item 5). |
| `fightstart_f7540.png`, `shot11_f7600.png` | 7540 = 3767; 3760 and 7600 = 3797; 3797 | Different before: the cyan prism from Chaos's head to the right edge. Fixed (item 5). The console's fight starts about 7 game frames later. |
| `ring_sparkle.png` | ours 15758 (the maintainer's tape, Emerald Coast); console 2763 of the scripted route | Different routes (below); the sparkle is the same small green burst on both after item 4, the console's a little brighter and yellower. |

Two frames asked for could not be paired. A **daytime Station Square**
frame needs the console to win the Chaos 0 fight on the maintainer's inputs, and
it does not: the console's fight diverges and reaches GAME OVER by game
frame 6700 (the desktop's fight ends at 6410). A **ring pickup with the
console in sync** has the same problem in Emerald Coast; the pickups of
the maintainer's tape in Station Square happen inside the cutscene (the event
gives the rings), so the console side of the sparkle montage is the
scripted route (the oracle with the Emerald Coast poke, as §9 item 9).
Two timing differences came out of the alignment and are open items: the
console plays the Chaos 0 arrival cutscene about 30 game frames slower
than we do (its lines start later: "Oh no! Our weapons are useless!" is
on the console's frame 3037, ours shows no line yet; our lines are driven
by the message renderer's own timers, the console's may wait on voice
playback, which the HLE finishes at once), and the Emerald Coast intro
runs the other way: at the scripted route's game frame 2763 the console
already shows the chase camera and the HUD at 2.66 s while ours is still
in the top-down pan.

**1. The subtitle flicker: an unstable sort.** The event scripts draw a
subtitle as two translucent 2D quads at the same depth (`njDrawPolygon2D`
at 1/w 0.8333): the black band (colour 0x80000000, 0..640 x 380..440)
and the text quad (0xEEDDDDEE, textured with the line). `kamui.c` sorts
the translucent strips with `qsort` by mean 1/w alone, and with equal keys
the order was whatever the other strips of the frame made it: on every
other rendered frame the band landed on top of the text and halved it
(text pixel 196 against 98 in consecutive screenshots, `flick/`). The
sort now breaks ties by submission order, which is what the PowerVR does
for equal depths. Sixteen consecutive screenshots at 60 Hz around frame
5392 read 196 throughout after the fix. The pacing was not involved: the
window presents only on rendered frames (kmFlip swaps only when the
frame rendered, §9 item 3), and the font texture's alpha was right.

**2. The NULL write at frame 16,263: two seam stubs.** `fn_8c0403c0` lerps
every vertex of a shape-motion node into the buffer at `[0x8C89002C]` and
points the model at it; the pointer is set by the SDK entry `0x8C60D1F2`
(one store; the game passes its static buffer `0x8C782984` at boot) and
`0x8C60D1F8` stores the per-node draw callback at `[0x8C890074]` (the
object walkers `0x8C040784`/`0x8C040880` call it for every node; the game
registers `0x8C0D215C` at frame 692, which turns particular nodes of the
player model). Both entries were seam stubs returning 0, so the dolphin's
lerp wrote to NULL and the callback never ran. Both are real now
(`model.c`); the null page stays as a guard. The whole tape runs with no
write outside RAM but the two QACR stores of §8 item 4.

**3. Fog: correct.** The night frames' tint and the distance fog agree
with the console within a few percent of luminance in every region
measured (table above); what the maintainer saw as black fog was the near
pavement, item 4.

**4. Sprites: the two forms of `njDrawSprite3D` were swapped.** The SDK's
dispatcher (`0x8C63EFE4`) sends a sprite with `NJD_SPRITE_SCALE` (attr
0x10) to `0x8C63F038`, a billboard whose world-unit corners scale with the
point's distance, and a sprite *without* it to `0x8C63F2D4`, which treats
the four corners as object-space points in the current matrix's x/y
plane, transforms and projects each on its own, so the matrix's scale and
rotation apply. The M2 body drew every 3D sprite as the billboard. Three
things followed, all on the route: the rain streaks (attr 0x32, 76 per
frame of the arrival cutscene) are billboards and were right, but the 123
rain splashes on the ground (attr 0x22, 14x14 units, a dark blue
translucent 64x64 ARGB4444 texture) stood upright facing the camera as
300 to 1,800-pixel squares and darkened the whole foreground, which is
the black pavement of `night_f5350.png` and the "black fog"; a splash
near the camera, 14 units wide at 1.2 units, is the cone of shot 8; and
the ring sparkle's particles (attr 0x29, a 32x32 quad the game scales
down with `njScale`) came out 32 units wide, the soft green blobs that
covered Sonic. `draw2d.c` now implements both forms (the object-space one
skips a quad with a corner behind the near plane; the SDK clips it
through `0x8C63F610`, not done). The foreground pavement, the rain and the
sparkle now match the console frames (`ours5/`). The sparkle's blend is
source alpha plus destination one (TSP 0x8491A45B), additive, and the
texture's alpha survives the 4444 decode and the GL upload; what remains
is brightness, likely the constant material the game sets per particle.

**5. The cyan slab and prism: a memory copy that did nothing.** The strips
were found with the KAMUI dump's new per-strip extents: the first four
translucent strips of Chaos's body (256, 214, 63 and 43 vertices,
alpha 0x99 and 0x7F) each held, after every pair of real vertices, the
same two vertices at (2635, -1617) and (2635, 551) with 1/w 0.012, UVs
(0,0) and (0,1) and an opaque colour, the extruded "prism". The x86 write
watch on the strip's header named the writer: the game's own near-plane
clipper `0x8C037EE0` (the banked one) under Chaos's model draw, and its
input vertices in the game's transformed-vertex cache were NaN
(0xFFFFFFFF x 3) for 71 of the node's vertices. The NaN came from the
model itself: Chaos's body model (`0x8C924D8C` in the B_CHAOS0 overlay)
has its vertex pointer redirected to a heap buffer (`0x8C82E340`), and
117 of its 159 vertices were still the allocator's all-ones fill. The
boss's init (`ovl_b_chaos0 0x8C9034C0`) allocates that buffer and copies
the model's vertices into it with the SDK entry `0x8C6044E0`, the
longword copy of the family whose byte copy (`0x8C604500`) the HLE already
had, and `0x8C6044E0` was a seam stub returning 0. The per-frame water
wobble then writes only the 42 vertices of its table through
`njCalcVector`, the rest stay NaN, the clipper fires on them and emits
one garbage intersection point per edge. The entry is real now (`cfg.c`),
and Chaos is whole in the cutscene and the fight (`shot10_f6680.png`,
`shot11_f7600.png`). The same stub was called at frame 1201 already; what
else it copied on the route (the opening) showed no visible fault.

**6. The crash at frame 18,403: a tail-call loop on the host stack.**
The maintainer's fourth run died with no report. The Windows minidump
(`tools/minidump.py` reads it) gave exception 0xC0000005, a *write* to
`0x20001000FF68`, 0x98 bytes below the root coroutine's stack, that is in
the no-access gap under it: a host stack overflow of the 1 MB root
context, with the probe never tripping. The maintainer's binary was rebuilt from
the same commit with the same options (`SA1_TRACE_FUNCS=ON` was in his
cache) and matched to the byte count, so the dump's stack symbolised:
1,786 repetitions of four frames, `fn_8c097abc` → `fn_8c097e6a` →
`fn_8c095b60` → `fn_8c097e5a`, under the game's main loop. `0x8C095B60`
dispatches on a format character ('c', 's', 'l', 'f', ...): it is the
game's printf-style formatter, whose loop is written as jumps between
blocks the emitter had split into functions, and the emitter lowered a
jump out of a function as "call, then return". With function
instrumentation on, clang cannot turn that into a jump, so every
iteration of the loop nested a host frame of about 600 bytes, and a
format that loops some 1,800 times (it terminates: the same replay now
runs through) exhausted the stack. The emitter now lowers every tail
(bra/jmp to another function, a conditional branch out, a switch case
out, and the unresolved `jmp @Rn` through a new `sa1_tail_target`) as
`SA1_TAIL(fn(c))`, a `musttail` return under clang, which guarantees the
jump at any -O level and under instrumentation (`sh4_rt.h`; 3,442 tails
and 64 conditional tails in the main image, the overlays likewise;
`tests/emitter` passes). The whole tape replays through frame 18,700 with
the check passing, and the root context's high-water mark is 6 KB where
it was 12 KB.
Why nothing was logged: the log was flushed per line already. The handler
could not run: with the stack pointer in a no-access gap, Windows cannot
even push the exception dispatch frame, so the process dies in the
kernel. Each coroutine stack now ends in a 64 KB band of guard pages above
the gap: the first touch raises an exception the dispatcher can deliver
(the band is committed for it), the vectored handler recognises the band
and reports a host stack overflow with the guest call stack, and the
report itself runs on a private 256 KB stack (`sa1_call_on_stack` in
`coro_switch.c`), so it survives whatever the faulting stack has left. The
save-state self-check still passes with the band in place.

**Aids added:** `SA1_KAMUI_DUMP` works under `--load` (the variable is
read on first use, not in `njInitSystem`); the dump's header lines carry
each strip's screen extent, 1/w range and first colour, and
`SA1_KAMUI_DUMP_VERTS=1` lists the vertices of strips that reach the near
plane or far off-screen; the dump also logs every `njDrawSprite2D/3D`
call (point, scale, anim size, UVs, texture) and, for the native model
drawer, every meshset with its worst vertex; `SA1_TA_BT=<guest address>`
prints the entry registers, the three vertex records and the guest call
stack of the game writer call that produced the strip at that translucent
buffer address; `texture_describe` prints the backend handle;
`--screenshot` takes 64 entries; `tools/minidump.py` reads a Windows
minidump (modules, exception, registers, the faulting thread's stack with
the exe offsets to symbolise against a matching build). A library of Flycast save states keyed by tape frame
(`tools/flycast/oracle_states.py`, `../flycast/states/` with a hashed
manifest) lets `run_oracle.py --from-state N` start the console from the
nearest state instead of from boot (`tests/replay/README.md`).

**Seam stubs still called on the tape** (first frame): the sound driver's
`0x8C643xxx` family (they test the driver's init flag and return an error
code; silent by design), `0x8C604370` at 4384 and `0x8C64489C` at 344 (to
be identified), and the boot-time set of §1.

**The fifth run** (keyboard or controller), from `sa1-psp`:
```
build-desktop\backend\desktop\sa1psp.exe ..\private\dump\sa1_us_1005\data\1ST_READ.BIN ..\private\dump\sa1_us_1005\data --record tests\replay\sonic_ec1.tape --log ..\private\exit-test-5\m2-run5.log
```
(F9 marks a screenshot with the bound textures when `--texdump DIR` is
given; F5/F8 save and load `quick.state`.) The route is the same; what
to look at: the Chaos 0 arrival and fight (rain, pavement, subtitles,
Chaos's body), the ring sparkle, and Emerald Coast past the point of the
fourth run's crash.

## 11. Round four: translucency at the TA level, and the exit test (2026-10-19)

The maintainer's fifth run completed the whole route with a controller (title →
Sonic → Station Square at night → Chaos 0, won → Station Square by day →
Emerald Coast act 1 to the goal → Station Square), 36,478 frames with a
clean end line and no crash; it is `tests/replay/sonic_ec1.tape`, with a
check file of seven milestones and the rings/lives/position samples, and
`tools/replay_test.py` passes on it (and on the two older tapes). His
verdict on the picture: the game plays mostly fine, the biggest issue is
the transparency of some textures, mostly particle effects, splashes and
sparkles; one police headlight spot z-fights. His shots: 17 and 18, the
Emerald Coast cave pool (TIME 02:00:06 and 02:10:46, guest frames 24988 and
25612) with the water "fully opaque cyan" and a splash "a solid white
particle block"; 12 and 14, the arrival cutscene with the headlight spots
on the pavement (frames 5000 and 5120); 13, a crop of a ring behind an
officer's legs (nothing identifiable); 15, 16 and 19 fine.

### The audit

`SA1_KAMUI_DUMP` now takes a list of frames and writes, under every
header it parses, an `audit:` line with the TA state decoded (§5), and the
texture record keeps the archive entry name and an alpha classification
(`texture_alpha_info`: no alpha channel / all 255 / used, with the
smallest texel alpha). One replay with `SA1_KAMUI_DUMP=5000,5120,5880,24988,25612`
gave every strip of the five frames (79–401 opaque, 72–396 translucent
headers per frame; 2,663 strips in all). What the game sets, without a
single exception across the five frames:

| Field | Value on every strip |
|---|---|
| list | opaque (0) or translucent (2); **no punch-through strip** on the route, no modifier volumes in these frames |
| ISP depth compare | GREATER-OR-EQUAL on 1/w, opaque and translucent alike |
| ISP Z-write disable | **never set**: the translucent strips write Z too |
| ISP culling | 1 (cull small), never the signed modes |
| TSP blend | opaque: ONE/ZERO (the writers force it); translucent: SRC_ALPHA/INV_SRC_ALPHA (the bulk), SRC_ALPHA/ONE (lights, flares, the sparkle, bubbles), ONE/ONE (the splash) |
| TSP use-alpha | 0 on the opaque list (forced), 1 on every translucent strip |
| TSP ignore-texture-alpha | 1 on the opaque list (forced), 0 on every translucent strip |
| TSP fog | table (0) everywhere but the sprites of the splash and the lens flares (none, 2); the per-vertex mode (1) never |
| TSP clamp / flip | per texture, as the landtable materials say (clamp 11 for sprites and most objects, 00/01/10 and flip on the sea and wall tiles) |
| colour type | packed (0) for the game's own strips, intensity (2) for every sprite and 2D quad (the face colour in the header, a float intensity per vertex); floating (1) never |
| shading | modulate-alpha (3) on the packed strips, modulate (1) on every intensity sprite, decal (0) on a few HUD/light quads; decal-alpha (2) never |

Per strip the ones in question:

| Strip | List, blend | Shading, colour | Texture | What it is |
|---|---|---|---|---|
| the cave pool, frame 24988 (3 strips of 41, 76 and 72 vertices; 25612 the same) | translucent, SRC_ALPHA/INV_SRC_ALPHA | modulate-alpha, packed colour `0x65A9A9A9` / `0x65BFBFBF` / `0x65D6D6D6` (alpha 0x65 = 0.40) | `m128_sand005`, `m128_sand008` (565, no alpha) | the pool's **sand floor drawn as a 40 % layer over the opaque water** below it: the water is the opaque cyan, the sand the translucent strip |
| the pool's edges, 12 strips | translucent, SRC_ALPHA/INV_SRC_ALPHA | modulate-alpha, packed `0x99F8F8F8` | `los_wt05` (565) | the shoreline foam |
| the splash, frame 25612 (11 quads of 4 vertices, 50–80 px) | translucent, **ONE/ONE**, fog none | modulate, intensity, face `0xFFFFFFFF`, intensity 1.0 | `splash_a06/07/08` (1555 VQ 64x64, alpha used, min 0) | the water splash particles |
| the headlight spots, frames 5000/5120/5880 (6 quads, 230–370 px wide, 12–50 high) | translucent, **SRC_ALPHA/ONE** | modulate, intensity, face `0x80FFFFFF` | `spotlight0` (565 32x32: a soft white disc on black, no alpha) | the additive light pool on the pavement, coplanar with it |
| the beacons' glow (`panlight`), 7–10 quads | translucent, SRC_ALPHA/ONE | modulate, intensity, face alpha 0x4E–0xE1 pulsing, RGB fixed per colour | 565 16x16 | the rotating police lights |
| the sun's lens flare (`lens02`–`lens07`), every frame | translucent, SRC_ALPHA/ONE | modulate, intensity, **face alpha 0x00** while the sun is out of view | 1555 (rings and discs, alpha used) | the flare elements, parked at alpha 0 |
| Chaos's body (`c2tx_mizu0`, 68 strips) | translucent, SRC_ALPHA/INV_SRC_ALPHA | modulate-alpha, packed, alpha 0x98 | 565 | the boss |
| Chaos's trail (`c_ato`, up to 299 quads) and drips (`drop`, 187) | translucent, SRC_ALPHA/INV_SRC_ALPHA | modulate, intensity, face alpha 0x09–0xF7, RGB 0 (`c_ato`) / `0xCCFFFFFF` (`drop`) | 4444 (alpha used) | off-screen in frame 5000 (y 1000–7500), on screen in the fight |
| the ring sparkle (frame 17684, first pickup) | translucent, SRC_ALPHA/ONE | modulate, intensity | `ring02/04/06` (4444) | the three expanding rings |

What the backend set for them before this round: blend factors, depth
compare and Z write per strip as the header says (all honoured since M2);
alpha test on list 4 (unused on the route); vertex alpha from packed,
floating and intensity colours (the face alpha carried through
`scale_argb`), `use_alpha` forcing it to 1; texture alpha always used, so
`ignore_tex_alpha` was not honoured (it is set only on the opaque list,
where the blend is ONE/ZERO anyway, so nothing drew differently); the
shading instruction mapped to GL's REPLACE / DECAL / MODULATE, so MODULATE
and MODULATE_ALPHA both multiplied the alphas; and 2D elements with 1/w
above 1 clamped to the near plane. So the state mapping was already what
the TA says, and the three guesses the round started with (water drawn
with blending off or alpha 1, the splash drawn as alpha blend instead of
additive, decals with Z-write disabled drawn with it on) are all false
for this game: the water layer is blended at 0.40 as asked, the splash is
drawn ONE/ONE, and **the game never disables Z-write**, not even for its
decals. Whatever differs from the console in those frames is not a flag.

### What changed in the backend

1. **The shading instruction is now a GL 1.3 combiner** (`draw_gl.c`),
   one rule per PowerVR mode (`draw.h` has the table): DECAL takes the
   texture alpha alone, DECAL_ALPHA interpolates the texture over the
   vertex colour by the texture alpha and takes the vertex alpha, the two
   MODULATE forms multiply colours and alphas; `ignore_tex_alpha` replaces
   the texture alpha with 1 (the env colour) in every mode, where GL's
   fixed modes could not. The PSP GE has the same switch (`sceGuTexFunc`
   with `GU_TCC_RGB` for the ignored alpha).
   The hardware manual gives MODULATE's alpha as the texture alpha alone,
   and the first build of this round did that; it lit every parked lens
   flare (face alpha 0, texture alpha 1 inside the ring: a blue disc over
   Sonic on the beach, rings over the police) and froze the beacons at full
   glow, none of which the console shows in the §10 montages. The game is
   the better reference than the manual here: both modulate forms take the
   vertex alpha, which is what the old mapping did.
2. **Translucent strips get a depth offset** of two units and one slope
   (`glPolygonOffset(-1, -2)` with `GL_POLYGON_OFFSET_FILL`, through a new
   `translucent` flag in `draw_state`). The game's decals sit on their
   surfaces without a Z-write change, so the only thing that keeps a
   headlight spot above the pavement is depth precision, and ours is the
   PSP's: 16 bits over `1 − 2/z`. The step in z grows with the square of
   the distance:

   | z (units) | 1/w | 16-bit step (`1 − 2/z`) | float32 1/w step |
   |---|---|---|---|
   | 20 | 0.050 | 0.006 | 1.5e-6 |
   | 50 | 0.020 | 0.038 | 4.7e-6 |
   | 100 | 0.010 | 0.15 | 9.3e-6 |
   | 200 | 0.005 | 0.61 | 1.9e-5 |
   | 400 | 0.0025 | 2.4 | 3.7e-5 |

   The spots in frames 5000–5880 are at 1/w 0.005–0.015 (z 70–200): a
   decal lifted a tenth of a unit resolves on the console's float
   compare and ties or loses on ours, so GEQUAL picks it or the pavement
   by rounding, pixel by pixel. Two units of offset win the tie. The PSP
   backend has the same 16-bit buffer and `sceGuDepthOffset` for the
   same purpose; the bigger lever there is the depth mapping itself (a
   per-frame range fit to the 1/w the lists actually use, instead of
   spending the top half of the buffer between z = 1 and z = 2 where
   nothing draws), noted in §6.
3. The offset-colour pass scales the highlight by the vertex alpha only
   under source-alpha blending (an additive strip adds it in full, as
   the hardware adds the offset before the blend).
4. The pass order is unchanged and matches the TA: opaque with depth
   write in submission order, punch-through with the alpha test, then the
   translucent strips sorted far to near by mean 1/w with ties in
   submission order, each strip with the depth write the header asks
   for (always on, in this game).

### Against the console

Method as in §10: ours replayed with `--screenshot`, the console through
the oracle from the state library (`tools/flycast/oracle_states.py build`
on the new tape: title 300, Station Square 1300, night cutscene 5300,
Chaos 0 fight 7500 reachable; everything after the fight dropped, because
the console loses the fight on the maintainer's inputs, twice over, and is back on
the title by vblank 29,760; a fifth state named `ss_day` at 12800 passed
the mode/level/act check but is the console mid-fight with three lives
against our four, so it must not be used). The console captures are taken
with the emulator thread held in the Lua vblank callback at the aligned
game frame (`shots_c4.lua`, scratch), so each pair is the same game
frame. Montages ours left, console right, in
`..\private\exit-test-3\compare4\` (not committed), with a `README.txt`
listing what each side shows:

| Montage | Frames (ours guest = game; console game) | Verdict |
|---|---|---|
| `night_surrounded_f5000.png` | 5000 = 2497; 2497 | Same shot, pose and tint. The console has thin rain streaks; ours none (below). |
| `night_surrender_f5120.png` | 5120 = 2557; 2557 | The maintainer's shot 12. Same composition; the headlight pools blend as on the console, with the same shape and no z-fighting at the car's spot in ours; theirs are brighter (40x40 patch at the pool centre: ours 153/163/164 RGB, console 193/203/202; the unlit asphalt next to it 49 against 49). Rain on the console. |
| `night_headlights_f5880.png` (and `_console+30.png`) | 5880 = 2937; 2937 (and 2967) | Same pose, camera and subtitle; Chaos's body, the beacons and the headlight pools blend the same. The console's searchlight at the top left is a soft bright shaft, ours is not there (a `smokelight` strip, ONE/ONE, below). The +30 capture shows the console cutscene is **not** 30 frames behind on this tape (the §10 lag was that tape's). |
| `night_f6040.png` | 6040 = 3017; 3017 | The maintainer's shot 14 neighbourhood: identical composition and tint; rain on the console. |
| `ec_scripted_g2763.png` | the scripted route (ec_scripted + the stage poke) 5531 = 2763; 2763 | The ring sparkle: the same small green burst on both; the console's a touch brighter (as §10). |
| `ec_scripted_g2800.png`, `g2830.png`, `g2850.png` | 5605, 5665, 5705 = 2800, 2830, 2850; the same | The shoreline: the sea over sand, the foam line, the umbrellas. The sea is translucent over the sand on both; ours is darker and bluer (open sea patch at 2830: ours 23/154/180 RGB, console 53/184/199; the dry sand 225 against 227) and the console has more bright glints on the water. |
| `ec_scripted_g2856.png` … `g3250.png` | from 2856 on | **Not rendering comparisons**: the routes split at game frame 2860 (below). |

What could not be paired: the cave pool and the splash of shots 17 and
18 (frames 24988 and 25612), which lie past the Chaos 0 fight; the
scripted route never reaches the cave. For those two the audit above is
the evidence: the sand layer over the pool is blended at the 0.40 the
header asks, and the splash is drawn ONE/ONE with its 1555 texture's alpha
intact (the particles are dense across the 64x64, so the quad reads as a
speckled square on any renderer); whether the console's look differs is
not established.

Verdict: the translucency of the frames that can be paired matches the
console in blend, shape, order and tint. The headlight pools' brightness
(ours about 20 % lower at the centre, the header's face alpha 0x80
honoured on both sides as far as the audit can tell) and the sea's
colour are differences of degree that the TA flags do not explain; the
searchlight shaft and the rain are absent in ours. Those four are listed
in §6 with what is known about each.

### Found on the way

- **The rain streaks are zero-wide.** The `drop` quads (187 per frame,
  the rain) come out of the sprite path with x0 = x1 to the pixel (e.g.
  `x -463..-463 y 9..10`), so nothing rasterises; the console draws them
  one or two pixels wide. §10 item 4 called the rain right after the
  billboard fix; on this tape's frames it is not. Sprite path, not
  translucency.
- **The searchlight cone** (`smokelight`, 565, ONE/ONE, modulate-alpha,
  colour `0xFFFFFFFF`/`0xB2FFFFFF`, 14 vertices, 1/w 0.005–0.018) is
  drawn additively with the night scene's fog off (the fog table here
  runs from z 7111, so nothing in view is fogged) and still leaves no
  shaft in ours. Not understood; the strip is identified for the next
  pass (`SA1_TA_BT` on it).
- **A collision difference on the scripted route.** The desktop and the
  console agree in Sonic's position to under a unit through game frame
  2851, both stop against the beach deck chairs at 2852, and at 2860 ours
  jumps 55 units forward past the chairs (z 407 → 463) while the console
  stays stuck there (it is then hit by the Egg Keeper at about 3070 and
  loses a life; ours runs on into the pier). The first divergence of the
  recompiled game from the console found by the oracle that is not
  timing; a seam or collision body to find.
- The console's stage timer reads three hundredths behind ours
  throughout Emerald Coast (the intro-pan timing of §10).

## 6. Open items

- **The exit test passed** with the fifth run (§11): `tests/replay/sonic_ec1.tape`
  and its check file; `tools/replay_test.py` passes on it. M2 is ticked
  in README §8. What the fourth round left (§11):
  - the headlight pools are about 20 % dimmer than the console's at the
    centre, with the asphalt beside them equal; the sea on the shoreline
    is darker and bluer (ours 23/154/180 against 53/184/199) with fewer
    glints. Neither is a TA flag; the offset-colour pass and the texture
    filtering are the suspects.
  - the searchlight shaft of the arrival cutscene (`smokelight`, ONE/ONE)
    does not show in ours; the rain (`drop` sprites) comes out of the
    sprite path zero pixels wide. Sprite/strip path work, not blending.
  - the cave pool and the splash (shots 17 and 18) could not be paired
    with the console (past the fight the console loses); the audit says
    both are drawn as the headers ask.
  - the scripted route diverges from the console at game frame 2860 at
    the beach deck chairs (ours passes them, the console stays stuck):
    a collision difference to find.
  - `..\flycast\states\sonic_ec1__ss_day.state` is misnamed and mid-fight
    on the console (lives differ); the builder's check should compare
    lives too, or the entry be dropped.
- Console timing (§10): the Chaos 0 arrival cutscene runs about 30 game
  frames slower on the console (voice playback the HLE finishes at once is
  the suspect), and the Emerald Coast intro pan ends later here than on
  the console. Neither breaks the route; both skew frame-aligned captures.
- Sprites (§10 item 4): an object-space `njDrawSprite3D` quad with a corner
  behind the near plane is skipped; the SDK clips it (`0x8C63F610`). The
  ring sparkle is a little dimmer than the console's.
- Seam stubs still called on the tape: `0x8C604370`, `0x8C64489C` (§10).
- A host stack overflow now reports itself (§10 item 6); the probe still
  samples only indirect calls and HLE entries, so a direct-call recursion
  reaches the guard band, not the probe.
- Rounding mode: the console runs the game in round-to-zero (§9 item 9);
  the host rounds to nearest. Positions agree to well under a unit, so it
  stays as it is; setting the host FPU mode at startup is the lever if a
  later comparison needs bit-exact floats (the HLE math would run in it too).
- `--goto` cannot leave the Emerald Coast start area on its own (walls,
  the sea); a route needs waypoints along the ring trail, and a position
  poke does not stick (the player struct is rewritten).
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
  per state change; the offset-colour pass is a second draw of every
  textured strip that has one. **Depth**: the 16-bit buffer over `1 − 2/z`
  resolves 0.6 units at z = 200 and 2.4 at z = 400 (§11), and the game's
  decals rely on the console's float 1/w with Z-write on; the desktop
  pulls translucent strips nearer by two depth units (`glPolygonOffset`),
  the GE has `sceGuDepthOffset` for the same, and a per-frame depth range
  fitted to the 1/w the lists use (nothing draws between z = 1 and 2, where
  half the buffer goes) would buy the rest. The shading rules of `draw.h`
  map to `sceGuTexFunc` (modulate / decal / replace, `GU_TCC_RGB` for
  ignore-texture-alpha).
- `njDrawPolygon2D` fans are approximated as strips; no fan on the route.
