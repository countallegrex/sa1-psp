/* native_land.c - the land table: models out of view skipped, the rest drawn
 * from converted meshes with hardware transform where the backend can
 * (docs/native-renderer.md; s.6 is this design, iteration 3).
 *
 * land_cull = on (default) | off (sa1psp.ini, or --land-cull; also in strict
 * runs: it changes only draw scratch, as frame skip's skipped frames do) and
 * renderer = native | ta | noland (sa1psp.ini, or --renderer, with force:
 * also in strict runs).  The game's own land draw runs unchanged (its
 * cull, visible list, matrices, njSetTexture; the land task's MainSub
 * 0x8C0519C0 calls the cull and the draw directly).  It has no view culling
 * of its own: about half of the land models it draws in Station Square lie
 * wholly outside the view.  When the game is about to transform a model of
 * the current land table (0x8C7596F4; every COL object's model, bounding
 * sphere from its points), native_land_decide (the 0x8C037294 wrapper, XMTRX
 * set) chooses one of:
 *   - OUT (land_cull): the sphere is wholly outside the view (behind the
 *     eye or past a screen edge): the transform, the lighting and every mesh
 *     set's strip writer are skipped; nothing it would have drawn is seen.
 *   - MODEL (native): the model has converted meshes
 *     (native/LAND_<level>_<act>.BIN, tools/land_convert.py), is neither
 *     translucent nor environment-mapped, and the backend draws it whole in
 *     hardware with this matrix (draw_mesh_accepts).  The transform and the
 *     lighting are skipped and the mesh-set loop (ta_mesh.c) only puts one
 *     marker for the whole model in the list; kamui hands it to draw_model,
 *     which draws the backend's persistent copy (on the PSP one call of a
 *     prebuilt display list).
 *   - SETS (native, once per model): as MODEL, but the model has no
 *     persistent copy yet.  The lighting runs this once and its per-point
 *     colours are kept (the palette light of static land is fixed for a land
 *     table: in world space for an act and a time of day); each mesh set
 *     goes as iteration 2's per-set mesh, and kamui reports the state each
 *     set drew with (native_land_captured).  With every set's state known the
 *     persistent copy is made (draw_model_create): vertices, kept colours and
 *     states, never gathered again.
 *   - otherwise the TA path, untouched.
 * The guest state differs from the TA path's only in draw scratch, as on a
 * skipped frame (hle/game/frame_skip.c): tapes and RAM compares check it.
 * renderer = noland (a measurement aid, never a default) treats every land
 * model as OUT: TA path minus noland is what the land costs.
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
int hle_land_cull = 1;              /* sa1psp.ini land_cull (default on) */
int hle_land_cull_cli = -1;         /* --land-cull on|off, wins */
static int cull;                    /* in force */
int hle_renderer_cli = -1;          /* backend option, wins */
int hle_renderer_force;             /* also in strict runs (tests) */
int native_land_window;             /* on: land_cull, or renderer native or noland */
int native_land_noland;             /* renderer = noland: the land not drawn at all */
static int mode;                    /* the renderer value in force */
static int check;                   /* SA1_NATIVE_CHECK=1: persistent models go per set and are compared */
static int nopersist;               /* renderer = native-sets: iteration 2's per-set way every frame (a check) */
static uint32_t chk_state, chk_col, chk_sets, chk_lit, chk_logged;
int native_land_all_sets;           /* the decided model is OUT: ta_mesh.c skips every set */

#define USE_ALPHA 0x00100000u
#define ENV_MAP   0x00400000u       /* UVs from the normals (not converted yet): the TA way */
#define LT_CUR    0x8c7596f4u
#define LV_ACT    0x8c7493bau       /* level (16 bits), then act at +2 */
#define ZERO_OFFS 0x8c03cf60u       /* the all-zero offset colours (IGNORE_LIGHT) */
#define MAX_SETS  4096u

