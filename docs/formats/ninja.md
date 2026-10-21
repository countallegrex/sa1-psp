# Ninja and KAMUI as SA1 US v1.005 uses them

What the HLE in `hle/ninja/` replaces, as observed in the game's own code
and in the SDK bodies inside `1ST_READ.BIN` (disassembled, never copied).
Addresses are P1 (`0x8C…`); guest structures are little endian. Companion
notes: `docs/formats/pvr.md` (textures), `docs/seam.md` (the seam itself),
`docs/m2-render-report.md` (what was measured).

## 1. How the game draws

SA1 does not call `njDrawModel` for its levels and characters. Its own
renderer (`0x8C036F00`–`0x8C038E00`, state in GBR-relative globals at
`0x8C8FFE00`, `0x8C036F64` sets GBR) transforms vertices with the XMTRX
(`ftrv`, inline), lights them, projects them into a scratch buffer
(`njInit3D(0x8C7AC8E0, 0x1000)`, 16-byte entries `x, y, 1/z, colour`), and
writes **PowerVR tile-accelerator parameters** 32 bytes at a time through
the SH-4 store queues (`0xE0000000` plus offset, then `pref`) into KAMUI's
vertex buffers in main RAM. KAMUI then DMAs the buffers to the TA at the end
of the frame. The polygon header words come from the Ninja render context,
the buffer write pointer from the same place:

| Address | What |
|---|---|
| `0x8C88FB34` | KAMUI strip head (static in the SDK; `0x8C88FCB4` points to it). Fields below. |
| `0x8C88FBC4`–`0x8C88FBD0` | Its compiled PCW, ISP/TSP, TSP, TCW (strip head + `0x90`..`0x9C`). The 2D writers copy them from here. |
| `0x8C88FCD4` | → vertex buffer descriptor: `+0` opaque write pointer, `+8` translucent, `+16` punch-through (`+4`, `+12` ends, never read). |
| `0x8C890104` | The game-visible context copy: `+0` PCW, `+4` ISP/TSP, `+8` TSP, `+12` TCW, `+16` opaque pointer, `+20` translucent pointer, `+24` punch-through pointer, `+28`/`+32` copies of descriptor `+4`/`+12`. Game code reads the PCW (14 sites) and TSP (12 sites) and edits them. |
| `0x8C890128` / `0x8C89012C` | Top bits of the last pointer and the address of the slot it came from (`njSetVertexBufferPointer` `0x8C6069A4` writes it back). |
| `0x8C88F64C` / `0x8C88F650` | `njInit3D`: the projected-vertex scratch buffer and its entry count. |
| `0x8C88F62C` | `njControl3D` flags (the game uses `0x36C`: constant attr, no clip check, offset material, …). |
| `0x8C88F660` / `664` / `668` | constant attribute AND / OR, constant material (4 floats). |
| `0x8C88F5E4`…`0x8C88F61C` | screen: dist, w, h, cx, cy, (stack ptr/base), **clip rectangle `0x8C88F600`..`0x8C88F60C` = left, top, right, bottom**, clip near/far, aspect x/y. The clip rectangle is what the game's own sphere culling (`0x8C038D00`) tests projected bounds against; it must be initialised (0, 0, 640, 480) or every object is culled. |
| `0x8C673480` / `48C` / `490` | current texlist, current global index, current bank (`njSetTexture`, `njSetTextureNum`). |
| `0x8C760974` | the texture memory list the game gives `njInitTexture` (0x800 entries of 68 bytes, §4). |
| `0x8C8EA3E0` | Ninja light list (32 pointers, count at `+0x80`). |

The writers (`recomp/sa1_us_1005/hle_wraps.tsv`, `hle/ninja/ta_stubs.c`):

| Function | Role |
|---|---|
| `0x8C037294` | project a point list into the scratch buffer (XMTRX, `1/z`, screen scale) |
| `0x8C037460` / `0x8C0374D4` / `0x8C037538` / `0x8C0375B0` | write a 32-byte polygon header from the context copy to the opaque (`+16`) or translucent (`+20`) list and return the advanced pointer. The opaque ones force the TSP to `(tsp & 0x03EFFFFF) | 0x20080000` (src ONE, dst ZERO, ignore texture alpha). |
| `0x8C037990`, `0x8C037C3C`, `0x8C037CCC`, `0x8C038668` | strip writers: vertex type 3 (`PCW, x, y, 1/z, u, v, base colour, offset colour`), `u, v` from 16-bit values × 1/256, end-of-strip `0xF0000000` |
| `0x8C038516`, `0x8C038802` | strip writers with near-plane handling; `0x8C037EE0` the clipper. **These three switch SH-4 register banks** (`stc sr; and ~0x20000000; ldc sr` … restore) to use r0–r7 as scratch; the recompiled bodies need the caller's r0–r7 preserved around them. |
| `0x8C041EAA`…`0x8C042B22` | 2D/HUD writers: take the compiled words from the strip head, the pointer from descriptor `+8` (translucent) |
| `0x8C09E778`/`880`/`B08`/`EE44` | textured quad writers used by sprites (`njSaveContext` → `njSetTexture` → `njSetTextureNum` → attributes → header callback → vertices → `njSetVertexBufferPointer` → `njRestoreContext`) |

