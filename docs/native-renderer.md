# PSP-native renderer: the static scenery slice (design)

Orders item 3b/d (2026-10-22): re-express the Dreamcast's land drawing the PSP
way instead of translating it every frame.  This note covers the first
vertical slice, Station Square's static scenery (the land table), its
desktop check, and the go/no-go measured on the console.  Status: NO-GO
(2026-10-24, section 5): the GE path drew correctly but was not cheaper
than the TA path; the code stays behind `renderer = native` (off by
default) and the big lever moves to the Media Engine.  Iteration 3
(section 6) found that half of the land the game draws is out of view: the
view cull (`land_cull`, on by default) and persistent GE models
(`renderer = native`) take Station Square from 50.2 to 37.6 ms.

Prior art read for ideas only (no code taken): JGR Portable builds GE
command packets per stage batch once and replays them with sceGuCallList,
with GE hardware transform and baked vertex colours
(private/api-worker/jgr-study.md, lever 1).

## 1. Why the land table

Measured on the desktop (sonic_to_ec1_start, a sample every 30 land draws,
geometry as submitted):

| Station Square | land strips / all | land strip vertices / all | land triangles / all |
|---|---|---|---|
| act 3 (night) | 1944 / 3518 (55 %) | 10.1k / 16.4k (62 %) | 6.2k / 9.4k (66 %) |
| act 4 (day) | 1350 / 2365 (57 %) | 7.6k / 13.1k (58 %) | 4.9k / 8.3k (58 %) |

Of the vertices the CPU transforms, 63-70 % are land.  Today every one of
them goes through the game's model pipeline every frame (transform
0x8C037294, lighting 0x8C037350, mesh-set loop 0x8C0376D0, the TA strip
writers), then kamui's parse, strip_emit and the CPU clipper.  The land is
static in world space, so all of that can be done once.

## 2. What the game does (the behaviour to keep)

- Current land table: `*(0x8C7596F4)`, from the per-level table at
  0x8C19E9FC `[level * 8 + act]`.  Station Square is level 26 (27 and 28
  share its six tables); its data is in `SONICADV/ADV00.PRS`, loaded at
  0x8C900000 with absolute pointers (no relocation when header flag 0x10 is
  clear, as in Station Square).
- Layout: header +0 COL count, +4 flags, +8 draw distance, +12 COL list,
  +24 texture list; COL (0x24 bytes): centre, radius, object, group mask
  (+28), flags (+32: 0x80000000 visible, 0x20 far-clip distance).  Each
  Station Square COL is one object with one basic model (triangle strips,
  UVs, normals, no vertex colours).
- Culling (fn_8c0520c8, every frame): a sphere test against a point half the
  draw distance ahead of the camera; the result is the visible list at
  0x8C75A30C (count 0x8C75A308).  No frustum or per-model culling.
- Drawing (fn_8c051e56): for each visible entry, last to first:
  `njTranslate`, `njRotateXYZ` (no scale), draw the basic model with no
  per-model culling (fn_8c037108), restore the matrix; the land context
  disables back-face culling and forces bilinear filtering.
- Materials: texture = material +12 into the level's texture list; opaque
  materials are ONE/ZERO with texture alpha ignored; USE_ALPHA goes to the
  translucent list with the material alpha; IGNORE_LIGHT is decal with no
  offset colour; ENV derives UVs from normals.
- Lighting: per vertex, index = clamp(trunc(127.5 (L.n) + 127.5), 0, 255)
  into a 256-entry palette (from `PL_<level><act>B.BIN`): base colour
  (modulates the texture) and, for materials with specular, an offset
  colour from the next bank.  L is fixed in world space for an act and a
  time of day (it is stored view-relative, then brought back to model space
  by the matrix), so the result per vertex is static.
