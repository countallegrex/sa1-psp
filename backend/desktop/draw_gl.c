/* draw_gl.c - backend/draw.h on SDL2 + OpenGL 1.x fixed function.
 *
 * Deliberately PSP-shaped (README section 4, rule 1): no shaders, no
 * framebuffer objects, no non-power-of-two textures, one blend equation,
 * 16-bit texture formats uploaded as the PSP would hold them.  Vertices
 * come in screen space with w = 1/z (PowerVR convention); they are handed
 * to GL as the homogeneous eye vector (x*z, y*z, -(z+1), z) so that the
 * rasteriser divides by z again (perspective-correct texturing) and depth
 * is 1 - 2/z, nearer = smaller.  Fog uses the eye distance z + 1 through
 * the fixed-function linear fog, which is what sceGuFog does on the PSP.
 *
 * Headless mode (CI, --frames runs without a display) keeps every call a
 * no-op except the statistics.
 *
 * Part of sa1-psp (MIT). */
#include "../draw.h"
#include "../texpool.h"
#include "hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SA1_NO_SDL
#include <SDL.h>
#include <SDL_opengl.h>
#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT 0x8370
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5_REV
#define GL_UNSIGNED_SHORT_5_6_5_REV 0x8364
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4_REV
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#endif
#ifndef GL_TEXTURE_BASE_LEVEL
#define GL_TEXTURE_BASE_LEVEL 0x813C
#endif
#ifndef GL_TEXTURE_LOD_BIAS
#define GL_TEXTURE_LOD_BIAS 0x8501
#endif
/* GL 1.3 texture combiners (the PowerVR shading instructions need an alpha
 * rule the four classic env modes cannot express) */
#ifndef GL_COMBINE
#define GL_COMBINE 0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_COMBINE_ALPHA 0x8572
#define GL_SOURCE0_RGB 0x8580
#define GL_SOURCE1_RGB 0x8581
#define GL_SOURCE2_RGB 0x8582
#define GL_SOURCE0_ALPHA 0x8588
#define GL_SOURCE1_ALPHA 0x8589
#define GL_OPERAND0_RGB 0x8590
#define GL_OPERAND1_RGB 0x8591
#define GL_OPERAND2_RGB 0x8592
#define GL_OPERAND0_ALPHA 0x8598
#define GL_OPERAND1_ALPHA 0x8599
#define GL_INTERPOLATE 0x8575
#define GL_CONSTANT 0x8576
#define GL_PRIMARY_COLOR 0x8577
#define GL_PREVIOUS 0x8578
#endif
static SDL_Window *win;
static SDL_GLContext glctx;
static SDL_GameController *pad;
#endif

#define DC_W 640.0f
#define DC_H 480.0f

static int headless = 1, win_w = 480, win_h = 272;
int draw_gl_hidden;   /* main.c --hidden: the window is created hidden (scripted screenshot runs) */
static int pace_on;                       /* fixed 60 Hz pacing (draw_gl_pace); off with SA1_NOVSYNC */
static int title_frame_on;                /* guest frame in the window title; off with SA1_NO_TITLE_FRAME */
static draw_stats stats;
static draw_state cur;
static int cur_valid;
static int aspect_wide = 1;
enum { PROJ_3D = 0, PROJ_2D, PROJ_FULL, PROJ_N };
static float proj[PROJ_N][16];
static int cur_proj = -1;
/* x in 640-space from `left` to `right` maps to NDC [lo, hi]; y 0..480 to 1..-1 */
static void build_proj(float *P, float left, float right, float lo, float hi) {
    memset(P, 0, 16 * sizeof *P);
    P[0] = (hi - lo) / (right - left); P[12] = lo - left * P[0];
    P[5] = -2.0f / DC_H; P[13] = 1.0f;
    P[10] = 2.0f;        P[14] = 3.0f;
    P[15] = 1.0f;
}
static void build_projs(void) {
    if (aspect_wide) build_proj(proj[PROJ_3D], DRAW_WIDE_LEFT, DRAW_WIDE_RIGHT, -1.0f, 1.0f);
    else build_proj(proj[PROJ_3D], 0.0f, DC_W, -0.75f, 0.75f);
    build_proj(proj[PROJ_2D], 0.0f, DC_W, -0.75f, 0.75f);
    build_proj(proj[PROJ_FULL], 0.0f, DC_W, -1.0f, 1.0f);
    cur_proj = -1;
}
void draw_set_aspect(int wide) {
    aspect_wide = wide ? 1 : 0; build_projs();
    sa1_log("draw: aspect %s; 3D x scale %g offset %g, 2D %g %g, full %g %g", aspect_wide ? "wide (hor+)" : "4:3",
            proj[PROJ_3D][0], proj[PROJ_3D][12], proj[PROJ_2D][0], proj[PROJ_2D][12], proj[PROJ_FULL][0], proj[PROJ_FULL][12]);
}

const draw_stats *draw_get_stats(void) { return &stats; }

/* ---- textures ---------------------------------------------------------------- */
#define TEX_MAX 2048
/* A CPU copy of every texture's pixels (and CLUT) is kept, headless too: save
 * states carry textures from it and re-create them under the same handles
 * (draw_gl_texture_state), and a state saved headless can load into a
 * window.  About 6 MB at the route's peak. */
typedef struct {
    uint32_t gl; int w, h; uint32_t bytes; int fmt, clut_fmt; uint32_t pix_n, clut_n; void *pix, *clut;
    int pblk[4];                  /* SA1_TEX_BUDGET: texpool block + 1 of the texture [0] and its PSP mirror variants [1..3]; -1 = mirror failed */
    int pw, ph, pbits, plevels, dropped;   /* its size on the PSP; top levels the pool dropped */
    uint32_t mfail[4];            /* mirror_epoch + 1 when the variant found no room (draw_gu.c) */
} tex_slot;
static tex_slot texs[TEX_MAX];

/* SA1_TEX_BUDGET=KB: texture memory as the PSP has it (backend/texpool.h),
 * on virtual chunks: every texture and every mirror variant the PSP would
 * build is sized as draw_gu.c sizes it, and one that does not fit in the
 * budget is not created, so it draws white as on the PSP.  SA1_TEX_POOL=0
 * turns off the pool's reclaiming (compaction, evictions, dropped levels):
 * the failures of a full heap, to reproduce them.  Dropped levels show as
 * GL_TEXTURE_BASE_LEVEL, the look the PSP gets. */
