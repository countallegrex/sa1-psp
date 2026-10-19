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
#define LIST_BYTES (2u << 20)
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
static struct { uint32_t list_high, dropped, offscreen, mirrors, mirror_fail, downscaled, frames;
                uint32_t flush_state, flush_proj, flush_offset, last_strips, last_draws, last_states;
                uint64_t gpu_wait_us, frame_us_sum, frame_us_max, last_end; } gs;

typedef struct { float u, v; uint32_t color; float x, y, z; } gu_vtx;
#define VTX_FMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

void draw_gu_set_cull_flip(int on) { cull_flip = on; }

/* ---- projection --------------------------------------------------------------- */
enum { PROJ_3D = 0, PROJ_2D, PROJ_FULL, PROJ_N };
static ScePspFMatrix4 proj[PROJ_N];
static int cur_proj = -1;
/* x in 640-space from left to right -> NDC [lo, hi]; y 0..480 -> 1..-1; the
 * vertex is (x*z, y*z, -z), clip w = z, clip z = z - 2 (draw_gl.c's
 * 1 - 2/z).  Columns are x, y, z, w (GL order). */
static void build_proj(ScePspFMatrix4 *m, float left, float right, float lo, float hi) {
    float sx = (hi - lo) / (right - left), ox = lo - left * sx;
    memset(m, 0, sizeof *m);
    m->x.x = sx;  m->z.x = -ox;
    m->y.y = -2.0f / DC_H; m->z.y = -1.0f;
    m->z.z = -1.0f; m->w.z = -2.0f;
    m->z.w = -1.0f;
}
static void build_projs(void) {
    if (aspect_wide) build_proj(&proj[PROJ_3D], DRAW_WIDE_LEFT, DRAW_WIDE_RIGHT, -1.0f, 1.0f);
    else build_proj(&proj[PROJ_3D], 0.0f, DC_W, -0.75f, 0.75f);
    build_proj(&proj[PROJ_2D], 0.0f, DC_W, -0.75f, 0.75f);
    build_proj(&proj[PROJ_FULL], 0.0f, DC_W, -1.0f, 1.0f);
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

/* Half size: a 2x2 box for direct colour, the top-left texel for indices. */
static uint8_t *halve(const uint8_t *src, int w, int h, int fmt) {
    int bits = fmt_bits(fmt), nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    uint32_t srb = row_bytes(w, bits), drb = row_bytes(nw, bits);
    uint8_t *dst = calloc(1, (size_t)drb * nh + 4);
    if (!dst) return NULL;
    int sx = w > 1 ? 2 : 1, sy = h > 1 ? 2 : 1;
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            if (bits <= 8) { px_set(dst, drb, bits, x, y, px_get(src, srb, bits, x * sx, y * sy)); continue; }
            unsigned acc[4] = {0, 0, 0, 0}, c[4];
            for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
                unpack(fmt, px_get(src, srb, bits, x * sx + (sx > 1 ? i : 0), y * sy + (sy > 1 ? j : 0)), c);
                for (int k = 0; k < 4; k++) acc[k] += c[k];
            }
            for (int k = 0; k < 4; k++) acc[k] = (acc[k] + 2) / 4;
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
    while (levels < TEX_LEVELS && (w >> levels) >= 8 && (h >> levels) >= 8) levels++;
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
        swizzle(t->mem + t->off[l], pad, rb[l], rows[l]);
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
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    build_projs();
    headless = 1;
    if (want_headless) return 0;
    sceKernelDcacheWritebackInvalidateRange(list_mem, sizeof list_mem);   /* nothing cached may land on it later */
    list = (uint32_t *)((uintptr_t)list_mem | 0x40000000u);
    sceGuInit();
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
    sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);
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
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    headless = 0;
    sa1_log("draw: sceGu, 480x272 RGB565 x2 + 16-bit Z in EDRAM (%u KB), display list %u KB", 3u * FB_BYTES / 1024u, LIST_BYTES / 1024u);
    return 0;
}

void draw_shutdown(void) {
    if (headless) return;
    sceGuTerm();
    headless = 1;
}

static void apply_fog(void) { sceGuFog(fog_near, fog_far, abgr(fog_color)); }

void draw_set_fog(uint32_t color_argb, float z_near, float z_far) {
    fog_color = color_argb; fog_near = z_near; fog_far = z_far > z_near ? z_far : z_near + 1.0f;
    if (!headless && list_open) apply_fog();
}

static int nbatch, nobatch;
static float tex_su = -1.0f, tex_sv = -1.0f;
static void batch_flush(void);

