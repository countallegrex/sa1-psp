/* texture.c - Ninja texture management: texlists, the texture memory
 * list the game gave njInitTexture, PVR/PVM decoding from guest RAM or
 * from loose .PVR files, and the backend textures behind them.
 *
 * Formats (docs/formats/pvr.md): a PVR is an optional GINDEX chunk (global
 * index) then "PVR-T", size, pixel format byte (0 ARGB1555, 1 RGB565,
 * 2 ARGB4444, 3 YUV422, 4 bump, 5 4-bit palette, 6 8-bit palette), data
 * format byte (1 twiddled, 2 twiddled+mips, 3 VQ, 4 VQ+mips, 5/6 pal4
 * twiddled (+mips), 7/8 pal8 twiddled (+mips), 9 rectangle, 0xB stride,
 * 0xD twiddled rectangle, 0x10/0x11 small VQ (+mips)), width, height,
 * texels.  A PVM is archive header, size, flags, count, entries {id, name[28] if
 * flags&8, format if flags&4, dims if flags&2, gbix if flags&1}, then the
 * PVRs back to back, each 32-byte aligned... as laid out by the tools.
 *
 * Every texture is converted once to a 16-bit PSP-order image (the same
 * bit depth as the source, so no quality loss) and handed to
 * draw_texture_create; the handle goes into the TCW's address field.
 *
 * The NJS_TEXMEMLIST entry (68 bytes, what game code reads):
 *   +0 globalIndex  +4 bank  +8 TSP size bits  +12 TCW  +16 texture address
 *   +20 texinfo.texaddr  +24 surface.Type  +28 BitDepth  +32 PixelFormat
 *   +36 nWidth  +40 nHeight  +44 TextureSize  +48 flags  +52 pSurface
 *   +56 pVirtual  +60 pPhysical  +64 count (16) | pad (16)
 *
 * Part of sa1-psp (MIT). */
#include "ninja.h"
#include "../shinobi/shinobi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G_MEMLIST      0x8C890030u   /* our own bookkeeping lives natively; these mirror the SDK globals */
#define G_TEXCACHE     0x8C89003Cu
#define G_TEX_FAILIDX  0x8C88F7A8u
#define G_TEX_ERR      0x8C88F7B0u
#define ENTRY_SIZE 68
#define MAX_ENTRIES 0x800

static uint32_t memlist_base, memlist_count;
typedef struct { uint32_t handle; uint32_t bytes; int used; uint32_t gbix; } mem_slot;
static mem_slot slots[MAX_ENTRIES];
static uint32_t total_bytes;
static int ntex;
static uint32_t palette[1024];        /* KAMUI palette RAM, ARGB8888 */

uint32_t texture_memory_bytes(void) { return total_bytes; }
int texture_count(void) { return ntex; }

/* ---- PVR decoding ------------------------------------------------------------ */
typedef struct { const uint8_t *p; uint32_t n; } span;
static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
/* chunk magics as little-endian words (the four-letter tags themselves are
 * on tools/check_clean.py's signature list, so they are not spelled here) */
#define MAGIC_GINDEX  0x58494247u   /* G B I X */
#define MAGIC_TEXTURE 0x54525650u   /* P V R T */
#define MAGIC_ARCHIVE 0x484D5650u   /* P V M H */
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* PowerVR -> PSP bit order (swap the red and blue fields). */
static uint16_t to_psp(uint16_t v, int pixfmt) {
    switch (pixfmt) {
    case 0: return (uint16_t)((v & 0x8000) | (v & 0x7C00) >> 10 | (v & 0x03E0) | (v & 0x001F) << 10);           /* ARGB1555 */
    case 1: return (uint16_t)((v & 0xF800) >> 11 | (v & 0x07E0) | (v & 0x001F) << 11);                           /* RGB565 */
    case 2: return (uint16_t)((v & 0xF000) | (v & 0x0F00) >> 8 | (v & 0x00F0) | (v & 0x000F) << 8);             /* ARGB4444 */
    default: return v;
    }
}
static int psp_fmt(int pixfmt) { return pixfmt == 0 ? DRAW_FMT_5551 : pixfmt == 1 ? DRAW_FMT_5650 : pixfmt == 2 ? DRAW_FMT_4444 : DRAW_FMT_5650; }