Not writers, although the M0 scan put them in the list (they test the
constant `0x10000000` as a flag): `0x8C0522C0`, `0x8C052518`, `0x8C0528AE`
(collision list), `0x8C07E0AE`, `0x8C07E480`, `0x8C07E642`, `0x8C07E9E6`
(texture/UV setup at load), `0x8C080FA2`.

The HLE keeps the three lists in a 2 MB area above the console's 16 MB
(`0x8D000000`, `recomp/emitter/runtime/sh4_rt.h`), parses them at
`kmRender` and draws: opaque in order, punch-through in order with alpha
test, translucent sorted far to near by the strip's mean `1/w`.

## 2. The strip head (what the Ninja setters write; kmCommit `0x8C654E60`)

| Offset | Field | Compiled into |
|---|---|---|
| `+0` | dirty flags | |
| `+4` | type (0/1 polygon, 2 sprite) | PCW 31–29 |
| `+8` | list type 0..4 | PCW 26–24 |
| `+16` | 16-bit UV | PCW 0, ISP 22 |
| `+20` | depth compare 0..7 | ISP 31–29 |
| `+24` | culling 0..3 | ISP 28–27 |
| `+32` | bit 1 texture, bit 0 gouraud | ISP 25 / 23, PCW 3 / 1 |
| `+36` | colour type | PCW 5–4 |
| `+40` | Z write disable | ISP 26 |
| `+44` / `+48` | `njColorBlendingMode` source / destination index 0..11 | TSP 31–29 / 28–26 through the tables below |
| `+52` / `+56` | source / destination select | TSP 25 / 24 |
| `+60` | fog control (0 table = on, 2 none) | TSP 23–22 |
| `+64` | offset colour | PCW 2, ISP 24 |
| `+68` / `+72` | use alpha / ignore texture alpha | TSP 20 / 19 |
| `+76` / `+80` | clamp / flip | TSP 16–15 / 18–17 |
| `+84` | filter index through table `{0, 2, 1, 3}` | TSP 14–13 |
| `+88` | super-sample | TSP 12 |
| `+92` | mipmap D adjust (0 → 4) | TSP 11–8 |
| `+96` | shading instruction | TSP 7–6 |
| `+100` | colour clamp | TSP 21 |
| `+112` | face colour a, r, g, b (intensity modes) | header words 4–7 |
| `+0x90`..`+0x9C` | the compiled PCW, ISP/TSP, TSP, TCW | |

`njColorBlendingMode(target, mode)` indices: 0 both-inv-alpha (src 5, dst 4),
1 both-src-alpha (4, 5), 2 dst alpha 6, 3 dst colour 2 (src only),
4 inv dst alpha 7, 5 inv dst colour 3 (src only), 6 inv src alpha 5,
7 inv src colour 3 (dst only), 8 src alpha 4, 9 src colour 2 (dst only),
10 one, 11 zero.

