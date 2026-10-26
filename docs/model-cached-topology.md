# Cached topology for dynamic models (m0, design)

Status: design only, no code (2026-10-26, cfa654e). Step-0 measurements
included; the GO decision is the orchestrator's. Supersedes the per-frame
writer of strip-path-collapse.md s.2 (f7 slice 1 measured -0.87 ms in
Emerald Coast against a 2 ms bar: any writer that converts every corner every
frame costs at least ~1.5 ms, which eats the saving).

## 1. Idea

Characters, objects and enemies are drawn per node through the game's
njDrawModel. Today each frame, per point: the transform 0x8C037294 and the
lighting 0x8C037350 (ta_xform.c). Per corner: the gather writer 0x8C037990
(ta_direct.c) into draw_vertex, kamui's marker parse, strip_class and
strip_emit (draw_gu.c) into GE vertices. A position is written three times
per corner per frame.

Cached topology does the per-corner work once, when a mesh set is first
seen:

- **Cached per mesh set (model space):** GE vertices (position, UV, a colour
  slot) and the strip index lists.
- **Per frame, per node:** the sphere test and one matrix, XMTRX through the
  land's homogeneous mapping (native-renderer.md s.3.3). The GE does the
  transform.
- **Per frame, per point:** the lighting colour word, with the Ninja
  arithmetic unchanged.
- **Per frame, per slot:** one 4-byte colour store into the vertex array
  (section 4: slots are 83 % of corners, so this is the only per-corner-like
  work, and it is a load plus a store, not a conversion).
- **Per translucent strip:** the sort key.

## 2. Classes

**Cached:**
- Nodes whose model-space points are frame-constant and that pass the sphere
  test (`draw_mesh_test`: inside the guard band, within one depth pass).
- Within those nodes, sets that are textured and not env-mapped, opaque,
  punch-through or translucent.

