/* native_land.c - the land table drawn from converted meshes with hardware
 * transform (docs/native-renderer.md, the Station Square slice).
 *
 * renderer = native (sa1psp.ini, or --renderer native|ta|force:native) turns
 * it on where the backend has draw_mesh; never in strict runs unless forced.
 * The game's own land draw runs unchanged (its cull, visible list, matrices,
 * njSetTexture; the land task's MainSub 0x8C0519C0 calls the cull and the
 * draw directly).  A model of the current land table (0x8C7596F4) whose
 * every mesh set has converted triangles in native/LAND_<level>_<act>.BIN
 * (tools/land_convert.py, loaded when the land table changes) and is neither
 * translucent nor environment-mapped is decided on when the game is about to
 * transform it (native_land_decide, from the 0x8C037294 wrapper, XMTRX set):
 * the backend says whether it draws a mesh with this matrix and the model's
 * bounding sphere in hardware (draw_mesh_accepts).  If it does:
 *   - the vertex transform 0x8C037294 returns at once (ta_stubs.c), only
 *     pointing gbr+60 at the vertex buffer as it would;
 *   - the lighting 0x8C037350 runs the first time and its per-point colours
 *     are kept; after that it is skipped and the kept colours are used (the
 *     palette lighting of static land is fixed for a land table: its light
 *     is fixed in world space for an act and a time of day);
 *   - the mesh-set loop (ta_mesh.c) writes each set's polygon header as
 *     always, then native_land_set() hands the set's triangles to kamui with
 *     those colours and the game's matrix (XMTRX times the screen scale and
 *     centre) instead of calling a strip writer: kamui draws it with the
 *     header's state, in list order (draw_mesh).
 * Otherwise the model goes the TA way untouched.  The guest state differs
 * from the TA path's only in draw scratch, as on a skipped frame
 * (hle/game/frame_skip.c): tapes and RAM compares check it.
 * renderer = noland (a measurement aid, never a default) drops the land's
 * models altogether: TA path minus noland is what the land costs.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "ninja.h"
#include "draw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hle_renderer_native;            /* sa1psp.ini renderer: 0 ta, 1 native, 2 noland */
int hle_renderer_cli = -1;          /* backend option, wins */
int hle_renderer_force;             /* also in strict runs (tests) */
int native_land_window;             /* on: the backend can draw meshes and the option is set */
int native_land_noland;              /* renderer = noland: the land not drawn at all */

#define USE_ALPHA 0x00100000u
#define ENV_MAP   0x00400000u       /* UVs from the normals (not converted yet): the TA way */
#define LT_CUR    0x8c7596f4u
#define LV_ACT    0x8c7493bau       /* level (16 bits), then act at +2 */
#define ZERO_OFFS 0x8c03cf60u       /* the all-zero offset colours (IGNORE_LIGHT) */

typedef struct { uint32_t model; uint16_t set, col, mat, pad; uint32_t vfirst, vcount, ifirst, icount; } nl_batch;
typedef struct { float x, y, z, nx, ny, nz, u, v; uint32_t pt; } nl_vert;
typedef struct {
    uint32_t model;
    int ok;                         /* converted, opaque, not environment-mapped */
    float bound[4];                 /* model-space sphere over its vertices */
    uint32_t npt;                   /* points lit (the model's point count) */
    uint32_t *col, *ocol;           /* kept lighting: base and offset colour per point */
} nl_model;

static struct {
    uint32_t lt;                    /* the land table this is for (0: none loaded) */
    int have;                       /* a file was found for it */
    uint8_t *file;
    const uint32_t *mats;           /* 4 words each */
    nl_batch *b;                    /* sorted by model, set */
    const nl_vert *v;
    const uint16_t *idx;
    uint32_t nb, nmat, nv, ni;
    nl_model *models;
    uint32_t nmodels;
} L;
static nl_model *dec;               /* the model the game is drawing now, if it goes natively */
static float dec_m[16];
static uint32_t stat_sets, stat_tris, stat_lit, stat_kept;