Setters observed: `0x8C605480` filter (0/1/2 → index 0/2/1), `0x8C6054C6`
super-sample, `0x8C60551E` clamp, `0x8C60556C` flip, `0x8C6055D4` culling,
`0x8C605628` blending, `0x8C63FB18` fog on (`+60 = 0`), `0x8C63FAE8` fog
off (`+60 = 2`), `0x8C606A08` polygon attributes straight into the copy
(`attr & 0x020000FF` → PCW list type bit 25 and object control bits,
`attr & 0x00180000` → TSP alpha bits), `0x8C605DAC` / `0x8C605E0A`
save/restore (strip head `+0x90..` and descriptor ↔ the copy),
`0x8C606A88` / `0x8C608D14` `njSetTextureNum` (TSP size bits and TCW from
the memory list entry, into the copy / the strip head; no bounds check on
the index: the game indexes past a list's own count).

## 3. Draw entries implemented natively (calling conventions)

| Entry | Convention |
|---|---|
| `njDrawSprite2D` `0x8C63EEB4` (sp, n, pri, attr) | `NJS_SPRITE {p[3], sx, sy, ang, texlist, tanim}`, `NJS_TEXANIM {sx, sy, cx, cy, u1, v1, u2, v2, texid, attr}` (16-bit, uv in 1/256). attr: 1 rotate, 2 constant material, 4 flip u, 8 flip v, 0x20 translucent. 2D depth `1/w = 1 / max(1, −pri)`. Vertex type 7 (textured, intensity 1.0). |
| `njDrawSprite3D` `0x8C63EF74` (sp, n, attr) | the point through XMTRX, then as above. |
| `njDrawPolygon` `0x8C63EC58` (vtx, n, trans) | `{x, y, 1/w, colour}` × n, vertex type 0. |
| `njDrawTexture` `0x8C63ED08` (vtx, n, gindex, trans) | `{x, y, 1/w, u, v, colour}` × n; the texture is named by **global index** (`0x8C606B66`). |
| `njDrawPolygon2D` `0x8C63EBC8` (p, n, pri, attr) | `NJS_POINT2COL {p, col, tex, num}`; attr 0x20 polygon outline in ring order (the SDK's writer `0x8C63E834` reorders 0, n-1, 1, n-2, … into a strip), 0x40 translucent (`0x8C63E258` takes attr & 0x40), 0x80000000 textured (`tex` entries are 16-bit u, v pairs in 1/256); without 0x20 the points are a strip as given. `0x8C63E1D4` is the line variant. |
| `njDrawLine3D` `0x8C63CE24` (p, n, attr) | n lines of one `NJS_POINT3COL` (`num` unread): pairs when attr & 0xF is 0, else a strip. Ends through XMTRX, far-rejected, near-clipped; each line one screen quad from -0.525 to +1.05 px across. attr 0x40 translucent, 0x80000000 textured (16-bit u, v in 1/256). The Chaos 0 rain. |
| `njDrawModel` `0x8C611028`, `njDrawObject` `0x8C61103C`, lit variants `0x8C6215D6` (object), `0x8C621574`, `0x8C638DEC`, `0x8C638E4E` | **Basic** models only: SA1 has no chunk models. `NJS_OBJECT {evalflags, model, pos[3], ang[3], scl[3], child, sibling}` (flags 1/2/4 unit pos/ang/scl, 8 hide, 0x10 break, 0x20 ZXY order), `NJS_MODEL {points, normals, nbPoint, meshsets, mats, nbMeshset:16, nbMat:16, center[3], r}`, `NJS_MESHSET {type|matId:16 (type bits 15–14: 0 tri, 1 quad, 2 n-gon, 3 strip), nbMesh:16, meshes, attrs, normals, vertcolor, vertuv}`, `NJS_MATERIAL {diffuse, specular, exponent, attr_texId (texid & 0x3FFF), attrflags}`. attrflags use the TSP bit layout: clamp V 15, clamp U 16, flip V 17, flip U 18, ignore specular 19, use alpha 20, use texture 21, use env 22, double side 23, flat 24, ignore light 25, dst blend 26–28, src blend 29–31. Lighting: first list light, direction in the camera space of slot 0 (`0x8C8E9F20`), `colour × light × clamp(ambient + diffuse × max(0, −n·l))`. |
| lights `0x8C63AA94` create, `0x8C63AC14` delete, `0x8C63AEE8` intensity (spe, dif, amb → `+0x130/+0x12C/+0x128`), `0x8C63AE2C` colour (`+0x14C..`), `0x8C63AE90` direction (`+0x4C..`, normalised, copy `+0x9C`), `0x8C63ADC0` angles, `0x8C63AF48` range | the record layout is in `hle/ninja/light.c` |
| fog `0x8C63FE20` colour, `0x8C63FDC0` default table, `0x8C63FB48` linear table (near, far), `0x8C63FE48` set table (near/far derived from the 128 entries: entry i covers `1/w = (16 + (i & 15)) / 16 · 2^((i >> 4) − 7)`) | the backend gets linear fog between near and far |
| frame `0x8C642440` (render the lists), `0x8C641F00` (present, restart the lists), `0x8C6433E0` (no-draw frame) | called once per rendered frame from `0x8C0517BE` / `0x8C0517DC`; `njWaitVSync` calls the first two too |
| `0x8C610CF8` / `D30` / `D3E` | per-frame draw parameters (stores at `0x8C8901F8`, `0x8C6735A8`) |

## 4. Texture memory list entry (68 bytes; game code reads `+36`/`+40`)

`+0` global index (0xFFFFFFFF free), `+4` bank, `+8` TSP size bits
(U size code bits 5–3, V 2–0, log2 − 3), `+12` TCW (format, non-twiddled
flag, and in the HLE the backend handle in the address bits), `+16` texture
address, `+20` texinfo.texaddr, `+24` surface type, `+28` bit depth, `+32`
pixel format, `+36` width, `+40` height, `+44` bytes, `+48`..`+60` flags and
pointers (0), `+64` reference count (16).

`NJS_TEXLIST {textures, nbTexture}`, `NJS_TEXNAME {filename, attr, texaddr}`:
`attr & 0x40000000` = `filename` points at PVR data in memory,
`0x01000000` = already loaded, else `filename` is a name to load as
`<NAME>.PVR` from the current GD directory. `0x8C608134(data, texlist)`
loads a PVM archive (`docs/formats/pvr.md`) into the list's names (matched
by name, else position) and sets `nbTexture`. `0x8C608DD4` re-uploads
after a display re-init (no-op here). `0x8C607264(list, names, strings, n)`
builds a list over game-owned 12-byte names and 28-byte strings.

## 5. Still stubs after M2 (executed on the route)

Sound (`sd*`, ADX), the debug print family (`0x8C6406AC`, `0x8C64075A`,
`0x8C640752`, `0x8C640922`), `0x8C60D1F2`/`F8` (work pointer, draw
callback), `0x8C6044E0` (word copy), modifier volumes (list types 1 and 3
are skipped by the parser; none appeared on the route).
