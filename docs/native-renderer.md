# PSP-native renderer: the static scenery slice (design)

Orders item 3b/d (2026-10-22): re-express the Dreamcast's land drawing the PSP
way instead of translating it every frame.  This note covers the first
vertical slice, Station Square's static scenery (the land table), its
desktop check, and the go/no-go measured on the console.  Status: design;
the renderer option is off by default until the console go/no-go.

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

`tools/land_convert.py` reads `ADV00` (decompressed with tools/prs.py) and
the level table in `1ST_READ.BIN`, and writes `data/native/LAND_<lv>_<act>.BIN`
per land table.  For every COL with the visible bit:

- the model's vertices pre-transformed to world space by the COL object's
  translation and XYZ rotation, computed exactly as the game's matrix
  helpers do (same float operation order, the fsca table, sh4_trig.h), plus
  the rotated normals for the runtime lighting bake;
- strips turned into indexed triangle lists per material (16-bit indices),
  merged across COLs per material where the draw order allows: opaque
  batches in any order, translucent ones in the game's order;
- materials reduced to what the backend needs: texture-list index, opaque or
  translucent, alpha, IGNORE_LIGHT, ENV, specular, flip/clamp;
- the COL index, so the runtime can draw exactly the game's visible list.

Data goes only to the user's data folder, never into the repository (it is
game data).

### 3.2 Runtime (HLE)

- A hook on the land draw fn_8c051e56: with the native renderer on and a
  converted file for the current land table, it runs the game's setup and
  matrix calls as today (njPushMatrix, njSetTexture, the context), then,
  instead of drawing each model through the pipeline, records the visible
  COLs (read from the game's list) and asks the backend to draw their
  batches with the current view.  Matrix pushes and pops stay balanced as in
  the original, so the guest state after the draw is the same as today's
  apart from the draw scratch (the frame-skip proof showed which addresses
  those are; tapes and RAM compares check it).
- Vertex colours are baked at the first draw of a land table and again when
  the palette or the light changes (a 32-byte compare per frame): the game's
  palette and light from guest RAM, applied to the converted normals with
  the game's own formula, so the colours match the pipeline's to the bit.
- Fog, the projection (Ninja screen distance and centre) and the far and
  near planes come from the same guest state the pipeline uses.

### 3.3 Backend interface

A new call next to `draw_strip`: `draw_mesh(mesh, view, proj)`. The backend
keeps the converted mesh in its own memory and draws its batches with
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