typedef struct { uint32_t model; uint16_t set, col, mat, pad; uint32_t vfirst, vcount, ifirst, icount; } nl_batch;
typedef struct { float x, y, z, nx, ny, nz, u, v; uint32_t pt; } nl_vert;
typedef struct {
    uint32_t model;                 /* guest address, 0x1FFFFFFF-masked */
    float bound[4];                 /* model-space sphere over its points */
    uint32_t ntri;                  /* converted triangles (0: none) */
    int conv;                       /* converted, opaque, not environment-mapped: may go natively */
    uint32_t b0, nb;                /* its batches in L.b, one per mesh set in order */
    uint32_t npt;                   /* points lit (the model's point count) */
    uint32_t *col, *ocol;           /* kept lighting: base and offset colour per point */
    draw_state *st;                 /* per mesh set, as kamui drew it once */
    uint8_t *zoff;                  /* per mesh set: the zero offset colours (IGNORE_LIGHT) */
    uint32_t nst;                   /* sets whose state is known */
    uint32_t handle;                /* draw_model_create's, 0: none yet */
    int failed;                     /* draw_model_create refused: the per-set way from now on */
    int alpha;                      /* has a translucent material */
    uint8_t why;                    /* not converted: 0 not in the file, 1 translucent or env-mapped, 2 sets differ */
    uint32_t ta_draws, out_draws;   /* this land table's draws the TA way, out of view */
} nl_model;

static struct {
    uint32_t lt;                    /* the land table this is for (0: none loaded) */
    uint8_t *file;
    const uint32_t *mats;           /* 4 words each */
    nl_batch *b;                    /* sorted by model, set */
    const nl_vert *v;
    const uint16_t *idx;
    uint32_t nb, nmat, nv, ni;
    nl_model *models;               /* sorted by model */
    uint32_t nmodels;
} L;

enum { DEC_NONE, DEC_OUT, DEC_MODEL, DEC_SETS };
static nl_model *dec;               /* the model the game is drawing now, if not the TA way */
static int dec_kind, dec_marked;
static float dec_m[16];
static struct { uint32_t out, model, sets, ta_test[5], ta_other, built, out_tris, model_tris, ta_tris; } st;

static int cmp_batch(const void *a, const void *b) {
    const nl_batch *x = a, *y = b;
    if (x->model != y->model) return x->model < y->model ? -1 : 1;
    return (int)x->set - (int)y->set;
}
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* The models of the table drawn the TA way most often (renderer native). */
static void report_ta(void) {
    if (mode != 1 || !L.nmodels) return;
    for (int k = 0; k < 8; k++) {
        nl_model *best = NULL;
        for (uint32_t i = 0; i < L.nmodels; i++)
            if (L.models[i].ta_draws && (!best || L.models[i].ta_draws > best->ta_draws)) best = &L.models[i];
        if (!best) break;
        sa1_log("native land: TA model 0x%08x: %u draws (%u out of view), %u points, %u sets, sphere r %.0f, %s",
                best->model, best->ta_draws, best->out_draws, best->npt, (unsigned)hle_rd16((best->model | 0x80000000u) + 20u), best->bound[3],
                best->conv ? "converted (GE refused)" : best->why == 0 ? "not in the file" : best->why == 1 ? "translucent or env-mapped" : "sets differ");
        best->ta_draws = 0;
    }
}

static void unload(void) {
    report_ta();
    for (uint32_t i = 0; i < L.nmodels; i++) {
        nl_model *md = &L.models[i];
        free(md->col); free(md->ocol); free(md->st); free(md->zoff);
        if (md->handle) draw_model_destroy(md->handle);
    }
    free(L.file); free(L.models);
    memset(&L, 0, sizeof L);
    dec = NULL;
}