static int pool_mode = -1;        /* -1 not read, 0 off, 1 budget, 2 budget + reclaim */
static int in_frame;
static uint32_t pool_frame, pool_log_frame = 0xFFFFFFFFu, pool_fail_logs, mirror_need, mirror_epoch, mirror_room_frame;
static void *pool_chunk_alloc(void) { return (void *)(uintptr_t)0x1000; }
static void pool_chunk_free(void *c) { (void)c; }
static void pool_moved(int owner, uint8_t *addr) { (void)owner; (void)addr; }
static void pool_evict(int owner) {
    tex_slot *t = &texs[owner & 0xFFFF];
    int k = owner >> 16;
    if (t->pblk[k] > 0) texpool_free(t->pblk[k] - 1);
    t->pblk[k] = 0;
}
static uint32_t pool_drop_top(int owner) {
    tex_slot *t = &texs[owner & 0xFFFF];
    if (t->plevels < 2) return 0;
    t->pw >>= 1; t->ph >>= 1; t->plevels--; t->dropped++;
    return texpool_psp_bytes(t->pw, t->ph, t->pbits, t->plevels, NULL);
}
static const texpool_ops pool_ops = { pool_chunk_alloc, pool_chunk_free, pool_moved, pool_evict, pool_drop_top };
static void pool_log(const char *why) {
    const texpool_stats *p = texpool_get_stats();
    sa1_log("texpool (%s): %u chunks (peak %u, %u refused), %u KB used (peak %u KB); %u compactions (%u KB moved), %u evictions, %u top levels dropped, %u failures",
            why, p->chunks, p->peak_chunks, p->refused_chunks, p->used / 1024u, p->peak_used / 1024u, p->compactions, p->moved_bytes / 1024u,
            p->evictions, p->drops, p->failures);
}
static int pool_alloc(uint32_t size, int owner, int cache) {
    const texpool_stats *p = texpool_get_stats();
    uint32_t ev = p->compactions + p->evictions + p->drops + p->failures;
    uint8_t *a;
    int b = texpool_alloc(size, owner, cache, pool_mode == 2 && !in_frame, &a);
    if (ev != p->compactions + p->evictions + p->drops + p->failures && pool_log_frame != pool_frame) {
        pool_log_frame = pool_frame;
        pool_log(b >= 0 ? "reclaimed" : "full");
    }
    return b;
}
static void pool_setup(void) {
    if (pool_mode >= 0) return;
    const char *b = getenv("SA1_TEX_BUDGET"), *r = getenv("SA1_TEX_POOL");
    pool_mode = 0;
    if (!b || atoi(b) <= 0) return;
    pool_mode = (r && !strcmp(r, "0")) ? 1 : 2;
    uint32_t chunks = (uint32_t)atoi(b) / (TEXPOOL_CHUNK / 1024u);
    texpool_init(&pool_ops, 1, chunks ? chunks : 1);
    sa1_log("draw: PSP texture budget %u chunks of %u KB, reclaiming %s", chunks, TEXPOOL_CHUNK / 1024u, pool_mode == 2 ? "on" : "off");
}
static int fmt_bits(int fmt) {
    switch (fmt) {
    case DRAW_FMT_5650: case DRAW_FMT_5551: case DRAW_FMT_4444: return 16;
    case DRAW_FMT_8888: return 32;
    case DRAW_FMT_T4: return 4;
    case DRAW_FMT_T8: return 8;
    default: return 0;
    }
}
/* The texture is drawn: touch it, and account the mirror variant the PSP
 * builds for a flipped repeat (draw_gu.c mirror_variant: inside a frame, so
 * never reclaiming; a failure is a plain repeat there, not white). */
static void pool_use(const draw_state *s) {
    tex_slot *t = &texs[s->texture];
    if (t->pblk[0] > 0) texpool_touch(t->pblk[0] - 1);
    int fu = s->flip_u && !s->clamp_u, fv = s->flip_v && !s->clamp_v, k = fu | fv << 1;
    if (!k) return;
    if (!t->pblk[k] && t->mfail[k] != mirror_epoch + 1) {
        int nw = fu ? t->pw * 2 : t->pw, nh = fv ? t->ph * 2 : t->ph;
        if (nw > 512 || nh > 512) t->pblk[k] = -1;
        else {
            uint32_t need = texpool_psp_bytes(nw, nh, t->pbits, 8, NULL);
            int b = pool_alloc(need, s->texture | k << 16, 1);
            if (b >= 0) t->pblk[k] = b + 1;
            else { t->mfail[k] = mirror_epoch + 1; if (need > mirror_need) mirror_need = need; }
        }
    }
    if (t->pblk[k] > 0) texpool_touch(t->pblk[k] - 1);
}

#ifndef SA1_NO_SDL
/* CLUT formats are expanded here (desktop GL has no palettes); the PSP
 * backend uploads them as they are. */
static uint16_t *expand_clut(int w, int h, int bits, const uint8_t *idx, const uint16_t *clut) {
    uint16_t *out = malloc((size_t)w * h * 2);
    if (!out) return NULL;
    for (int i = 0; i < w * h; i++) {
        unsigned k = bits == 4 ? ((idx[i >> 1] >> ((i & 1) * 4)) & 15) : idx[i];
        out[i] = clut[k];
    }
    return out;
}
#endif

static uint32_t texture_pixel_bytes(int width, int height, int fmt) {
    switch (fmt) {
    case DRAW_FMT_5650: case DRAW_FMT_5551: case DRAW_FMT_4444: return (uint32_t)width * height * 2;
    case DRAW_FMT_8888: return (uint32_t)width * height * 4;
    case DRAW_FMT_T4: return (uint32_t)width * height / 2;
    case DRAW_FMT_T8: return (uint32_t)width * height;
    default: return 0;
    }
}

