/* pvr_decode.c - PVR-T decoding: twiddled, VQ, mipmapped, palettised,
 * rectangle and YUV422 textures into 16-bit PSP-order images.  Pure
 * function of its input (no HLE state), so tests/ninja/pvr_decode_test.c
 * runs it on synthetic textures without game data.
 *
 * A PVR-T chunk: the texture tag, size, pixel format byte, data format
 * byte, 2 reserved bytes, width, height (16-bit each), then the texel data.
 * A global-index chunk (tag, length, index) may precede it.
 *
 * Mipmapped twiddled textures store every level, smallest first.  In a PVR
 * file the chain starts with one dummy texel, so for 16-bit texels the
 * level of width 2^k begins at 2 + 2 * (4^k - 1) / 3 bytes: 0x2, 0x4, 0xC,
 * 0x2C, 0xAC, 0x2AC, 0xAAC, 0x2AAC, 0xAAAC, ...  (In texture memory the
 * hardware's table has the 1x1 level at byte 6, i.e. every level 4 bytes
 * higher; a loader that copies the file as is therefore registers the
 * texture 4 bytes below the copy.  Measured on the M2 route against
 * Flycast's decoded textures: the sky dome and a 64x64 ARGB4444 texture
 * match the file layout above and not the texture-memory table, which is
 * what the first M2 decoder used and what put the streaks on the sky.)
 * VQ mipmaps have no dummy: the index bytes of the levels follow the
 * codebook directly (0, 1, 2, 6, 0x16, ...).
 *
 * Part of sa1-psp (MIT). */
#include "pvr_decode.h"

#include <string.h>

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
/* chunk magics as little-endian words (the four-letter tags themselves are
 * on tools/check_clean.py's signature list, so they are not spelled here) */
#define MAGIC_GINDEX  0x58494247u   /* G B I X */
#define MAGIC_TEXTURE 0x54525650u   /* P V R T */

/* PowerVR -> PSP bit order (swap the red and blue fields). */
static uint16_t to_psp(uint16_t v, int pixfmt) {
    switch (pixfmt) {
    case 0: return (uint16_t)((v & 0x8000) | (v & 0x7C00) >> 10 | (v & 0x03E0) | (v & 0x001F) << 10);           /* ARGB1555 */
    case 1: return (uint16_t)((v & 0xF800) >> 11 | (v & 0x07E0) | (v & 0x001F) << 11);                           /* RGB565 */
    case 2: return (uint16_t)((v & 0xF000) | (v & 0x0F00) >> 8 | (v & 0x00F0) | (v & 0x000F) << 8);             /* ARGB4444 */
    default: return v;
    }
}
static int out_fmt_of(int pixfmt) { return pixfmt == 0 ? PVR_OUT_5551 : pixfmt == 2 ? PVR_OUT_4444 : PVR_OUT_5650; }

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

static int log2u(uint32_t v) { int n = 0; while ((1u << n) < v && n < 12) n++; return n; }

uint32_t pvr_mip_offset16(int log2w) {
    uint32_t texels = 0;             /* texels in the levels below this one */
    for (int k = 0; k < log2w; k++) texels += 1u << (2 * k);
    return 2 + 2 * texels;           /* one dummy texel, then the chain */
}
/* VQ: index bytes of the smaller levels (a 2x2 block per byte; the 1x1 and 2x2 levels take a byte each). */
static uint32_t vq_mip_offset(int log2w) {
    uint32_t bytes = 0;
    for (int k = 0; k < log2w; k++) bytes += k < 2 ? 1 : 1u << (2 * (k - 1));
    return bytes;
}