/* Every COL object's model in the land table, with a sphere over its points. */
static void walk_table(uint32_t lt) {
    const uint32_t ncol = (uint32_t)(uint16_t)hle_rd16(lt), cols = hle_rd32(lt + 12u);
    if (!ncol || !hle_in_ram(cols)) return;
    uint32_t *ms = malloc(ncol * sizeof *ms), n = 0;
    if (!ms) return;
    for (uint32_t i = 0; i < ncol; i++) {
        const uint32_t obj = hle_rd32(cols + 0x24u * i + 24u);
        if (!hle_in_ram(obj)) continue;
        const uint32_t m = hle_rd32(obj + 4u);
        if (!hle_in_ram(m) || !hle_in_ram(m + 40u)) continue;
        ms[n++] = m & 0x1FFFFFFFu;
    }
    qsort(ms, n, sizeof *ms, cmp_u32);
    L.models = calloc(n ? n : 1, sizeof *L.models);
    if (!L.models) { free(ms); return; }
    for (uint32_t i = 0; i < n; i++) {
        if (L.nmodels && L.models[L.nmodels - 1].model == ms[i]) continue;
        const uint32_t g = ms[i] | 0x80000000u, pts = hle_rd32(g);
        const int32_t npt = (int32_t)hle_rd32(g + 8u);
        if (npt <= 0 || npt > 65536 || !hle_in_ram(pts) || !hle_in_ram(pts + 12u * (uint32_t)npt - 4u)) continue;
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (int32_t k = 0; k < npt; k++)
            for (int a = 0; a < 3; a++) {
                const float q = hle_rdf(pts + 12u * (uint32_t)k + 4u * (uint32_t)a);
                if (q < lo[a]) lo[a] = q;
                if (q > hi[a]) hi[a] = q;
            }
        nl_model *md = &L.models[L.nmodels++];
        float r2 = 0.0f;
        for (int a = 0; a < 3; a++) { md->bound[a] = 0.5f * (lo[a] + hi[a]); float d = hi[a] - md->bound[a]; r2 += d * d; }
        md->bound[3] = sqrtf(r2) * 1.0001f + 1e-3f;
        md->model = ms[i];
        md->npt = (uint32_t)npt;
        {   /* any translucent material (USE_ALPHA in the attribute flags at +16) */
            const uint32_t mats = hle_rd32(g + 16u), nmat = (uint32_t)(uint16_t)hle_rd16(g + 22u);
            for (uint32_t k = 0; k < nmat && hle_in_ram(mats + 20u * k + 16u); k++) if (hle_rd32(mats + 20u * k + 16u) & USE_ALPHA) md->alpha = 1;
        }
    }
    free(ms);
}

static nl_model *model_of(uint32_t model) {
    model &= 0x1FFFFFFFu;
    uint32_t lo = 0, hi = L.nmodels;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (L.models[mid].model < model) lo = mid + 1; else hi = mid;
    }
    return lo < L.nmodels && L.models[lo].model == model ? &L.models[lo] : NULL;
}