**TA path, unchanged:**
- env-mapped sets (UVs from normals each frame);
- sphere-rejected nodes;
- near-plane nodes (the game's r13 test, which sends the sets to 0x8C037CCC);
- welded or posed nodes (points written per frame);
- untextured sets (0x8C037C3C);
- the game's other direct writers (0x8C09E778, HUD 2D, SDK draw2d).

A node that is only partly admissible: the transform runs for the TA sets
and the cached sets skip the writer, as land iteration 4 splits per set.

Near-plane nodes could follow later. The GE clips near-crossing triangles on
the PSP-3001 (`--ge-near-test`, native-renderer.md s.6.2-6.3: crossing
triangles drawn clipped, wholly behind not drawn, depth correct in both
passes). Only triangles wholly nearer than the depth split may be GE-clipped
in the near pass. Admitting them adds ~2 % of corners (section 4), so not in
slice 1.

## 3. Mechanics

**Identity and constancy.** The key is the model address (points, strip
lists, UV and material pointers as the walk of 0x8C037990 reads them). The
cache entry stores a content hash taken at build time.

Hashing every frame would put the per-point cost back. Instead:
- Nodes that were ever seen with more than one hash (Sonic's welded parts)
  are marked "posed" for the level and stay on the TA path.
- Otherwise a cheap per-frame probe: the hash of the first, middle and last
  point plus the counts.
- A full re-hash every N frames, and every frame in check mode.

Entries are dropped on level and overlay load, and when the texture slot of
a set changes.

**Vertex layout.** Per slot (a unique point and UV pair of a set): float
x, y, z model space, float u, v, the 8888 colour. That is 24 bytes, the
layout strip_emit writes today but in model space. The offset colour gets a
second, untextured, additive draw from a parallel array (position plus
colour), as sh_offset_pass does today.

**Colour hazard.** The GE reads frame N's arrays while the CPU lights frame
N+1. Colours are therefore double-buffered:
- The whole set array exists twice, alternating per drawn frame. A skipped
  frame writes nothing.
- The CPU writes the colours into the next copy through the cache and writes
  that copy's lines back once, at the end of lighting.

**Matrix and depth.** Per node, XMTRX times the screen scale and centre,
exactly as native_land builds its per-COL matrix. Depth passes and fog are
the land's: the far pass by default, the near pass with the depth floor when
the node's sphere lies nearer than the split.

**Opaque draws.** Per node, one item in replay: the matrix, then per set its
state (kamui's header decoding, done once and cached with the set; the
material alpha re-checked and the TCW resolved to a texture handle per frame
through texture_from_tcw, since textures load and the pool evicts) and an indexed strip draw. Sets of a
node with the same state merge into one draw.

**Translucent.** One sort entry per strip, so the sort keeps its order
unit. The key is kamui's: the smallest 1/w over the strip's corners, from
the same float operations ta_xform uses (z = XMTRX row 3 per point, w = 1/z).
That is computed per point, then a minimum over the strip's point list,
cached as point indices.

The payload is the draw item: matrix index and index range. Equal keys keep
write order, as cmp_key and radix_keys do. Check mode compares every key bit
for bit with the TA path's.

**Guest state.** An accepted node's transform and gather do not run, so the
transformed-vertex buffer, the TA pointers and the header words differ, as
on a skipped frame. The rest of guest RAM must stay equal (tapes --exact,
RAM compare outside draw scratch). ta_mesh still runs for its header words
when a later TA set of the same node needs them.

**Fallback.** Any failure (cache full, hash mismatch, a set the decoder does
not handle) runs today's path for that node. The cache is capped (1 MB,
LRU), and the heap rule stays (largest free > 1.5 MB in ec).

**Desktop.** The same items go through GL vertex arrays, so the check mode
and the screenshots run on the desktop first.

## 4. Step-0 measurements (desktop census, Emerald Coast)

Method: `private/m0/census.md`. A temporary hook in 0x8C037294, ta_mesh and
kamui's parse; the corners of each set credited from its header.
- **Idle:** frames 200-460, Sonic standing, the bench window.
- **Running:** ec_scripted.tape f5460-5900.

Non-land node corners per frame, idle (running): 8777 (7772).

| class | idle | running |
|---|---|---|
| written by 0x8C037990 | 96 % | 96 % |
| env-mapped sets | 29 % | ~32 % |
| near-plane (r13) | 3.1 % | 2.7 % |
| changed or new hash (posed, not drawn last frame) | 5.7 % | 12.9 % |
| sphere pass | 96.9 % | 95.9 % |

**(b) Admissible share** (constant, sphere pass, textured, non-env, not
near, 3D), in % of dynamic corners:

| | opaque + punch-through | translucent | total |
|---|---|---|---|
| idle | 40.2 % | 20.8 % | **61.0 %** |
| running | 30.8 % | 19.5 % | **50.3 %** |

- Read literally ("frame-constant and passing the sphere test", any class),
  the share is above 90 % in both windows.
- With the class filter it is 61 % idle and 50.3 % running: **the < 50 %
  NO-GO line holds, narrowly in motion**.
- Running undercounts by up to 2.7 % (nodes not drawn on the previous frame
  count as new).
- Under the section 3 rule (a node ever seen with two hashes stays posed for
  the level), model 0x0C1D9B60 (447 corners, constant idle, changing when
  running) leaves the cache after the first motion: the steady idle share is
  about **56 %**, not 61 %.

Admissible per frame, idle (running):
- nodes: 111 (85), of which 79 (60) are wholly admissible;
- sets: 255 (193);
- strips: 1296 (971);
- points: 2361 (1827);
- slots: 4441 (3476), 83 % of corners.

**(a) Class split of the addressable time** (p13 profile, ec 60.09 ms):

| bucket | ms |
|---|---|
| ta_xform | 5.8 (transform ~3.3, lighting ~2.5) |
| ta_direct + clip | 4.9 |
| ta_mesh | 2.3 |
| kamui | 5.6 |
| draw_gu strip work | ~7 |

Of the kamui and draw_gu shares, translucent land (~5.7, TA way) and the
other writers are not addressable, which leaves the ~17-18 ms gross of
plan-30fps.md s.10.

By corner share:
- removable gather: ~2.8;
- transform of points in wholly admissible nodes: ~1.7;
- kamui parse of the cached strips: ~2.0;
- strip_class/strip_emit/merging of the cached corners: ~3.0;
- **~9.5 ms gross idle, ~8 running.**

**(c) Near-plane console test:** done in f6/f6c (see section 2). No rerun
was needed for the design. Near-plane admission is ~2 % of corners and is
left out of slice 1.

## 5. Estimate (EC, ms per drawn frame)

| item | idle | running |
|---|---|---|
| gross removed (section 4) | -9.5 | -8.0 |
| per-node items in replay (111 / 85 nodes; land models cost 10-19 us each incl. matrix, p1) | +1.1..+2.1 | +0.9..+1.6 |
| per-slot colour store + line writeback (4441 slots x (1 + offset share), ~107 KB+; basis: claiming the strip store's ~236 KB/frame of lines saved ~0.7 ms, 4247e12) | +0.2..+0.4 | +0.2..+0.3 |
| translucent keys (2361 points, 397 strips) and sort items | +0.4..+0.6 | +0.3..+0.5 |
| sphere tests, probes, cache bookkeeping | +0.2..+0.3 | +0.2 |
| **raw net** | **-5.1..-7.6** | **-4.2..-6.4** |
| **discounted x0.5** | **-2.6..-3.8** | **-2.1..-3.2** |

- **GO bar:** >= 3.5 ms net ec, ss not slower.
- The discounted estimate reaches the bar only in its best case idle and
  misses it in motion.
- This is lower than plan s.10's -4..-5, because the measured admissible
  share is 50-61 %, not the ~70 % implied there, and env sets (29 %) stay
  out.
- ss has fewer dynamic corners per node, so the per-node cost weighs more
  there. ss must be measured, not assumed.
- Memory: the level's distinct admissible sets, two copies at 24 B per slot,
  likely 0.3-1 MB. Capped.

## 6. Plan if GO (25-35 h)

0.5. **Spike (~2 h, before anything else):** the per-node replay cost
   decides most of the section 5 range. Issue the real admissible nodes as
   extra replay items (matrix + the set's state + a degenerate one-triangle
   indexed draw per set) alongside the unchanged TA path, same-binary
   switch, ABBA x3 ec + ss. **Kill line:** more than 2 ms at ~111 nodes /
   255 sets means NO-GO; under ~1.2 ms puts the estimate near its upper end.
1. **Desktop cache and check mode:** build, probe, keys; the TA path still
   draws; compare cached positions through XMTRX against ta_xform's
   records within tolerance, and colours, UVs and keys bit for bit
   (8-10 h).
2. **Opaque cached draws on the desktop and PSP**, switch `--model-cache`
   (default off until GO): pictures under the gate, tapes --exact, ABBA x3
   ss + ec. **Kill point:** opaque alone must show >= 1.5 ms ec net
   (8-10 h).
3. **Translucent strips into the sort:** key parity in check mode, the
   picture gate on the sorted water and effects (6-8 h).
4. **Default on** after the maintainer's play-test; the hot-order regeneration
   (tools/psp_hot_regen.sh) (2 h).

## 7. Risks

- **Per-node replay cost:** the largest unknown. 111 items each with a
  matrix and state; it decides most of the range above. Prebuilt per-set
  display lists (as persistent models) would cut it but need stable
  texture handles.
- **Motion:** the running window is at the kill line (50.3 %), and a level
  with more posed characters (Chao, cutscenes) is below it.
- **Hidden consumers of guest draw scratch:** a game routine reading
  transformed records, such as shadows or lens effects. Tapes --exact and
  the RAM compare catch it, and the node then stays on the TA path.
- **24-bit GE matrices near the camera:** the picture gate.
- **Seams** where a cached set meets a TA set of the same node (welded
  joints): depth fighting at the joint. Check the shots at Sonic's joints.
- **The colour double buffer and skipped frames:** a skipped frame must
  not flip the buffer.