void draw_begin_frame(uint32_t clear_argb) {
    stats.draw_calls = stats.triangles = stats.strips = stats.state_changes = 0;
    cur_valid = 0; cur_proj = -1; bound_tex = 0; nbatch = 0; nobatch = 0;
    if (headless) return;
    tex_su = tex_sv = -1.0f;        /* re-sent with the first texture */
    /* the buffer we are about to draw is on screen until the swap's vblank */
    if (swap_pending && sceDisplayGetVcount() == swap_vcount) sceDisplayWaitVblankStart();
    swap_pending = 0;
    sceGuStart(GU_DIRECT, list);
    list_open = 1;
    sceGuDepthOffset(0);
    sceGuDepthMask(GU_FALSE);
    if (!aspect_wide) {             /* 4:3: black pillars, the game's colour and all drawing in the middle */
        sceGuScissor(0, 0, SCR_W, SCR_H);
        sceGuClearColor(0xFF000000u); sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        sceGuScissor(SCR_W / 8, 0, SCR_W * 7 / 8, SCR_H);
        sceGuClearColor(abgr(clear_argb) | 0xFF000000u);
        sceGuClear(GU_COLOR_BUFFER_BIT);
    } else {
        sceGuScissor(0, 0, SCR_W, SCR_H);
        sceGuClearColor(abgr(clear_argb) | 0xFF000000u); sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    }
    apply_fog();
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
    batch_flush();
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
    sa1_log("draw: %u frames, frame time avg %.2f ms max %.2f ms, GE wait %.2f ms/frame; list high water %u KB; "
            "%u strips dropped (list full), %u off the GE's guard band",
            gs.frames, gs.frames ? gs.frame_us_sum / 1000.0 / gs.frames : 0.0, gs.frame_us_max / 1000.0,
            gs.frames ? gs.gpu_wait_us / 1000.0 / gs.frames : 0.0, gs.list_high / 1024u, gs.dropped, gs.offscreen);
    sa1_log("draw: last frame %u strips -> %u GE draws, %u state changes; batch breaks by state %u, projection %u; "
            "%u mirrors (%u too big), %u textures downscaled", gs.last_strips, gs.last_draws, gs.last_states,
            gs.flush_state, gs.flush_proj, gs.mirrors, gs.mirror_fail, gs.downscaled);
    gs.flush_state = gs.flush_proj = gs.flush_offset = 0;
    gs.frames = 0; gs.frame_us_sum = gs.frame_us_max = gs.gpu_wait_us = 0; gs.dropped = gs.offscreen = 0;
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
        sceGuTexMode(fmt_psm(t->fmt), t->levels - 1, 0, 1);
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
    sceGuDepthFunc(depth_func(s->depth_func));
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

void draw_set_state(const draw_state *s) {
    if (cur_valid && memcmp(s, &cur, sizeof cur) == 0) return;
    if (!headless && list_open) { if (nbatch) gs.flush_state++; batch_flush(); }
    cur = *s; cur_valid = 1;
    stats.state_changes++;
    if (headless) return;
    if (!list_open) { cur_valid = 0; return; }   /* applied when a frame is open */
    apply_state(s);
}

/* ---- primitives ----------------------------------------------------------------- */
#define BATCH_MAX 6144
static gu_vtx batch[BATCH_MAX];

static void select_proj(const draw_vertex *v, int n) {
    int p = PROJ_3D;
    if (cur.layer == DRAW_LAYER_2D) {
        float xmin = v[0].x, xmax = v[0].x;
        for (int i = 1; i < n; i++) { if (v[i].x < xmin) xmin = v[i].x; if (v[i].x > xmax) xmax = v[i].x; }
        p = (xmin <= 0.5f && xmax >= DC_W - 0.5f) ? PROJ_FULL : PROJ_2D;
    }
    if (p != cur_proj) { if (nbatch) gs.flush_proj++; batch_flush(); sceGuSetMatrix(GU_PROJECTION, &proj[p]); cur_proj = p; }
}

static inline uint32_t sat_add(uint32_t c, uint32_t o) {
    unsigned r = ((c >> 16) & 255) + ((o >> 16) & 255), g = ((c >> 8) & 255) + ((o >> 8) & 255), b = (c & 255) + (o & 255);
    return (r > 255 ? 255u : r) << 16 | (g > 255 ? 255u : g) << 8 | (b > 255 ? 255u : b);
}

/* Vertex colour by the shading rules (draw.h; the texture function is set
 * in bind_texture): alpha forced to 1 without use_alpha, and for DECAL with
 * the texture alpha ignored (REPLACE + GU_TCC_RGB takes the vertex's). */
static inline void conv(gu_vtx *o, const draw_vertex *v) {
    float w = v->w > 1e-6f ? v->w : 1e-6f, z = 1.0f / w;
    if (z < 1.0f) z = 1.0f;
    o->u = v->u; o->v = v->v;
    o->x = v->x * z; o->y = v->y * z; o->z = -z;
    uint32_t c = v->color, a = cur.use_alpha ? c >> 24 : 255u;
    if (!cur.texture) c = sat_add(c, v->ocolor);
    else if (cur.shade == DRAW_SHADE_DECAL && cur.ignore_tex_alpha) a = 255u;
    o->color = a << 24 | (c & 0xFF00u) | ((c >> 16) & 255u) | ((c & 255u) << 16);
}

/* The offset (specular) colour of textured strips: a second, additive,
 * untextured draw at equal depth (draw_gl.c's emit_offset).  Strips of one
 * state run collect their offset vertices in a batch of their own, drawn
 * after the run's main batch; within a run that orders highlights after
 * all the run's surfaces instead of after each, which the depth test makes
 * the same picture for opaque runs. */
typedef struct { uint32_t color; float x, y, z; } gu_ovtx;
#define OVTX_FMT (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
static gu_ovtx obatch[BATCH_MAX];

static void batch_flush(void) {
    if (nbatch) {
        gu_vtx *p = sceGuGetMemory((int)(nbatch * sizeof *p));
        memcpy(p, batch, nbatch * sizeof *p);
        sceGuDrawArray(GU_TRIANGLE_STRIP, VTX_FMT, nbatch, 0, p);
        stats.draw_calls++;
        nbatch = 0;
    }
    if (nobatch) {
        gu_ovtx *p = sceGuGetMemory((int)(nobatch * sizeof *p));
        memcpy(p, obatch, nobatch * sizeof *p);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuEnable(GU_BLEND); sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFu, 0xFFFFFFu);
        sceGuDepthFunc(GU_GEQUAL); sceGuDepthMask(GU_TRUE);
        sceGuDisable(GU_ALPHA_TEST);
        sceGuDrawArray(GU_TRIANGLE_STRIP, OVTX_FMT, nobatch, 0, p);
        stats.draw_calls++;
        nobatch = 0;
        apply_blend_depth(&cur);      /* the texture stays bound; only its enable changed */
    }
}

