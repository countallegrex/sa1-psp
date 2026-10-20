/* draw_gu.c - backend/draw.h on the PSP's GE through sceGu.
 *
 * The same contract as backend/desktop/draw_gl.c, which was written to be
 * this file's shape (README section 4, rule 1):
 *
 *  - Frame: one display list per frame (GU_DIRECT, so the GE starts on it
 *    while the CPU is still parsing the TA lists), two 480x272 RGB565 frame
 *    buffers and a 16-bit Z buffer in EDRAM (0.80 MB of the 2 MB), the list
 *    in main RAM through its uncached alias.  draw_end_frame waits for the
 *    GE and swaps at the next vblank; main_psp.c paces guest frames to the
 *    vblank count.
 *  - Vertices: the game's screen-space x, y and w = 1/z become the GE vertex
 *    (x*z, y*z, -z) under a projection whose clip w is z again, so the GE's
 *    own divide gives perspective-correct texturing, and its clip z is
 *    -z - 2: depth 1 - 2/z as on the desktop.  The depth range is inverted
 *    (65535 near, 0 far) so a larger depth is nearer, the PowerVR's sense of
 *    1/w, and the TA's compare codes map one to one.  View-space z is -z,
 *    which is what sceGuFog measures, so the fog distances are the game's.
 *  - Textures: in main RAM, in the formats draw.h already uses (the PSP's
 *    bit order), CLUT4/CLUT8 kept paletted, swizzled, with mip levels built
 *    at load (box filter; index formats point-sampled), at most 512x512
 *    (larger ones lose their top level).  The GE has no mirrored repeat: a
 *    flip_u/flip_v strip gets a texture of twice the size holding the
 *    mirror, built the first time it is asked for, at half the UV scale.
 *  - State: the PowerVR TSP/ISP codes map to sceGuBlendFunc (the table in
 *    blend_factor), sceGuDepthFunc/DepthMask, the cull bit, the alpha test
 *    (punch-through, alpha >= 0.5), sceGuTexFunc for the four shading
 *    instructions (draw.h: DECAL = REPLACE, DECAL_ALPHA = DECAL,
 *    MODULATE(_ALPHA) = MODULATE, GU_TCC_RGB for ignore-texture-alpha), the
 *    fog enable, flat or Gouraud, and sceGuDepthOffset for translucent strips
 *    (the desktop's glPolygonOffset).  The offset colour is a second,
 *    additive, untextured draw at equal depth, as on the desktop.
 *  - Strips: consecutive strips with the same state and projection are
 *    merged into one GE strip through degenerate triangles (the game writes
 *    two-to-four-triangle strips, ~4,000 per frame in Station Square).
 *
 * Headless mode keeps every call a no-op except the statistics.
 *
 * Part of sa1-psp (MIT). */
#include "../draw.h"
#include "hle.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspctrl.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DC_W 640.0f
#define DC_H 480.0f
#define SCR_W 480
#define SCR_H 272
#define BUF_W 512
#define FB_BYTES (BUF_W * SCR_H * 2)          /* 0x44000 */
#define LIST_BYTES (640u << 10)               /* commands only: the vertices are in vpool */
#define LIST_SLACK (64u << 10)                /* stop adding strips this close to the end */

static uint32_t list_mem[LIST_BYTES / 4] __attribute__((aligned(64)));
static uint32_t *list;                        /* the uncached alias of list_mem */
static int headless = 1, list_open, aspect_wide = 1;
static draw_stats stats;
static draw_state cur;
static int cur_valid;
static uint32_t draw_fb;                      /* EDRAM offset of the buffer being drawn */
static int swap_pending;
static unsigned swap_vcount;
static const char *shot_path;
static int cull_flip;                         /* --cull-flip (main_psp.c): the other winding convention */

/* Counters for the report beyond draw_stats. */
static struct { uint32_t list_high, pool_high, pool_full, dropped, offscreen, mirrors, mirror_fail, downscaled, frames;
                uint32_t flush_state, flush_proj, flush_offset, last_strips, last_draws, last_states;
                float zmin_frame, zmin_win, zmin_last; uint32_t znear_verts[4];   /* nearest 3D z; vertices with z < 2, 4, 8, 16 */
                uint64_t gpu_wait_us, swap_wait_us, replay_us, frame_us_sum, frame_us_max, last_end; } gs;

typedef struct { float u, v; uint32_t color; float x, y, z; } gu_vtx;
#define VTX_FMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

void draw_gu_set_cull_flip(int on) { cull_flip = on; }
/* --gu-debug N (main_psp.c): bisection switches for console-only drawing
 * differences.  1 = mip level 0 only (GU_TEXTURE_CONST), 2 = no mip levels
 * built, 4 = no swizzle, 8 = no depth test on the 2D layer. */
#define TEX_LEVELS_DEFAULT 8
static int gu_debug;
void draw_gu_set_debug(int flags) { gu_debug = flags; }
static float lod_bias;
void draw_gu_set_lod_bias(float b) { lod_bias = b; }
static int max_levels = TEX_LEVELS_DEFAULT;
void draw_gu_set_max_mip(int n) { max_levels = n < 0 ? 1 : n + 1 > 8 ? 8 : n + 1; }

/* ---- projection --------------------------------------------------------------- */
enum { PROJ_3D = 0, PROJ_2D, PROJ_FULL, PROJ_N };
/* ---- depth: two passes ------------------------------------------------------
 * The PowerVR compares float 1/w per pixel; the GE has one 16-bit buffer.
 * Linear in w over z 1..infinity, a step is z^2/65534 units: 0.075 at
 * z = 70, where Sonic stands in Station Square, and his face and eye-white
 * strips, a few hundredths apart, then lost to each other by rounding on
 * the console (the maintainer's mono-eye).  No single range fixes that: a nearer
 * start clamps the floor in front of the camera (z ~5).  So the frame is
 * drawn in two passes split at z = depth_split: the far pass (z >= split,
 * depth 1 - 2 split/z, 16 bits over w 0..1/split, 16x finer at split 16)
 * and, after a depth clear, the near pass (z 1..split, the 16 bits over
 * w 1/split..1).  The far pass is entirely behind the near one, so the
 * clear loses nothing.  Draws are recorded during the frame and replayed by
 * draw_end_frame, pass by pass, in their order (opaque, punch-through,
 * sorted translucent); a strip that spans the split is cut there (w is
 * linear in screen space, as for the guard band).  --gu-split 0: one pass,
 * the old 1 - 2/z. */
enum { PASS_FAR = 0, PASS_NEAR = 1, PASSES };
static ScePspFMatrix4 proj[PASSES][PROJ_N];
static float depth_split = 16.0f;
static const float depth_zn = 1.0f;          /* nearer vertices (the 2D layer's) draw at z = 1, the front */
static void build_projs(void);
void draw_gu_set_split(float z) { depth_split = z > 1.0f ? z : 0.0f; build_projs(); }
static int cur_proj = -1;
/* x in 640-space from left to right -> NDC [lo, hi]; y 0..480 -> 1..-1; the
 * vertex is (x*z, y*z, -z) and clip w = z.  Depth: clip z = a z + b gives
 * NDC z = a + b/z, from -1 at z = z0 to +1 at z = z1 (z1 = 0: infinity).
 * Columns are x, y, z, w (GL order). */
static void build_proj(ScePspFMatrix4 *m, float left, float right, float lo, float hi, float z0, float z1) {
    float sx = (hi - lo) / (right - left), ox = lo - left * sx;
    float a, b;
    if (z1 <= 0.0f) { a = 1.0f; b = -2.0f * z0; }
    else { b = -2.0f * z0 * z1 / (z1 - z0); a = -1.0f - b / z0; }
    memset(m, 0, sizeof *m);
    m->x.x = sx;  m->z.x = -ox;
    m->y.y = -2.0f / DC_H; m->z.y = -1.0f;
    m->z.z = -a; m->w.z = b;
    m->z.w = -1.0f;
}
static void build_projs(void) {
    for (int pass = 0; pass < PASSES; pass++) {
        /* the far pass starts a hair before the split and the near pass ends
         * a hair after it, so the cut's own vertices stay inside both ranges */
        float z0 = depth_split ? (pass == PASS_FAR ? depth_split * 0.999f : depth_zn) : depth_zn;
        float z1 = depth_split && pass == PASS_NEAR ? depth_split * 1.001f : 0.0f;
        if (aspect_wide) build_proj(&proj[pass][PROJ_3D], DRAW_WIDE_LEFT, DRAW_WIDE_RIGHT, -1.0f, 1.0f, z0, z1);
        else build_proj(&proj[pass][PROJ_3D], 0.0f, DC_W, -0.75f, 0.75f, z0, z1);
        build_proj(&proj[pass][PROJ_2D], 0.0f, DC_W, -0.75f, 0.75f, z0, z1);
        build_proj(&proj[pass][PROJ_FULL], 0.0f, DC_W, -1.0f, 1.0f, z0, z1);
    }
    cur_proj = -1;
}
void draw_set_aspect(int wide) {
    aspect_wide = wide ? 1 : 0;
    build_projs();
    sa1_log("draw: aspect %s", aspect_wide ? "wide (hor+)" : "4:3");
}