/* Morton order inside a square: index = interleave(y bits, x bits), y in the low bit. */
static uint32_t twiddle(uint32_t x, uint32_t y) {
    uint32_t r = 0;
    for (int b = 0; b < 10; b++) r |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
    return r;
}
/* Rectangles are twiddled in min(w,h) squares along the long side. */
static uint32_t twiddled_index(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t s = w < h ? w : h;
    uint32_t bx = x / s, by = y / s;
    uint32_t block = w >= h ? bx : by;
    return block * s * s + twiddle(x % s, y % s);
}

static const uint32_t MIP_POINT[11] = {0x6, 0x8, 0x10, 0x30, 0xB0, 0x2B0, 0xAB0, 0x2AB0, 0xAAB0, 0x2AAB0, 0xAAAB0};
static const uint32_t VQ_MIP_POINT[11] = {0x0, 0x1, 0x2, 0x6, 0x16, 0x56, 0x156, 0x556, 0x1556, 0x5556, 0x15556};
static int log2u(uint32_t v) { int n = 0; while ((1u << n) < v && n < 12) n++; return n; }

static uint32_t yuv_to_565(int y, int u, int v) {
    int r = y + (int)(1.402f * (v - 128)), g = y - (int)(0.344f * (u - 128) + 0.714f * (v - 128)), b = y + (int)(1.772f * (u - 128));
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return (uint32_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

/* Decodes the PVR-T at p (length n) into a 16-bit PSP-order image.
 * Returns the backend handle, 0 on failure; *w, *h, *fmt describe it. */
static const char *decode_name = "";      /* the texture being decoded, for the debug log and dumps */
static uint32_t decode_pvrt(const uint8_t *p, uint32_t n, int *out_w, int *out_h, int *out_fmt, uint32_t *out_bytes, uint32_t *gbix_out) {
    uint32_t off = 0;
    if (gbix_out) *gbix_out = 0xFFFFFFFFu;
    if (n >= 16 && rd32le(p) == MAGIC_GINDEX) {
        uint32_t l = rd32le(p + 4);
        if (gbix_out && l >= 4) *gbix_out = rd32le(p + 8);
        off = 8 + l;
    }
    if (off + 16 > n || rd32le(p + off) != MAGIC_TEXTURE) return 0;
    uint32_t size = rd32le(p + off + 4);
    int pixfmt = p[off + 8], datafmt = p[off + 9];
    uint32_t w = rd16le(p + off + 12), h = rd16le(p + off + 14);
    const uint8_t *d = p + off + 16;
    uint32_t dn = size >= 8 ? size - 8 : 0;
    if (off + 8 + dn > n) dn = n - off - 16;
    if (w == 0 || h == 0 || w > 1024 || h > 1024 || (w & (w - 1)) || (h & (h - 1))) {
        sa1_log("texture: unsupported size %ux%u (fmt %d/%d)", w, h, pixfmt, datafmt);
        return 0;
    }
    uint16_t *img = calloc((size_t)w * h, 2);
    if (!img) return 0;
    int fmt = psp_fmt(pixfmt);
    static int tex_log = -1;
    if (tex_log < 0) tex_log = getenv("SA1_TEX_LOG") != NULL;       /* debug aid: every decode */
    if (tex_log) sa1_log("texture: decode %s %ux%u pixel %d data 0x%x size %u avail %u", decode_name, w, h, pixfmt, datafmt, size, n);
    int ok = 1;
    switch (datafmt) {
    case 1: case 2: case 0xD: {                        /* twiddled (+mips) */
        uint32_t base = 0;
        if (datafmt == 2) base = MIP_POINT[log2u(w)];
        if (datafmt == 2 && w != h) base = 0;             /* mips on rectangles: not a DC format */
        if (pixfmt == 3) {
            /* YUV422: 16-bit units twiddled like any texel; the unit at even x
             * holds (U, Y0), the one at x+1 holds (V, Y1), low byte first. */
            fmt = DRAW_FMT_5650;
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x + 1 < w; x += 2) {
                uint32_t i0 = base + twiddled_index(x, y, w, h) * 2, i1 = base + twiddled_index(x + 1, y, w, h) * 2;
                uint16_t w0 = i0 + 1 < dn ? rd16le(d + i0) : 0, w1 = i1 + 1 < dn ? rd16le(d + i1) : 0;
                int u = w0 & 255, y0 = w0 >> 8, v = w1 & 255, y1 = w1 >> 8;
                img[y * w + x] = to_psp((uint16_t)yuv_to_565(y0, u, v), 1);
                img[y * w + x + 1] = to_psp((uint16_t)yuv_to_565(y1, u, v), 1);
            }
            break;
        }
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            uint32_t i = base + twiddled_index(x, y, w, h) * 2;
            uint16_t v = i + 1 < dn ? rd16le(d + i) : 0;
            img[y * w + x] = to_psp(v, pixfmt);
        }
        break;
    }
    case 3: case 4: case 0x10: case 0x11: {            /* VQ: 256-entry codebook of 2x2 texels, twiddled indices */
        uint32_t cb_entries = 256;
        if (datafmt >= 0x10) {               /* small VQ: the codebook shrinks with the texture */
            uint32_t idx_bytes = (w * h) / 4;
            if (datafmt == 0x11) idx_bytes += VQ_MIP_POINT[log2u(w)];
            cb_entries = dn > idx_bytes ? (dn - idx_bytes) / 8 : 0;
            if (cb_entries > 256) cb_entries = 256;
        }
        uint32_t cb_bytes = cb_entries * 8;
        uint32_t base = cb_bytes + ((datafmt == 4 || datafmt == 0x11) ? VQ_MIP_POINT[log2u(w)] : 0);
        uint32_t hw = w / 2, hh = h / 2;
        for (uint32_t y = 0; y < hh; y++) for (uint32_t x = 0; x < hw; x++) {
            uint32_t i = base + twiddled_index(x, y, hw, hh);
            uint32_t code = i < dn ? d[i] : 0;
            if (code >= cb_entries) code = 0;
            const uint8_t *e = d + code * 8;
            /* texel order inside the entry: (0,0), (0,1), (1,0), (1,1) */
            img[(2 * y) * w + 2 * x] = to_psp(rd16le(e), pixfmt);
            img[(2 * y + 1) * w + 2 * x] = to_psp(rd16le(e + 2), pixfmt);
            img[(2 * y) * w + 2 * x + 1] = to_psp(rd16le(e + 4), pixfmt);
            img[(2 * y + 1) * w + 2 * x + 1] = to_psp(rd16le(e + 6), pixfmt);
        }
        break;
    }
    case 5: case 6: case 7: case 8: {                  /* palettised, twiddled: expand with the palette RAM */
        int bits = datafmt <= 6 ? 4 : 8;
        uint32_t base = 0;        /* mips ignored: level 0 is last; approximate with the start */
        if (datafmt == 6 || datafmt == 8) {
            uint32_t texels = 0;
            for (uint32_t s = 1; s < w; s *= 2) texels += s * s;
            base = bits == 4 ? (texels + 1) / 2 : texels;
        }
        fmt = DRAW_FMT_4444;
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            uint32_t t = twiddled_index(x, y, w, h);
            uint32_t k;
            if (bits == 4) { uint32_t i = base + t / 2; k = i < dn ? (d[i] >> ((t & 1) * 4)) & 15 : 0; }
            else { uint32_t i = base + t; k = i < dn ? d[i] : 0; }
            uint32_t c = palette[k & 1023];
            if (c == 0) c = 0xFF000000u | (k * 0x010101u);
            img[y * w + x] = (uint16_t)((c >> 28) << 12 | ((c >> 4) & 15) << 8 | ((c >> 12) & 15) << 4 | ((c >> 20) & 15));
        }
        break;
    }
    case 9: case 0xB: {                                  /* rectangle, linear */
        if (pixfmt == 3) {                                /* YUV422: pairs of texels */
            fmt = DRAW_FMT_5650;
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x + 1 < w; x += 2) {
                uint32_t i = (y * w + x) * 2;
                if (i + 3 >= dn) break;
                int u = d[i], y0 = d[i + 1], v = d[i + 2], y1 = d[i + 3];
                uint32_t a = yuv_to_565(y0, u, v), b = yuv_to_565(y1, u, v);
                img[y * w + x] = to_psp((uint16_t)a, 1); img[y * w + x + 1] = to_psp((uint16_t)b, 1);
            }
        } else {
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
                uint32_t i = (y * w + x) * 2;
                img[y * w + x] = to_psp(i + 1 < dn ? rd16le(d + i) : 0, pixfmt);
            }
        }
        break;
    }
    default:
        sa1_log("texture: data format 0x%x not supported (pixel format %d, %ux%u)", datafmt, pixfmt, w, h);
        ok = 0;
    }
    uint32_t handle = 0;
    if (ok) handle = draw_texture_create((int)w, (int)h, fmt, img, 0, NULL);
    const char *dump_dir = getenv("SA1_TEX_DUMP_DIR");      /* debug aid: every decoded texture as PPM, 16-bit expanded */
    if (ok && dump_dir && w >= (uint32_t)(getenv("SA1_TEX_DUMP_MIN") ? atoi(getenv("SA1_TEX_DUMP_MIN")) : 128)) {
        char path[512]; FILE *o;
        static unsigned dump_seq;
        snprintf(path, sizeof path, "%s/tex_%03u_%s_%ux%u_p%d_d%x.ppm", dump_dir, dump_seq++, decode_name, w, h, pixfmt, datafmt);
        if ((o = fopen(path, "wb")) != NULL) {
            fprintf(o, "P6\n%u %u\n255\n", w, h);
            for (uint32_t i = 0; i < w * h; i++) {
                uint16_t v = img[i]; uint8_t rgb[3];
                if (fmt == DRAW_FMT_5650) { rgb[0] = (v & 31) << 3; rgb[1] = ((v >> 5) & 63) << 2; rgb[2] = ((v >> 11) & 31) << 3; }
                else if (fmt == DRAW_FMT_5551) { rgb[0] = (v & 31) << 3; rgb[1] = ((v >> 5) & 31) << 3; rgb[2] = ((v >> 10) & 31) << 3; }
                else { rgb[0] = (v & 15) << 4; rgb[1] = ((v >> 4) & 15) << 4; rgb[2] = ((v >> 8) & 15) << 4; }
                fwrite(rgb, 1, 3, o);
            }
            fclose(o);
        }
    }
    free(img);
    if (!handle) return 0;
    *out_w = (int)w; *out_h = (int)h; *out_fmt = pixfmt; *out_bytes = w * h * 2;
    return handle;
}

