# PVR textures and PVM archives (as SA1 ships them)

Written from the files on the US v1.005 disc and the way the game's
loaders hand them to Ninja; nothing here comes from an SDK document.
Implementation: `hle/ninja/texture.c`.

## PVR (one texture)

```
[ global-index tag (4 ASCII letters, G-B-I-X)  u32 length  u32 global index  (+ padding) ]   optional
  texture tag (4 ASCII letters, P-V-R-T)  u32 size
  u8 pixel format   u8 data format   u16 0   u16 width   u16 height
  texels
```

(The two tags are spelled with dashes here and kept as integer constants in
the code because `tools/check_clean.py` refuses files that carry them as
bytes: they are the signature of game data.)

Pixel formats: 0 ARGB1555, 1 RGB565, 2 ARGB4444, 3 YUV422, 4 bump,
5 4-bit palette, 6 8-bit palette. The HLE converts each to the PSP order
of the same depth (red in the low bits), so no quality is lost:
ARGB1555 → ABGR1555, RGB565 → BGR565, ARGB4444 → ABGR4444; YUV422 → BGR565;
palettes are expanded through the palette RAM (grey ramp when unset).

Data formats seen in the game's files:

| Byte | Layout |
|---|---|
| 1 | twiddled (Morton order, y in the low bit; rectangles in min(w,h) squares along the long side) |
| 2 | twiddled with mipmaps; level 0 at byte offset `{0x6, 0x8, 0x10, 0x30, 0xB0, 0x2B0, 0xAB0, 0x2AB0, 0xAAB0, 0x2AAB0, 0xAAAB0}[log2 w]` |
| 3 | VQ: 256-entry codebook of 2×2 texels (8 bytes each, texel order (0,0) (0,1) (1,0) (1,1)), then one index byte per 2×2 block, twiddled over the half-size grid |
| 4 | VQ with mipmaps; index data for level 0 at `{0, 1, 2, 6, 0x16, 0x56, 0x156, 0x556, 0x1556, 0x5556, 0x15556}[log2 w]` after the codebook |
| 5–8 | 4-bit / 8-bit palette, twiddled (+ mipmaps) |
| 9, 0xB | rectangle, linear (0xB with a stride) |
| 0xD | twiddled rectangle |
| 0x10, 0x11 | small VQ (+ mipmaps): the codebook holds whatever precedes the index data |

Sizes are powers of two from 8 to 1024; the PSP keeps the same limits.

## PVM (an archive of PVRs)

```
archive tag (4 ASCII letters, P-V-M-H)   u32 header size   u16 flags   u16 count
count × { u16 id  [char name[28] if flags & 8]  [u16 format if flags & 4]
          [u16 dimensions if flags & 2]  [u32 global index if flags & 1] }
then the PVRs, each with its own global-index chunk, 32-byte aligned
```

The game's files use flags `0x109` (names, dimensions, global indices).
The loader (`0x8C608134`) sets the texlist's count to the archive's, loads
each PVR into a free memory-list entry (entries with a global index already
in memory are shared and reference-counted) and writes the entry address
into the matching `NJS_TEXNAME.texaddr`.

## Budget

Every texture costs `width × height × 2` bytes once converted (palettes
are expanded on the desktop; the PSP backend will keep them as CLUTs).
Station Square after its loads: 972 textures, 5.99 MB; Emerald Coast:
`docs/m2-render-report.md`.
