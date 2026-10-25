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
 * plane.  bound is a model-space sphere (x, y, z, radius) holding every
 * vertex: draw_mesh_accepts(m, bound) says whether this backend draws such a
 * mesh in hardware (the caller decides before it transforms anything, and
 * draws it the strip way otherwise); draw_mesh draws a mesh the GE refuses
 * (near plane, guard band, split) through the CPU path (draw_cpu_tri).
 * draw_mesh_supported() is 0 where a backend has no such path yet. */
typedef struct draw_mesh_vertex { float x, y, z, u, v; uint32_t color, ocolor; } draw_mesh_vertex;
int draw_mesh_supported(void);
int draw_mesh_accepts(const float m[16], const float bound[4]);
/* The same test without counting: the pass (>= 0) or why not, DRAW_MESH_NO_*. */
enum { DRAW_MESH_NO_OTHER = -1, DRAW_MESH_NO_NEAR = -2, DRAW_MESH_NO_GUARD = -3, DRAW_MESH_NO_SPLIT = -4,
       DRAW_MESH_OUT = -5 /* wholly outside the view: nothing to draw */ };
int draw_mesh_test(const float m[16], const float bound[4]);
void draw_mesh(const float m[16], const float bound[4], const draw_mesh_vertex *v, int nv, const uint16_t *idx, int ni);
/* Persistent models (docs/native-renderer.md s.6): the backend takes a
 * model's vertices once, per mesh set with that set's state (colours by the
 * same rules as draw_mesh), and keeps them in its own memory with whatever
 * it builds from them (on the PSP a display list called once per draw).
 * draw_model draws one with matrix m as draw_mesh would, its sets' own
 * states (the current state is not used and stays); 0 when it could not
 * (the caller then draws it another way).  Handles are > 0. */
/* Cells (docs/native-renderer.md s.6.2): a persistent model's triangles
 * binned in model space (tools/land_convert.py), each cell one index range
 * per set (a span: ifirst into the model's indices, of sets[set]), spans in
 * set order and, within a set, contiguous.  A model the GE cannot take whole
 * (draw_mesh_test < 0) is then drawn cell by cell: a cell wholly out of view
 * is skipped, one the GE takes safely (in view, every vertex at z >=
 * max(split, 1 + 2 * its longest edge), inside the guard band) goes from
 * the persistent copy, and the rest through the CPU path (draw_cpu_tri:
 * the strip path's picture, near-clipped at z = 1 as the game's clipper
 * 0x8C037EE0 does, then the strip clipper), so no visible triangle that
 * needs clipping reaches the GE.  Without cells (ncells 0) a model draws
 * only where the GE takes it whole. */
typedef struct draw_model_cell { float bound[4]; float edge; } draw_model_cell;
typedef struct draw_model_span { uint32_t ifirst, icount; uint16_t cell, set; } draw_model_span;
typedef struct draw_model_set { draw_state state; uint32_t vfirst, vcount, ifirst, icount; } draw_model_set;
uint32_t draw_model_create(const draw_mesh_vertex *v, int nv, const uint16_t *idx, int ni,
                           const draw_model_set *sets, int nsets, const float bound[4],
                           const draw_model_cell *cells, int ncells, const draw_model_span *spans, int nspans);
/* A model drawing base's vertices, indices and cells with other set states
 * (an animated texture's next frame: the texture differs, the colour rules
 * do not); 0 when the rules differ or it cannot (then draw_model_create).
 * It stays valid if base is destroyed first. */
uint32_t draw_model_variant(uint32_t base, const draw_state *states, int nstates);
void draw_model_destroy(uint32_t handle);
int draw_model(uint32_t handle, const float m[16]);

/* Per-cell counters since the last call (cells and triangles: out of view,
 * on the GE, through the CPU path; models drawn by cells; triangles drawn
 * the CPU way by draw_mesh), then reset. */
typedef struct draw_cell_stats { uint32_t models, cells[5], tris[5], mesh_cpu_tris, whole[2]; } draw_cell_stats;   /* by DRAW_CELL_*; whole: models on the GE whole in the near pass, in both */
void draw_cell_stats_take(draw_cell_stats *out);
/* The native land renderer is on (native_land.c, at its start): a backend
 * may then draw refused models on the GE in more cases (draw_gu.c's near
 * pass depth floor).  draw_mesh_ge_whole: such a model, refused by
 * draw_mesh_test, still goes to the GE whole (draw_model takes it). */
void draw_set_native(int on);
int draw_mesh_ge_whole(const float m[16], const float bound[4]);

/* The GE's limits as the PSP backend has them (draw_gu.c), shared so the
 * desktop can apply the same rules for its counts and pictures.  The guard
 * band is in the game's 640x480 screen space. */
#define DRAW_GE_GUARD_X0 (-2000.0f)
#define DRAW_GE_GUARD_X1 2640.0f
#define DRAW_GE_GUARD_Y0 (-2000.0f)
#define DRAW_GE_GUARD_Y1 2480.0f
#define DRAW_GE_SPLIT 16.0f          /* the 16-bit depth passes' split (draw_gu.c depth_split) */
enum { DRAW_PASS_FAR = 0, DRAW_PASS_NEAR = 1 };