/* Creates the texture in slot `slot` (0 = the first free one). */
static uint32_t texture_create_at(int slot, int width, int height, int fmt, const void *pixels, int clut_fmt, const void *clut) {
    if (slot == 0) for (slot = 1; slot < TEX_MAX; slot++) if (texs[slot].w == 0) break;
    if (slot <= 0 || slot >= TEX_MAX || texs[slot].w) return 0;
    if ((width & (width - 1)) || (height & (height - 1)) || width < 1 || height < 1) {
        sa1_log("draw: texture %dx%d is not a power of two", width, height);
        return 0;
    }
    uint32_t bytes;
    switch (fmt) {
    case DRAW_FMT_5650: case DRAW_FMT_5551: case DRAW_FMT_4444: bytes = (uint32_t)width * height * 2; break;
    case DRAW_FMT_8888: bytes = (uint32_t)width * height * 4; break;
    case DRAW_FMT_T4: bytes = (uint32_t)width * height / 2 + 16 * 2; break;
    case DRAW_FMT_T8: bytes = (uint32_t)width * height + 256 * 2; break;
    default: return 0;
    }
    pool_setup();
    if (pool_mode > 0) {
        int pw = width, ph = height, lv;
        while (pw > 512 || ph > 512) { if (pw > 1) pw /= 2; if (ph > 1) ph /= 2; }
        int b = pool_alloc(texpool_psp_bytes(pw, ph, fmt_bits(fmt), 8, &lv), slot, 0);
        if (b < 0) {
            if (pool_fail_logs++ < 64) sa1_log("draw: no memory for a %dx%d texture in the PSP budget (%u textures, %u KB)", width, height, stats.textures_alive, stats.texture_bytes / 1024u);
            return 0;
        }
        memset(texs[slot].pblk, 0, sizeof texs[slot].pblk);
        memset(texs[slot].mfail, 0, sizeof texs[slot].mfail);
        texs[slot].pblk[0] = b + 1;
        texs[slot].pw = pw; texs[slot].ph = ph; texs[slot].pbits = fmt_bits(fmt); texs[slot].plevels = lv; texs[slot].dropped = 0;
    }
    texs[slot].w = width; texs[slot].h = height; texs[slot].bytes = bytes; texs[slot].gl = 0;
    texs[slot].fmt = fmt; texs[slot].clut_fmt = clut_fmt;
    texs[slot].pix_n = texture_pixel_bytes(width, height, fmt);
    texs[slot].clut_n = fmt == DRAW_FMT_T4 ? 16 * 2 : fmt == DRAW_FMT_T8 ? 256 * 2 : 0;
    texs[slot].pix = malloc(texs[slot].pix_n); texs[slot].clut = texs[slot].clut_n ? malloc(texs[slot].clut_n) : NULL;
    if (texs[slot].pix) memcpy(texs[slot].pix, pixels, texs[slot].pix_n);
    if (texs[slot].clut && clut) memcpy(texs[slot].clut, clut, texs[slot].clut_n);
    stats.textures_alive++; stats.texture_bytes += bytes;
#ifndef SA1_NO_SDL
    if (!headless) {
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const void *data = pixels;
        uint16_t *tmp = NULL;
        GLenum gfmt = GL_RGB, gtype = GL_UNSIGNED_SHORT_5_6_5_REV;
        GLint ifmt = GL_RGB;
        int f = fmt;
        if (fmt == DRAW_FMT_T4 || fmt == DRAW_FMT_T8) {
            tmp = expand_clut(width, height, fmt == DRAW_FMT_T4 ? 4 : 8, pixels, clut);
            data = tmp; f = clut_fmt;
        }
        switch (f) {
        case DRAW_FMT_5650: gfmt = GL_RGB; gtype = GL_UNSIGNED_SHORT_5_6_5_REV; ifmt = GL_RGB; break;
        case DRAW_FMT_5551: gfmt = GL_RGBA; gtype = GL_UNSIGNED_SHORT_1_5_5_5_REV; ifmt = GL_RGBA; break;
        case DRAW_FMT_4444: gfmt = GL_RGBA; gtype = GL_UNSIGNED_SHORT_4_4_4_4_REV; ifmt = GL_RGBA; break;
        case DRAW_FMT_8888: gfmt = GL_RGBA; gtype = GL_UNSIGNED_BYTE; ifmt = GL_RGBA; break;
        }
        /* Mip levels: the PowerVR samples them (sky domes and far ground
         * minify heavily; without levels they alias into a dot mosaic).  GL
         * 1.4 generates them; the GE takes levels built at load.  The oracle
         * (Flycast's DX11 renderer) samples trilinear with a fixed LOD bias of
         * -1.5, which leaves far ground much sharper than GL's own choice: at
         * 640x480, pixel-aligned with its frames, -1.5 is the best bias (sea
         * MAE 6.3 -> 4.9, high-frequency energy 0.77 -> 0.95 of the console's;
         * -1.75 and -2 are worse, as is the nearest level). */
        glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
        glTexImage2D(GL_TEXTURE_2D, 0, ifmt, width, height, 0, gfmt, gtype, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, -1.5f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        free(tmp);
        texs[slot].gl = id;
        cur_valid = 0;
    }
#else
    (void)pixels; (void)clut_fmt; (void)clut;
#endif
    return (uint32_t)slot;
}

uint32_t draw_texture_create(int width, int height, int fmt, const void *pixels, int clut_fmt, const void *clut) {
    return texture_create_at(0, width, height, fmt, pixels, clut_fmt, clut);
}

void draw_texture_destroy(uint32_t handle) {
    if (handle == 0 || handle >= TEX_MAX || texs[handle].w == 0) return;
#ifndef SA1_NO_SDL
    if (texs[handle].gl) { GLuint id = texs[handle].gl; glDeleteTextures(1, &id); }
#endif
    stats.textures_alive--; stats.texture_bytes -= texs[handle].bytes;
    for (int k = 0; k < 4; k++) if (texs[handle].pblk[k] > 0) texpool_free(texs[handle].pblk[k] - 1);
    free(texs[handle].pix); free(texs[handle].clut);
    memset(&texs[handle], 0, sizeof texs[handle]);
    cur_valid = 0;
}

/* Save states (backend/desktop/savestate.c): every live texture from its CPU
 * copy; a load drops the current textures and re-creates the saved ones
 * under their handles, which guest RAM refers to. */
#include "state.h"
typedef struct tex_rec { uint32_t handle, pix_n, clut_n; int32_t w, h, fmt, clut_fmt; } tex_rec;
void draw_gl_texture_state(hle_state_io *io) {
    uint32_t count = 0;
    if (!hle_state_loading(io)) for (int i = 1; i < TEX_MAX; i++) count += texs[i].w != 0;
    hle_state_blob(io, "textures", &count, sizeof count);
    if (hle_state_loading(io)) {
        if (hle_state_error(io)) return;
        for (int i = 1; i < TEX_MAX; i++) if (texs[i].w) draw_texture_destroy((uint32_t)i);
        for (uint32_t k = 0; k < count; k++) {
            tex_rec r;
            hle_state_blob(io, "tex", &r, sizeof r);
            uint32_t pn = 0, cn = 0;
            const void *pix = hle_state_var(io, "tex.pix", NULL, &pn);
            const void *clut = hle_state_var(io, "tex.clut", NULL, &cn);
            if (hle_state_error(io)) return;
            if (pn != r.pix_n || cn != r.clut_n || !pix) { sa1_log("draw: texture %u in the state has %u/%u bytes, expected %u/%u", r.handle, pn, cn, r.pix_n, r.clut_n); continue; }
            if (texture_create_at((int)r.handle, r.w, r.h, r.fmt, pix, r.clut_fmt, cn ? clut : NULL) != r.handle)
                sa1_log("draw: cannot re-create texture %u (%dx%d fmt %d)", r.handle, r.w, r.h, r.fmt);
        }
        return;
    }
    for (int i = 1; i < TEX_MAX; i++) {
        if (!texs[i].w) continue;
        tex_rec r = { (uint32_t)i, texs[i].pix ? texs[i].pix_n : 0, texs[i].clut ? texs[i].clut_n : 0, texs[i].w, texs[i].h, texs[i].fmt, texs[i].clut_fmt };
        hle_state_blob(io, "tex", &r, sizeof r);
        uint32_t pn = r.pix_n, cn = r.clut_n;
        hle_state_var(io, "tex.pix", texs[i].pix, &pn);
        hle_state_var(io, "tex.clut", texs[i].clut, &cn);
    }
}

/* ---- window ------------------------------------------------------------------- */
int draw_init(int scale, int want_headless) {
    headless = 1;
    if (want_headless) return 0;
#ifndef SA1_NO_SDL
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        sa1_log("draw: SDL_Init failed: %s (running headless)", SDL_GetError());
        return -1;
    }
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    win_w = 480 * scale; win_h = 272 * scale;
    win = SDL_CreateWindow("sa1-psp (desktop)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           win_w, win_h, SDL_WINDOW_OPENGL | (draw_gl_hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!win) { sa1_log("draw: window: %s", SDL_GetError()); return -1; }
    glctx = SDL_GL_CreateContext(win);
    if (!glctx) { sa1_log("draw: GL context: %s", SDL_GetError()); return -1; }
    pace_on = getenv("SA1_NOVSYNC") == NULL;
    SDL_GL_SetSwapInterval(pace_on ? 1 : 0);   /* SA1_NOVSYNC=1: run unpaced, for timing */
    title_frame_on = getenv("SA1_NO_TITLE_FRAME") == NULL;
    sa1_log("draw: GL %s on %s, window %dx%d", (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_RENDERER), win_w, win_h);
    SDL_DisplayMode mode;
    if (SDL_GetWindowDisplayMode(win, &mode) == 0)
        sa1_log("draw: display %d Hz; %s", mode.refresh_rate, pace_on ? "guest frames paced to 60 Hz (vsync on against tearing)" : "unpaced (SA1_NOVSYNC)");
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) break; }
    sa1_log("draw: controller %s", pad ? SDL_GameControllerName(pad) : "none (keyboard: arrows, Z=A X=B A=X S=Y Enter=Start Q/E=L/R)");
    headless = 0;
    glViewport(0, 0, win_w, win_h);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    { static const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f}; glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, one); }   /* the "texture alpha = 1" source */
    glDisable(GL_LIGHTING);
    glShadeModel(GL_SMOOTH);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glHint(GL_FOG_HINT, GL_NICEST);
    /* Projection: x,y from DC pixels, depth 1 - 2/z (see the file comment);
     * one matrix per layer (draw.h, aspect), loaded per strip. */
    build_projs();
    glMatrixMode(GL_PROJECTION); glLoadMatrixf(proj[PROJ_3D]); cur_proj = PROJ_3D;
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    return 0;
#else
    (void)scale;
    return -1;