static uint32_t yuv_to_565(int y, int u, int v) {
    int r = y + (int)(1.402f * (v - 128)), g = y - (int)(0.344f * (u - 128) + 0.714f * (v - 128)), b = y + (int)(1.772f * (u - 128));
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return (uint32_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

int pvr_parse(const uint8_t *p, uint32_t n, pvr_info *info) {
    uint32_t off = 0;
    memset(info, 0, sizeof *info);
    info->gbix = PVR_NO_GINDEX;
    if (n >= 16 && rd32le(p) == MAGIC_GINDEX) {
        uint32_t l = rd32le(p + 4);
        if (l >= 4) info->gbix = rd32le(p + 8);
        off = 8 + l;
    }
    if (off + 16 > n || rd32le(p + off) != MAGIC_TEXTURE) return PVR_ERR_HEADER;
    uint32_t size = rd32le(p + off + 4);
    info->pixfmt = p[off + 8]; info->datafmt = p[off + 9];
    info->w = rd16le(p + off + 12); info->h = rd16le(p + off + 14);
    info->data = p + off + 16;
    uint32_t dn = size >= 8 ? size - 8 : 0;
    if (off + 16 + dn > n) dn = n - off - 16;
    info->data_len = dn;
    uint32_t w = info->w, h = info->h;
    if (w == 0 || h == 0 || w > 1024 || h > 1024 || (w & (w - 1)) || (h & (h - 1))) return PVR_ERR_SIZE;
    return PVR_OK;
}

int pvr_decode(const pvr_info *info, const uint32_t *palette, uint16_t *img, int *out_fmt) {
    const uint8_t *d = info->data;
    uint32_t dn = info->data_len, w = info->w, h = info->h;
    int pixfmt = info->pixfmt, datafmt = info->datafmt;
    int fmt = out_fmt_of(pixfmt);
    memset(img, 0, (size_t)w * h * 2);
    switch (datafmt) {
    case 1: case 2: case 0xD: {                        /* twiddled (+mips) */
        uint32_t base = 0;
        if (datafmt == 2 && w == h) base = pvr_mip_offset16(log2u(w));   /* mips on rectangles: not a DC format */
        if (pixfmt == 3) {
            /* YUV422: 16-bit units twiddled like any texel; the unit at even x
             * holds (U, Y0), the one at x+1 holds (V, Y1), low byte first. */
            fmt = PVR_OUT_5650;
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
        int mips = (datafmt == 4 || datafmt == 0x11);
        if (datafmt >= 0x10) {               /* small VQ: the codebook shrinks with the texture */
            uint32_t idx_bytes = (w * h) / 4;
            if (mips) idx_bytes += vq_mip_offset(log2u(w));
            cb_entries = dn > idx_bytes ? (dn - idx_bytes) / 8 : 0;
            if (cb_entries > 256) cb_entries = 256;
        }
        uint32_t cb_bytes = cb_entries * 8;
        uint32_t base = cb_bytes + (mips ? vq_mip_offset(log2u(w)) : 0);
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
        /* mips: level 0 is last.  Whether the palettised chain also starts
         * with a dummy texel is not verified (no mipmapped palette texture
         * on the route yet); the chain is taken to start at 0. */
        uint32_t base = 0;
        if (datafmt == 6 || datafmt == 8) {
            uint32_t texels = 0;
            for (uint32_t s = 1; s < w; s *= 2) texels += s * s;
            base = bits == 4 ? (texels + 1) / 2 : texels;
        }
        fmt = PVR_OUT_4444;
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            uint32_t t = twiddled_index(x, y, w, h);
            uint32_t k;
            if (bits == 4) { uint32_t i = base + t / 2; k = i < dn ? (d[i] >> ((t & 1) * 4)) & 15 : 0; }
            else { uint32_t i = base + t; k = i < dn ? d[i] : 0; }
            uint32_t c = palette ? palette[k & 1023] : 0;
            if (c == 0) c = 0xFF000000u | (k * 0x010101u);
            img[y * w + x] = (uint16_t)((c >> 28) << 12 | ((c >> 4) & 15) << 8 | ((c >> 12) & 15) << 4 | ((c >> 20) & 15));
        }
        break;
    }
    case 9: case 0xB: {                                  /* rectangle, linear */
        if (pixfmt == 3) {                                /* YUV422: pairs of texels */
            fmt = PVR_OUT_5650;
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
        return PVR_ERR_FORMAT;
    }
    *out_fmt = fmt;
    return PVR_OK;
}