/* draw_mesh_test's rule (the PSP's mesh_geo): the pass a mesh with matrix m
 * and sphere bound draws in, or DRAW_MESH_NO_*.  zn the near limit, split 0
 * for no split. */
static inline int draw_ge_geo(const float *m, const float *bound, float zn, float split) {
    float mn[4], mx[4];
    for (int r = 0; r < 4; r++) {
        float b = m[r] * bound[0] + m[4 + r] * bound[1] + m[8 + r] * bound[2] + m[12 + r];
        float q = m[r] * m[r] + m[4 + r] * m[4 + r] + m[8 + r] * m[8 + r];
        float e = bound[3] * __builtin_sqrtf(q);
        mn[r] = b - e; mx[r] = b + e;
    }
    if (mn[3] < zn) return DRAW_MESH_NO_NEAR;
    float xs[4] = { mn[0] / mn[3], mn[0] / mx[3], mx[0] / mn[3], mx[0] / mx[3] };
    float ys[4] = { mn[1] / mn[3], mn[1] / mx[3], mx[1] / mn[3], mx[1] / mx[3] };
    float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int k = 1; k < 4; k++) { x0 = xs[k] < x0 ? xs[k] : x0; x1 = xs[k] > x1 ? xs[k] : x1; y0 = ys[k] < y0 ? ys[k] : y0; y1 = ys[k] > y1 ? ys[k] : y1; }
    if (!(x0 > DRAW_GE_GUARD_X0 && x1 < DRAW_GE_GUARD_X1 && y0 > DRAW_GE_GUARD_Y0 && y1 < DRAW_GE_GUARD_Y1)) return DRAW_MESH_NO_GUARD;
    if (!split) return DRAW_PASS_NEAR;
    if (mn[3] >= split) return DRAW_PASS_FAR;
    if (mx[3] <= split) return DRAW_PASS_NEAR;
    return DRAW_MESH_NO_SPLIT;
}

/* The per-cell test for one matrix: rows 0, 1, 3 of m (x z, y z, z), their
 * lengths and those of the five view planes (native_land.c view_out's: z >=
 * 0, the wide view's x range and y 0..480, an 8-pixel margin). */
typedef struct draw_cell_view { const float *m; float n0, n1, n3, pn[5]; float zfloor, split; int ge_near; } draw_cell_view;
static const float draw_cell_planes[5][3] = {   /* coefficients of x z, y z, z */
    { 0, 0, 1 }, { 1, 0, -(DRAW_WIDE_LEFT - 8.0f) }, { -1, 0, DRAW_WIDE_RIGHT + 8.0f }, { 0, 1, 8.0f }, { 0, -1, 488.0f } };