#endif
}

void draw_shutdown(void) {
    if (pool_mode > 0) pool_log("exit");
#ifndef SA1_NO_SDL
    if (headless) return;
    if (pad) SDL_GameControllerClose(pad);
    SDL_GL_DeleteContext(glctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
#endif
    headless = 1;
}

void draw_begin_frame(uint32_t clear_argb) {
    stats.draw_calls = stats.triangles = stats.strips = stats.state_changes = 0;
    in_frame = 1;
    if (pool_mode > 0) texpool_set_frame(++pool_frame);
    if (pool_mode == 2 && mirror_need && pool_frame - mirror_room_frame >= 60) {
        mirror_room_frame = pool_frame;
        if (texpool_make_room(mirror_need)) { mirror_epoch++; mirror_need = 0; }
    }
    cur_valid = 0;
    cur_proj = -1;                /* the clear below resets the scissor; the first strip sets it again */
#ifndef SA1_NO_SDL
    if (headless) return;
    glDepthMask(GL_TRUE);
    glClearColor(((clear_argb >> 16) & 255) / 255.0f, ((clear_argb >> 8) & 255) / 255.0f, (clear_argb & 255) / 255.0f, 1.0f);
    if (!aspect_wide) {           /* 4:3: black pillars, the game's colour and all drawing in the middle */
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(win_w / 8, 0, win_w * 3 / 4, win_h);
        glClearColor(((clear_argb >> 16) & 255) / 255.0f, ((clear_argb >> 8) & 255) / 255.0f, (clear_argb & 255) / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    } else {
        glDisable(GL_SCISSOR_TEST);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
#else
    (void)clear_argb;
#endif
}

void draw_end_frame(void) {
    in_frame = 0;
#ifndef SA1_NO_SDL
    if (headless) return;
    if (title_frame_on) {
        /* The guest frame in the title bar, so screen captures carry it;
         * every 10 rendered frames to keep the call cheap. */
        static unsigned title_tick;
        if (title_tick++ % 10 == 0) {
            char buf[64];
            snprintf(buf, sizeof buf, "sa1-psp (desktop)  f%lu", (unsigned long)hle_frame_count());
            SDL_SetWindowTitle(win, buf);
        }
    }
    SDL_GL_SwapWindow(win);
#endif
}

/* Pacing (desktop only, main.c calls it once per guest frame): guest frame
 * N is due at t0 + N / 60 s, whatever the display's refresh rate; vsync
 * only stops tearing.  Sleep to within 2 ms of the deadline, then spin.
 * If the run falls more than a quarter second behind (loading, a dragged
 * window) the clock is re-based instead of fast-forwarding. */
static uint64_t pace_t0;
static uint32_t pace_frame0;
void draw_gl_pace(uint32_t frame) {
#ifndef SA1_NO_SDL
    if (headless || !pace_on) return;
    uint64_t now = sa1_plat_time_us();
    if (!pace_t0) { pace_t0 = now; pace_frame0 = frame; return; }
    uint64_t due = pace_t0 + (uint64_t)(frame - pace_frame0) * 1000000ull / 60ull;
    if (now > due + 250000ull) { pace_t0 = now - (uint64_t)hle_frame_skip_hold() * 1000000ull / 60ull; pace_frame0 = frame; return; }
    if (due > now + 2500ull) SDL_Delay((Uint32)((due - now - 2000ull) / 1000ull));
    while (sa1_plat_time_us() < due) { }
#else
    (void)frame;
#endif
}
int sa1_plat_pace_late(uint32_t frame) {
#ifndef SA1_NO_SDL
    if (headless || !pace_on || !pace_t0) return 0;
    int64_t late = (int64_t)(sa1_plat_time_us() - pace_t0) * 60 / 1000000 - (int64_t)(frame - pace_frame0);
    return (int)late;
#else
    (void)frame;
    return 0;
#endif
}

void draw_set_fog(uint32_t color_argb, float z_near, float z_far) {
#ifndef SA1_NO_SDL
    if (headless) return;
    float col[4] = {((color_argb >> 16) & 255) / 255.0f, ((color_argb >> 8) & 255) / 255.0f, (color_argb & 255) / 255.0f, 1.0f};
    glFogfv(GL_FOG_COLOR, col);
    glFogf(GL_FOG_START, z_near + 1.0f);
    glFogf(GL_FOG_END, z_far + 1.0f);
#else
    (void)color_argb; (void)z_near; (void)z_far;
#endif
}

/* ---- state --------------------------------------------------------------------- */
#ifndef SA1_NO_SDL
static GLenum blend_factor(int code, int is_dst) {
    switch (code & 7) {
    case DRAW_BLEND_ZERO: return GL_ZERO;
    case DRAW_BLEND_ONE: return GL_ONE;
    case DRAW_BLEND_OTHER_COLOR: return is_dst ? GL_SRC_COLOR : GL_DST_COLOR;
    case DRAW_BLEND_INV_OTHER_COLOR: return is_dst ? GL_ONE_MINUS_SRC_COLOR : GL_ONE_MINUS_DST_COLOR;
    case DRAW_BLEND_SRC_ALPHA: return GL_SRC_ALPHA;
    case DRAW_BLEND_INV_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case DRAW_BLEND_DST_ALPHA: return GL_DST_ALPHA;
    default: return GL_ONE_MINUS_DST_ALPHA;
    }
}
/* PowerVR compares 1/w (greater = nearer); our depth is 1 - 2/z (less = nearer). */
static GLenum depth_func(int code) {
    switch (code & 7) {
    case DRAW_DEPTH_NEVER: return GL_NEVER;
    case DRAW_DEPTH_LESS: return GL_GREATER;
    case DRAW_DEPTH_EQUAL: return GL_EQUAL;
    case DRAW_DEPTH_LEQUAL: return GL_GEQUAL;
    case DRAW_DEPTH_GREATER: return GL_LESS;
    case DRAW_DEPTH_NOTEQUAL: return GL_NOTEQUAL;
    case DRAW_DEPTH_GEQUAL: return GL_LEQUAL;
    default: return GL_ALWAYS;
    }
}
#endif

void draw_set_state(const draw_state *s) {
    if (cur_valid && memcmp(s, &cur, sizeof cur) == 0) return;
    cur = *s; cur_valid = 1;
    stats.state_changes++;
    if (pool_mode > 0 && s->texture && s->texture < TEX_MAX && texs[s->texture].w) pool_use(s);
#ifndef SA1_NO_SDL
    if (headless) return;
    if (s->blend_src == DRAW_BLEND_ONE && s->blend_dst == DRAW_BLEND_ZERO) glDisable(GL_BLEND);
    else { glEnable(GL_BLEND); glBlendFunc(blend_factor(s->blend_src, 0), blend_factor(s->blend_dst, 1)); }
    glDepthFunc(depth_func(s->depth_func));
    /* The translucent list is auto-sorted on the console: the ISP orders its
     * polygons per pixel, so one never hides another by depth whatever its
     * ISP z-write bit says (Flycast draws the sorted pass with depth writes
     * off).  Writing them here cut the overlapping headlight pools in Emerald
     * Coast at each other's edge, a fifth dimmer at the centre. */
    glDepthMask(s->depth_write && !s->translucent ? GL_TRUE : GL_FALSE);
    if (s->cull == DRAW_CULL_NEGATIVE || s->cull == DRAW_CULL_POSITIVE) {
        glEnable(GL_CULL_FACE);
        /* Screen space has y down, so a positive PowerVR area is a clockwise
         * triangle for GL after the projection flips y. */
        glFrontFace(s->cull == DRAW_CULL_POSITIVE ? GL_CW : GL_CCW);
        glCullFace(GL_BACK);
    } else glDisable(GL_CULL_FACE);
    if (s->alpha_test) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GEQUAL, 0.5f); }
    else glDisable(GL_ALPHA_TEST);
    if (s->texture && s->texture < TEX_MAX && texs[s->texture].gl) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texs[s->texture].gl);
        if (pool_mode > 0) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, texs[s->texture].dropped);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s->clamp_u ? GL_CLAMP_TO_EDGE : s->flip_u ? GL_MIRRORED_REPEAT : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, s->clamp_v ? GL_CLAMP_TO_EDGE : s->flip_v ? GL_MIRRORED_REPEAT : GL_REPEAT);
        GLint f = s->filter == DRAW_FILTER_POINT ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s->filter == DRAW_FILTER_POINT ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        /* The PowerVR shading instruction (draw.h): rgb from texture and
         * vertex colour, alpha by its own rule.  A combiner, because the two
         * DECAL forms take one alpha and ignore_tex_alpha replaces the
         * texture alpha with 1 (the env colour) in every mode. */
        int tex_a = !s->ignore_tex_alpha;
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        switch (s->shade) {
        case DRAW_SHADE_DECAL:
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, tex_a ? GL_TEXTURE : GL_CONSTANT);
            break;
        case DRAW_SHADE_DECAL_ALPHA:
            if (tex_a) {           /* T * A + V * (1 - A) */
                glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
                glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
                glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
                glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_TEXTURE);
                glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA);
            } else {
                glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
                glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
            }
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
            break;
        default:                   /* MODULATE and MODULATE_ALPHA (draw.h: the same alpha rule) */
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, tex_a ? GL_MODULATE : GL_REPLACE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, tex_a ? GL_TEXTURE : GL_PRIMARY_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
            break;
        }
    } else glDisable(GL_TEXTURE_2D);
    /* Translucent strips sit on the surfaces they decorate (the headlight
     * spots on the pavement, ripples on the water) at offsets far below what
     * the 16-bit buffer resolves at that distance (section 11 of the M2
     * report); pull them nearer by two depth units and one slope so the
     * GEQUAL compare of the console's 32-bit 1/w comes out the same way. */
    if (s->translucent) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1.0f, -2.0f); }
    else glDisable(GL_POLYGON_OFFSET_FILL);
    if (s->fog == DRAW_FOG_TABLE || s->fog == DRAW_FOG_TABLE2) glEnable(GL_FOG); else glDisable(GL_FOG);
    glShadeModel(s->gouraud ? GL_SMOOTH : GL_FLAT);