- Nothing logic reads is written by the land draw (only renderer and
  context state; the cull's 0x8C75C30C has no reader).

## 3. Design

### 3.1 Offline converter (launcher side, the user's own data)

`tools/land_convert.py --data DUMP/data --out DATA` reads `ADV00`
(decompressed with tools/prs.py) and the level table in `1ST_READ.BIN`,
and writes `DATA/native/LAND_<level>_<act>.BIN` per land table (Station
Square: six tables, 43-698 KB; act 0 has 240 drawn COLs, 11,468 triangles,
679 mesh sets).  For every COL with the visible bit, each mesh set of its
model becomes indexed triangles (16-bit indices) in model space, keyed by
the model's address and the set's index, with each vertex's UV (the game's
UV scale applied) and the index of the model point it came from.  Raw
material words travel along for checks.  `--world` writes world-space
positions instead (the COL object's njTranslate and njRotateXYZ), for a
later one-matrix draw.  Data goes only to the user's data folder, never
into the repository (it is game data).

### 3.2 Runtime (HLE)

The cleanest hook is inside our native mesh-set loop (hle/ninja/ta_mesh.c,
0x8C0376D0), which already runs for every land model:

- An HLE override of the land task 0x8C051E00 (`sa1_register_fn`; the game
  calls it through a task pointer) marks the land window and runs the
  game's own task: the cull, the visible list, njPushMatrix/njSetTexture,
  every COL's njTranslate/njRotateXYZ and the context, exactly as today.
- In that window the vertex transform 0x8C037294 returns at once (as on a
  skipped frame).  The lighting 0x8C037350 still runs, so each model point's
  base and offset colours are the game's own, bit for bit; caching them per
  model while the light and palette stay the same is the next saving.
- The mesh-set loop still computes each set's polygon header words, the
  texture and the material alpha, but instead of calling a strip writer it
  hands the set's converted triangles to the backend with that header (its
  draw state through kamui's header decoding), the per-point colours, and
  the game's matrix for that COL (XMTRX) combined with the screen scale and
  centre (0x8C8FFE00..).  One matrix per COL, no CPU transform, no TA bytes,
  no parse, no CPU clip.
- The guest state after the land draw differs from today's only in draw
  scratch (the transformed-vertex buffer, TA pointers, header words), as on
  a skipped frame; tapes and RAM compares check it.

### 3.3 Backend interface

A new call next to `draw_strip`: `draw_mesh(matrix, vertices, indices)`,
where the matrix takes a model-space point to the homogeneous vector the
strip path already feeds the backend, (x z, y z, -(z + 1), z) for screen
x, y and depth z: that map is linear, so the backend's own projection,
depth and fog stay as they are and its hardware clips at the near plane.
The backend keeps the converted mesh in its own memory and draws it with
hardware transform:

- PSP: indexed triangles, GU_TRANSFORM_3D, one model-view matrix for the
  whole land (the vertices are in world space), prebuilt display lists per
  batch replayed with sceGuCallList; textures from the existing texture
  handles (swizzled, mipped, the texture pool).  Offline PSP texture formats
  (CLUT/DXT, VRAM pinning) are a later step.
- Desktop: the same through GL vertex arrays and a matching projection, for
  the screenshot check.

The offset (specular) colour has no per-vertex equivalent on the GE: the
slice draws materials with specular in a second additive pass (first check
how many there are in Station Square; if none, nothing to do).

### 3.4 Toggle and checks

- `renderer = native | ta` (sa1psp.ini) and `--renderer`: native off by
  default until the console go/no-go; `ta` always in strict, oracle and test
  runs unless forced, like the frame skip.
- Equality of game state: the replay tapes with the native renderer forced
  (watch traces identical, guest RAM identical outside the draw scratch).
- Look: desktop screenshots, native against today's path and against the
  oracle's shots, at several Station Square views by day and night.

## 4. Go/no-go (console)

Measure Station Square with the slice on and off (ABBA, tools/psp_bench.py).
Go if it saves at least ~60 % of the land's measured render time from the
fresh profile; then characters (GE skinning), effects and shadows, one type
at a time.  No-go: write up why and move to the Media Engine instead.

## 5. Result (2026-10-24, PSP-3001, `--stage ss`, frame skip off, ABBA x3)

| run | Station Square | note |
|---|---|---|
| TA path | 48.99 ms | today |
| land not drawn (`renderer = noland`) | 27.75 ms | the land costs **21.24 ms**: the go bar was a saving of 12.7 ms |
| native #1 (8980e72: sphere test inside draw_mesh, CPU per-triangle fallback) | 57.98 ms | +9.0: 46 % of meshes fell back to a per-triangle CPU path dearer than TA |
| native #2 (decision per model before the transform, failures untouched TA, lighting kept per model) | 51.51 ms | **+2.5**: 45 % of the land's models accepted (5,932 vs 7,306 left to TA) |

Pictures matched on the console (0.02-0.05 % of pixels differ, none by more
than 60 levels) and the game state matched on the desktop (tapes, watch
traces, guest RAM outside draw scratch), but by the kill criterion agreed
beforehand (the meshes on the GE path must be cheaper than the same meshes
on TA) the slice is a no-go.  Where the time went in #2: kamui's CPU time
fell only from 14.08 to 13.78 ms while the replay rose from 3.30 to 4.80 ms.
Skipping the transform and the lighting saves little on accepted models
because the per-frame work that replaces them is of the same order: every
native vertex is gathered (kept colours, positions, UVs) into a draw record,
converted again into the GE layout in the frame's pool, and each mesh set
costs an sceGuDrawArray syscall and an sceGuSetMatrix in the replay.  Half of
the land's models do not qualify at all (large models crossing the guard
band or the near/far split of the 16-bit depth passes), so they keep the TA
cost.  What it would take to pay (not done: no second iteration without new
evidence): persistent GE vertex buffers per model (no per-frame gather or
conversion), prebuilt display lists replayed with one call, and splitting
the land's large models at convert time so most qualify; even then the bar
is 12.7 ms out of a 21.2 ms land cost.