static int cmp_batch(const void *a, const void *b) {
    const nl_batch *x = a, *y = b;
    if (x->model != y->model) return x->model < y->model ? -1 : 1;
    return (int)x->set - (int)y->set;
}

static void unload(void) {
    for (uint32_t i = 0; i < L.nmodels; i++) { free(L.models[i].col); free(L.models[i].ocol); }
    free(L.file); free(L.models);
    memset(&L, 0, sizeof L);
    dec = NULL;
}

static void load(uint32_t lt) {
    unload();
    L.lt = lt;
    char path[64];
    snprintf(path, sizeof path, "native/LAND_%u_%u.BIN", (unsigned)hle_rd16(LV_ACT), (unsigned)hle_rd16(LV_ACT + 2));
    uint32_t size = 0;
    if (sa1_plat_file_size(path, &size) != 0 || size < 36) {
        sa1_log("native land: no %s for land table 0x%08x; drawing it the TA way", path, lt);
        return;
    }
    uint8_t *f = malloc(size);
    if (!f || sa1_plat_file_read(path, 0, f, size) != (int)size) { free(f); return; }
    uint32_t h[8];
    memcpy(h, f + 4, sizeof h);     /* version, lt, ncol, nbatch, nmat, nvert, nidx, uv scale */
    size_t need = 36 + (size_t)h[4] * 16 + (size_t)h[3] * sizeof(nl_batch) + (size_t)h[5] * sizeof(nl_vert) + (size_t)h[6] * 2;
    if (memcmp(f, "SA1N", 4) || h[0] != 1 || (h[1] & 0x1FFFFFFFu) != (lt & 0x1FFFFFFFu) || need > size) {
        sa1_log("native land: %s does not match land table 0x%08x; drawing it the TA way", path, lt);
        free(f);
        return;
    }
    L.file = f;
    L.nmat = h[4]; L.nb = h[3]; L.nv = h[5]; L.ni = h[6];
    L.mats = (const uint32_t *)(const void *)(f + 36);
    L.b = (nl_batch *)(void *)(f + 36 + (size_t)L.nmat * 16);
    L.v = (const nl_vert *)(const void *)((uint8_t *)L.b + (size_t)L.nb * sizeof(nl_batch));
    L.idx = (const uint16_t *)(const void *)((const uint8_t *)L.v + (size_t)L.nv * sizeof(nl_vert));
    for (uint32_t i = 0; i < L.nb; i++) L.b[i].model &= 0x1FFFFFFFu;
    qsort(L.b, L.nb, sizeof *L.b, cmp_batch);
    L.models = calloc(L.nb ? L.nb : 1, sizeof *L.models);
    for (uint32_t i = 0; i < L.nb; ) {
        uint32_t m = L.b[i].model, j = i;
        nl_model *md = &L.models[L.nmodels++];
        int ok = 1;
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (; j < L.nb && L.b[j].model == m; j++) {
            if (L.b[j].mat >= L.nmat || (L.mats[4 * L.b[j].mat + 1] & (USE_ALPHA | ENV_MAP))) ok = 0;
            if (L.b[j].vfirst + L.b[j].vcount > L.nv || L.b[j].ifirst + L.b[j].icount > L.ni) { ok = 0; continue; }
            for (uint32_t k = 0; k < L.b[j].vcount; k++) {
                const nl_vert *s = &L.v[L.b[j].vfirst + k];
                const float q[3] = { s->x, s->y, s->z };
                for (int a = 0; a < 3; a++) { if (q[a] < lo[a]) lo[a] = q[a]; if (q[a] > hi[a]) hi[a] = q[a]; }
            }
        }
        float r2 = 0.0f;
        for (int a = 0; a < 3; a++) { md->bound[a] = 0.5f * (lo[a] + hi[a]); float d = hi[a] - md->bound[a]; r2 += d * d; }
        md->bound[3] = sqrtf(r2) * 1.0001f + 1e-3f;
        uint32_t nset = hle_rd16((m | 0x80000000u) + 20u);
        if (j - i != nset) ok = 0;
        md->model = m; md->ok = ok;
        md->npt = (uint32_t)(int32_t)hle_rd32((m | 0x80000000u) + 8u);
        if (md->npt > 65536u) md->ok = 0;
        i = j;
    }
    L.have = 1;
    uint32_t nok = 0;
    for (uint32_t k = 0; k < L.nmodels; k++) nok += L.models[k].ok;
    sa1_log("native land: %s for land table 0x%08x: %u mesh sets, %u vertices, %u triangles; %u of %u models can go natively (the rest translucent or environment-mapped)",
            path, lt, L.nb, L.nv, L.ni / 3, nok, L.nmodels);
}