/* ---- the texture memory list ----------------------------------------------------- */
static uint32_t entry_addr(uint32_t i) { return memlist_base + i * ENTRY_SIZE; }

static int find_free(void) {
    for (uint32_t i = 0; i < memlist_count; i++) if (!slots[i].used) return (int)i;
    return -1;
}
static int find_gbix(uint32_t gbix) {
    if (gbix == 0xFFFFFFFFu) return -1;
    for (uint32_t i = 0; i < memlist_count; i++) if (slots[i].used && slots[i].gbix == gbix) return (int)i;
    return -1;
}

/* TSP size codes: log2(size) - 3 in 3 bits each, U high. */
static uint32_t size_code(int w, int h) {
    uint32_t cu = (uint32_t)(log2u((uint32_t)w) - 3), cv = (uint32_t)(log2u((uint32_t)h) - 3);
    return (cu & 7) << 3 | (cv & 7);
}

/* Loads the PVR-T at host memory p into a memlist entry; returns the entry index or -1. */
static int load_entry(const uint8_t *p, uint32_t n, uint32_t gbix_hint, const char *what) {
    int w, h, pixfmt; uint32_t bytes, gbix = 0xFFFFFFFFu;
    decode_name = what;
    uint32_t handle = decode_pvrt(p, n, &w, &h, &pixfmt, &bytes, &gbix);
    if (gbix == 0xFFFFFFFFu) gbix = gbix_hint;
    if (!handle) { sa1_log("texture: %s: decode failed", what); return -1; }
    int shared = find_gbix(gbix);
    if (shared >= 0) {              /* already in memory: share it, like the SDK's global index cache */
        draw_texture_destroy(handle);
        uint32_t e = entry_addr((uint32_t)shared);
        hle_wr32(e + 64, hle_rd32(e + 64) + 1);
        return shared;
    }
    int i = find_free();
    if (i < 0) { draw_texture_destroy(handle); sa1_log("texture: memory list full (%u entries)", memlist_count); return -1; }
    slots[i].used = 1; slots[i].handle = handle; slots[i].bytes = bytes; slots[i].gbix = gbix;
    total_bytes += bytes; ntex++;
    uint32_t e = entry_addr((uint32_t)i);
    hle_wr32(e + 0, gbix); hle_wr32(e + 4, 0);
    hle_wr32(e + 8, size_code(w, h));
    hle_wr32(e + 12, (uint32_t)pixfmt << 27 | 1u << 26 | handle);   /* TCW: format, non-twiddled flag, our handle as the address */
    hle_wr32(e + 16, handle * 8u);
    hle_wr32(e + 20, 0);
    hle_wr32(e + 24, 1); hle_wr32(e + 28, 16); hle_wr32(e + 32, (uint32_t)pixfmt);
    hle_wr32(e + 36, (uint32_t)w); hle_wr32(e + 40, (uint32_t)h); hle_wr32(e + 44, bytes);
    hle_wr32(e + 48, 0); hle_wr32(e + 52, 0); hle_wr32(e + 56, 0); hle_wr32(e + 60, 0);
    hle_wr32(e + 64, 1);
    return i;
}