## 6. Iteration 3 (2026-10-24): view culling and persistent models

A reason counter on the decision (`draw_mesh_test`; console, `--stage ss`,
224 frames, 13,568 land model draws, 676k triangles) showed where
iteration 2's rejections came from: **53 % of the land's model draws (50 %
of its triangles) lie wholly outside the view**, behind the camera or past
a screen edge.  The game's land cull (s.2) is a sphere around a point ahead
of the camera, not a view test, so the TA path transformed, lit, clipped
and threw away all of them every frame.  The rest: 26 % of the draws
qualified for the GE whole (28 % of the triangles), near plane 15 %
(16 %), guard band 5 % (6 %), depth split none; iteration 2's "other"
rejections were an artefact (the backend's layer at decision time was the
previous frame's last).  Splitting models at convert time would barely
help: per model 80 % of draws are GE-or-out, with cells of 128 units 84 %,
of 32 units 88 %; it would only move triangles from the TA path to a CPU
clip path, so it was not built.

What iteration 3 does (`hle/ninja/native_land.c`, `backend/psp/draw_gu.c`):

- **View culling** (`land_cull = on`, the default; `opaque`, `off`;
  `--land-cull on|opaque|off`;
  independent of the renderer, on in strict runs too): every model of the
  current land table gets a sphere over its points when the table changes
  (no converted data needed: it works in every level).  A model whose
  sphere lies wholly outside one of the view's half-spaces (z >= 0, the
  wide view's x range and 0..480 in y, with an 8-pixel margin) skips the
  transform, the lighting and the mesh-set loop, exactly as a skipped frame
  does; nothing it would have drawn is on screen.  Translucent models are
  culled too (since the run-break fix below); `opaque` keeps 62c9300's
  rule, which never culled them, for console A/B runs.  With that rule the
  console pictures were the TA path's to the byte (Station Square and
  Emerald Coast, frames 200 and 400).