#endif
}

#ifndef SA1_NO_SDL
static void select_proj(const draw_vertex *v, int n) {
    int p = PROJ_3D;
    if (cur.layer == DRAW_LAYER_2D) {
        float xmin = v[0].x, xmax = v[0].x;
        for (int i = 1; i < n; i++) { if (v[i].x < xmin) xmin = v[i].x; if (v[i].x > xmax) xmax = v[i].x; }
        p = (xmin <= 0.5f && xmax >= DC_W - 0.5f) ? PROJ_FULL : PROJ_2D;
    } else if (cur.layer == DRAW_LAYER_3D_43) p = PROJ_2D;
    if (p != cur_proj) {
        glMatrixMode(GL_PROJECTION); glLoadMatrixf(proj[p]); glMatrixMode(GL_MODELVIEW);
        /* The 2D layer's 4:3 area is the game's whole screen: what it draws
         * past x 0 or 640 (the title's water wobble) is cut at the area's
         * edges, as a console's screen would, not drawn into the sides (the
         * PSP backend does the same; 4:3 mode scissors everything already). */
        if (aspect_wide) {
            if (p == PROJ_2D) { glEnable(GL_SCISSOR_TEST); glScissor(win_w / 8, 0, win_w * 3 / 4, win_h); }
            else glDisable(GL_SCISSOR_TEST);
        }
        cur_proj = p;
    }
}
static void emit(const draw_vertex *v, int n, GLenum mode) {
    if (headless) return;
    select_proj(v, n);
    glBegin(mode);
    for (int i = 0; i < n; i++) {
        uint32_t c = v[i].color;
        unsigned a = cur.use_alpha ? (c >> 24) & 255 : 255;
        if (!cur.texture) {       /* offset colour has nowhere to go but the base when untextured */
            uint32_t o = v[i].ocolor;
            unsigned r = ((c >> 16) & 255) + ((o >> 16) & 255), g = ((c >> 8) & 255) + ((o >> 8) & 255), b = (c & 255) + (o & 255);
            glColor4ub(r > 255 ? 255 : (GLubyte)r, g > 255 ? 255 : (GLubyte)g, b > 255 ? 255 : (GLubyte)b, (GLubyte)a);
        } else glColor4ub((GLubyte)((c >> 16) & 255), (GLubyte)((c >> 8) & 255), (GLubyte)(c & 255), (GLubyte)a);
        float w = v[i].w > 1e-6f ? v[i].w : 1e-6f;
        float z = 1.0f / w;
        if (z < 1.0f) z = 1.0f;   /* 1/w above 1 (2D elements pushed in front): nearest depth, not clipped */
        glTexCoord2f(v[i].u, v[i].v);
        glVertex4f(v[i].x * z, v[i].y * z, -(z + 1.0f), z);
    }
    glEnd();
}
#endif