static void release_entry(uint32_t e) {
    if (!memlist_base || e < memlist_base || e >= memlist_base + memlist_count * ENTRY_SIZE) return;
    uint32_t i = (e - memlist_base) / ENTRY_SIZE;
    if (!slots[i].used) return;
    uint32_t cnt = hle_rd32(e + 64) & 0xFFFF;
    if (cnt > 1) { hle_wr32(e + 64, cnt - 1); return; }
    draw_texture_destroy(slots[i].handle);
    total_bytes -= slots[i].bytes; ntex--;
    memset(&slots[i], 0, sizeof slots[i]);
    hle_wr32(e + 0, 0xFFFFFFFFu); hle_wr32(e + 12, 0); hle_wr32(e + 64, 0);
}

/* Selects the texture with this global index into the context copy (the
 * njDrawTexture family names textures by global index, 0x8C606B66). */
int texture_select_gindex(uint32_t gbix) {
    int i = find_gbix(gbix);
    if (i < 0) return -1;
    uint32_t e = entry_addr((uint32_t)i);
    hle_wr32(NJ_CUR_GINDEX, gbix); hle_wr32(NJ_CUR_BANK, hle_rd32(e + 4));
    hle_wr32(KM_CTX_TSP, (hle_rd32(KM_CTX_TSP) & ~63u) | (hle_rd32(e + 8) & 63));
    hle_wr32(KM_CTX_TCW, hle_rd32(e + 12));
    return 1;
}