- **The run break** (`backend/psp/draw_gu.c` strip_emit).  Culling the
  translucent models changed five pixels of Sonic on the console, not on
  the desktop.  The cause found (to be confirmed by console pictures) is
  the GE backend's batching, not the sort:
  same-state strips go as one GE draw, and the run's offset (specular,
  additive) draw is replayed after the whole run, so in a translucent run
  strip B blended before strip A's offset instead of after it; another set
  of land models in front of the list changed where the runs began.  A
  translucent strip now never continues a run with offset vertices
  pending, so each translucent strip's base-then-offset order is the
  desktop's per-strip order (draw_gl.c's emit_offset).
- **The decision's cost** (`native_land_decide`).  A transform outside the
  land table's address range (the lowest to the highest model) is
  rejected with two compares; a land model is found through a packed key
  array (4 bytes a probe) behind a next-model hint (the land draw's order
  stands from frame to frame: 88-89 % of finds in Station Square and Emerald
  Coast); `view_out_fast` moves the sphere's centre by XMTRX once and tests
  it against plane normals made only when the viewport words change (about
  500 times a tape), instead of building the game matrix for every model.
  `game_matrix` runs only for the native renderer.  `--land-decide old`
  is 62c9300's path (game matrix, view_out, plain binary search), kept as
  a measurement switch; `--land-decide check` makes both decisions and
  logs the mismatches at exit (0 of 567k in sonic_to_ec1_start, 0 of 886k
  in sonic_ec1, translucent models included).