const draw_stats *draw_get_stats(void) { return &stats; }

/* ---- textures ----------------------------------------------------------------- */
#define TEX_MAX 2048
#define TEX_LEVELS 8
typedef struct {
    int w, h, fmt, clut_fmt, levels;
    uint8_t *mem;                 /* every level, swizzled, one block */
    uint32_t off[TEX_LEVELS];     /* level offsets in mem */
    int tbw[TEX_LEVELS];          /* buffer width in pixels */
    uint16_t *clut;
    int own_clut;
    uint32_t bytes;
    uint32_t mirror[4];           /* variants by flip_u | flip_v << 1; 0 = not built */
    int hidden;                   /* a mirror variant (not a handle the HLE holds) */
    int dying;                    /* destroyed during a frame: freed after the GE is done */
} gtex;
static gtex texs[TEX_MAX];
static uint32_t bound_tex;        /* slot whose image is in the GE, 0 = none */
static uint32_t free_later[TEX_MAX];
static unsigned nfree_later;

static int fmt_bits(int fmt) {
    switch (fmt) {
    case DRAW_FMT_5650: case DRAW_FMT_5551: case DRAW_FMT_4444: return 16;
    case DRAW_FMT_8888: return 32;
    case DRAW_FMT_T4: return 4;
    case DRAW_FMT_T8: return 8;
    default: return 0;
    }
}
static int fmt_psm(int fmt) {
    switch (fmt) {
    case DRAW_FMT_5650: return GU_PSM_5650;
    case DRAW_FMT_5551: return GU_PSM_5551;
    case DRAW_FMT_4444: return GU_PSM_4444;
    case DRAW_FMT_8888: return GU_PSM_8888;
    case DRAW_FMT_T4: return GU_PSM_T4;
    default: return GU_PSM_T8;
    }
}
static uint32_t row_bytes(int w, int bits) { return (uint32_t)w * (uint32_t)bits / 8u; }

/* Pixel access on a linear buffer (index formats and 32-bit included). */
static uint32_t px_get(const uint8_t *b, uint32_t rb, int bits, int x, int y) {
    const uint8_t *r = b + (size_t)y * rb;
    switch (bits) {
    case 4: return (r[x >> 1] >> ((x & 1) * 4)) & 15u;
    case 8: return r[x];
    case 16: return ((const uint16_t *)r)[x];
    default: return ((const uint32_t *)r)[x];
    }
}
static void px_set(uint8_t *b, uint32_t rb, int bits, int x, int y, uint32_t v) {
    uint8_t *r = b + (size_t)y * rb;
    switch (bits) {
    case 4: r[x >> 1] = (uint8_t)((r[x >> 1] & ~(15u << ((x & 1) * 4))) | ((v & 15u) << ((x & 1) * 4))); break;
    case 8: r[x] = (uint8_t)v; break;
    case 16: ((uint16_t *)r)[x] = (uint16_t)v; break;
    default: ((uint32_t *)r)[x] = v; break;
    }
}

/* Direct formats to and from 8-bit RGBA, for the box filter. */
static void unpack(int fmt, uint32_t p, unsigned c[4]) {
    switch (fmt) {
    case DRAW_FMT_5650: c[0] = (p & 31) * 255 / 31; c[1] = ((p >> 5) & 63) * 255 / 63; c[2] = ((p >> 11) & 31) * 255 / 31; c[3] = 255; break;
    case DRAW_FMT_5551: c[0] = (p & 31) * 255 / 31; c[1] = ((p >> 5) & 31) * 255 / 31; c[2] = ((p >> 10) & 31) * 255 / 31; c[3] = (p >> 15) ? 255 : 0; break;
    case DRAW_FMT_4444: c[0] = (p & 15) * 17; c[1] = ((p >> 4) & 15) * 17; c[2] = ((p >> 8) & 15) * 17; c[3] = ((p >> 12) & 15) * 17; break;
    default: c[0] = p & 255; c[1] = (p >> 8) & 255; c[2] = (p >> 16) & 255; c[3] = p >> 24; break;
    }
}
static uint32_t pack(int fmt, const unsigned c[4]) {
    switch (fmt) {
    case DRAW_FMT_5650: return (c[0] * 31 / 255) | (c[1] * 63 / 255) << 5 | (c[2] * 31 / 255) << 11;
    case DRAW_FMT_5551: return (c[0] * 31 / 255) | (c[1] * 31 / 255) << 5 | (c[2] * 31 / 255) << 10 | (c[3] >= 128 ? 0x8000u : 0);
    case DRAW_FMT_4444: return (c[0] / 17) | (c[1] / 17) << 4 | (c[2] / 17) << 8 | (c[3] / 17) << 12;
    default: return c[0] | c[1] << 8 | c[2] << 16 | (uint32_t)c[3] << 24;
    }
}

/* The 2x2 box of the 16-bit formats on whole texels: the channels are
 * split into two masks with a free bit gap above each field, four texels
 * summed in 32 bits, rounded and shifted back.  unpack/pack per texel cost
 * some twenty integer divides (~36 cycles each on the Allegrex): the
 * Station Square load spent most of its texture time here.  5551's alpha
 * is the sum's top bit: set when two or more of the four are, as the
 * 8-bit average (>= 128) had it. */
static void halve16(uint16_t *dst, const uint16_t *src, int w, int nw, int nh, int fmt) {
    uint32_t m0, m1, r0, r1;
    switch (fmt) {
    case DRAW_FMT_5650: m0 = 0xF81Fu; m1 = 0x07E0u; r0 = 0x1002u; r1 = 0x0040u; break;
    case DRAW_FMT_5551: m0 = 0x7C1Fu; m1 = 0x03E0u; r0 = 0x0802u; r1 = 0x0040u; break;
    default:            m0 = 0x0F0Fu; m1 = 0xF0F0u; r0 = 0x0202u; r1 = 0x2020u; break;   /* 4444 */
    }
    for (int y = 0; y < nh; y++) {
        const uint16_t *a = src + (size_t)(2 * y) * (size_t)w, *b = a + w;
        uint16_t *d = dst + (size_t)y * (size_t)nw;
        for (int x = 0; x < nw; x++, a += 2, b += 2) {
            uint32_t p0 = a[0], p1 = a[1], p2 = b[0], p3 = b[1];
            uint32_t s0 = (p0 & m0) + (p1 & m0) + (p2 & m0) + (p3 & m0);
            uint32_t s1 = (p0 & m1) + (p1 & m1) + (p2 & m1) + (p3 & m1);
            uint32_t v = (((s0 + r0) >> 2) & m0) | (((s1 + r1) >> 2) & m1);
            if (fmt == DRAW_FMT_5551 && ((p0 >> 15) + (p1 >> 15) + (p2 >> 15) + (p3 >> 15)) >= 2) v |= 0x8000u;
            d[x] = (uint16_t)v;
        }
    }
}

/* Half size: a 2x2 box for direct colour, the top-left texel for indices. */
static uint8_t *halve(const uint8_t *src, int w, int h, int fmt) {
    int bits = fmt_bits(fmt), nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    uint32_t srb = row_bytes(w, bits), drb = row_bytes(nw, bits);
    uint8_t *dst = calloc(1, (size_t)drb * nh + 4);
    if (!dst) return NULL;
    if (bits == 16 && w > 1 && h > 1 && !(gu_debug & 16)) {
        halve16((uint16_t *)dst, (const uint16_t *)src, w, nw, nh, fmt);
        return dst;
    }
    int sx = w > 1 ? 2 : 1, sy = h > 1 ? 2 : 1;
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            if (bits <= 8) { px_set(dst, drb, bits, x, y, px_get(src, srb, bits, x * sx, y * sy)); continue; }
            unsigned acc[4] = {0, 0, 0, 0}, c[4], amin = 255;
            for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
                unpack(fmt, px_get(src, srb, bits, x * sx + (sx > 1 ? i : 0), y * sy + (sy > 1 ? j : 0)), c);
                for (int k = 0; k < 4; k++) acc[k] += c[k];
                if (c[3] < amin) amin = c[3];
            }
            for (int k = 0; k < 4; k++) acc[k] = (acc[k] + 2) / 4;
            if (gu_debug & 16) acc[3] = amin;
            px_set(dst, drb, bits, x, y, pack(fmt, acc));
        }
    return dst;
}