static inline void draw_cell_prep(draw_cell_view *cv, const float *m, float zfloor) {
    cv->m = m; cv->zfloor = zfloor; cv->split = 0.0f; cv->ge_near = 0;
    cv->n0 = __builtin_sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    cv->n1 = __builtin_sqrtf(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
    cv->n3 = __builtin_sqrtf(m[3] * m[3] + m[7] * m[7] + m[11] * m[11]);
    for (int k = 0; k < 5; k++) {
        const float *p = draw_cell_planes[k];
        float a0 = p[0] * m[0] + p[1] * m[1] + p[2] * m[3], a1 = p[0] * m[4] + p[1] * m[5] + p[2] * m[7], a2 = p[0] * m[8] + p[1] * m[9] + p[2] * m[11];
        cv->pn[k] = __builtin_sqrtf(a0 * a0 + a1 * a1 + a2 * a2);
    }
}
/* The classes: out of view; on the GE in the far pass; the CPU path; and,
 * with ge_near (the near pass's depth floor, draw_gu.c): on the GE in the
 * near pass (wholly nearer than the split; the GE clips it at z = 1 if it
 * crosses there, as the console test showed), or in both passes (every
 * vertex at z >= 1, spanning the split: the far pass's projection clips at
 * the split, the near pass's depth floor drops what lies past its far end).
 * The guard band holds for the part at z >= 1 (behind it the GE clips). */
enum { DRAW_CELL_OUT = 0, DRAW_CELL_GE = 1, DRAW_CELL_CPU = 2, DRAW_CELL_NEAR = 3, DRAW_CELL_BOTH = 4 };
static inline int draw_cell_guard(float x0, float x1, float y0, float y1, float zlo, float zhi) {
    return x0 > DRAW_GE_GUARD_X0 * zlo && x0 > DRAW_GE_GUARD_X0 * zhi && x1 < DRAW_GE_GUARD_X1 * zlo && x1 < DRAW_GE_GUARD_X1 * zhi &&
           y0 > DRAW_GE_GUARD_Y0 * zlo && y0 > DRAW_GE_GUARD_Y0 * zhi && y1 < DRAW_GE_GUARD_Y1 * zlo && y1 < DRAW_GE_GUARD_Y1 * zhi;
}
static inline int draw_cell_class(const draw_cell_view *cv, const draw_model_cell *c) {
    const float *m = cv->m, *b = c->bound, r = b[3];
    const float h0 = m[0] * b[0] + m[4] * b[1] + m[8] * b[2] + m[12];
    const float h1 = m[1] * b[0] + m[5] * b[1] + m[9] * b[2] + m[13];
    const float h3 = m[3] * b[0] + m[7] * b[1] + m[11] * b[2] + m[15];
    for (int k = 0; k < 5; k++) {
        const float *p = draw_cell_planes[k];
        if (p[0] * h0 + p[1] * h1 + p[2] * h3 < -r * cv->pn[k]) return DRAW_CELL_OUT;
    }
    const float zmin = h3 - r * cv->n3, zmax = h3 + r * cv->n3, zs = 1.0f + 2.0f * c->edge * cv->n3;   /* the edge in view units */
    const float x0 = h0 - r * cv->n0, x1 = h0 + r * cv->n0, y0 = h1 - r * cv->n1, y1 = h1 + r * cv->n1;
    if (zmin >= cv->zfloor && zmin >= zs) return draw_cell_guard(x0, x1, y0, y1, zmin, zmax) ? DRAW_CELL_GE : DRAW_CELL_CPU;
    if (!cv->ge_near || !draw_cell_guard(x0, x1, y0, y1, zmin > 1.0f ? zmin : 1.0f, zmax)) return DRAW_CELL_CPU;
    if (zmax <= cv->split) return DRAW_CELL_NEAR;
    return zmin >= 1.0f ? DRAW_CELL_BOTH : DRAW_CELL_CPU;
}

/* The CPU path's triangle: a, b, c through m to the strip path's vertices
 * (x, y in DC pixels, w = 1/z), cut at z = 1 where it crosses (attributes
 * linear in view space, as the game's clipper), dropped when every vertex
 * is nearer than z = 1 (the game's strip writer 0x8C037CCC).  Writes a strip
 * of 3 or 4 vertices into out (the winding of a, b, c) and returns its
 * length, 0 when nothing is left. */
static inline uint32_t draw_lerp_argb(uint32_t a, uint32_t b, float t) {
    if (a == b) return a;
    uint32_t o = 0;
    for (int s = 0; s < 32; s += 8) {
        float x = (float)((a >> s) & 255u), y = (float)((b >> s) & 255u);
        int v = (int)(x + (y - x) * t + 0.5f);
        o |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << s;
    }
    return o;
}
static inline int draw_cpu_tri(const float *m, const draw_mesh_vertex *a, const draw_mesh_vertex *b, const draw_mesh_vertex *c, draw_vertex *out) {
    const draw_mesh_vertex *p[3] = { a, b, c };
    float h[3][3];
    int in = 0;
    for (int i = 0; i < 3; i++) {
        const draw_mesh_vertex *q = p[i];
        h[i][0] = m[0] * q->x + m[4] * q->y + m[8] * q->z + m[12];
        h[i][1] = m[1] * q->x + m[5] * q->y + m[9] * q->z + m[13];
        h[i][2] = m[3] * q->x + m[7] * q->y + m[11] * q->z + m[15];
        in |= (h[i][2] >= 1.0f) << i;
    }
    if (!in) return 0;
    draw_vertex poly[4];
    int k = 0;
    for (int i = 0; i < 3; i++) {
        const int j = i == 2 ? 0 : i + 1, ii = (in >> i) & 1, ij = (in >> j) & 1;
        if (ii) {
            const float z = h[i][2], iz = 1.0f / z;
            poly[k].x = h[i][0] * iz; poly[k].y = h[i][1] * iz; poly[k].w = iz;
            poly[k].u = p[i]->u; poly[k].v = p[i]->v; poly[k].color = p[i]->color; poly[k].ocolor = p[i]->ocolor;
            k++;
        }
        if (ii != ij) {
            const float t = (1.0f - h[i][2]) / (h[j][2] - h[i][2]);
            poly[k].x = h[i][0] + (h[j][0] - h[i][0]) * t; poly[k].y = h[i][1] + (h[j][1] - h[i][1]) * t; poly[k].w = 1.0f;
            poly[k].u = p[i]->u + (p[j]->u - p[i]->u) * t; poly[k].v = p[i]->v + (p[j]->v - p[i]->v) * t;
            poly[k].color = draw_lerp_argb(p[i]->color, p[j]->color, t);
            poly[k].ocolor = draw_lerp_argb(p[i]->ocolor, p[j]->ocolor, t);
            k++;
        }
    }
    out[0] = poly[0]; out[1] = poly[1];
    if (k == 3) { out[2] = poly[2]; return 3; }
    out[2] = poly[3]; out[3] = poly[2];          /* the quad 0 1 2 3 as the strip 0 1 3 2 */
    return 4;
}

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