- **Persistent models** (`native`): a converted, opaque model the GE takes
  whole is drawn once per set the iteration 2 way, which hands kamui's
  decoded state of each set back to native_land; with every set's state
  known, `draw_model_create` converts the vertices once (kept lighting
  baked, the set's colour rules) into memory of their own.  From then on
  the transform, the lighting and the strip writers are skipped and the
  mesh-set loop puts one marker in the list; the backend sets one model
  matrix and CALLs a display list it built at the model's first draw
  (every set's state, texture bind and one indexed draw).  A change to any
  texture's memory rebuilds the lists at their next draw; the model's
  textures are touched in the pool every frame it is drawn.
- The lighting is baked at the first draw from the game's own lighting
  rather than in land_convert.py: the result is the same (the palette light
  is fixed per land table), it needs no copy of the lighting code, and the
  check mode (`SA1_NATIVE_CHECK=1`: persistent models drawn per set and
  compared) found 0 differing states and 0 differing colours over 193k set
  draws of sonic_to_ec1_start.

### 6.1 Iteration 4 (2026-10-24): Emerald Coast's models

With Emerald Coast converted (`land_convert.py --level 1`: STG01.PRS) only
5 of its models went persistent on the console; ~16k land draws a stage
start went the TA way as "not converted".  The scoping study blamed the
translucent and environment-mapped sets of mixed models; the desktop's
reason counter found three causes, the split being the smallest:

- **Models under several COLs** (19 of 102 in act 0, 25 in Station
  Square's act 0): the converter wrote a copy of the model's batches per
  COL, which the loader read as "sets differ".  The converter now writes a
  model once (act 0: 1,320 -> 696 KB); the loader keeps the first copy of
  older files.
- **Their lighting**: the lighting takes the light into model space
  through XMTRX (ta_xform.c 0x8C037350), so a model turned differently
  under another COL is lit differently.  A model now keeps one lighting per
  model-space light (to 1e-5, `MAX_VARS` = 16 a model, the rest the TA
  way), each with its own kept colours and persistent copy.
- **Animated textures**: Emerald Coast's shore cycles a texture by
  rewriting the texture list's entry, which a persistent copy's state
  would freeze.  The textures the opaque sets would bind are noted while a
  model draws per set and compared at every persistent decision; a change
  sends the model the TA way for good.
- **The split**: a model with translucent or env-mapped sets besides
  opaque ones (17 in act 0) puts its opaque sets in the persistent copy
  and leaves the others to the game's strip writers (transform and
  lighting run as on the TA path; kamui's translucent sort is untouched).
  The GE test stays per model.

Memory: the converted file no longer stays loaded.  Its tables (header,
materials, batches: 14 KB) are read at the land table's change, a model's
opaque vertices and indices only for its per-set draws and freed once all
its lightings have a copy (staging at most 146 KB at the stage start); the
copies in the PSP layout come to 280 KB at Emerald Coast's start, 381 KB
after the scripted walks, 277 KB in Station Square (`native land: memory`
at exit; draw_gu.c's model report gives the bytes made).  Desktop, stage
start (700 frames, desktop GE test always accepts): Emerald Coast 19,560
persistent model draws of 24,525 (TA 3,918: no opaque set or animated; 981 out of view;
29 copies, 6 models dropped as animated), Station Square 18,054 of 37,034 with 16,700
out of view (50 copies).  Checks:
`SA1_NATIVE_CHECK=1` over sonic_to_ec1_start 0 of 699,509 set states and
0 of 200,590 lightings differ; the tapes pass with default, `force:native`
and `--land-cull opaque`; desktop pictures native against ta at most 1
level apart but for a handful of edge pixels (<= 0.001 %).

The strip clipper (draw_gu.c strip_split) drops a triangle wholly past one
edge of what any projection shows before clipping or cutting it; the
console counts them beside the guard-band strips.

One pspsdk trap: `sceGuDepthOffset` re-sends the depth range from the
current context's stored planes, which a GU_CALL context never had; every
list left a 0..0 range behind (4.7 % of pixels wrong) until the list starts
with draw_init's `sceGuDepthRange`.

Correctness: the four tapes pass with `force:cull` and `force:native`;
guest RAM equals the TA path's outside draw scratch (the transformed-vertex
buffer, the model renderer's gbr scratch, kamui's buffer descriptor and
list pointers); console pictures: cull identical to ta, native 0.025-0.051
% of pixels (iteration 2: 0.02-0.05 %).

Console (PSP-3001, ABBA x3, frame skip off, mean of f300/f450, medians):

| run | Station Square | Emerald Coast |
|---|---|---|
| `--land-cull off` (TA path) | 49.46 ms | 79.28 ms |
| land_cull (default) | 42.14 ms (-7.32) | 80.19 ms (+0.91) |
| land_cull + `renderer = native` | 40.43 ms (-9.03) | 80.41 ms (+1.13) |

An earlier batch that culled translucent models too gave 39.14 / 37.56 ms
in Station Square: the translucent rule cost about 3 ms (now the default
`on`; `--land-cull opaque` measures the old rule).  Emerald Coast has
little land out of view; what it paid was the decision per model (now the
cheaper path above; `--land-decide old` for the A/B).  GE busy time (no pipelining, the stall address withheld so
the GE runs after the list is closed): 4.4 ms (TA) and 4.5 ms (native) per
frame, far from the frame time.  Heap at exit, Station Square: 32.8 MB
(TA), 32.6 (cull), 33.5 (native, largest free block 2.1 MB).  The game's
own land cull and draw loop costs about 0.3 ms (profile), so hooking the
land draw higher up buys nothing.  What is left of the land in Station
Square (native 40.4 against 27.75 with no land): the models the GE refuses
whole (near plane, guard band; about 12 a frame), the translucent and
environment-mapped models (about 26 a frame, not converted), and the
translucent ones no longer culled.

### 6.2 Iteration 5 (2026-10-24): cells, the CPU path, animated textures

Console counters at b263c92 (Emerald Coast start, f460): of the land draws
the TA way, 1,297 were models the GE refuses whole (near plane 1,079, guard
band 218) and 2,361 "not converted".  The desktop, which now applies the
PSP's rule (`draw_mesh_test` is `draw.h draw_ge_geo` on both backends),
splits the latter into models with no opaque set (translucent or
environment-mapped only: water, shadows; 1,926 draws, 44k tris over 700
frames) and models with an animated texture (1,920 draws, 258k tris: the
shore's opaque sets, whose texture list entry cycles).  No model was left
out of the file for its mesh set types: the 45 "not in the file" are
invisible COLs, never drawn.

- **Cells** (`land_convert.py`, file version 2, `--cell 32`): each model's
  opaque triangles are binned by centroid into 32-unit cubes in model
  space, ordered by cell within each set (a cell is one index range per
  set, a span), and each cell gets a sphere and its longest edge.  The
  converter also takes triangle and quad lists (types 0, 1: fixed lengths 3
  and 4 at 0x8C037980) and N-gons, so Emerald Coast act 1 converts.
  Version 1 files still load (no cells: refused models go the TA way).
- **Per-cell rule** (`draw_model`, both backends): a model `mesh_geo`
  refuses whole is classified cell by cell (draw.h `draw_cell_class`):
  wholly outside a view plane: skipped; in view with every vertex at z >=
  max(split, 1 + 2 x longest edge) and the sphere inside the guard band: an
  indexed draw of the persistent copy in the far pass (neighbouring cells of
  a set merged into one draw, the set's state recorded as for a strip); the
  rest: the CPU path (`draw_cpu_tri`): the vertices through the game's
  matrix, cut at z = 1 with attributes linear in view space and dropped
  when wholly nearer, as the game's clipper 0x8C037EE0 and its writer
  0x8C037CCC do, then `draw_strip` (guard band, split).  So no visible
  triangle that needs clipping reaches the GE.  The whole-model test is the
  pretest: per-cell tests run only for refused models.  The per-set draws
  that capture a refused model's states go the CPU way whole (`draw_mesh`).
- **Animated textures**: the textures the opaque sets bind (`tex_now`) are
  part of the variant key beside the light (`MAX_VARS` 32); a new set of
  textures under a lighting already copied becomes `draw_model_variant`: the
  copy's vertices, indices and cells with the new states (only its call
  list is its own).  Emerald Coast's shore makes 84 such variants at the
  start.

Desktop, 700 frames, `--renderer native` (the PSP's GE rule):

| | Emerald Coast start | Station Square (act 3) |
|---|---|---|
| persistent whole | 21,432 draws, 1.91M tris | 18,404, 855k |
| refused whole, by cells | 1,962 (near 1,635, guard 327); cells out 51.8k / GE 27.5k / CPU 23.0k; tris out 273k / GE 107k / CPU 87k | 5,367 (near 3,670, guard 1,697); cells out 84.0k / GE 8.0k / CPU 39.6k; tris out 167k / GE 27k / CPU 72k |
| TA way | 1,962 (45k tris), all "no opaque set" | 1,943 (25k), all "no opaque set" |
| split models' TA sets | 248k tris | 48k tris |
| land tris drawn: GE / CPU path / TA | 84 % / 4 % / 12 % | 86 % / 7 % / 7 % |
| persistent memory (PSP layout) | 587 KB (cells 89 KB) | 334 KB (cells 56 KB) |

At b263c92 the same runs made 280 and 277 KB.  Checks: the tapes pass with
default, `force:native` and `--land-cull opaque`; `SA1_NATIVE_CHECK=1` over
sonic_to_ec1_start: 0 of 731,352 set states and 0 of 211,352 lightings
differ (that mode draws every refused model the CPU way); desktop pictures
native against ta: Emerald Coast start 0.002 % of pixels more than 1 level
apart (the shore water, now on the GE, is 1 level off on 1.6 %), Station
Square 0.001 % (max 2), walks w1/w3 at most 0.002 %.

`--ge-near-test FILE.ppm` (PSP only, main_psp.c) draws six flat triangles
through sceGu with the far pass's projection and GU_CLIP_PLANES (crossing
the eye plane, crossing the far pass's near plane in front of the eye,
wholly behind the eye, past the 4096-pixel window, both, and a control),
writes the frame and exits: what the console's GE does with them decides
whether the CPU path's share could be handed back to the GE.