/* The GE's swizzle: 16-byte x 8-row blocks, row-major. */
static void swizzle(uint8_t *dst, const uint8_t *src, uint32_t rb, uint32_t rows) {
    uint32_t blocks_x = rb / 16;
    for (uint32_t by = 0; by < rows / 8; by++)
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            uint32_t *d = (uint32_t *)(dst + (by * blocks_x + bx) * 128);
            for (uint32_t y = 0; y < 8; y++) {
                const uint32_t *s = (const uint32_t *)(src + (by * 8 + y) * rb + bx * 16);
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                d += 4;
            }
        }
}
static void unswizzle(uint8_t *dst, const uint8_t *src, uint32_t rb, uint32_t rows) {
    uint32_t blocks_x = rb / 16;
    for (uint32_t by = 0; by < rows / 8; by++)
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            const uint32_t *s = (const uint32_t *)(src + (by * blocks_x + bx) * 128);
            for (uint32_t y = 0; y < 8; y++) {
                uint32_t *d = (uint32_t *)(dst + (by * 8 + y) * rb + bx * 16);
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                s += 4;
            }
        }
}

/* Builds slot `slot` from linear level-0 pixels (rows of w*bits/8 bytes).
 * The CLUT pointer is taken over when own_clut. */
static int texture_build(int slot, int w, int h, int fmt, const uint8_t *pixels, int clut_fmt, uint16_t *clut, int own_clut) {
    int bits = fmt_bits(fmt);
    gtex *t = &texs[slot];
    memset(t, 0, sizeof *t);
    uint8_t *lin[TEX_LEVELS] = {0};
    int lw = w, lh = h, levels = 1;
    lin[0] = (uint8_t *)pixels;
    while (!(gu_debug & 2) && levels < max_levels && (w >> levels) >= 8 && (h >> levels) >= 8) levels++;
    for (int l = 1; l < levels; l++) {
        lin[l] = halve(lin[l - 1], w >> (l - 1), h >> (l - 1), fmt);
        if (!lin[l]) { levels = l; break; }
    }
    /* Each level: rows padded to 16 bytes (the buffer width), height to 8
     * rows, so every level can be swizzled. */
    uint32_t total = 0, rb[TEX_LEVELS], rows[TEX_LEVELS];
    for (int l = 0; l < levels; l++) {
        lw = w >> l; lh = h >> l;
        rb[l] = row_bytes(lw, bits);
        if (rb[l] < 16) rb[l] = 16;
        rb[l] = (rb[l] + 15u) & ~15u;
        rows[l] = ((uint32_t)lh + 7u) & ~7u;
        t->off[l] = total;
        t->tbw[l] = (int)(rb[l] * 8u / (uint32_t)bits);
        total += rb[l] * rows[l];
    }
    t->mem = memalign(16, total);
    uint8_t *pad = t->mem ? malloc(rb[0] * rows[0]) : NULL;
    if (!t->mem || !pad) {
        free(t->mem); free(pad);
        for (int l = 1; l < levels; l++) free(lin[l]);
        memset(t, 0, sizeof *t);
        if (own_clut) free(clut);
        sa1_log("draw: no memory for a %dx%d texture (%u bytes)", w, h, total);
        return -1;
    }
    for (int l = 0; l < levels; l++) {
        lw = w >> l; lh = h >> l;
        uint32_t src_rb = row_bytes(lw, bits);
        if (src_rb == 0) src_rb = 1;
        memset(pad, 0, rb[l] * rows[l]);
        for (int y = 0; y < lh; y++) memcpy(pad + (size_t)y * rb[l], lin[l] + (size_t)y * src_rb, src_rb);
        if (gu_debug & 4) memcpy(t->mem + t->off[l], pad, rb[l] * rows[l]);
        else swizzle(t->mem + t->off[l], pad, rb[l], rows[l]);
        if (l) free(lin[l]);
    }
    free(pad);
    sceKernelDcacheWritebackRange(t->mem, total);
    t->w = w; t->h = h; t->fmt = fmt; t->clut_fmt = clut_fmt; t->levels = levels;
    t->clut = clut; t->own_clut = own_clut;
    if (clut) sceKernelDcacheWritebackRange(clut, fmt == DRAW_FMT_T4 ? 32 : 512);
    t->bytes = total + (own_clut ? (fmt == DRAW_FMT_T4 ? 32u : 512u) : 0u);
    return 0;
}

static int free_slot(void) {
    for (int s = 1; s < TEX_MAX; s++) if (!texs[s].w && !texs[s].mem) return s;
    return 0;
}

uint32_t draw_texture_create(int width, int height, int fmt, const void *pixels, int clut_fmt, const void *clut) {
    int bits = fmt_bits(fmt);
    if (!bits) return 0;
    if ((width & (width - 1)) || (height & (height - 1)) || width < 1 || height < 1) {
        sa1_log("draw: texture %dx%d is not a power of two", width, height);
        return 0;
    }
    int slot = free_slot();
    if (!slot) { sa1_log("draw: all %d texture slots in use", TEX_MAX); return 0; }
    uint16_t *cl = NULL;
    if (fmt == DRAW_FMT_T4 || fmt == DRAW_FMT_T8) {
        unsigned n = fmt == DRAW_FMT_T4 ? 16 : 256;
        cl = memalign(16, n * 2);
        if (!cl) return 0;
        if (clut) memcpy(cl, clut, n * 2); else memset(cl, 0, n * 2);
    }
    /* The GE samples at most 512 x 512: drop top levels until it fits. */
    const uint8_t *src = pixels;
    uint8_t *tmp = NULL;
    while (width > 512 || height > 512) {
        uint8_t *h2 = halve(src, width, height, fmt);
        free(tmp);
        if (!h2) { free(cl); return 0; }
        tmp = h2; src = h2;
        if (width > 1) width /= 2;
        if (height > 1) height /= 2;
        gs.downscaled++;
    }
    int r = texture_build(slot, width, height, fmt, src, clut_fmt, cl, cl != NULL);
    free(tmp);
    if (r != 0) return 0;
    stats.textures_alive++; stats.texture_bytes += texs[slot].bytes;
    return (uint32_t)slot;
}

static void texture_release(uint32_t h) {
    gtex *t = &texs[h];
    if (!t->hidden) stats.textures_alive--;
    stats.texture_bytes -= t->bytes;
    for (int i = 1; i < 4; i++) if (t->mirror[i] && t->mirror[i] != h) texture_release(t->mirror[i]);
    if (t->own_clut) free(t->clut);
    free(t->mem);
    if (bound_tex == h) bound_tex = 0;
    memset(t, 0, sizeof *t);
}

void draw_texture_destroy(uint32_t handle) {
    if (handle == 0 || handle >= TEX_MAX || texs[handle].w == 0 || texs[handle].hidden || texs[handle].dying) return;
    if (list_open && nfree_later < TEX_MAX) { free_later[nfree_later++] = handle; texs[handle].dying = 1; return; }   /* the GE may still read it this frame */
    texture_release(handle);
    cur_valid = 0;
}

/* Save states (savestate_psp.c): every live texture as level-0 pixels in
 * the linear layout draw_texture_create takes, and its CLUT; a load drops
 * the textures and builds the saved ones under their handles, which guest
 * RAM refers to (mip levels and mirrors are derived again). */
#include "state.h"
typedef struct tex_rec { uint32_t handle, pix_n, clut_n; int32_t w, h, fmt, clut_fmt; } tex_rec;
static uint32_t src_row_bytes(int w, int bits) { uint32_t r = row_bytes(w, bits); return r ? r : 1; }

void draw_gu_texture_state(hle_state_io *io) {
    uint32_t count = 0;
    if (!hle_state_loading(io)) for (int i = 1; i < TEX_MAX; i++) count += texs[i].w && !texs[i].hidden && !texs[i].dying;
    hle_state_blob(io, "textures", &count, sizeof count);
    if (hle_state_loading(io)) {
        if (hle_state_error(io)) return;
        for (int i = 1; i < TEX_MAX; i++) if (texs[i].mem && !texs[i].hidden) texture_release((uint32_t)i);
        for (int i = 1; i < TEX_MAX; i++) if (texs[i].mem) texture_release((uint32_t)i);
        nfree_later = 0; bound_tex = 0; cur_valid = 0;
        for (uint32_t k = 0; k < count; k++) {
            tex_rec r;
            hle_state_blob(io, "tex", &r, sizeof r);
            uint32_t pn = 0, cn = 0;
            const void *pix = hle_state_var(io, "tex.pix", NULL, &pn);
            const void *clut = hle_state_var(io, "tex.clut", NULL, &cn);
            if (hle_state_error(io)) return;
            uint16_t *cl = NULL;
            if (cn) { cl = memalign(16, cn < 32 ? 32 : cn); if (cl) memcpy(cl, clut, cn); }
            if (!pix || r.handle == 0 || r.handle >= TEX_MAX || texture_build((int)r.handle, r.w, r.h, r.fmt, pix, r.clut_fmt, cl, cl != NULL) != 0) {
                sa1_log("draw: cannot rebuild texture %u (%dx%d fmt %d) from the state", r.handle, r.w, r.h, r.fmt);
                continue;
            }
            stats.textures_alive++; stats.texture_bytes += texs[r.handle].bytes;
        }
        return;
    }
    for (int i = 1; i < TEX_MAX; i++) {
        gtex *t = &texs[i];
        if (!t->w || t->hidden || t->dying) continue;
        int bits = fmt_bits(t->fmt);
        uint32_t rb = row_bytes(t->w, bits) < 16 ? 16 : ((row_bytes(t->w, bits) + 15u) & ~15u), rows = ((uint32_t)t->h + 7u) & ~7u;
        uint32_t srb = src_row_bytes(t->w, bits);
        uint8_t *lin = malloc(rb * rows), *packed = malloc((size_t)srb * t->h);
        if (!lin || !packed) { free(lin); free(packed); sa1_log("draw: no memory to save texture %d", i); continue; }
        if (gu_debug & 4) memcpy(lin, t->mem + t->off[0], rb * rows); else unswizzle(lin, t->mem + t->off[0], rb, rows);
        for (int y = 0; y < t->h; y++) memcpy(packed + (size_t)y * srb, lin + (size_t)y * rb, srb);
        free(lin);
        tex_rec r = { (uint32_t)i, srb * (uint32_t)t->h, t->clut ? (t->fmt == DRAW_FMT_T4 ? 32u : 512u) : 0u, t->w, t->h, t->fmt, t->clut_fmt };
        hle_state_blob(io, "tex", &r, sizeof r);
        uint32_t pn = r.pix_n, cn = r.clut_n;
        hle_state_var(io, "tex.pix", packed, &pn);
        hle_state_var(io, "tex.clut", t->clut, &cn);
        free(packed);
    }
}