static nl_model *find_model(uint32_t model) {
    uint32_t lt = hle_rd32(LT_CUR);
    if (lt != L.lt) { if (lt) load(lt); else unload(); }
    if (!L.have) return NULL;
    model &= 0x1FFFFFFFu;
    uint32_t lo = 0, hi = L.nmodels;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (L.models[mid].model < model) lo = mid + 1; else hi = mid;
    }
    return lo < L.nmodels && L.models[lo].model == model ? &L.models[lo] : NULL;
}

/* The game's matrix for the model being drawn, as draw.h takes it: the fed
 * vector (sx z, sy z, -(z + 1), z) from XMTRX's (x', y', z'), sx = x' Sx / z'
 * + Cx, sy = y' Sy / z' + Cy (ta_xform.c 0x8C037294). */
static void game_matrix(const sh4_ctx *c, float *m) {
    const float cx = hle_rdf(0x8c8ffe00u), cy = hle_rdf(0x8c8ffe04u), sx = hle_rdf(0x8c8ffe08u), sy = hle_rdf(0x8c8ffe0cu) * sx;
    const float *x = c->xf;
    for (int col = 0; col < 4; col++) {
        float x0 = x[4 * col], x1 = x[4 * col + 1], x2 = x[4 * col + 2];
        float w = col == 3 ? 1.0f : 0.0f;
        m[4 * col + 0] = sx * x0 + cx * x2;
        m[4 * col + 1] = sy * x1 + cy * x2;
        m[4 * col + 2] = -x2 - w;
        m[4 * col + 3] = x2;
    }
}

/* From the transform's wrapper, XMTRX set: 1 = this model goes natively
 * (the transform is skipped), 0 = the TA way.  The answer stands until the
 * next model's transform (lighting and mesh loop ask native_land_model_ok). */
int native_land_decide(sh4_ctx *c, uint32_t model) {
    dec = NULL;
    if (!native_land_window) return 0;
    nl_model *md = find_model(model);
    if (!md) return 0;
    if (native_land_noland) { dec = md; return 1; }
    /* the material override (0x8C8FFE1C: enable, AND, OR; the land draw sets it) may turn
     * USE_ALPHA or the environment map on: then the TA way */
    if (!md->ok || (hle_rd32(0x8c8ffe1cu) && (hle_rd32(0x8c8ffe24u) & (USE_ALPHA | ENV_MAP)))) return 0;
    game_matrix(c, dec_m);
    if (!draw_mesh_accepts(dec_m, md->bound)) return 0;
    dec = md;
    return 1;
}

int native_land_model_ok(uint32_t model) {
    return dec && dec->model == (model & 0x1FFFFFFFu);
}

/* From the lighting's wrapper for a native model: 1 = skip the lighting (the
 * kept colours are used), 0 = run it, then call native_land_lit(). */
