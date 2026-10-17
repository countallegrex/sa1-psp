# M2 Desktop graphics

Exit test (README §8): Emerald Coast completed with a controller on
desktop, draw order correct, no desktop-only features.

Status: **renderer done, exit test not run (2026-10-17).** The desktop
build draws every scene on the scripted route from guest RAM, and Emerald
Coast when the stage variables are forced; a human run with a controller
through the real route (Station Square → Chaos 0 → Emerald Coast) is still
needed to tick the milestone. Everything below was measured with
`build-desktop/backend/desktop/sa1psp.exe` (clang 23, game code `-O1`),
the US v1.005 dump and the `SONICADV` tree, on a GeForce GTX 1070 Ti.

*(Screenshots of the running game are not included in the public repository.)*

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

`--headless` (no window; the M1 behaviour), `--scale N`, `--screenshot
FRAME:FILE.ppm`, `--stats N` (the render counters every N frames),
`--dump FRAME:FILE` (guest RAM), `--poke FRAME:ADDR:VALUE[:SIZE]`,
`SA1_KAMUI_DUMP=<frame>` (every header, pointer hand-off and writer call of
that frame), `SA1_MODEL_DEBUG=1`, `SA1_NOVSYNC=1`, `SA1_REPORT_TOP=N`.
The stage variables: `0x8C7493BA` current level, `0x8C7493BC` act,
`0x8C7493BE`/`C0` next level/act (Emerald Coast = 1, Chaos 0 = 15,
Station Square = 26); GameMode at `0x8C760970`.

## 6. Open items

- **The exit test.** Needs a controller run through the real route. The
  Chaos 0 fight must be won (the overlays doc says the scripted pad cannot).
- Title logo (§3).
- `hle_wraps.tsv` still lists the eight non-writers (three collision
  functions, five flag tests) as wrapped; they run unchanged through the
  wrappers. Dropping the rows means a main-image re-emit.
- Emitter: register-bank switches (`SR.RB`) should be lowered, or at
  least detected and reported.
- PSP shape to keep for M4: 32 MB desktop window vs 18 MB on the PSP
  (§1 item 4); CLUT textures stay paletted there; strips should be merged
  per state change; the 16-bit depth buffer with `1 − 2/z` loses precision
  far away (no artefact seen at 480x272).
- `njDrawPolygon2D` fans are approximated as strips; no fan on the route.