/* The mirrored variant of slot h (fu, fv as in the TSP flip bits): a
 * texture twice as wide and/or tall holding the image and its mirror. */
static uint32_t mirror_variant(uint32_t h, int fu, int fv) {
    gtex *t = &texs[h];
    int k = fu | fv << 1;
    if (t->mirror[k]) return t->mirror[k];
    int nw = fu ? t->w * 2 : t->w, nh = fv ? t->h * 2 : t->h, bits = fmt_bits(t->fmt);
    if (nw > 512 || nh > 512) {
        if (!gs.mirror_fail++) sa1_log("draw: a %dx%d texture is flipped; its mirror would pass 512, drawn as a plain repeat", t->w, t->h);
        return t->mirror[k] = h;
    }
    uint32_t rb = row_bytes(t->w, bits) < 16 ? 16 : ((row_bytes(t->w, bits) + 15u) & ~15u), rows = ((uint32_t)t->h + 7u) & ~7u;
    uint8_t *lin = malloc(rb * rows), *out = calloc(1, (size_t)row_bytes(nw, bits) * nh + 4);
    int slot = free_slot();
    if (!lin || !out || !slot) { free(lin); free(out); return t->mirror[k] = h; }
    unswizzle(lin, t->mem + t->off[0], rb, rows);
    uint32_t orb = row_bytes(nw, bits);
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            int sx = x < t->w ? x : 2 * t->w - 1 - x, sy = y < t->h ? y : 2 * t->h - 1 - y;
            px_set(out, orb, bits, x, y, px_get(lin, rb, bits, sx, sy));
        }
    free(lin);
    int ok = texture_build(slot, nw, nh, t->fmt, out, t->clut_fmt, t->clut, 0) == 0;
    free(out);
    if (!ok) return t->mirror[k] = h;
    texs[slot].hidden = 1;
    stats.texture_bytes += texs[slot].bytes;
    gs.mirrors++;
    return t->mirror[k] = (uint32_t)slot;
}

/* ---- frame -------------------------------------------------------------------- */
static uint32_t fog_color = 0xFF808080u;
static float fog_near = 1.0f, fog_far = 1000.0f;
static uint32_t abgr(uint32_t argb) { return (argb & 0xFF00FF00u) | ((argb >> 16) & 255u) | ((argb & 255u) << 16); }

int draw_init(int scale, int want_headless) {
    (void)scale;
    gs.zmin_frame = gs.zmin_win = 1e30f; gs.zmin_last = 1.0f;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    build_projs();
    headless = 1;
    if (want_headless) return 0;
    sceKernelDcacheWritebackInvalidateRange(list_mem, sizeof list_mem);   /* nothing cached may land on it later */
    list = (uint32_t *)((uintptr_t)list_mem | 0x40000000u);
    sa1_log("draw: sceGuInit (list at %p)", (void *)list);
    sceGuInit();
    /* pspsdk swaps with PSP_DISPLAY_SETBUF_IMMEDIATE unless told otherwise:
     * the display then changes buffer mid-scan, and at the console's
     * 4-14 fps every swap showed one refresh torn between two frames (a
     * turning Sonic doubled at character select, the maintainer's run of ca5c25f).
     * At the next vblank instead; the frame after waits for that vblank
     * before it touches the buffer (frame_clear). */
    guSwapBuffersBehaviour(PSP_DISPLAY_SETBUF_NEXTFRAME);
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_5650, (void *)0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)FB_BYTES, BUF_W);
    sceGuDepthBuffer((void *)(2 * FB_BYTES), BUF_W);
    draw_fb = 0;
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuShadeModel(GU_SMOOTH);
    sceGuTexLevelMode((gu_debug & 1) ? GU_TEXTURE_CONST : GU_TEXTURE_AUTO, (gu_debug & 1) ? 0.0f : lod_bias);
    sa1_log("draw: --gu-debug %d, depth split z %.1f (0: one pass)", gu_debug, depth_split);
    static const ScePspIMatrix4 dither = { {-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2} };
    sceGuSetDither(&dither);
    sceGuEnable(GU_DITHER);
    ScePspFMatrix4 ident;
    memset(&ident, 0, sizeof ident);
    ident.x.x = ident.y.y = ident.z.z = ident.w.w = 1.0f;
    sceGuSetMatrix(GU_VIEW, &ident);
    sceGuSetMatrix(GU_MODEL, &ident);
    sceGuClearColor(0); sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sa1_log("draw: first list built");
    sceGuFinish();
    sceGuSync(0, 0);
    sa1_log("draw: first list done");
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    headless = 0;
    sa1_log("draw: sceGu, 480x272 RGB565 x2 + 16-bit Z in EDRAM (%u KB), display list %u KB", 3u * FB_BYTES / 1024u, LIST_BYTES / 1024u);
    return 0;
}

void draw_shutdown(void) {
    if (headless) return;
    sceGuSync(0, 0);                    /* let the GE finish the last list */
    sceGuDisplay(GU_FALSE);
    sceGuTerm();
    headless = 1;
}

static void apply_fog(void) { sceGuFog(fog_near, fog_far, abgr(fog_color)); }

void draw_set_fog(uint32_t color_argb, float z_near, float z_far) {
    fog_color = color_argb; fog_near = z_near; fog_far = z_far > z_near ? z_far : z_near + 1.0f;
    if (!headless && list_open) apply_fog();
}

static float tex_su = -1.0f, tex_sv = -1.0f;
static uint32_t frame_clear_argb;
static void rec_reset(void);
static void replay(void);

void draw_begin_frame(uint32_t clear_argb) {
    stats.draw_calls = stats.triangles = stats.strips = stats.state_changes = 0;
    cur_valid = 0; cur_proj = -1; bound_tex = 0;
    rec_reset();
    if (headless) return;
    tex_su = tex_sv = -1.0f;        /* re-sent with the first texture */
    frame_clear_argb = clear_argb;
    sceGuStart(GU_DIRECT, list);   /* the GE runs the list as it grows: nothing touches the buffer before frame_clear */
    list_open = 1;
    apply_fog();
}

/* The first thing replay() draws.  The buffer is the one on screen until
 * the vblank after the last swap (NEXTFRAME); a frame of the game takes
 * longer than a refresh, so the wait is almost never taken. */
static void frame_clear(void) {
    if (swap_pending && sceDisplayGetVcount() == swap_vcount) {
        uint64_t t = sa1_plat_time_us();
        sceDisplayWaitVblankStart();
        gs.swap_wait_us += sa1_plat_time_us() - t;
    }
    swap_pending = 0;
    sceGuDepthOffset(0);
    sceGuDepthMask(GU_FALSE);
    if (!aspect_wide) {             /* 4:3: black pillars, the game's colour and all drawing in the middle */
        sceGuScissor(0, 0, SCR_W, SCR_H);
        sceGuClearColor(0xFF000000u); sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        sceGuScissor(SCR_W / 8, 0, SCR_W * 7 / 8, SCR_H);
        sceGuClearColor(abgr(frame_clear_argb) | 0xFF000000u);
        sceGuClear(GU_COLOR_BUFFER_BIT);
    } else {
        sceGuScissor(0, 0, SCR_W, SCR_H);
        sceGuClearColor(abgr(frame_clear_argb) | 0xFF000000u); sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    }
}