uint32_t texture_from_tcw(uint32_t tcw) {
    uint32_t h = tcw & 0x1FFFFFu;
    for (uint32_t i = 0; i < memlist_count; i++) if (slots[i].used && slots[i].handle == h) return h;
    return 0;
}

void texture_init(void) { memset(slots, 0, sizeof slots); total_bytes = 0; ntex = 0; }

/* ---- texlist helpers --------------------------------------------------------------- */
#define ATTR_GLOBALINDEX 0x80000000u
#define ATTR_MEMORY      0x40000000u
#define ATTR_FRAMEBUFFER 0x20000000u
#define ATTR_LOADED      0x01000000u

static void texname_string(uint32_t texname, char *out, unsigned n) {
    uint32_t fn = hle_rd32(texname);
    if (!fn || !hle_in_ram(fn)) { out[0] = 0; return; }
    unsigned i = 0;
    for (; i + 1 < n && i < 28; i++) { char ch = (char)hle_rd8(fn + i); if (!ch) break; out[i] = ch; }
    out[i] = 0;
}

/* Loads one NJS_TEXNAME; returns the entry index or -1. */
static int load_texname(uint32_t texname) {
    uint32_t attr = hle_rd32(texname + 4), fn = hle_rd32(texname);
    if (attr & ATTR_FRAMEBUFFER) return -1;
    if (attr & ATTR_MEMORY) {
        if (!fn || !hle_in_ram(fn)) return -1;
        uint32_t avail = SA1_RAM_SIZE + SA1_RAM_EXTRA - (fn & SA1_RAM_MASK);
        return load_entry(hle_ptr(fn), avail, (attr & ATTR_GLOBALINDEX) ? (attr & 0x00FFFFFFu) : 0xFFFFFFFFu, "memory texture");
    }
    char name[40], rel[160], path[200];
    texname_string(texname, name, sizeof name);
    if (!name[0]) return -1;
    snprintf(rel, sizeof rel, "%s.PVR", name);
    shinobi_gdfs_path(path, sizeof path, rel);
    uint32_t size = 0;
    if (sa1_plat_file_size(path, &size) != 0 || size == 0) { sa1_log("texture: %s not found", path); return -1; }
    uint8_t *buf = malloc(size);
    if (!buf) return -1;
    int got = sa1_plat_file_read(path, 0, buf, size);
    int r = got > 0 ? load_entry(buf, (uint32_t)got, 0xFFFFFFFFu, name) : -1;
    free(buf);
    return r;
}

