/* draw.h - the draw interface hle/ninja targets.  One implementation per
 * backend: backend/desktop/draw_gl.c (SDL2 + OpenGL 1.x fixed function) and,
 * from M4, backend/psp/draw_gu.c (sceGu).  Everything here is expressible on
 * the PSP's GE: no shaders, power-of-two textures only, 16-bit texture
 * formats and CLUTs, screen-space vertices with a 1/w, a Z buffer, one
 * blend equation, alpha test, linear fog.
 *
 * Coordinates: vertices arrive already projected by the game (the
 * Dreamcast's 640x480 screen, y down) with w = 1/z as the PowerVR takes
 * them (larger = nearer).  The backend maps 640x480 to its own frame
 * buffer and derives depth from w; texturing is perspective correct.
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Texture pixel formats, in the PSP's bit order (R in the low bits): */
enum {
    DRAW_FMT_5650 = 0,   /* RGB565 */
    DRAW_FMT_5551 = 1,   /* ABGR1555: R bits 4-0, G 9-5, B 14-10, A 15 */
    DRAW_FMT_4444 = 2,   /* ABGR4444 */
    DRAW_FMT_8888 = 3,   /* ABGR8888 (the fog/debug paths only; avoid for game data) */
    DRAW_FMT_T4 = 4,     /* 4-bit CLUT index, 16 x 16-bit entries follow */
    DRAW_FMT_T8 = 5,     /* 8-bit CLUT index, 256 x 16-bit entries */
    DRAW_FMT_DXT1 = 8,   /* optional; the desktop backend may decline */
};

/* PowerVR TSP codes, kept as the game sets them (the backend maps them). */
enum {
    DRAW_BLEND_ZERO = 0, DRAW_BLEND_ONE, DRAW_BLEND_OTHER_COLOR, DRAW_BLEND_INV_OTHER_COLOR,
    DRAW_BLEND_SRC_ALPHA, DRAW_BLEND_INV_SRC_ALPHA, DRAW_BLEND_DST_ALPHA, DRAW_BLEND_INV_DST_ALPHA
};
enum { DRAW_DEPTH_NEVER = 0, DRAW_DEPTH_LESS, DRAW_DEPTH_EQUAL, DRAW_DEPTH_LEQUAL,
       DRAW_DEPTH_GREATER, DRAW_DEPTH_NOTEQUAL, DRAW_DEPTH_GEQUAL, DRAW_DEPTH_ALWAYS };
/* The compare is on 1/w, as on the PowerVR: GREATER means "nearer wins". */
enum { DRAW_CULL_NONE = 0, DRAW_CULL_SMALL, DRAW_CULL_NEGATIVE, DRAW_CULL_POSITIVE };
enum { DRAW_SHADE_DECAL = 0, DRAW_SHADE_MODULATE, DRAW_SHADE_DECAL_ALPHA, DRAW_SHADE_MODULATE_ALPHA };
enum { DRAW_FILTER_POINT = 0, DRAW_FILTER_BILINEAR, DRAW_FILTER_TRILINEAR_A, DRAW_FILTER_TRILINEAR_B };
enum { DRAW_FOG_TABLE = 0, DRAW_FOG_VERTEX, DRAW_FOG_NONE, DRAW_FOG_TABLE2 };

typedef struct draw_vertex {
    float x, y;          /* screen pixels, 0..640 / 0..480 */
    float w;             /* 1/z, > 0 */
    float u, v;          /* texture coordinates, 0..1 (wrap outside) */
    uint32_t color;      /* ARGB8888 base colour */
    uint32_t ocolor;     /* ARGB8888 offset (specular) colour; backends may ignore */
} draw_vertex;

typedef struct draw_state {
    uint32_t texture;        /* handle from draw_texture_create, 0 = none */
    uint8_t blend_src, blend_dst;
    uint8_t depth_func, depth_write;
    uint8_t cull;
    uint8_t alpha_test;      /* punch-through: discard alpha < 0.5 */
    uint8_t use_alpha;       /* vertex alpha used (else forced opaque) */
    uint8_t ignore_tex_alpha;
    uint8_t clamp_u, clamp_v, flip_u, flip_v;
    uint8_t filter;
    uint8_t shade;           /* DRAW_SHADE_* */
    uint8_t fog;             /* DRAW_FOG_* */
    uint8_t gouraud;
    uint8_t layer;           /* DRAW_LAYER_*: where the 640x480 coordinates land (aspect) */
    uint8_t translucent;     /* from the translucent list: drawn after the opaque and punch-through
                              * passes, sorted, and pulled nearer by a small depth offset so a decal
                              * on a surface (the headlight spots) wins the 16-bit depth compare */
} draw_state;