static void write_shot(void) {
    const uint16_t *fb = (const uint16_t *)((uintptr_t)sceGeEdramGetAddr() + draw_fb + 0x40000000u);
    FILE *f = fopen(shot_path, "wb");
    if (!f) { sa1_log("screenshot: cannot write %s", shot_path); return; }
    fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H);
    static uint8_t row[SCR_W * 3];
    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            uint16_t p = fb[y * BUF_W + x];
            row[x * 3] = (uint8_t)((p & 31) * 255 / 31); row[x * 3 + 1] = (uint8_t)(((p >> 5) & 63) * 255 / 63); row[x * 3 + 2] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
        }
        fwrite(row, 1, sizeof row, f);
    }
    fclose(f);
    sa1_log("screenshot: %s", shot_path);
}

void draw_end_frame(void) {
    if (headless || !list_open) return;
    uint64_t t_replay = sa1_plat_time_us();
    sceKernelDcacheWritebackAll();    /* the arena's vertices into RAM before the GE reads them */
    replay();
    gs.replay_us += sa1_plat_time_us() - t_replay;
    uint32_t used = (uint32_t)sceGuCheckList();
    if (used > gs.list_high) gs.list_high = used;
    sceGuFinish();
    uint64_t t = sa1_plat_time_us();
    sceGuSync(0, 0);
    uint64_t now = sa1_plat_time_us();
    gs.gpu_wait_us += now - t;
    if (gs.last_end) {
        uint64_t ft = now - gs.last_end;
        gs.frame_us_sum += ft;
        if (ft > gs.frame_us_max) gs.frame_us_max = ft;
        gs.frames++;
    }
    gs.last_end = now;
    gs.last_strips = stats.strips; gs.last_draws = stats.draw_calls; gs.last_states = stats.state_changes;
    if (gs.zmin_frame < gs.zmin_win) gs.zmin_win = gs.zmin_frame;
    gs.zmin_last = gs.zmin_frame < 1e29f ? gs.zmin_frame : 1.0f;   /* no 3D this frame: the full range */
    gs.zmin_frame = 1e30f;
    list_open = 0;
    for (unsigned i = 0; i < nfree_later; i++) texture_release(free_later[i]);
    nfree_later = 0;
    if (shot_path) { write_shot(); shot_path = NULL; }
    sceGuSwapBuffers();
    draw_fb ^= FB_BYTES;
    swap_pending = 1;
    swap_vcount = sceDisplayGetVcount();
}

/* Frame-time and renderer counters since the last call (main_psp.c --stats). */
void draw_gu_report(void) {
    sa1_log("draw: %u frames, frame time avg %.2f ms max %.2f ms, list replay %.2f ms/frame, GE wait %.2f ms/frame, swap wait %.2f ms/frame; list high water %u KB, vertices %u KB (%u strips over); "
            "%u strips dropped (list full), %u clipped to the GE's guard band",
            gs.frames, gs.frames ? gs.frame_us_sum / 1000.0 / gs.frames : 0.0, gs.frame_us_max / 1000.0,
            gs.frames ? gs.replay_us / 1000.0 / gs.frames : 0.0,
            gs.frames ? gs.gpu_wait_us / 1000.0 / gs.frames : 0.0, gs.frames ? gs.swap_wait_us / 1000.0 / gs.frames : 0.0,
            gs.list_high / 1024u, gs.pool_high / 1024u, gs.pool_full, gs.dropped, gs.offscreen);
    sa1_log("draw: last frame %u strips -> %u GE draws, %u state changes; batch breaks by state %u, projection %u; "
            "%u mirrors (%u too big), %u textures downscaled", gs.last_strips, gs.last_draws, gs.last_states,
            gs.flush_state, gs.flush_proj, gs.mirrors, gs.mirror_fail, gs.downscaled);
    sa1_log("draw: depth split z %.1f; nearest 3D z %.2f; 3D vertices nearer than z 2 / 4 / 8 / 16: %u / %u / %u / %u",
            depth_split, gs.zmin_win < 1e29f ? gs.zmin_win : 0.0f, gs.znear_verts[0], gs.znear_verts[1], gs.znear_verts[2], gs.znear_verts[3]);
    gs.zmin_win = 1e30f; memset(gs.znear_verts, 0, sizeof gs.znear_verts);
    gs.flush_state = gs.flush_proj = gs.flush_offset = 0;
    gs.frames = 0; gs.frame_us_sum = gs.frame_us_max = gs.gpu_wait_us = gs.swap_wait_us = gs.replay_us = 0; gs.dropped = gs.offscreen = 0;
}

/* ---- state ---------------------------------------------------------------------- */
static void blend_factor(int code, int is_dst, int *f, unsigned *fix) {
    *fix = 0;
    switch (code & 7) {
    case DRAW_BLEND_ZERO: *f = GU_FIX; *fix = 0; break;
    case DRAW_BLEND_ONE: *f = GU_FIX; *fix = 0xFFFFFFu; break;
    case DRAW_BLEND_OTHER_COLOR: *f = is_dst ? GU_SRC_COLOR : GU_DST_COLOR; break;
    case DRAW_BLEND_INV_OTHER_COLOR: *f = is_dst ? GU_ONE_MINUS_SRC_COLOR : GU_ONE_MINUS_DST_COLOR; break;
    case DRAW_BLEND_SRC_ALPHA: *f = GU_SRC_ALPHA; break;
    case DRAW_BLEND_INV_SRC_ALPHA: *f = GU_ONE_MINUS_SRC_ALPHA; break;
    /* The frame buffer is RGB565 (no destination alpha); the desktop's GL
     * window has none either, where it reads as 1. */
    case DRAW_BLEND_DST_ALPHA: *f = GU_FIX; *fix = 0xFFFFFFu; break;
    default: *f = GU_FIX; *fix = 0; break;
    }
}
/* Both compare "larger = nearer" (the PowerVR on 1/w, the GE on the
 * inverted depth range), so the codes map one to one. */
static int depth_func(int code) {
    static const int map[8] = { GU_NEVER, GU_LESS, GU_EQUAL, GU_LEQUAL, GU_GREATER, GU_NOTEQUAL, GU_GEQUAL, GU_ALWAYS };
    return map[code & 7];
}

static void bind_texture(const draw_state *s) {
    uint32_t h = s->texture;
    int fu = s->flip_u && !s->clamp_u, fv = s->flip_v && !s->clamp_v;
    if (fu || fv) h = mirror_variant(h, fu, fv);
    float su = (fu && h != s->texture) ? 0.5f : 1.0f, sv = (fv && h != s->texture) ? 0.5f : 1.0f;
    if (h != bound_tex) {
        gtex *t = &texs[h];
        if (t->clut) {
            sceGuClutMode(fmt_psm(t->clut_fmt), 0, 0xFF, 0);
            sceGuClutLoad(t->fmt == DRAW_FMT_T4 ? 1 : 16, t->clut);
        }
        sceGuTexMode(fmt_psm(t->fmt), t->levels - 1, 0, (gu_debug & 4) ? 0 : 1);
        for (int l = 0; l < t->levels; l++) sceGuTexImage(l, t->w >> l, t->h >> l, t->tbw[l], t->mem + t->off[l]);
        bound_tex = h;
    }
    if (su != tex_su || sv != tex_sv) { sceGuTexScale(su, sv); tex_su = su; tex_sv = sv; }
    sceGuTexWrap(s->clamp_u ? GU_CLAMP : GU_REPEAT, s->clamp_v ? GU_CLAMP : GU_REPEAT);
    if (s->filter == DRAW_FILTER_POINT) sceGuTexFilter(GU_NEAREST_MIPMAP_NEAREST, GU_NEAREST);
    else sceGuTexFilter(GU_LINEAR_MIPMAP_LINEAR, GU_LINEAR);
    int tcc = s->ignore_tex_alpha ? GU_TCC_RGB : GU_TCC_RGBA;
    switch (s->shade) {
    case DRAW_SHADE_DECAL: sceGuTexFunc(GU_TFX_REPLACE, tcc); break;          /* rgb T, alpha A (1: the vertex alpha is forced to 1) */
    case DRAW_SHADE_DECAL_ALPHA:                                                /* T*A + V*(1-A), alpha a */
        if (s->ignore_tex_alpha) sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
        else sceGuTexFunc(GU_TFX_DECAL, GU_TCC_RGBA);
        break;
    default: sceGuTexFunc(GU_TFX_MODULATE, tcc); break;                         /* T*V, A*a */
    }
}

/* Blend, depth compare and write, alpha test and the texture enable: what
 * the offset-colour pass changes and puts back. */
static void apply_blend_depth(const draw_state *s) {
    if (s->blend_src == DRAW_BLEND_ONE && s->blend_dst == DRAW_BLEND_ZERO) sceGuDisable(GU_BLEND);
    else {
        int sf, df; unsigned sfix, dfix;
        blend_factor(s->blend_src, 0, &sf, &sfix);
        blend_factor(s->blend_dst, 1, &df, &dfix);
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, sf, df, sfix, dfix);
    }
    sceGuDepthFunc((gu_debug & 8) && s->layer == DRAW_LAYER_2D ? GU_ALWAYS : depth_func(s->depth_func));
    /* depth_write is the game's, except for translucent 2D strips at either
     * end of the buffer (rec_depth_write) */
    sceGuDepthMask(s->depth_write ? GU_FALSE : GU_TRUE);     /* GU_TRUE masks (disables) the write */
    if (s->alpha_test) { sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_GEQUAL, 0x80, 0xFF); }
    else sceGuDisable(GU_ALPHA_TEST);
    if (s->texture && s->texture < TEX_MAX && texs[s->texture].mem) sceGuEnable(GU_TEXTURE_2D);
    else sceGuDisable(GU_TEXTURE_2D);
}