/* ---- entries ------------------------------------------------------------------------ */
void sdk_8c608c2a(sh4_ctx *c) {         /* njInitTexture(memlist, n) */
    HLE_ENTER(0x8c608c2au, "njInitTexture");
    memlist_base = ARG0; memlist_count = ARG1 < MAX_ENTRIES ? ARG1 : MAX_ENTRIES;
    for (uint32_t i = 0; i < memlist_count; i++) { hle_wr32(entry_addr(i), 0xFFFFFFFFu); hle_wr32(entry_addr(i) + 64, 0); }
    hle_wr32(G_MEMLIST, ARG0); hle_wr32(G_TEXCACHE, 0);
    texture_init();
    sa1_log("njInitTexture: %u entries at 0x%08x", memlist_count, memlist_base);
}
/* 0x8C608DD4 (memlist, n): after a display re-init, re-upload every live
 * entry to texture memory (KAMUI 0x8C64DF66 per entry).  Our textures
 * never leave the backend, so there is nothing to do. */
void sdk_8c608dd4(sh4_ctx *c) { HLE_ENTER(0x8c608dd4u, "njReloadTextures"); (void)c; }

void sdk_8c608c82(sh4_ctx *c) {         /* njLoadTexture(texlist) */
    HLE_ENTER(0x8c608c82u, "njLoadTexture");
    uint32_t tl = ARG0;
    if (!tl) { RET(-1); return; }
    uint32_t names = hle_rd32(tl), count = hle_rd32(tl + 4);
    int fail = 0, loaded = 0;
    for (uint32_t i = 0; i < count && i < 2048; i++) {
        uint32_t tn = names + i * 12;
        uint32_t attr = hle_rd32(tn + 4);
        if ((attr & ATTR_LOADED) && hle_rd32(tn + 8)) continue;
        int e = load_texname(tn);
        if (e < 0) { if (!fail) hle_wr32(G_TEX_FAILIDX, i); fail = 1; hle_wr32(tn + 8, 0); continue; }
        hle_wr32(tn + 8, entry_addr((uint32_t)e));
        loaded++;
    }
    sa1_log("njLoadTexture(0x%08x): %d of %u textures%s", tl, loaded, count, fail ? " (some failed)" : "");
    RET(fail ? -1 : 1);
}