int native_land_skip_light(uint32_t model) {
    if (!native_land_model_ok(model)) return 0;
    return native_land_noland || dec->col != NULL;
}
void native_land_lit(sh4_ctx *c, uint32_t model) {
    if (!native_land_model_ok(model) || dec->col || !dec->npt) return;
    uint32_t *col = malloc(dec->npt * 4u), *ocol = malloc(dec->npt * 4u);
    if (!col || !ocol) { free(col); free(ocol); return; }
    const uint32_t vbuf = hle_rd32(c->gbr + 60), offs = hle_rd32(c->gbr + 64);
    for (uint32_t i = 0; i < dec->npt; i++) {
        col[i] = hle_rd32(vbuf + 16u * i + 12u);
        ocol[i] = offs ? hle_rd32(offs + 4u * i) : 0;
    }
    dec->col = col; dec->ocol = ocol;
    stat_lit++;
}

/* Draws mesh set `set` of the native model natively: returns 1 and the TA
 * pointer after the marker in *ta_out, or 0 (then the caller draws it the TA
 * way; not expected once decided). */
int native_land_set(sh4_ctx *c, uint32_t model, uint32_t set, uint32_t ta, uint32_t *ta_out) {
    if (!native_land_model_ok(model)) return 0;
    if (native_land_noland) { *ta_out = ta; return 1; }
    model &= 0x1FFFFFFFu;
    uint32_t lo = 0, hi = L.nb;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (L.b[mid].model < model || (L.b[mid].model == model && L.b[mid].set < set)) lo = mid + 1; else hi = mid;
    }
    if (lo >= L.nb || L.b[lo].model != model || L.b[lo].set != set) return 0;
    const nl_batch *b = &L.b[lo];
    uint32_t rec;
    draw_mesh_vertex *out = kamui_native_mesh(b->vcount, dec_m, dec->bound, L.idx + b->ifirst, b->icount, &rec);
    if (!out) return 0;
    const uint32_t ocol = hle_rd32(c->gbr + 56);
    const int zero_off = ocol == ZERO_OFFS || !ocol;
    const uint32_t *kc = dec->col, *ko = dec->ocol;
    const uint32_t vbuf = hle_rd32(c->gbr + 60);
    for (uint32_t i = 0; i < b->vcount; i++) {
        const nl_vert *s = &L.v[b->vfirst + i];
        out[i].x = s->x; out[i].y = s->y; out[i].z = s->z; out[i].u = s->u; out[i].v = s->v;
        if (kc && s->pt < dec->npt) { out[i].color = kc[s->pt]; out[i].ocolor = zero_off ? 0 : ko[s->pt]; }
        else { out[i].color = hle_rd32(vbuf + 16u * s->pt + 12u); out[i].ocolor = zero_off ? 0 : hle_rd32(ocol + 4u * s->pt); }
    }
    if (kc) stat_kept++;
    hle_wr32(ta, 0xC0000001u);       /* kamui: a native mesh record (type 6, kind 1) */
    hle_wr32(ta + 4, rec);
    hle_wr32(ta + 8, 0);
    *ta_out = ta + 32;
    stat_sets++; stat_tris += b->icount / 3;
    return 1;
}

void native_land_init(void) {
    int on = hle_renderer_cli >= 0 ? hle_renderer_cli : hle_renderer_native;
    if (!on) return;
    if (hle_strict && !hle_renderer_force) { sa1_log("native land: off (strict run)"); return; }
    if (!draw_mesh_supported()) { sa1_log("native land: this backend has no draw_mesh yet; the TA way"); return; }
    native_land_window = 1;
    native_land_noland = on == 2;
    sa1_log("native land: on%s", native_land_noland ? " (noland: the land's models are not drawn; a measurement aid)" : "");
}

void native_land_report(void) {
    if (stat_sets || stat_lit)
        sa1_log("native land: %u mesh sets (%u triangles) drawn natively, %u with kept lighting; %u models lit once and kept",
                stat_sets, stat_tris, stat_kept, stat_lit);
}