static void apply_state(const draw_state *s) {
    apply_blend_depth(s);
    if (s->cull == DRAW_CULL_NEGATIVE || s->cull == DRAW_CULL_POSITIVE) {
        /* draw_gl.c: POSITIVE = GL front face CW in GL's y-up window space,
         * which is counter-clockwise in the GE's y-down screen space. */
        int pos = (s->cull == DRAW_CULL_POSITIVE) ^ cull_flip;
        sceGuEnable(GU_CULL_FACE);
        sceGuFrontFace(pos ? GU_CCW : GU_CW);
    } else sceGuDisable(GU_CULL_FACE);
    if (s->texture && s->texture < TEX_MAX && texs[s->texture].mem) bind_texture(s);
    sceGuDepthOffset(s->translucent ? 4 : 0);
    if (s->fog == DRAW_FOG_TABLE || s->fog == DRAW_FOG_TABLE2) sceGuEnable(GU_FOG); else sceGuDisable(GU_FOG);
    sceGuShadeModel(s->gouraud ? GU_SMOOTH : GU_FLAT);
}

static void batch_flush(void);
static int rec_state(const draw_state *s);

void draw_set_state(const draw_state *s) {
    if (cur_valid && memcmp(s, &cur, sizeof cur) == 0) return;
    if (!headless && list_open) batch_flush();
    cur = *s; cur_valid = 1;
    stats.state_changes++;
    if (headless) return;
    if (!list_open || rec_state(s) != 0) { cur_valid = 0; return; }   /* recorded only while a frame is open */
}

/* ---- primitives ----------------------------------------------------------------- */
#define BATCH_MAX 4096
#define STATE_MAX 4096
#define ITEM_MAX 6144
typedef struct { uint32_t color; float x, y, z; } gu_ovtx;
#define OVTX_FMT (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

/* One recorded draw: a run of strips (or a line strip) of one state and
 * projection in one pass, its vertices already in the display list (the
 * list jumps over them), the offset-colour vertices beside them. */
typedef struct { uint16_t state; uint8_t proj, prim; uint16_t n, on; void *v, *ov; } gu_item;
static draw_state rstates[STATE_MAX];
static int nrstates;
static gu_item items[PASSES][ITEM_MAX];
static int nitems[PASSES];
/* The frame's vertices, written once where the GE reads them.  They used to
 * be converted into a static batch and copied into the display list at each
 * flush: two passes over memory far larger than the 16 KB data cache, the
 * second through the list's uncached alias (memcpy was 3.6 % of the frame).
 * Now a run grows in place inside a chunk of a cached pool (batch[pass] is
 * where the open run starts, vend[pass] its chunk's end) and a flush only
 * records where it starts.  The four streams (vertices and offset vertices,
 * far and near pass) take chunks from one pool as they need them: their
 * shares differ from scene to scene (the far pass took 760 KB of vertices
 * and over 256 KB of offset vertices late in title_menu.tape, the near pass
 * 20 KB).  draw_end_frame writes the cache back before replay() hands the
 * runs to the GE, and the frame ends with sceGuSync, so the pool restarts at
 * every frame. */
#define VPOOL_BYTES (1792u << 10)
static uint8_t vpool[VPOOL_BYTES] __attribute__((aligned(64)));
static uint32_t vpool_top;
static gu_vtx *batch[PASSES], *vend[PASSES];
static gu_ovtx *obatch[PASSES], *oend[PASSES];
static int nbatch[PASSES], nobatch[PASSES];

static void *pool_chunk(uint32_t bytes) {
    if (vpool_top + bytes > VPOOL_BYTES) return NULL;
    void *c = vpool + vpool_top;
    vpool_top += bytes;
    return c;
}

static void rec_reset(void) {
    nrstates = 0;
    if (vpool_top > gs.pool_high) gs.pool_high = vpool_top;
    vpool_top = 0;
    for (int p = 0; p < PASSES; p++) {
        nitems[p] = nbatch[p] = nobatch[p] = 0;
        batch[p] = vend[p] = NULL;
        obatch[p] = oend[p] = NULL;
    }
}

static int rec_state(const draw_state *s) {
    if (nrstates == STATE_MAX) { gs.dropped++; return -1; }
    rstates[nrstates++] = *s;
    return 0;
}

static void batch_flush_pass(int pass) {
    if (!nbatch[pass]) return;
    if (nitems[pass] == ITEM_MAX || nrstates == 0) { gs.dropped++; nbatch[pass] = nobatch[pass] = 0; return; }
    gu_item *it = &items[pass][nitems[pass]++];
    it->state = (uint16_t)(nrstates - 1); it->proj = (uint8_t)cur_proj; it->prim = GU_TRIANGLE_STRIP;
    it->n = (uint16_t)nbatch[pass]; it->on = (uint16_t)nobatch[pass];
    it->v = batch[pass];
    batch[pass] += nbatch[pass];
    it->ov = NULL;
    if (nobatch[pass]) {
        it->ov = obatch[pass];
        obatch[pass] += nobatch[pass];
    }
    nbatch[pass] = nobatch[pass] = 0;
}

static void batch_flush(void) {
    for (int p = 0; p < PASSES; p++) batch_flush_pass(p);
}

static void select_proj(const draw_vertex *v, int n) {
    int p = PROJ_3D;
    if (cur.layer == DRAW_LAYER_2D) {
        float xmin = v[0].x, xmax = v[0].x;
        for (int i = 1; i < n; i++) { if (v[i].x < xmin) xmin = v[i].x; if (v[i].x > xmax) xmax = v[i].x; }
        p = (xmin <= 0.5f && xmax >= DC_W - 0.5f) ? PROJ_FULL : PROJ_2D;
    } else if (cur.layer == DRAW_LAYER_3D_43) p = PROJ_2D;
    if (p != cur_proj) {
        if (nbatch[0] || nbatch[1]) gs.flush_proj++;
        batch_flush();
        cur_proj = p;
    }
}

/* Sets the projection of pass for proj p and the scissor that goes with it:
 * the 2D layer's 4:3 area is the game's whole screen, so what it draws past
 * x 0 or 640 (the title's water wobble) is cut at the area's edges.  In
 * 4:3 mode everything is in the area already (begin_frame). */
static void set_proj(int pass, int p) {
    sceGuSetMatrix(GU_PROJECTION, &proj[pass][p]);
    if (aspect_wide) {
        if (p == PROJ_2D) sceGuScissor(SCR_W / 8, 0, SCR_W * 7 / 8, SCR_H);
        else sceGuScissor(0, 0, SCR_W, SCR_H);
    }
}

/* Draws what the frame recorded: the far pass, a depth clear, the near pass. */
static void replay(void) {
    batch_flush();
    frame_clear();
    for (int pass = 0; pass < PASSES; pass++) {
        if (!nitems[pass]) continue;
        if (pass == PASS_NEAR && nitems[PASS_FAR]) {
            sceGuScissor(0, 0, SCR_W, SCR_H);
            sceGuDepthMask(GU_FALSE);
            sceGuClearDepth(0);
            sceGuClear(GU_DEPTH_BUFFER_BIT);
            if (!aspect_wide) sceGuScissor(SCR_W / 8, 0, SCR_W * 7 / 8, SCR_H);
        }
        int st = -1, pj = -1;
        for (int i = 0; i < nitems[pass]; i++) {
            gu_item *it = &items[pass][i];
            const draw_state *s = &rstates[it->state];
            if (it->state != st) { apply_state(s); st = it->state; }
            if (it->proj != pj) { set_proj(pass, it->proj); pj = it->proj; }
            sceGuDrawArray(it->prim, VTX_FMT, it->n, 0, it->v);
            stats.draw_calls++;
            if (it->on) {                 /* the offset colour: additive, untextured, at equal depth */
                sceGuDisable(GU_TEXTURE_2D);
                sceGuEnable(GU_BLEND); sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFu, 0xFFFFFFu);
                sceGuDepthFunc(GU_GEQUAL); sceGuDepthMask(GU_TRUE);
                sceGuDisable(GU_ALPHA_TEST);
                sceGuDrawArray(GU_TRIANGLE_STRIP, OVTX_FMT, it->on, 0, it->ov);
                stats.draw_calls++;
                apply_blend_depth(s);     /* the texture stays bound; only its enable changed */
            }
        }
    }
}