/* The converted meshes (native only): each model's batches, one per set. */
static void load_file(uint32_t lt) {
    char path[64];
    snprintf(path, sizeof path, "native/LAND_%u_%u.BIN", (unsigned)hle_rd16(LV_ACT), (unsigned)hle_rd16(LV_ACT + 2));
    uint32_t size = 0;
    if (sa1_plat_file_size(path, &size) != 0 || size < 36) {
        sa1_log("native land: no %s for land table 0x%08x; its models are culled or drawn the TA way", path, lt);
        return;
    }
    uint8_t *f = malloc(size);
    if (!f || sa1_plat_file_read(path, 0, f, size) != (int)size) { free(f); return; }
    uint32_t h[8];
    memcpy(h, f + 4, sizeof h);     /* version, lt, ncol, nbatch, nmat, nvert, nidx, uv scale */
    size_t need = 36 + (size_t)h[4] * 16 + (size_t)h[3] * sizeof(nl_batch) + (size_t)h[5] * sizeof(nl_vert) + (size_t)h[6] * 2;
    if (memcmp(f, "SA1N", 4) || h[0] != 1 || (h[1] & 0x1FFFFFFFu) != (lt & 0x1FFFFFFFu) || need > size) {
        sa1_log("native land: %s does not match land table 0x%08x; its models are culled or drawn the TA way", path, lt);
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
    uint32_t nconv = 0;
    for (uint32_t i = 0; i < L.nb; ) {
        uint32_t m = L.b[i].model, j = i;
        int ok = 1;
        uint32_t ntri = 0;
        for (; j < L.nb && L.b[j].model == m; j++) {
            if (L.b[j].mat >= L.nmat || (L.mats[4 * L.b[j].mat + 1] & (USE_ALPHA | ENV_MAP))) ok = 0;
            if (L.b[j].vfirst + L.b[j].vcount > L.nv || L.b[j].ifirst + L.b[j].icount > L.ni || L.b[j].set != j - i) ok = 0;
            ntri += L.b[j].icount / 3;
        }
        nl_model *md = model_of(m);
        uint32_t nset = hle_rd16((m | 0x80000000u) + 20u);
        if (md) md->why = ok ? 2 : 1;
        if (md && ok && j - i == nset && nset <= MAX_SETS) {
            md->conv = 1; md->b0 = i; md->nb = j - i; md->ntri = ntri;
            nconv++;
        }
        i = j;
    }
    sa1_log("native land: %s for land table 0x%08x: %u mesh sets, %u vertices, %u triangles; %u of %u models can go natively (the rest translucent or environment-mapped)",
            path, lt, L.nb, L.nv, L.ni / 3, nconv, L.nmodels);
}

static nl_model *find_model(uint32_t model) {
    uint32_t lt = hle_rd32(LT_CUR);
    if (lt != L.lt) {
        unload();
        L.lt = lt;
        if (lt && hle_in_ram(lt)) {
            walk_table(lt);
            if (mode == 1) load_file(lt);
        }
    }
    return L.nmodels ? model_of(model) : NULL;
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

/* Whether a model-space sphere lies wholly outside the view: behind the eye
 * (z < 0) or past an edge of the screen, the wide view's x range whatever
 * the aspect, with a margin.  Each test is a half-space every visible point
 * lies in, so nothing seen is ever dropped. */
static int view_out(const float *m, const float *bound) {
    static const float pl[5][3] = {   /* coefficients of the rows x z, y z, z */
        { 0, 0, 1 }, { 1, 0, -(DRAW_WIDE_LEFT - 8.0f) }, { -1, 0, DRAW_WIDE_RIGHT + 8.0f }, { 0, 1, 8.0f }, { 0, -1, 488.0f } };
    for (int k = 0; k < 5; k++) {
        float a[4];
        for (int c = 0; c < 4; c++) a[c] = pl[k][0] * m[4 * c] + pl[k][1] * m[4 * c + 1] + pl[k][2] * m[4 * c + 3];
        const float d = a[0] * bound[0] + a[1] * bound[1] + a[2] * bound[2] + a[3];
        if (d < 0.0f && d * d > bound[3] * bound[3] * (a[0] * a[0] + a[1] * a[1] + a[2] * a[2])) return 1;
    }
    return 0;
}

/* From the transform's wrapper, XMTRX set: 1 = not the TA way (the transform
 * is skipped), 0 = the TA way.  The answer stands until the next model's
 * transform (lighting and mesh loop ask native_land_model_ok). */
int native_land_decide(sh4_ctx *c, uint32_t model) {
    dec = NULL; dec_kind = DEC_NONE; dec_marked = 0; native_land_all_sets = 0;
    if (!native_land_window) return 0;
    nl_model *md = find_model(model);
    if (!md) return 0;
    if (native_land_noland) { dec = md; dec_kind = DEC_OUT; native_land_all_sets = 1; return 1; }
    game_matrix(c, dec_m);
    /* models with a translucent material are never culled: their strips go to the
     * depth-sorted list, whose order for the rest changes without them (a few
     * pixels of Sonic differed on the console) */
    if (cull && !md->alpha && view_out(dec_m, md->bound)) { dec = md; dec_kind = DEC_OUT; native_land_all_sets = 1; st.out++; st.out_tris += md->ntri; md->out_draws++; return 1; }
    /* the material override (0x8C8FFE1C: enable, AND, OR; the land draw sets it) may turn
     * USE_ALPHA or the environment map on: then the TA way */
    if (mode != 1 || !md->conv || (hle_rd32(0x8c8ffe1cu) && (hle_rd32(0x8c8ffe24u) & (USE_ALPHA | ENV_MAP)))) { st.ta_other++; st.ta_tris += md->ntri; md->ta_draws++; return 0; }
    const int p = draw_mesh_test(dec_m, md->bound);
    if (p < 0) { st.ta_test[-p]++; st.ta_tris += md->ntri; md->ta_draws++; return 0; }
    dec = md;
    dec_kind = md->handle && !check ? DEC_MODEL : DEC_SETS;
    if (dec_kind == DEC_MODEL) { st.model++; st.model_tris += md->ntri; } else st.sets++;
    return 1;
}

int native_land_model_ok(uint32_t model) {
    return dec && dec->model == (model & 0x1FFFFFFFu);
}

/* From the lighting's wrapper for a model not drawn the TA way: 1 = skip the
 * lighting, 0 = run it, then call native_land_lit(). */
int native_land_skip_light(uint32_t model) {
    if (!native_land_model_ok(model)) return 0;
    return dec_kind != DEC_SETS || (dec->col != NULL && !(check && dec->handle));
}
void native_land_lit(sh4_ctx *c, uint32_t model) {
    if (check && native_land_model_ok(model) && dec_kind == DEC_SETS && dec->col && dec->handle) {
        const uint32_t vbuf = hle_rd32(c->gbr + 60), offs = hle_rd32(c->gbr + 64);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < dec->npt; i++)
            if (dec->col[i] != hle_rd32(vbuf + 16u * i + 12u) || (offs && dec->ocol[i] != hle_rd32(offs + 4u * i))) bad++;
        chk_lit++;
        if (bad) { chk_col++; if (chk_logged++ < 20) sa1_log("native check: model 0x%08x: %u of %u kept colours differ", dec->model, bad, dec->npt); }
        return;
    }
    if (!native_land_model_ok(model) || dec_kind != DEC_SETS || dec->col || !dec->npt) return;
    uint32_t *col = malloc(dec->npt * 4u), *ocol = malloc(dec->npt * 4u);
    if (!col || !ocol) { free(col); free(ocol); return; }
    const uint32_t vbuf = hle_rd32(c->gbr + 60), offs = hle_rd32(c->gbr + 64);
    for (uint32_t i = 0; i < dec->npt; i++) {
        col[i] = hle_rd32(vbuf + 16u * i + 12u);
        ocol[i] = offs ? hle_rd32(offs + 4u * i) : 0;
    }
    dec->col = col; dec->ocol = ocol;
}

/* One converted vertex with the kept colours. */
static void vertex(const nl_model *md, const nl_vert *s, int zero_off, draw_mesh_vertex *o) {
    o->x = s->x; o->y = s->y; o->z = s->z; o->u = s->u; o->v = s->v;
    const int k = md->col && s->pt < md->npt;
    o->color = k ? md->col[s->pt] : 0xFFFFFFFFu;
    o->ocolor = k && !zero_off ? md->ocol[s->pt] : 0;
}

/* Mesh set `set` of the model being drawn: returns 1 and the TA pointer
 * after anything written in *ta_out, or 0 (then the caller draws it the TA
 * way; not expected once decided). */
int native_land_set(sh4_ctx *c, uint32_t model, uint32_t set, uint32_t ta, uint32_t *ta_out) {
    if (!native_land_model_ok(model)) return 0;
    if (dec_kind == DEC_OUT) { *ta_out = ta; return 1; }
    if (dec_kind == DEC_MODEL) {    /* one marker for the whole model, at its first set */
        uint32_t rec;
        if (!dec_marked && kamui_native_model(dec->handle, dec_m, &rec)) {
            hle_wr32(ta, 0xC0000002u);   /* kamui: a persistent model record (type 6, kind 2) */
            hle_wr32(ta + 4, rec);
            hle_wr32(ta + 8, 0);
            ta += 32;
        }
        dec_marked = 1;
        *ta_out = ta;
        return 1;
    }
    nl_model *md = dec;
    if (set >= md->nb) return 0;
    const nl_batch *b = &L.b[md->b0 + set];
    if (!md->st) {
        md->st = calloc(md->nb, sizeof *md->st);
        md->zoff = calloc(md->nb, 1);
        if (!md->st || !md->zoff) { free(md->st); free(md->zoff); md->st = NULL; md->zoff = NULL; }
    }
    const uint32_t ocol = hle_rd32(c->gbr + 56);
    const int zero_off = ocol == ZERO_OFFS || !ocol;
    if (md->zoff) md->zoff[set] = (uint8_t)zero_off;
    uint32_t rec;
    const uint32_t tag = (uint32_t)(md - L.models) << 12 | set;
    draw_mesh_vertex *out = kamui_native_mesh(b->vcount, dec_m, md->bound, L.idx + b->ifirst, b->icount, tag, &rec);
    if (!out) return 0;
    for (uint32_t i = 0; i < b->vcount; i++) vertex(md, &L.v[b->vfirst + i], zero_off, &out[i]);
    hle_wr32(ta, 0xC0000001u);       /* kamui: a native mesh record (type 6, kind 1) */
    hle_wr32(ta + 4, rec);
    hle_wr32(ta + 8, 0);
    *ta_out = ta + 32;
    return 1;
}

/* The persistent copy, once every set's state is known. */
static void build(nl_model *md) {
    uint32_t nv = 0, ni = 0;
    for (uint32_t s = 0; s < md->nb; s++) { nv += L.b[md->b0 + s].vcount; ni += L.b[md->b0 + s].icount; }
    draw_mesh_vertex *v = malloc((nv ? nv : 1) * sizeof *v);
    uint16_t *ix = malloc((ni ? ni : 1) * 2u);
    draw_model_set *sets = malloc(md->nb * sizeof *sets);
    if (v && ix && sets) {
        uint32_t vo = 0, io = 0;
        for (uint32_t s = 0; s < md->nb; s++) {
            const nl_batch *b = &L.b[md->b0 + s];
            for (uint32_t i = 0; i < b->vcount; i++) vertex(md, &L.v[b->vfirst + i], md->zoff[s], &v[vo + i]);
            memcpy(ix + io, L.idx + b->ifirst, b->icount * 2u);
            sets[s].state = md->st[s];
            sets[s].vfirst = vo; sets[s].vcount = b->vcount; sets[s].ifirst = io; sets[s].icount = b->icount;
            vo += b->vcount; io += b->icount;
        }
        md->handle = draw_model_create(v, (int)nv, ix, (int)ni, sets, (int)md->nb, md->bound);
        if (md->handle) st.built++;
    }
    if (!md->handle) md->failed = 1;
    free(v); free(ix); free(sets);
}

void native_land_captured(uint32_t tag, const draw_state *s) {
    const uint32_t i = tag >> 12, set = tag & 0xFFFu;
    if (i >= L.nmodels) return;
    nl_model *md = &L.models[i];
    if (check && md->handle && md->st && set < md->nb) {
        chk_sets++;
        if (memcmp(&md->st[set], s, sizeof *s)) {
            chk_state++;
            const draw_state *k = &md->st[set];
            if (chk_logged++ < 40)
                sa1_log("native check: model 0x%08x set %u: state differs: texture %u/%u blend %u,%u/%u,%u fog %u/%u shade %u/%u filter %u/%u clamp %u%u/%u%u flip %u%u/%u%u alpha %u/%u layer %u/%u cull %u/%u",
                        md->model, set, k->texture, s->texture, k->blend_src, k->blend_dst, s->blend_src, s->blend_dst, k->fog, s->fog,
                        k->shade, s->shade, k->filter, s->filter, k->clamp_u, k->clamp_v, s->clamp_u, s->clamp_v,
                        k->flip_u, k->flip_v, s->flip_u, s->flip_v, k->use_alpha, s->use_alpha, k->layer, s->layer, k->cull, s->cull);
        }
        return;
    }
    if (!md->st || set >= md->nb || md->handle || md->failed || nopersist) return;
    md->st[set] = *s;
    if (++md->nst == md->nb) build(md);
}

void native_land_init(void) {
    int on = hle_renderer_cli >= 0 ? hle_renderer_cli : hle_renderer_native;
    cull = hle_land_cull_cli >= 0 ? hle_land_cull_cli : hle_land_cull;
    if (on && hle_strict && !hle_renderer_force) { sa1_log("native land: renderer off (strict run)"); on = 0; }
    if (!on) {
        native_land_window = cull;
        sa1_log("native land: land_cull %s, renderer ta", cull ? "on (models wholly out of view are not drawn)" : "off");
        return;
    }
    if (on == 4) { on = 1; nopersist = 1; }
    if (on == 1 && !draw_mesh_supported()) { sa1_log("native land: this backend has no draw_mesh yet; the TA way"); on = 0; native_land_window = cull; return; }
    mode = on;
    { const char *e = getenv("SA1_NATIVE_CHECK"); check = e && *e == '1'; }
    native_land_window = 1;
    native_land_noland = on == 2;
    sa1_log("native land: land_cull %s, renderer %s", cull ? "on" : "off",
            on == 2 ? "noland (the land's models are not drawn; a measurement aid)" : "native (converted models on the GE)");
}

void native_land_report(void) {
    report_ta();
    if (check) sa1_log("native check: %u of %u set draws with another state than kept, %u of %u lightings with other colours", chk_state, chk_sets, chk_col, chk_lit);
    const uint32_t ta = st.ta_other + st.ta_test[0] + st.ta_test[1] + st.ta_test[2] + st.ta_test[3] + st.ta_test[4];
    if (st.out || st.model || st.sets || ta)
        sa1_log("native land: model draws: out of view %u (%u tris), persistent %u (%u tris), per-set %u, TA %u (%u tris; GE refused: near plane %u, guard band %u, depth split %u, other %u; not converted %u); %u models made persistent",
                st.out, st.out_tris, st.model, st.model_tris, st.sets, ta, st.ta_tris,
                st.ta_test[2], st.ta_test[3], st.ta_test[4], st.ta_test[1] + st.ta_test[0], st.ta_other, st.built);
}