/* The GE drops a primitive with a vertex outside its 4096-pixel window
 * (screen x -1808..2288 here) instead of clipping it; counted. */
static int offscreen(const gu_vtx *v, int n, int p) {
    float sx = proj[p].x.x, ox = -proj[p].z.x;
    for (int i = 0; i < n; i++) {
        float z = -v[i].z, xn = sx * v[i].x / z + ox;
        float yn = 1.0f - 2.0f / DC_H * v[i].y / z;
        if (xn < -7.5f || xn > 7.5f || yn < -15.0f || yn > 15.0f) return 1;
    }
    return 0;
}

static int list_full(void) {
    if ((uint32_t)sceGuCheckList() + LIST_SLACK < LIST_BYTES) return 0;
    gs.dropped++;
    return 1;
}

/* The offset vertex: the colour scaled by the vertex alpha under
 * source-alpha blending (the PowerVR adds it before the blend), in full for
 * an additive strip. */
static inline void conv_offset(gu_ovtx *o, const draw_vertex *v) {
    uint32_t c = v->ocolor;
    unsigned a = (cur.use_alpha && cur.blend_dst == DRAW_BLEND_INV_SRC_ALPHA) ? (v->color >> 24) & 255 : 255;
    unsigned r = ((c >> 16) & 255) * a / 255, g = ((c >> 8) & 255) * a / 255, b = (c & 255) * a / 255;
    float w = v->w > 1e-6f ? v->w : 1e-6f, z = 1.0f / w;
    if (z < 1.0f) z = 1.0f;
    o->x = v->x * z; o->y = v->y * z; o->z = -z;
    o->color = 0xFF000000u | b << 16 | g << 8 | r;
}

void draw_strip(const draw_vertex *v, int n) {
    if (n < 3) return;
    stats.strips++; stats.triangles += (uint32_t)(n - 2);
    if (headless || !list_open || !cur_valid || n > BATCH_MAX - 4 || list_full()) return;
    select_proj(v, n);
    int has_offset = 0;
    if (cur.texture) for (int i = 0; i < n && !has_offset; i++) if (v[i].ocolor & 0x00FFFFFFu) has_offset = 1;
    if (nbatch + n + 3 > BATCH_MAX || (has_offset && nobatch + n + 3 > BATCH_MAX)) batch_flush();
    if (nbatch) {                     /* degenerate joint; keep the next strip's winding parity */
        gu_vtx last = batch[nbatch - 1];
        int odd = nbatch & 1;
        batch[nbatch++] = last;
        if (odd) batch[nbatch++] = last;
        conv(&batch[nbatch++], &v[0]);
    }
    int first = nbatch;
    for (int i = 0; i < n; i++) conv(&batch[nbatch++], &v[i]);
    if (offscreen(&batch[first], n, cur_proj)) gs.offscreen++;
    if (has_offset) {
        if (nobatch) {
            gu_ovtx last = obatch[nobatch - 1];
            int odd = nobatch & 1;
            obatch[nobatch++] = last;
            if (odd) obatch[nobatch++] = last;
            conv_offset(&obatch[nobatch++], &v[0]);
        }
        for (int i = 0; i < n; i++) conv_offset(&obatch[nobatch++], &v[i]);
    }
}

void draw_line_strip(const draw_vertex *v, int n) {
    if (n < 2) return;
    if (headless || !list_open || !cur_valid || list_full()) return;
    select_proj(v, n);
    batch_flush();
    gu_vtx *p = sceGuGetMemory((int)(n * sizeof *p));
    for (int i = 0; i < n; i++) conv(&p[i], &v[i]);
    sceGuDrawArray(GU_LINE_STRIP, VTX_FMT, n, 0, p);
    stats.draw_calls++;
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