static inline uint32_t sat_add(uint32_t c, uint32_t o) {
    unsigned r = ((c >> 16) & 255) + ((o >> 16) & 255), g = ((c >> 8) & 255) + ((o >> 8) & 255), b = (c & 255) + (o & 255);
    return (r > 255 ? 255u : r) << 16 | (g > 255 ? 255u : g) << 8 | (b > 255 ? 255u : b);
}

/* Vertex colour by the shading rules (draw.h; the texture function is set
 * in bind_texture): alpha forced to 1 without use_alpha, and for DECAL with
 * the texture alpha ignored (REPLACE + GU_TCC_RGB takes the vertex's). */
/* The GE's z of a vertex: 1/w, clamped to the nearest the passes draw. */
static inline float vtx_z(const draw_vertex *v) {
    float w = v->w > 1e-6f ? v->w : 1e-6f, z = 1.0f / w;
    return z < depth_zn ? depth_zn : z;
}
/* conv's mode for the current state (CONV_*), taken once per strip. */
enum { CONV_ALPHA = 1, CONV_ADD_OFFSET = 2 };
static inline int conv_mode(void) {
    int alpha = cur.use_alpha && !(cur.texture && cur.shade == DRAW_SHADE_DECAL && cur.ignore_tex_alpha);
    return (alpha ? CONV_ALPHA : 0) | (cur.texture ? 0 : CONV_ADD_OFFSET);
}
static inline void conv(gu_vtx *o, const draw_vertex *v, float z, int mode) {
    o->u = v->u; o->v = v->v;
    o->x = v->x * z; o->y = v->y * z; o->z = -z;
    uint32_t c = v->color, a = (mode & CONV_ALPHA) ? c >> 24 : 255u;
    if (mode & CONV_ADD_OFFSET) c = sat_add(c, v->ocolor);
    o->color = a << 24 | (c & 0xFF00u) | ((c >> 16) & 255u) | ((c & 255u) << 16);
}

static int list_full(void) {
    if ((uint32_t)sceGuCheckList() + LIST_SLACK < LIST_BYTES) return 0;
    gs.dropped++;
    return 1;
}

/* The offset (specular) colour of textured strips: a second, additive,
 * untextured draw at equal depth (draw_gl.c's emit_offset), collected per
 * recorded run beside the run's own vertices and drawn after them.  The
 * colour is scaled by the vertex alpha under source-alpha blending (the
 * PowerVR adds it before the blend), in full for an additive strip. */
static inline unsigned mul255(unsigned c, unsigned a) { unsigned t = c * a + 128; return (t + (t >> 8)) >> 8; }   /* c * a / 255, rounded */
static inline void conv_offset(gu_ovtx *o, const draw_vertex *v, float z, int scale) {
    uint32_t c = v->ocolor;
    unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if (scale) { unsigned a = (v->color >> 24) & 255; r = mul255(r, a); g = mul255(g, a); b = mul255(b, a); }
    o->x = v->x * z; o->y = v->y * z; o->z = -z;
    o->color = 0xFF000000u | b << 16 | g << 8 | r;
}

/* ---- the GE's guard band -------------------------------------------------
 * The GE rejects a whole primitive when a vertex lands outside its
 * 4096-pixel drawing window instead of clipping it (PPSSPP draws it anyway):
 * on the console, ground and walls that run from near the camera far past
 * the screen edge vanished at the edges of the view (the maintainer, build 3).  So a
 * strip with a vertex beyond GUARD_* (in the game's 640x480 screen space,
 * inside the window for every projection: draw_set_aspect's 3D and 2D
 * layers reach x -3093..3733, the full-width one -2240..2880, y -3120..3600)
 * is cut into triangles and each is clipped here, in screen space: x, y
 * and w = 1/z are linear there, the texture coordinates and colours are
 * interpolated through w (perspective-correct, as the GE does).  The same
 * clipper cuts a strip at the depth split (w = 1/split). */
#define GUARD_X0 (-2000.0f)
#define GUARD_X1 2640.0f
#define GUARD_Y0 (-2000.0f)
#define GUARD_Y1 2480.0f

static int in_guard(const draw_vertex *v, int n) {
    for (int i = 0; i < n; i++)
        if (v[i].x < GUARD_X0 || v[i].x > GUARD_X1 || v[i].y < GUARD_Y0 || v[i].y > GUARD_Y1) return 0;
    return 1;
}

static uint32_t lerp_color(uint32_t a, float wa, uint32_t b, float wb, float t, float iw) {
    if (a == b) return a;
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        float ca = (float)((a >> sh) & 255) * wa, cb = (float)((b >> sh) & 255) * wb;
        float c = (ca + (cb - ca) * t) * iw;
        int ci = (int)(c + 0.5f);
        out |= (uint32_t)(ci < 0 ? 0 : ci > 255 ? 255 : ci) << sh;
    }
    return out;
}

static draw_vertex lerp_vertex(const draw_vertex *a, const draw_vertex *b, float t) {
    draw_vertex o;
    o.x = a->x + (b->x - a->x) * t;
    o.y = a->y + (b->y - a->y) * t;
    o.w = a->w + (b->w - a->w) * t;
    float iw = 1.0f / (o.w > 1e-9f ? o.w : 1e-9f);
    o.u = (a->u * a->w + (b->u * b->w - a->u * a->w) * t) * iw;
    o.v = (a->v * a->w + (b->v * b->w - a->v * a->w) * t) * iw;
    o.color = lerp_color(a->color, a->w, b->color, b->w, t, iw);
    o.ocolor = lerp_color(a->ocolor, a->w, b->ocolor, b->w, t, iw);
    return o;
}

/* One plane of Sutherland-Hodgman on x (axis 0), y (1) or w (2): keep the
 * side where (coord * sign) <= lim * sign. */
static float axis_of(const draw_vertex *v, int axis) { return axis == 0 ? v->x : axis == 1 ? v->y : v->w; }
static int clip_plane(const draw_vertex *in, int n, draw_vertex *out, int axis, float lim, float sign) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const draw_vertex *a = &in[i], *b = &in[(i + 1) % n];
        float da = (axis_of(a, axis) - lim) * sign, db = (axis_of(b, axis) - lim) * sign;
        if (da <= 0.0f) out[m++] = *a;
        if ((da <= 0.0f) != (db <= 0.0f)) out[m++] = lerp_vertex(a, b, da / (da - db));
    }
    return m;
}

static void strip_emit(int pass, const draw_vertex *v, int n);

/* A convex polygon (consistent winding) as a strip, zigzag from vertex 0. */
static void poly_emit(int pass, const draw_vertex *poly, int k) {
    draw_vertex strip[16];
    int lo = 1, hi = k - 1, m = 0;
    strip[m++] = poly[0];
    while (lo <= hi) {
        strip[m++] = poly[lo++];
        if (lo <= hi) strip[m++] = poly[hi--];
    }
    strip_emit(pass, strip, m);
}

/* Which pass a run of vertices belongs to: -1 when it spans the split. */
static int pass_of(const draw_vertex *v, int n) {
    if (!depth_split || (cur.layer == DRAW_LAYER_2D && v[0].w >= 1.0f)) return PASS_NEAR;
    float ws = 1.0f / depth_split;
    int near = 0, far = 0;
    for (int i = 0; i < n; i++) { if (v[i].w > ws) near = 1; else far = 1; }
    return near && far ? -1 : near ? PASS_NEAR : PASS_FAR;
}

/* Triangle by triangle: clipped to the guard band when guard, cut at the
 * split, each piece to its pass. */
static void strip_split(const draw_vertex *v, int n, int guard) {
    if (guard) gs.offscreen++;
    float ws = depth_split ? 1.0f / depth_split : 0.0f;
    for (int i = 0; i + 2 < n; i++) {
        draw_vertex poly[2][16];
        poly[0][0] = v[(i & 1) ? i + 1 : i];
        poly[0][1] = v[(i & 1) ? i : i + 1];
        poly[0][2] = v[i + 2];
        int k = 3, c = 0;
        if (guard) {
            k = clip_plane(poly[c], k, poly[c ^ 1], 0, GUARD_X0, -1.0f); c ^= 1;
            if (k >= 3) { k = clip_plane(poly[c], k, poly[c ^ 1], 0, GUARD_X1, 1.0f); c ^= 1; }
            if (k >= 3) { k = clip_plane(poly[c], k, poly[c ^ 1], 1, GUARD_Y0, -1.0f); c ^= 1; }
            if (k >= 3) { k = clip_plane(poly[c], k, poly[c ^ 1], 1, GUARD_Y1, 1.0f); c ^= 1; }
            if (k < 3) continue;
        }
        int pass = pass_of(poly[c], k);
        if (pass >= 0) { poly_emit(pass, poly[c], k); continue; }
        draw_vertex part[16];
        int kn = clip_plane(poly[c], k, part, 2, ws, -1.0f);     /* w >= ws: near */
        if (kn >= 3) poly_emit(PASS_NEAR, part, kn);
        int kf = clip_plane(poly[c], k, part, 2, ws, 1.0f);      /* w <= ws: far */
        if (kf >= 3) poly_emit(PASS_FAR, part, kf);
    }
}