#ifndef SA1_NO_SDL
/* PowerVR adds the offset (specular) colour after texturing: colour = tex *
 * base + offset.  The fixed pipeline has no second colour stage, so a
 * textured strip with a non-zero offset colour gets a second, additive,
 * untextured pass at equal depth (the GE will do the same). */
static void emit_offset(const draw_vertex *v, int n, GLenum mode) {
    int any = 0;
    for (int i = 0; i < n && !any; i++) if (v[i].ocolor & 0x00FFFFFFu) any = 1;
    if (!any || headless) return;
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
    glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE);
    glDisable(GL_ALPHA_TEST);
    glBegin(mode);
    for (int i = 0; i < n; i++) {
        uint32_t o = v[i].ocolor;
        /* the PowerVR adds the offset before blending, so under source-alpha
         * blending a faded polygon fades its highlight too; an additive
         * strip (ONE/ONE, SRC_ALPHA/ONE) adds the offset in full */
        unsigned a = (cur.use_alpha && cur.blend_dst == DRAW_BLEND_INV_SRC_ALPHA) ? (v[i].color >> 24) & 255 : 255;
        glColor4ub((GLubyte)(((o >> 16) & 255) * a / 255), (GLubyte)(((o >> 8) & 255) * a / 255), (GLubyte)((o & 255) * a / 255), 255);
        float w = v[i].w > 1e-6f ? v[i].w : 1e-6f;
        float z = 1.0f / w;
        if (z < 1.0f) z = 1.0f;
        glVertex4f(v[i].x * z, v[i].y * z, -(z + 1.0f), z);
    }
    glEnd();
    stats.draw_calls++;
    cur_valid = 0;                 /* the next draw_set_state restores the state */
    draw_set_state(&cur);
}
#endif

void draw_strip(const draw_vertex *v, int n) {
    if (n < 3) return;
    stats.draw_calls++; stats.strips++; stats.triangles += (uint32_t)(n - 2);
#ifndef SA1_NO_SDL
    emit(v, n, GL_TRIANGLE_STRIP);
    if (cur.texture) emit_offset(v, n, GL_TRIANGLE_STRIP);
#else
    (void)v;
#endif
}

/* ---- meshes (draw.h draw_mesh) --------------------------------------------- */
#ifndef SA1_NO_SDL
static void mesh_vec(const float *m, const draw_mesh_vertex *p, float *h) {
    for (int r = 0; r < 4; r++) h[r] = m[r] * p->x + m[4 + r] * p->y + m[8 + r] * p->z + m[12 + r];
}
#endif

int draw_mesh_supported(void) { return 1; }
int draw_mesh_accepts(const float m[16], const float bound[4]) { (void)m; (void)bound; return 1; }   /* GL clips everything */
/* GL clips everything, but the test is the PSP's (draw.h draw_ge_geo, its
 * near plane and split), so the native renderer takes the same paths here:
 * its counts are the console's and the CPU path's pictures are checked. */
int draw_mesh_test(const float m[16], const float bound[4]) { return draw_ge_geo(m, bound, 1.0f, DRAW_GE_SPLIT); }

static draw_cell_stats cst;
/* The PSP's near pass GE draws (draw_gu.c draw_set_native: on with the
 * native renderer), mirrored so the paths and counts are the console's. */
static int native_on;
void draw_set_native(int on) { native_on = on; }
static int model_whole(const float *m, const float *bound) {
    if (!native_on) return DRAW_CELL_CPU;
    draw_cell_view cv;
    draw_cell_prep(&cv, m, DRAW_GE_SPLIT);
    cv.split = DRAW_GE_SPLIT; cv.ge_near = 1;
    const draw_model_cell c = { { bound[0], bound[1], bound[2], bound[3] }, 0.0f };
    return draw_cell_class(&cv, &c);
}
int draw_mesh_ge_whole(const float m[16], const float bound[4]) {
    const int w = model_whole(m, bound);
    return w == DRAW_CELL_NEAR || w == DRAW_CELL_BOTH;
}
/* The CPU path (draw.h draw_cpu_tri), as draw_gu.c's cpu_tris. */
static void cpu_tris(const float *m, const draw_mesh_vertex *v, int nv, const uint16_t *idx, int ni) {
    draw_vertex t[4];
    for (int i = 0; i + 2 < ni; i += 3) {
        if (idx[i] >= nv || idx[i + 1] >= nv || idx[i + 2] >= nv) continue;
        int k = draw_cpu_tri(m, &v[idx[i]], &v[idx[i + 1]], &v[idx[i + 2]], t);
        if (k) draw_strip(t, k);
    }
}

static void mesh_emit(const float m[16], const draw_mesh_vertex *v, int nv, const uint16_t *idx, int ni);
void draw_mesh(const float m[16], const float bound[4], const draw_mesh_vertex *v, int nv, const uint16_t *idx, int ni) {
    if (ni < 3) return;
    if (cur.layer == DRAW_LAYER_3D && draw_ge_geo(m, bound, 1.0f, DRAW_GE_SPLIT) < 0) {   /* as the PSP: refused whole, the CPU path */
        cst.mesh_cpu_tris += (uint32_t)(ni / 3);
        cpu_tris(m, v, nv, idx, ni);
        return;
    }
    mesh_emit(m, v, nv, idx, ni);
}

