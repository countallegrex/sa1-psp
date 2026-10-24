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

- **View culling** (`land_cull = on`, the default, `--land-cull off`;
  independent of the renderer, on in strict runs too): every model of the
  current land table gets a sphere over its points when the table changes
  (no converted data needed: it works in every level).  A model whose
  sphere lies wholly outside one of the view's half-spaces (z >= 0, the
  wide view's x range and 0..480 in y, with an 8-pixel margin) skips the
  transform, the lighting and the mesh-set loop, exactly as a skipped frame
  does; nothing it would have drawn is on screen.  Models with a
  translucent material are never culled: their strips go to the
  depth-sorted list, and without them five pixels of Sonic came out
  slightly different on the console (the sort's order of the rest
  changes).  With that rule the console pictures are the TA path's to the
  byte (Station Square and Emerald Coast, frames 200 and 400).
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
in Station Square: the translucent rule costs about 3 ms.  Emerald Coast
has little land out of view; what it pays is the decision per model (to be
made cheaper).  GE busy time (no pipelining, the stall address withheld so
the GE runs after the list is closed): 4.4 ms (TA) and 4.5 ms (native) per
frame, far from the frame time.  Heap at exit, Station Square: 32.8 MB
(TA), 32.6 (cull), 33.5 (native, largest free block 2.1 MB).  The game's
own land cull and draw loop costs about 0.3 ms (profile), so hooking the
land draw higher up buys nothing.  What is left of the land in Station
Square (native 40.4 against 27.75 with no land): the models the GE refuses
whole (near plane, guard band; about 12 a frame), the translucent and
environment-mapped models (about 26 a frame, not converted), and the
translucent ones no longer culled.