/* Translucent 2D strips (menus, HUD, text) at either end of the 16-bit
 * buffer do not write depth: they are drawn sorted far to near, so their
 * order among themselves is the painter's, and they still test against the
 * 3D depth.  With writes, the buffer decided ties between 2D layers at the
 * same 1/w by the GE's rounding: on the console the character select's
 * text lost to its own backing quads (all at 1/w 0.00002, a unit or two
 * above the far end) where PPSSPP's exact compare let it through (694b9e8).
 * Between the ends they write as the game asks: the Station Square
 * opening's skyline and rooftop are one translucent 2D quad at 1/w 0.001
 * (station3, priority -1000), and without its depth the sea (sswater01,
 * z ~1400 there, behind it) showed through as a blue wedge over the roof
 * (2026-10-19).  The 2D depth is 1/max(1, -priority) (draw2d.c), so the
 * ends are the priorities beyond -10000 and the HUD's 1.  3D translucent
 * strips always keep their writes: the characters are largely in the
 * translucent list and need them for their own front and back (with them
 * off, Sonic's back showed through his front, the maintainer's run of 694b9e8).
 * The state recorded for the strip carries the write it gets. */
#define UI_FAR_W 1e-4f
static void rec_depth_write(const draw_vertex *v) {
    if (!cur.translucent || cur.layer != DRAW_LAYER_2D || !cur.depth_write || !nrstates) return;
    uint8_t want = v[0].w >= UI_FAR_W && v[0].w < 1.0f;
    if (rstates[nrstates - 1].depth_write == want) return;
    batch_flush();
    draw_state s = cur;
    s.depth_write = want;
    rec_state(&s);
}

void draw_strip(const draw_vertex *v, int n) {
    if (n < 3) return;
    stats.strips++; stats.triangles += (uint32_t)(n - 2);
    if (headless || !list_open || !cur_valid || n > BATCH_MAX - 4 || list_full()) return;
    select_proj(v, n);
    rec_depth_write(v);
    int guard = !in_guard(v, n), pass = guard ? -1 : pass_of(v, n);
    if (gu_debug & (32 | 64)) {       /* bisection: 32 skips 3D strips that reach the near plane, 64 guard-band ones */
        int near1 = 0;
        for (int i = 0; i < n; i++) if (v[i].w >= 0.999f) near1 = 1;
        if ((gu_debug & 32) && near1 && cur.layer != DRAW_LAYER_2D) return;
        if ((gu_debug & 64) && guard) return;
    }
    if (pass >= 0) strip_emit(pass, v, n);
    else strip_split(v, n, guard);
}

static void strip_emit(int pass, const draw_vertex *v, int n) {
    int has_offset = 0;
    if (cur.texture) for (int i = 0; i < n && !has_offset; i++) if (v[i].ocolor & 0x00FFFFFFu) has_offset = 1;
    if (nbatch[pass] + n + 3 > BATCH_MAX || (has_offset && nobatch[pass] + n + 3 > BATCH_MAX)) batch_flush_pass(pass);
    int need_v = !vend[pass] || batch[pass] + nbatch[pass] + n + 3 > vend[pass];
    int need_o = has_offset && (!oend[pass] || obatch[pass] + nobatch[pass] + n + 3 > oend[pass]);
    if (need_v || need_o) {           /* a new chunk: the open runs end here (a run lies in one chunk) */
        batch_flush_pass(pass);
        if (need_v) { batch[pass] = pool_chunk(BATCH_MAX * sizeof(gu_vtx)); vend[pass] = batch[pass] ? batch[pass] + BATCH_MAX : NULL; }
        if (need_o) { obatch[pass] = pool_chunk(BATCH_MAX * sizeof(gu_ovtx)); oend[pass] = obatch[pass] ? obatch[pass] + BATCH_MAX : NULL; }
        if (!batch[pass] || (has_offset && !obatch[pass])) {
            gs.dropped++; gs.pool_full++;   /* the pool is spent: as with a full list */
            return;
        }
    }
    /* Locals, not the globals: the build has -fno-strict-aliasing, so every
     * store into the batch would otherwise reload them. */
    gu_vtx *b = batch[pass] + nbatch[pass];
    gu_ovtx *o = obatch[pass] + nobatch[pass];
    int nb = nbatch[pass], no = nobatch[pass];
    int scale = cur.use_alpha && cur.blend_dst == DRAW_BLEND_INV_SRC_ALPHA, mode = conv_mode();
    float z0 = vtx_z(&v[0]);
    if (nb) {                         /* degenerate joint; keep the next strip's winding parity */
        gu_vtx last = b[-1];
        *b++ = last;
        if (nb & 1) *b++ = last;
        conv(b++, &v[0], z0, mode);
    }
    if (has_offset && no) {
        gu_ovtx last = o[-1];
        *o++ = last;
        if (no & 1) *o++ = last;
        conv_offset(o++, &v[0], z0, scale);
    }
    float wmax = 0.0f;
    unsigned near16 = 0;
    for (int i = 0; i < n; i++) {
        float z = i ? vtx_z(&v[i]) : z0, w = v[i].w;
        conv(b++, &v[i], z, mode);
        if (has_offset) conv_offset(o++, &v[i], z, scale);
        if (w > wmax) wmax = w;
        near16 += w > 0.0625f;
    }
    nbatch[pass] = (int)(b - batch[pass]);
    nobatch[pass] = (int)(o - obatch[pass]);
    /* the depth statistics (--stats): nearest z, and the vertices nearer than z 2, 4, 8, 16 (w > 1/2 ...) */
    if (cur.layer != DRAW_LAYER_2D && wmax > 0.0f) {
        float zmin = 1.0f / wmax;
        if (zmin < gs.zmin_frame) gs.zmin_frame = zmin;
        if (near16) {
            gs.znear_verts[3] += near16;
            for (int i = 0; i < n; i++) {
                float w = v[i].w;
                gs.znear_verts[0] += w > 0.5f; gs.znear_verts[1] += w > 0.25f; gs.znear_verts[2] += w > 0.125f;
            }
        }
    }
}

void draw_line_strip(const draw_vertex *v, int n) {
    if (n < 2) return;
    if (headless || !list_open || !cur_valid || list_full() || n > 4096) return;
    select_proj(v, n);
    int pass = pass_of(v, n);
    if (pass < 0) pass = PASS_NEAR;
    batch_flush_pass(pass);
    if (nitems[pass] == ITEM_MAX) { gs.dropped++; return; }
    gu_vtx *p = sceGuGetMemory((int)(n * sizeof *p));
    int mode = conv_mode();
    for (int i = 0; i < n; i++) conv(&p[i], &v[i], vtx_z(&v[i]), mode);
    gu_item *it = &items[pass][nitems[pass]++];
    it->state = (uint16_t)(nrstates - 1); it->proj = (uint8_t)cur_proj; it->prim = GU_LINE_STRIP;
    it->n = (uint16_t)n; it->on = 0; it->v = p; it->ov = NULL;
}

/* Written after the GE finishes the frame (draw_end_frame), from EDRAM. */
int draw_screenshot(const char *path) {
    if (headless) return -1;
    shot_path = path;
    return 0;
}

/* ---- input ---------------------------------------------------------------------
 * sceCtrl to the Dreamcast pad (hle.h): cross A, circle B, square X,
 * triangle Y, Start, the d-pad, the nub as the analog stick, L/R as the
 * triggers (digital: 0 or 255).  The nub rests up to ~20 off centre. */
#define NUB_DEAD 28
static int nub(int raw) {
    int d = raw - 128;
    if (d > -NUB_DEAD && d < NUB_DEAD) return 0;
    d = d > 0 ? (d - NUB_DEAD) * 127 / (127 - NUB_DEAD) : (d + NUB_DEAD) * 128 / (128 - NUB_DEAD);
    return d > 127 ? 127 : d < -128 ? -128 : d;
}

int draw_poll_input(struct hle_pad *p, int *quit) {
    *quit = 0;
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) return -1;
    uint32_t b = 0, k = pad.Buttons;
    if (k & PSP_CTRL_CROSS) b |= 4;
    if (k & PSP_CTRL_CIRCLE) b |= 2;
    if (k & PSP_CTRL_SQUARE) b |= 0x400;
    if (k & PSP_CTRL_TRIANGLE) b |= 0x200;
    if (k & PSP_CTRL_START) b |= 8;
    if (k & PSP_CTRL_UP) b |= 0x10;
    if (k & PSP_CTRL_DOWN) b |= 0x20;
    if (k & PSP_CTRL_LEFT) b |= 0x40;
    if (k & PSP_CTRL_RIGHT) b |= 0x80;
    p->buttons = b;
    p->x = (int8_t)nub(pad.Lx);
    p->y = (int8_t)nub(pad.Ly);
    p->ltrig = (k & PSP_CTRL_LTRIGGER) ? 255 : 0;
    p->rtrig = (k & PSP_CTRL_RTRIGGER) ? 255 : 0;
    return 0;
}