static void mesh_emit(const float m[16], const draw_mesh_vertex *v, int nv, const uint16_t *idx, int ni) {
    if (ni < 3) return;
    stats.draw_calls++; stats.triangles += (uint32_t)(ni / 3);
#ifndef SA1_NO_SDL
    if (headless) return;
    if (cur_proj != PROJ_3D) { glMatrixMode(GL_PROJECTION); glLoadMatrixf(proj[PROJ_3D]); glMatrixMode(GL_MODELVIEW); cur_proj = PROJ_3D; }
    int any_off = 0;
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < ni; i++) {
        if (idx[i] >= nv) continue;
        const draw_mesh_vertex *p = &v[idx[i]];
        uint32_t c = p->color;
        unsigned a = cur.use_alpha ? (c >> 24) & 255 : 255;
        if (!cur.texture) {
            uint32_t o = p->ocolor;
            unsigned r = ((c >> 16) & 255) + ((o >> 16) & 255), g = ((c >> 8) & 255) + ((o >> 8) & 255), b = (c & 255) + (o & 255);
            glColor4ub(r > 255 ? 255 : (GLubyte)r, g > 255 ? 255 : (GLubyte)g, b > 255 ? 255 : (GLubyte)b, (GLubyte)a);
        } else {
            glColor4ub((GLubyte)((c >> 16) & 255), (GLubyte)((c >> 8) & 255), (GLubyte)(c & 255), (GLubyte)a);
            if (p->ocolor & 0x00FFFFFFu) any_off = 1;
        }
        float h[4]; mesh_vec(m, p, h);
        glTexCoord2f(p->u, p->v);
        glVertex4f(h[0], h[1], h[2], h[3]);
    }
    glEnd();
    if (!any_off) return;
    /* the offset colour, added as emit_offset does for strips */
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
    glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE);
    glDisable(GL_ALPHA_TEST);
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < ni; i++) {
        if (idx[i] >= nv) continue;
        const draw_mesh_vertex *p = &v[idx[i]];
        uint32_t o = p->ocolor;
        unsigned a = (cur.use_alpha && cur.blend_dst == DRAW_BLEND_INV_SRC_ALPHA) ? (p->color >> 24) & 255 : 255;
        glColor4ub((GLubyte)(((o >> 16) & 255) * a / 255), (GLubyte)(((o >> 8) & 255) * a / 255), (GLubyte)((o & 255) * a / 255), 255);
        float h[4]; mesh_vec(m, p, h);
        glVertex4f(h[0], h[1], h[2], h[3]);
    }
    glEnd();
    stats.draw_calls++;
    cur_valid = 0;
    draw_set_state(&cur);
#else
    (void)m; (void)v; (void)nv; (void)idx;
#endif
}

/* Persistent models: copies of what the caller gave, drawn set by set with
 * draw_mesh (the desktop is the reference for pictures, not for speed). */
typedef struct { draw_mesh_vertex *v; uint16_t *idx; draw_model_set *sets; int nv, ni, nsets; float bound[4];
                 draw_model_cell *cells; draw_model_span *spans; int ncells, nspans; } dmodel;
#define DMODEL_MAX 4096
static dmodel dmodels[DMODEL_MAX];

uint32_t draw_model_create(const draw_mesh_vertex *v, int nv, const uint16_t *idx, int ni,
                           const draw_model_set *sets, int nsets, const float bound[4],
                           const draw_model_cell *cells, int ncells, const draw_model_span *spans, int nspans) {
    for (uint32_t h = 1; h < DMODEL_MAX; h++) {
        dmodel *d = &dmodels[h];
        if (d->sets) continue;
        d->v = malloc((size_t)nv * sizeof *v); d->idx = malloc((size_t)ni * 2); d->sets = malloc((size_t)nsets * sizeof *sets);
        if (!d->v || !d->idx || !d->sets) { free(d->v); free(d->idx); free(d->sets); memset(d, 0, sizeof *d); return 0; }
        memcpy(d->v, v, (size_t)nv * sizeof *v); memcpy(d->idx, idx, (size_t)ni * 2); memcpy(d->sets, sets, (size_t)nsets * sizeof *sets);
        d->nv = nv; d->ni = ni; d->nsets = nsets;
        memcpy(d->bound, bound, sizeof d->bound);
        if (cells && spans && ncells > 0 && nspans > 0) {
            int ok = 1;             /* spans in set order, inside their set's indices (as draw_gu.c) */
            for (int k = 0; k < nspans && ok; k++) {
                const draw_model_span *q = &spans[k];
                ok = q->set < nsets && q->cell < ncells && (k == 0 || q->set >= spans[k - 1].set) &&
                     q->ifirst >= sets[q->set].ifirst && q->ifirst + q->icount <= sets[q->set].ifirst + sets[q->set].icount && q->icount % 3 == 0;
            }
            d->cells = ok ? malloc((size_t)ncells * sizeof *cells) : NULL;
            d->spans = ok ? malloc((size_t)nspans * sizeof *spans) : NULL;
            if (d->cells && d->spans) {
                memcpy(d->cells, cells, (size_t)ncells * sizeof *cells);
                memcpy(d->spans, spans, (size_t)nspans * sizeof *spans);
                d->ncells = ncells; d->nspans = nspans;
            } else { free(d->cells); free(d->spans); d->cells = NULL; d->spans = NULL; }
        }
        return h;
    }
    return 0;
}

/* draw.h: a copy of base with other states (the desktop copies; the rules
 * checked are the PSP's, so both refuse the same variants). */
uint32_t draw_model_variant(uint32_t base, const draw_state *states, int nstates) {
    if (base == 0 || base >= DMODEL_MAX || !dmodels[base].sets || nstates != dmodels[base].nsets) return 0;
    const dmodel *b = &dmodels[base];
    for (int s = 0; s < nstates; s++) {
        const draw_state *x = &b->sets[s].state, *y = &states[s];
        if (!x->texture != !y->texture || x->use_alpha != y->use_alpha || x->shade != y->shade || x->ignore_tex_alpha != y->ignore_tex_alpha ||
            x->blend_dst != y->blend_dst) return 0;
    }
    draw_model_set *sets = malloc((size_t)nstates * sizeof *sets);
    if (!sets) return 0;
    memcpy(sets, b->sets, (size_t)nstates * sizeof *sets);
    for (int s = 0; s < nstates; s++) sets[s].state = states[s];
    uint32_t h = draw_model_create(b->v, b->nv, b->idx, b->ni, sets, nstates, b->bound, b->cells, b->ncells, b->spans, b->nspans);
    free(sets);
    return h;
}