/* 0x8C608134: load a PVM archive in memory into a texlist.  The texlist's
 * name array is matched by name when the archive has names and the list
 * has them, else by position; nbTexture is set to the archive's count. */
void sdk_8c608134(sh4_ctx *c) {
    HLE_ENTER(0x8c608134u, "njLoadTexturePvmMemory");
    uint32_t data = ARG0, tl = ARG1;
    if (!data || !tl || !hle_in_ram(data)) { hle_wr32(G_TEX_ERR, 4); RET(-1); return; }
    const uint8_t *p = hle_ptr(data);
    uint32_t avail = SA1_RAM_SIZE + SA1_RAM_EXTRA - (data & SA1_RAM_MASK);
    if (avail < 16 || rd32le(p) != MAGIC_ARCHIVE) { hle_wr32(G_TEX_ERR, 4); RET(-1); return; }
    uint32_t hsize = rd32le(p + 4);
    uint32_t flags = rd16le(p + 8), count = rd16le(p + 10);
    uint32_t q = 12;
    struct { uint32_t id; char name[29]; uint32_t gbix; } *ents = calloc(count ? count : 1, sizeof *ents);
    if (!ents) { RET(-1); return; }
    for (uint32_t i = 0; i < count; i++) {
        ents[i].id = rd16le(p + q); q += 2;
        ents[i].gbix = 0xFFFFFFFFu;
        if (flags & 8) { memcpy(ents[i].name, p + q, 28); ents[i].name[28] = 0; q += 28; }
        if (flags & 4) q += 2;
        if (flags & 2) q += 2;
        if (flags & 1) { ents[i].gbix = rd32le(p + q); q += 4; }
    }
    uint32_t names = hle_rd32(tl), listn = hle_rd32(tl + 4);
    if (!hle_in_ram(names)) names = 0;
    if (listn > 2048) listn = 0;
    hle_wr32(tl + 4, count);
    /* the PVR-T chunks follow the header, each prefixed by its own GINDEX */
    uint32_t off = 8 + hsize;
    int fail = 0;
    for (uint32_t i = 0; i < count; i++) {
        /* find the next chunk start */
        while (off + 8 <= avail && rd32le(p + off) != MAGIC_GINDEX && rd32le(p + off) != MAGIC_TEXTURE) off += 4;
        if (off + 16 > avail) { fail = 1; break; }
        uint32_t start = off;
        if (rd32le(p + off) == MAGIC_GINDEX) off += 8 + rd32le(p + off + 4);
        if (off + 8 > avail || rd32le(p + off) != MAGIC_TEXTURE) { fail = 1; break; }
        uint32_t chunk_end = off + 8 + rd32le(p + off + 4);
        if (chunk_end > avail) chunk_end = avail;
        /* which texlist slot gets it: by name when both sides have names, else position */
        uint32_t slot = i;
        if ((flags & 8) && names && listn) {
            for (uint32_t j = 0; j < listn; j++) {
                char nm[40]; texname_string(names + j * 12, nm, sizeof nm);
                if (nm[0] && strncmp(nm, ents[i].name, 28) == 0) { slot = j; break; }
            }
        }
        int e = load_entry(p + start, chunk_end - start, ents[i].gbix, ents[i].name[0] ? ents[i].name : "pvm entry");
        if (names && slot < 2048) {
            uint32_t tn = names + slot * 12;
            hle_wr32(tn + 8, e >= 0 ? entry_addr((uint32_t)e) : 0);
            if (e >= 0 && ents[i].gbix != 0xFFFFFFFFu) hle_wr32(tn + 4, (hle_rd32(tn + 4) & 0xFF000000u) | ATTR_GLOBALINDEX | (ents[i].gbix & 0x00FFFFFFu));
        }
        if (e < 0) fail = 1;
        off = chunk_end;
    }
    sa1_log("njLoadTexturePvm(0x%08x -> 0x%08x names 0x%08x): %u textures (flags 0x%x, first gbix %u)%s, %d live, %u bytes", data, tl, names, count, flags, count ? ents[0].gbix : 0, fail ? " (some failed)" : "", ntex, total_bytes);
    free(ents);
    RET(fail ? -1 : 1);
}

