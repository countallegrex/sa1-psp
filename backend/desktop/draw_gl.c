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
static SDL_Window *win;
static SDL_GLContext glctx;
static SDL_GameController *pad;
#endif

#define DC_W 640.0f
#define DC_H 480.0f

static int headless = 1, win_w = 480, win_h = 272;
static draw_stats stats;
static draw_state cur;
static int cur_valid;

const draw_stats *draw_get_stats(void) { return &stats; }

/* ---- textures ---------------------------------------------------------------- */
#define TEX_MAX 2048
typedef struct { uint32_t gl; int w, h; uint32_t bytes; } tex_slot;
static tex_slot texs[TEX_MAX];

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

uint32_t draw_texture_create(int width, int height, int fmt, const void *pixels, int clut_fmt, const void *clut) {
    int slot;
    for (slot = 1; slot < TEX_MAX; slot++) if (texs[slot].w == 0) break;
    if (slot >= TEX_MAX) return 0;
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
    texs[slot].w = width; texs[slot].h = height; texs[slot].bytes = bytes; texs[slot].gl = 0;
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
        glTexImage2D(GL_TEXTURE_2D, 0, ifmt, width, height, 0, gfmt, gtype, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
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

void draw_texture_destroy(uint32_t handle) {
    if (handle == 0 || handle >= TEX_MAX || texs[handle].w == 0) return;
#ifndef SA1_NO_SDL
    if (texs[handle].gl) { GLuint id = texs[handle].gl; glDeleteTextures(1, &id); }
#endif
    stats.textures_alive--; stats.texture_bytes -= texs[handle].bytes;
    memset(&texs[handle], 0, sizeof texs[handle]);
    cur_valid = 0;
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
                           win_w, win_h, SDL_WINDOW_OPENGL);
    if (!win) { sa1_log("draw: window: %s", SDL_GetError()); return -1; }
    glctx = SDL_GL_CreateContext(win);
    if (!glctx) { sa1_log("draw: GL context: %s", SDL_GetError()); return -1; }
    SDL_GL_SetSwapInterval(1);
    sa1_log("draw: GL %s on %s, window %dx%d", (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_RENDERER), win_w, win_h);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) break; }
    sa1_log("draw: controller %s", pad ? SDL_GameControllerName(pad) : "none (keyboard: arrows, Z=A X=B A=X S=Y Enter=Start Q/E=L/R)");
    headless = 0;
    glViewport(0, 0, win_w, win_h);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_LIGHTING);
    glShadeModel(GL_SMOOTH);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glHint(GL_FOG_HINT, GL_NICEST);
    /* Projection: x,y from DC pixels, depth 1 - 2/z (see the file comment). */
    float P[16] = {0};
    P[0] = 2.0f / DC_W;  P[12] = -1.0f;
    P[5] = -2.0f / DC_H; P[13] = 1.0f;
    P[10] = 2.0f;        P[14] = 3.0f;
    P[15] = 1.0f;
    glMatrixMode(GL_PROJECTION); glLoadMatrixf(P);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    return 0;
#else
    (void)scale;
    return -1;
#endif
}

void draw_shutdown(void) {
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
    cur_valid = 0;
#ifndef SA1_NO_SDL
    if (headless) return;
    glDepthMask(GL_TRUE);
    glClearColor(((clear_argb >> 16) & 255) / 255.0f, ((clear_argb >> 8) & 255) / 255.0f, (clear_argb & 255) / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#else
    (void)clear_argb;
#endif
}

void draw_end_frame(void) {
#ifndef SA1_NO_SDL
    if (headless) return;
    SDL_GL_SwapWindow(win);
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
#ifndef SA1_NO_SDL
    if (headless) return;
    if (s->blend_src == DRAW_BLEND_ONE && s->blend_dst == DRAW_BLEND_ZERO) glDisable(GL_BLEND);
    else { glEnable(GL_BLEND); glBlendFunc(blend_factor(s->blend_src, 0), blend_factor(s->blend_dst, 1)); }
    glDepthFunc(depth_func(s->depth_func));
    glDepthMask(s->depth_write ? GL_TRUE : GL_FALSE);
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
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s->clamp_u ? GL_CLAMP_TO_EDGE : s->flip_u ? GL_MIRRORED_REPEAT : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, s->clamp_v ? GL_CLAMP_TO_EDGE : s->flip_v ? GL_MIRRORED_REPEAT : GL_REPEAT);
        GLint f = s->filter == DRAW_FILTER_POINT ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        switch (s->shade) {
        case DRAW_SHADE_DECAL: glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE); break;
        case DRAW_SHADE_DECAL_ALPHA: glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL); break;
        default: glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); break;
        }
    } else glDisable(GL_TEXTURE_2D);
    if (s->fog == DRAW_FOG_TABLE || s->fog == DRAW_FOG_TABLE2) glEnable(GL_FOG); else glDisable(GL_FOG);
    glShadeModel(s->gouraud ? GL_SMOOTH : GL_FLAT);
#endif
}

#ifndef SA1_NO_SDL
static void emit(const draw_vertex *v, int n, GLenum mode) {
    if (headless) return;
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
        glTexCoord2f(v[i].u, v[i].v);
        glVertex4f(v[i].x * z, v[i].y * z, -(z + 1.0f), z);
    }
    glEnd();
}
#endif

void draw_strip(const draw_vertex *v, int n) {
    if (n < 3) return;
    stats.draw_calls++; stats.strips++; stats.triangles += (uint32_t)(n - 2);
#ifndef SA1_NO_SDL
    emit(v, n, GL_TRIANGLE_STRIP);
#else
    (void)v;
#endif
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
int draw_poll_input(struct hle_pad *p, int *quit) {
    *quit = 0;
#ifndef SA1_NO_SDL
    if (headless) return -1;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) *quit = 1;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) *quit = 1;
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