void draw_model_destroy(uint32_t h) {
    if (h == 0 || h >= DMODEL_MAX || !dmodels[h].sets) return;
    free(dmodels[h].v); free(dmodels[h].idx); free(dmodels[h].sets); free(dmodels[h].cells); free(dmodels[h].spans);
    memset(&dmodels[h], 0, sizeof dmodels[h]);
}

/* A model the PSP's GE would refuse whole, cell by cell as draw_gu.c's
 * model_cells: cells out of view skipped, the GE's cells drawn from the copy,
 * the others through the CPU path. */
static uint8_t *cell_cls;
static int cell_cls_cap;
static int model_cells(const dmodel *d, const float *m) {
    if (d->ncells > cell_cls_cap) {
        uint8_t *c = realloc(cell_cls, (size_t)d->ncells);
        if (!c) return 0;
        cell_cls = c; cell_cls_cap = d->ncells;
    }
    draw_cell_view cv;
    draw_cell_prep(&cv, m, DRAW_GE_SPLIT);
    if (native_on) { cv.split = DRAW_GE_SPLIT; cv.ge_near = 1; }
    for (int c = 0; c < d->ncells; c++) { int k = draw_cell_class(&cv, &d->cells[c]); cell_cls[c] = (uint8_t)k; cst.cells[k]++; }
    cst.models++;
    const draw_state keep = cur;
    int k = 0;
    for (int s = 0; s < d->nsets; s++) {
        const draw_model_set *ms = &d->sets[s];
        const draw_mesh_vertex *v = d->v + ms->vfirst;
        int set_on = 0;
        for (; k < d->nspans && d->spans[k].set == s; k++) {
            const draw_model_span *q = &d->spans[k];
            const int cls = cell_cls[q->cell];
            cst.tris[cls] += q->icount / 3u;
            if (cls == DRAW_CELL_OUT) continue;
            if (!set_on) { draw_set_state(&ms->state); set_on = 1; }
            if (cls != DRAW_CELL_CPU) mesh_emit(m, v, (int)ms->vcount, d->idx + q->ifirst, (int)q->icount);
            else cpu_tris(m, v, (int)ms->vcount, d->idx + q->ifirst, (int)q->icount);
        }
    }
    draw_set_state(&keep);
    return 1;
}

int draw_model(uint32_t h, const float m[16]) {
    if (h == 0 || h >= DMODEL_MAX || !dmodels[h].sets) return 0;
    const dmodel *d = &dmodels[h];
    if (d->ncells && draw_ge_geo(m, d->bound, 1.0f, DRAW_GE_SPLIT) < 0) {
        const int w = model_whole(m, d->bound);
        if (w == DRAW_CELL_NEAR) cst.whole[0]++;
        else if (w == DRAW_CELL_BOTH) cst.whole[1]++;
        else return model_cells(d, m);
    }
    const draw_state keep = cur;
    for (int s = 0; s < d->nsets; s++) {
        const draw_model_set *ms = &d->sets[s];
        draw_set_state(&ms->state);
        mesh_emit(m, d->v + ms->vfirst, (int)ms->vcount, d->idx + ms->ifirst, (int)ms->icount);
    }
    draw_set_state(&keep);
    return 1;
}

void draw_cell_stats_take(draw_cell_stats *out) {
    *out = cst;
    memset(&cst, 0, sizeof cst);
}

void draw_line_strip(const draw_vertex *v, int n) {
    if (n < 2) return;
    stats.draw_calls++;
#ifndef SA1_NO_SDL
    emit(v, n, GL_LINE_STRIP);
#else
    (void)v;
#endif
}

/* ---- screenshot (binary PPM) ---------------------------------------------------- */
int draw_screenshot(const char *path) {
#ifndef SA1_NO_SDL
    if (headless) return -1;
    unsigned char *buf = malloc((size_t)win_w * win_h * 3);
    if (!buf) return -1;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, win_w, win_h, GL_RGB, GL_UNSIGNED_BYTE, buf);
    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", win_w, win_h);
    for (int y = win_h - 1; y >= 0; y--) fwrite(buf + (size_t)y * win_w * 3, 1, (size_t)win_w * 3, f);
    fclose(f); free(buf);
    return 0;
#else
    (void)path;
    return -1;
#endif
}

/* ---- input --------------------------------------------------------------------- */
static int mark_pressed, quick_key;
/* The mark key (F9) since the last call (desktop only; main.c). */
int draw_gl_mark_pressed(void) { int m = mark_pressed; mark_pressed = 0; return m; }
/* The quick save / load keys since the last call: 5 (F5), 8 (F8), or 0. */
int draw_gl_quick_key(void) { int k = quick_key; quick_key = 0; return k; }

int draw_poll_input(struct hle_pad *p, int *quit) {
    *quit = 0;
#ifndef SA1_NO_SDL
    if (headless) return -1;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) *quit = 1;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) *quit = 1;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F9 && !e.key.repeat) mark_pressed = 1;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F5 && !e.key.repeat) quick_key = 5;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F8 && !e.key.repeat) quick_key = 8;
        if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
    }
    uint32_t b = 0; int x = 0, y = 0, lt = 0, rt = 0;
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    if (k[SDL_SCANCODE_Z]) b |= 4;        /* A */
    if (k[SDL_SCANCODE_X]) b |= 2;        /* B */
    if (k[SDL_SCANCODE_A]) b |= 0x400;    /* X */
    if (k[SDL_SCANCODE_S]) b |= 0x200;    /* Y */
    if (k[SDL_SCANCODE_RETURN]) b |= 8;   /* Start */
    if (k[SDL_SCANCODE_I]) b |= 0x10;
    if (k[SDL_SCANCODE_K]) b |= 0x20;
    if (k[SDL_SCANCODE_J]) b |= 0x40;
    if (k[SDL_SCANCODE_L]) b |= 0x80;
    if (k[SDL_SCANCODE_LEFT]) x = -127;
    if (k[SDL_SCANCODE_RIGHT]) x = 127;
    if (k[SDL_SCANCODE_UP]) y = -127;
    if (k[SDL_SCANCODE_DOWN]) y = 127;
    if (k[SDL_SCANCODE_Q]) lt = 255;
    if (k[SDL_SCANCODE_E]) rt = 255;
    if (pad) {
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) b |= 4;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) b |= 2;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) b |= 0x400;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) b |= 0x200;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) b |= 8;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= 0x10;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= 0x20;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= 0x40;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= 0x80;
        int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / 258;
        int ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / 258;
        if (ax > 8 || ax < -8) x = ax;
        if (ay > 8 || ay < -8) y = ay;
        int l = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 128;
        int r = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 128;
        if (l > lt) lt = l;
        if (r > rt) rt = r;
    }
    p->buttons = b; p->x = (int8_t)x; p->y = (int8_t)y; p->ltrig = (uint8_t)lt; p->rtrig = (uint8_t)rt;
    return 0;
#else
    (void)p;
    return -1;
#endif
}