void sdk_8c6088bc(sh4_ctx *c) {         /* njReleaseTexture(texlist) */
    HLE_ENTER(0x8c6088bcu, "njReleaseTexture");
    uint32_t tl = ARG0;
    if (!tl) { RET(-1); return; }
    uint32_t names = hle_rd32(tl), count = hle_rd32(tl + 4);
    for (uint32_t i = 0; i < count && i < 2048; i++) {
        uint32_t tn = names + i * 12, e = hle_rd32(tn + 8);
        if (e) release_entry(e);
        hle_wr32(tn + 8, 0);
    }
    RET(1);
}
void sdk_8c608c5c(sh4_ctx *c) {         /* njReleaseTextureAll */
    HLE_ENTER(0x8c608c5cu, "njReleaseTextureAll");
    for (uint32_t i = 0; i < memlist_count; i++) if (slots[i].used) { hle_wr32(entry_addr(i) + 64, 1); release_entry(entry_addr(i)); }
    hle_wr32(G_TEXCACHE, 0);
    (void)c;
}
void sdk_8c6087f4(sh4_ctx *c) { HLE_ENTER(0x8c6087f4u, "njTextureGarbage"); (void)c; }

/* pure-memory helpers */
void sdk_8c607264(sh4_ctx *c) {         /* build a texlist: list->textures = names; names[i].filename = base + 28 i */
    HLE_ENTER(0x8c607264u, "njSetTextureList");
    for (uint32_t i = 0; i < ARG3 && i < 2048; i++) { hle_wr32(ARG1 + i * 12, ARG2 + 28 * i); }
    hle_wr32(ARG0, ARG1);
}
void sdk_8c608914(sh4_ctx *c) {         /* njSetTextureInfo(info, tex, type, w, h) */
    HLE_ENTER(0x8c608914u, "njSetTextureInfo");
    hle_wr32(ARG0, ARG1); hle_wr32(ARG0 + 4, ARG2 << 16); hle_wr32(ARG0 + 16, ARG3); hle_wr32(ARG0 + 20, STACK_ARG(0));
}
void sdk_8c608922(sh4_ctx *c) {         /* njSetTextureName(name, addr, attr) */
    HLE_ENTER(0x8c608922u, "njSetTextureName");
    hle_wr32(ARG0, ARG1); hle_wr32(ARG0 + 4, ARG2); hle_wr32(ARG0 + 8, 0);
}