/* PowerVR shading instructions (TSP bits 7-6).  The texture colour T, the
 * vertex colour V and the offset colour O give, with A the texture alpha
 * (1 when ignore_tex_alpha) and a the vertex alpha (1 unless use_alpha):
 *   DECAL          rgb = T,                  alpha = A
 *   MODULATE       rgb = T * V,              alpha = A * a
 *   DECAL_ALPHA    rgb = T * A + V * (1-A),  alpha = a
 *   MODULATE_ALPHA rgb = T * V,              alpha = A * a
 * plus O on the rgb in every mode.  The hardware manual gives MODULATE
 * alpha = A alone; the game contradicts it (M2 report section 11: the sun's
 * lens-flare sprites are MODULATE with face alpha 0 while the sun is out of
 * view, the police beacons pulse their face alpha), so the vertex alpha
 * counts in both modulate forms.  The two DECAL forms do not multiply. */

/* Aspect.  The game projects for a 4:3 640x480 screen; the frame is 16:9.
 *   DRAW_LAYER_3D: hor+ in wide mode (the vertical field of view is kept and
 *     the frame shows 853x480 of the game's screen space, x from -106.67 to
 *     746.67, which the HLE's clip rectangle lets the game draw); in 4:3
 *     mode the 640 columns sit pillarboxed in the middle.
 *   DRAW_LAYER_2D: HUD, menus and sprites placed by 640x480 coordinates;
 *     always the centred 4:3 area, except that a strip spanning the full
 *     640 width (fades, backdrops) covers the whole frame.
 *   DRAW_LAYER_3D_43: 3D framed for a 4:3 screen (the title and menus: the
 *     title's water ripple is a 3D mesh whose edges a console's screen
 *     cut): the 4:3 area's projection, and nothing drawn outside it. */
enum { DRAW_LAYER_3D = 0, DRAW_LAYER_2D = 1, DRAW_LAYER_3D_43 = 2 };
#define DRAW_WIDE_LEFT  (-(480.0f * 16.0f / 9.0f - 640.0f) * 0.5f)   /* -106.67 */
#define DRAW_WIDE_RIGHT (640.0f - DRAW_WIDE_LEFT)               /* 746.67 */
void draw_set_aspect(int wide);              /* 1 = hor+ widescreen (default), 0 = 4:3 pillarbox */

/* Window / frame */
int  draw_init(int scale, int headless);     /* 480x272 window at integer scale; headless = no window at all */
void draw_shutdown(void);
void draw_begin_frame(uint32_t clear_argb);
void draw_end_frame(void);                   /* present; paces to vsync when a window exists */
/* Global fog: linear in z (= 1/w) between near and far, this colour. */
void draw_set_fog(uint32_t color_argb, float z_near, float z_far);

/* State and primitives.  Strips are triangle strips in draw order. */
void draw_set_state(const draw_state *s);
void draw_strip(const draw_vertex *v, int n);
void draw_line_strip(const draw_vertex *v, int n);

/* Textures.  width/height are powers of two from 8 to 1024.  pixels holds
 * width*height texels in the given format, the CLUT (for T4/T8) follows
 * as 16-bit entries in clut_fmt.  Returns 0 on failure. */
/* Indexed triangles with the current state, transformed by the backend
 * (docs/native-renderer.md): m (column-major) takes a vertex to the
 * homogeneous vector the strip path feeds the rasteriser, (x z, y z,
 * -(z + 1), z) for screen x, y in DC pixels and depth z, so projection,
 * depth and fog are the strip path's and the hardware clips at the near
 * plane.  draw_mesh_supported() is 0 where a backend has no such path yet. */
typedef struct draw_mesh_vertex { float x, y, z, u, v; uint32_t color, ocolor; } draw_mesh_vertex;
int draw_mesh_supported(void);
void draw_mesh(const float m[16], const draw_mesh_vertex *v, int nv, const uint16_t *idx, int ni);

uint32_t draw_texture_create(int width, int height, int fmt, const void *pixels, int clut_fmt, const void *clut);
void draw_texture_destroy(uint32_t handle);
/* Debug: dump the current frame buffer as a PPM (desktop only; PSP returns -1). */
int  draw_screenshot(const char *path);

/* Statistics for the milestone reports, reset by draw_begin_frame. */
typedef struct draw_stats { uint32_t draw_calls, triangles, strips, state_changes, textures_alive, texture_bytes; } draw_stats;
const draw_stats *draw_get_stats(void);

/* Input (desktop: SDL controller/keyboard; PSP: sceCtrl).  Fills the
 * Dreamcast pad record (hle.h layout).  Returns 0 when a real controller
 * or the keyboard produced the state, -1 when nothing is attached. */
struct hle_pad;
int  draw_poll_input(struct hle_pad *pad, int *quit);
