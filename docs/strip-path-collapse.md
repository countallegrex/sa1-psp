# Collapsing the strip path for dynamic models (f7, design)

Status: design only (2026-10-24, b263c92). Scope: what the land native
path (native-renderer.md s.6) does not cover: characters, objects and
enemies, drawn per node through the game's njDrawModel. The game's own
direct TA writers, sprites, 2D and nodes the GE refuses keep the TA path.

## 1. Today, per vertex

Per model point: the transform 0x8C037294 (ta_xform.c) writes a 16-byte
screen-space record into the guest vertex buffer; the lighting 0x8C037350
moves the light into model space once per call (three FIPRs) and writes the
colour word into that record and the offset colour into a 4-byte table.
Per strip corner: the writer 0x8C037990 (ta_direct.c) gathers record, UV and
offset by index into a 28-byte host `draw_vertex`, plus one 32-byte marker
per strip in the TA buffer. At kmRender, kamui reads the marker and calls
`draw_set_state` and `draw_strip` per strip (a further copy into `verts` for
translucent strips and other header shapes). `strip_emit` then converts every
corner again (reciprocal, x*z, y*z, colour swap, offset vertex) into the
GE pool. A position is written three times per corner. Clipping is split:
the near plane on the CPU in the game's own writer when a point lies at or
before it (0x8C037CCC and its clipper), the guard band and the depth split
in draw_gu per strip.

f4 profile (EC, ms per drawn frame, total / non-land): ta_xform 6.6 / 3.6,
ta_direct 7.4 / 3.6 (+ clip 2.5 / 0.5), kamui 9.0 / 6.0, draw_gu 22.0 / 9.1,
ta_mesh 2.1 / 1.4. The land share is native_land's job; f7's base is the
non-land column.

## 2. Proposed path

Iteration 2 of the land lost because its per-frame work (gather, convert
again, a draw call per set) cost as much as what it removed. So each corner
is written once, in the GE layout, and never converted again.

- **Decision per node, before the transform** (as `native_land_decide`):
  the model's own centre and radius through XMTRX, against the same test as
  `mesh_geo` (in front of the near plane with a margin, inside the guard
  band, within one depth pass). A rejected node runs exactly today's path,
  so a reject costs one sphere test.
- **Accepted node**: the transform is skipped (as on a skipped frame); the
  lighting runs with today's arithmetic into a per-point host scratch, no
  guest writes. For each qualifying mesh set the writer reserves GE vertex
  space from the backend and writes every corner once: model-space position
  from the point, UV, colour, and the offset vertex where the set has one.
  kamui gets one marker per set carrying the header state; the parse records
  one draw item (matrix index, vertex range) with no copy. Replay sets the
  node's matrix once (XMTRX with the screen scale and centre, the land's
  homogeneous mapping) and issues one raw-word strip draw per set. The GE
  transforms; the guard-band test made sure it never has to clip x or y.
- **Mixed nodes** (some sets translucent or env-mapped): the transform runs,
  the qualifying sets still take the new writer, the others the TA writers,
  as iteration 4's split does for land.
- **Per frame vs cached**: positions are read every frame (characters are
  welded and posed); the slice caches nothing. Caching the strip topology
  per mesh set (corner order, UVs) is a later step.
- **Translucent** (later slice): one sort entry per strip, keyed as kamui
  keys it today (smallest 1/z over the strip, z from XMTRX's third row), in
  write order, whose payload is the item above. The first slice leaves
  translucent sets on the TA path, so the sort is untouched.
- **Desktop**: the same items through GL arrays, so the checks run first.

## 3. Estimate (EC, ms per drawn frame)

| bucket | non-land | slice 1 removes | all model types (rough) |
|---|---|---|---|
| ta_xform | 3.6 | 0.5-0.7 (transform of whole-slice nodes) | ~1.5 |
| ta_direct (+clip) | 3.6 (+0.5) | 1.6-1.9 (the gather) | ~3.0 |
| kamui | 6.0 | 0.8-1.2 (marker parse, per-strip state calls) | ~2.0 |
| draw_gu | 9.1 | 2.0-2.5 (strip_emit, strip_class, merging) | ~4.0 |
| new writer and draws | - | +1.3-1.8 | ~+3.0 |
| **net** | | **about -3 to -5** | **about -7** |

Kept: the lighting arithmetic (moved), ta_mesh's header words (guest state),
the near-plane writer, the game's other writers, 2D, the sort, state changes.
Go bar: at least 2 ms net in EC.

## 4. First slice

Desktop dump (two frames at the EC start of sonic_to_ec1_start, renderer
noland; not the bench window): about 96 node draws and 4,400 corner records
a frame. Textured opaque non-env sets without a near point are 54 % of the
records (31 % in nodes made only of such sets: 46 % of the nodes, the ones
that skip the transform); translucent 29 %, env-mapped 14 %, near-plane
writer 3.5 %; the other direct writers add about 4 % (the untextured
writer 0x8C037C3C is not traced; the profile puts it at 0.07 ms). **Slice 1: textured, opaque,
non-env sets through 0x8C037990, nodes passing the sphere test.** Fallback:
today's path for the node or the set.

Measure: psp_bench ABBA x3, EC and SS, frame skip off, f300/f450 medians,
with a switch (ini and CLI, off in strict, oracle and tape runs unless
`force:`); a psp_prof rerun to see the buckets move; console shots at fixed
frames under the gate (at most 1 % of pixels differ from TA, none by more
than 32 levels except isolated edge pixels). Correctness: the four tapes
with the path forced; guest RAM equal outside draw scratch (vertex buffer,
offset table, gbr+60/64, TA pointers); a check mode in the spirit of
`SA1_NATIVE_CHECK` that runs both paths per node and compares set state,
colour and UV bit for bit, positions within a tolerance of the TA path's
x, y, 1/z, logging mismatches at exit; a reject-reason counter.

## 5. Risks and open questions

- The depth split at z = 16: how often does the player's node straddle it?
  Measure with the reason counter first; it may decide the slice.
- Does the stored radius bound a welded or posed node? The check mode
  should compare it against the actual extent.
- Does the GE clip at the near plane on hardware? A one-triangle console
  test would settle whether near-plane nodes could go native too.
- 24-bit GE matrices close to the camera against the picture gate.
- Seams or depth fighting where a GE-path node meets a TA-path surface
  (a mixed node's sets, shadows and decals drawn by the direct writers).
- Skipped frames and save states: the same rules as the land path.
- GE load: 5.2 ms busy today, far from the limit; recheck after.
