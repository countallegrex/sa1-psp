/* native_land.c - the land table: models out of view skipped, the rest drawn
 * from converted meshes with hardware transform where the backend can
 * (docs/native-renderer.md; s.6 is this design, iteration 3).
 *
 * land_cull = on (default) | opaque | off (sa1psp.ini, or --land-cull; also
 * in strict runs: it changes only draw scratch, as frame skip's skipped
 * frames do; opaque never culls a translucent model, a console A/B aid) and
 * renderer = native (default) | ta | noland (sa1psp.ini, or --renderer, with
 * force: also in strict runs; strict runs otherwise take ta).  The game's own land draw runs unchanged (its
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
 *   - SPLIT (MODEL or SETS for a model with translucent or env-mapped mesh
 *     sets besides opaque ones, iteration 4): the opaque sets go as MODEL or
 *     SETS (one persistent list for those sets), but the transform and the
 *     lighting run as on the TA path and the other sets are drawn by the
 *     game's strip writers (kamui's translucent sort untouched).
 *   - CELLS (MODEL or SETS for a model the GE refuses whole, near plane,
 *     guard band or split, iteration 5): with the converter's cells (file
 *     version 2) the backend draws it cell by cell (draw.h draw_model_create:
 *     cells out of view skipped, safe ones on the GE, the rest through the
 *     CPU path, near-clipped as the game does); its per-set draws before the
 *     copy exists go the CPU way whole (draw_mesh).
 *   - otherwise the TA path, untouched.
 * Kept colours and persistent copies are per variant (nl_var): per
 * lighting (a model under COLs that turn it is lit per turn) and per set of
 * textures its opaque sets bind (animated texture lists: each frame of the
 * cycle gets its copy).
 * The converted file stays on the card: its tables are read at the land
 * table's change, a model's vertices and indices only for its per-set
 * draws (freed once its persistent copy is drawn), so no copy of the file
 * stays in memory.
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

int hle_renderer_native = 1;        /* sa1psp.ini renderer: 0 ta, 1 native (default), 2 noland */
int hle_land_cull = 1;              /* sa1psp.ini land_cull: 0 off, 1 on (default), 2 opaque */
int hle_land_cull_cli = -1;         /* --land-cull on|opaque|off, wins */
int hle_land_decide_cli;            /* --land-decide new (0, default) | old (1) | check (2) */
static int cull;                    /* in force */
static int cull_alpha;              /* translucent models culled too (land_cull on, not opaque) */
static int decide_old, cull_check;  /* --land-decide old: 62c9300's path; check: both, mismatches counted */
int hle_renderer_cli = -1;          /* backend option, wins */
int hle_renderer_force;             /* also in strict runs (tests) */
int native_land_window;             /* on: land_cull, or renderer native or noland */
int native_land_noland;             /* renderer = noland: the land not drawn at all */
static int mode;                    /* the renderer value in force */
static int check;                   /* SA1_NATIVE_CHECK=1: persistent models go per set and are compared */
static int nopersist;               /* renderer = native-sets: iteration 2's per-set way every frame (a check) */
static uint32_t chk_state, chk_col, chk_sets, chk_lit, chk_logged;
int native_land_all_sets;           /* the decided model is OUT: ta_mesh.c skips every set */

int hle_parse_land_cull(const char *v) {
    return !strcmp(v, "off") ? 0 : !strcmp(v, "opaque") ? 2 : 1;
}

int hle_parse_land_decide(const char *v) {
    return !strcmp(v, "new") ? 0 : !strcmp(v, "old") ? 1 : !strcmp(v, "check") ? 2 : -1;
}

#define USE_ALPHA 0x00100000u
#define ENV_MAP   0x00400000u       /* UVs from the normals (not converted yet): the TA way */
#define LT_CUR    0x8c7596f4u
#define LV_ACT    0x8c7493bau       /* level (16 bits), then act at +2 */
#define ZERO_OFFS 0x8c03cf60u       /* the all-zero offset colours (IGNORE_LIGHT) */
#define MAX_SETS  4096u

typedef struct { uint32_t model; uint16_t set, col, mat, pad; uint32_t vfirst, vcount, ifirst, icount; } nl_batch;
typedef struct { float x, y, z, nx, ny, nz, u, v; uint32_t pt; } nl_vert;
/* A mesh set of a converted model: where it is in the file (vertex and
 * index numbers) and, for an opaque one, in the model's staging copy. */
typedef struct { uint32_t fv, vcount, fi, icount, sv, si; uint8_t nat; } nl_set;
typedef struct { uint32_t model, cfirst, ccount, sfirst, scount; } nl_dir;          /* file v2: a model's cells and spans */
typedef struct { uint16_t set, cell; uint32_t ifirst, icount; } nl_span;
/* One variant of a model: the game lights a model's normals from the light
 * direction in model space (ta_xform.c 0x8C037350: the light at 0x8C038F24
 * through XMTRX's first three columns), so instances under COLs that turn
 * it differently are lit differently.  Each such light (to MODEL_LIGHT_EPS)
 * gets its kept colours and its own persistent copy, and so does each set
 * of textures the opaque sets bind (tex_now: an animated texture list). */
#define MAX_VARS 32u
#define MODEL_LIGHT_EPS 1e-5f
typedef struct {
    float ml[3];                    /* the light in model space */
    uint32_t *tex;                  /* per opaque set, the two words tex_now gives (ngroup x 2) */
    draw_state *st;                 /* per mesh set, as kamui drew it with this variant (freed with the staging) */
    uint32_t *col, *ocol;           /* kept lighting: base and offset colour per point */
    uint32_t nst;                   /* opaque sets drawn per set with these colours (the build at ngroup) */
    uint32_t handle;                /* draw_model_create's, 0: none yet */
    int failed;                     /* draw_model_create refused: the per-set way from now on */
} nl_var;
typedef struct {
    uint32_t model;                 /* guest address, 0x1FFFFFFF-masked */
    float bound[4];                 /* model-space sphere over its points */
    uint32_t ntri;                  /* converted triangles, every set (0: none) */
    uint32_t gtri;                  /* of those, in the opaque sets (the ones drawn natively) */
    int conv;                       /* converted with an opaque, not environment-mapped set: may go natively */
    int mixed;                      /* conv, and translucent or env-mapped sets too: those go the TA way (SPLIT) */
    nl_set *sets;                   /* per mesh set (nset), from the file */
    uint32_t nset, ngroup;          /* mesh sets; of those opaque */
    uint32_t snv, sni;              /* the opaque sets' vertices and indices */
    nl_vert *sv;                    /* staging: the opaque sets' vertices, then their indices (six); NULL when not read */
    uint16_t *six;
    uint32_t npt;                   /* points lit (the model's point count) */
    uint8_t *zoff;                  /* per mesh set: the zero offset colours (IGNORE_LIGHT) */
    nl_var *var;                    /* its lightings (nvar), MAX_VARS at most */
    uint32_t nvar, nbuilt;          /* lightings; of those with a persistent copy */
    int failed;                     /* a build failed: staging stays (that lighting draws per set) */
    int alpha;                      /* has a translucent material */
    uint8_t why;                    /* not converted: 0 not in the file, 1 no opaque set, 2 sets differ, 3 more variants than MAX_VARS (by draw) */
    uint32_t cfirst, ncell, sfirst, nspan;   /* its cells and spans in the file (v2; ncell 0: none) */
    uint32_t gtri_all;              /* triangles the game draws of it, every set (from the model: the counts) */
    uint32_t ta_draws, out_draws;   /* this land table's draws the TA way, out of view */
} nl_model;

static struct {
    uint32_t lt;                    /* the land table this is for (0: none loaded) */
    char path[64];                  /* its converted file */
    uint32_t voff, ioff, nv, ni;    /* the file's vertex and index arrays: offsets, counts */
    uint32_t coff, soff;            /* v2: the cell and span arrays' offsets */
    nl_model *models;               /* sorted by model */
    uint32_t nmodels;
    uint32_t *keys;                 /* models[i].model, packed: the search touches 4 bytes a probe, not a model */
    uint32_t lo, hi;                /* the smallest and largest model address: the rest is no land model */
    uint32_t *succ;                 /* per model, the model found after it last time: the land draw's order stands from frame to frame */
    uint32_t hint;                  /* the last model found */
} L;

/* view_out_fast's plane normals for the viewport last seen (view_normals). */
static struct { uint32_t in[4]; float n[5][3]; int valid; } pc;
static uint32_t chk_dec, chk_mis, chk_mis_logged, pc_miss, n_decide, n_range, n_inr, n_hint;

enum { DEC_NONE, DEC_OUT, DEC_MODEL, DEC_SETS };
static nl_model *dec;               /* the model the game is drawing now, if not the TA way */
static nl_var *dvar;                /* its lighting (MODEL, SETS) */
static int dec_kind, dec_marked;
static float dec_m[16];
static struct { uint32_t out, model, sets, split, ta_test[5], ta_other, ta_var, built, tex_vars, variants, out_tris, model_tris, ta_tris, split_ta_tris;
                uint32_t cell_test[5], cell_tris;           /* refused whole, drawn by cells: why (draw_mesh_test), their opaque triangles */
                uint32_t nc[5], nc_tris[5]; } st;           /* the TA way, not converted: no file or not in it, no opaque set, sets differ, material override, renderer not native */
enum { NC_FILE, NC_NOOPAQUE, NC_DIFF, NC_OVERRIDE, NC_MODE };
/* memory held here (bytes): staging copies now and at most, kept colours and
 * states, the file's tables; persistent models in the PSP backend's layout
 * (draw_gu.c draw_model_create: 24-byte vertices, 16 more where a set has an
 * offset colour, u16 indices, GMODEL_LIST_PER_SET list bytes a set) */
static struct { uint32_t src, src_peak, kept, tables, psp_est, psp_sets, staged, freed, cells; } mem;

static int cmp_batch(const void *a, const void *b) {
    const nl_batch *x = a, *y = b;
    if (x->model != y->model) return x->model < y->model ? -1 : 1;
    if (x->set != y->set) return (int)x->set - (int)y->set;
    return x->vfirst < y->vfirst ? -1 : x->vfirst > y->vfirst;   /* a model under two COLs: its first copy first */
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
                best->why == 3 ? "more variants than MAX_VARS" : best->conv ? "converted (GE refused)" : best->why == 0 ? "not in the file" : best->why == 1 ? "no opaque mesh set" : "sets differ");
        best->ta_draws = 0;
    }
}

static void free_src(nl_model *md) {
    if (!md->sv) return;
    free(md->sv);
    md->sv = NULL; md->six = NULL;
    mem.src -= md->snv * (uint32_t)sizeof(nl_vert) + md->sni * 2u;
}

static void free_var_kept(const nl_model *md, nl_var *v) {
    if (v->col) mem.kept -= md->npt * 8u;
    free(v->col); free(v->ocol);
    v->col = v->ocol = NULL;
}

/* What only the per-set draws and the builds use, once every lighting of
 * the model has its persistent copy (never with SA1_NATIVE_CHECK or
 * native-sets, which keep drawing per set): called from a decision, which
 * runs after the replay that built the last copy, so no record still points
 * into the staging copy.  A lighting met later reads it again (load_src). */
static void free_model_src(nl_model *md) {
    if (md->sv) mem.freed++;
    free_src(md);
    for (uint32_t k = 0; k < md->nvar; k++) {
        if (md->var[k].st) mem.kept -= md->nset * (uint32_t)sizeof *md->var[k].st;
        free(md->var[k].st);
        md->var[k].st = NULL;
    }
    if (md->zoff) mem.kept -= md->nset;
    free(md->zoff);
    md->zoff = NULL;
}

static void unload(void) {
    report_ta();
    for (uint32_t i = 0; i < L.nmodels; i++) {
        nl_model *md = &L.models[i];
        free_model_src(md);
        for (uint32_t k = 0; k < md->nvar; k++) {
            free_var_kept(md, &md->var[k]);
            free(md->var[k].tex);
            if (md->var[k].handle) draw_model_destroy(md->var[k].handle);
        }
        free(md->var);
        free(md->sets);
    }
    free(L.models); free(L.keys);
    memset(&L, 0, sizeof L);
    mem.tables = mem.psp_est = mem.psp_sets = mem.cells = 0;
    dec = NULL; dvar = NULL;
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
        {   /* the triangles its sets draw: lists of 3 or 4 (types 0, 1), strips and N-gons (2, 3: length words) */
            const uint32_t msets = hle_rd32(g + 12u), nms = (uint32_t)(uint16_t)hle_rd16(g + 20u);
            for (uint32_t k = 0; k < nms && hle_in_ram(msets + 24u * k + 4u); k++) {
                const uint32_t ty = (uint32_t)(uint16_t)hle_rd16(msets + 24u * k) >> 14, ns = (uint32_t)(uint16_t)hle_rd16(msets + 24u * k + 2u);
                uint32_t q = hle_rd32(msets + 24u * k + 4u);
                if (ty < 2) { md->gtri_all += ns * (ty + 1u); continue; }
                for (uint32_t j = 0; j < ns && hle_in_ram(q); j++) {
                    const uint32_t n = (uint32_t)(uint16_t)hle_rd16(q) & 0x3FFFu;
                    md->gtri_all += n > 2 ? n - 2 : 0;
                    q += 2u + 2u * n;
                }
            }
        }
        {   /* any translucent material (USE_ALPHA in the attribute flags at +16) */
            const uint32_t mats = hle_rd32(g + 16u), nmat = (uint32_t)(uint16_t)hle_rd16(g + 22u);
            for (uint32_t k = 0; k < nmat && hle_in_ram(mats + 20u * k + 16u); k++) if (hle_rd32(mats + 20u * k + 16u) & USE_ALPHA) md->alpha = 1;
        }
    }
    free(ms);
    if (L.nmodels) {
        L.keys = malloc(2u * L.nmodels * sizeof *L.keys);
        if (L.keys) {
            L.succ = L.keys + L.nmodels;
            for (uint32_t i = 0; i < L.nmodels; i++) { L.keys[i] = L.models[i].model; L.succ[i] = i + 1 < L.nmodels ? i + 1 : 0; }
        }
        L.lo = L.models[0].model; L.hi = L.models[L.nmodels - 1].model;
    }
}

static nl_model *model_of(uint32_t model) {
    model &= 0x1FFFFFFFu;
    if (!decide_old && (model < L.lo || model > L.hi)) return NULL;
    n_inr++;
    if (!L.keys || decide_old) {    /* 62c9300's search */
        uint32_t lo = 0, hi = L.nmodels;
        while (lo < hi) {
            uint32_t mid = (lo + hi) >> 1;
            if (L.models[mid].model < model) lo = mid + 1; else hi = mid;
        }
        return lo < L.nmodels && L.models[lo].model == model ? &L.models[lo] : NULL;
    }
    const uint32_t *k = L.keys;
    const uint32_t h = L.hint, g = L.succ[h];
    if (k[g] == model) { n_hint++; L.hint = g; return &L.models[g]; }
    if (k[h] == model) { n_hint++; return &L.models[h]; }
    uint32_t lo = 0, hi = L.nmodels;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (k[mid] < model) lo = mid + 1; else hi = mid;
    }
    if (lo < L.nmodels && k[lo] == model) { L.succ[h] = lo; L.hint = lo; return &L.models[lo]; }
    return NULL;
}

/* The converted meshes (native only): the file's tables (header, materials,
 * batches) are read here, each model's sets noted with their place in the
 * file; vertices and indices stay on the card until load_src.  A model under
 * several COLs has a copy of its batches per COL (model space, the same
 * bytes): the first copy is taken.  A model is converted when every one of
 * its mesh sets is in the file and one at least is opaque and not
 * environment-mapped (only those go natively: SPLIT when there are others). */
static void load_file(uint32_t lt) {
    char *path = L.path;
    snprintf(path, sizeof L.path, "native/LAND_%u_%u.BIN", (unsigned)hle_rd16(LV_ACT), (unsigned)hle_rd16(LV_ACT + 2));
    uint32_t size = 0, h[12] = { 0 };
    if (sa1_plat_file_size(path, &size) != 0 || size < 36 || sa1_plat_file_read(path, 0, h, 36) != 36) {
        sa1_log("native land: no %s for land table 0x%08x; its models are culled or drawn the TA way", path, lt);
        return;
    }
    /* "SA1N", version, lt, ncol, nbatch, nmat, nvert, nidx, uv scale; v2: models, cells, spans */
    const uint32_t hb = h[1] == 2 ? 48u : 36u;
    if (h[1] == 2 && (size < 48 || sa1_plat_file_read(path, 36, h + 9, 12) != 12)) h[1] = 0;
    const uint32_t nb = h[4], nmat = h[5], nv = h[6], ni = h[7], ndir = h[9], ncell = h[10], nspan = h[11];
    const uint64_t need = hb + (uint64_t)nmat * 16 + (uint64_t)nb * sizeof(nl_batch) + (uint64_t)nv * sizeof(nl_vert) + ((uint64_t)ni * 2 + 3u) / 4u * 4u +
                          (uint64_t)ndir * sizeof(nl_dir) + (uint64_t)ncell * sizeof(draw_model_cell) + (uint64_t)nspan * sizeof(nl_span);
    if (memcmp(h, "SA1N", 4) || (h[1] != 1 && h[1] != 2) || (h[2] & 0x1FFFFFFFu) != (lt & 0x1FFFFFFFu) || need > size) {
        sa1_log("native land: %s does not match land table 0x%08x; its models are culled or drawn the TA way", path, lt);
        return;
    }
    uint32_t *mats = malloc((nmat ? nmat : 1) * 16u);
    nl_batch *b = malloc((nb ? nb : 1) * sizeof *b);
    if (!mats || !b || sa1_plat_file_read(path, hb, mats, nmat * 16u) != (int)(nmat * 16u) ||
        sa1_plat_file_read(path, hb + nmat * 16u, b, nb * (uint32_t)sizeof *b) != (int)(nb * sizeof *b)) {
        free(mats); free(b);
        sa1_log("native land: %s could not be read; its models are culled or drawn the TA way", path);
        return;
    }
    L.nv = nv; L.ni = ni;
    L.voff = hb + nmat * 16u + nb * (uint32_t)sizeof *b;
    L.ioff = L.voff + nv * (uint32_t)sizeof(nl_vert);
    const uint32_t doff = L.ioff + (ni * 2u + 3u) / 4u * 4u;
    L.coff = doff + ndir * (uint32_t)sizeof(nl_dir);
    L.soff = L.coff + ncell * (uint32_t)sizeof(draw_model_cell);
    for (uint32_t i = 0; i < nb; i++) b[i].model &= 0x1FFFFFFFu;
    qsort(b, nb, sizeof *b, cmp_batch);
    uint32_t nwhole = 0, nsplit = 0, nnone = 0, ndiff = 0, ndup = 0;
    for (uint32_t i = 0; i < nb; ) {
        const uint32_t m = b[i].model;
        uint32_t j = i;
        while (j < nb && b[j].model == m) j++;
        nl_model *md = model_of(m);
        const uint32_t nset = hle_rd16((m | 0x80000000u) + 20u);
        if (!md) { i = j; continue; }
        nl_set *sets = nset && nset <= MAX_SETS ? calloc(nset, sizeof *sets) : NULL;
        uint32_t k = 0, ntri = 0, gtri = 0, ng = 0, snv = 0, sni = 0;
        int ok = sets != NULL, dup = 0;
        for (uint32_t q = i; q < j && ok; q++) {
            const nl_batch *x = &b[q];
            if (k && x->set == k - 1) { dup = 1; continue; }   /* another COL's copy */
            if (x->set != k || k >= nset || x->mat >= nmat || x->vfirst + x->vcount > nv || x->ifirst + x->icount > ni ||
                x->vfirst + x->vcount < x->vfirst || x->ifirst + x->icount < x->ifirst) { ok = 0; break; }
            nl_set *s = &sets[k++];
            s->fv = x->vfirst; s->vcount = x->vcount; s->fi = x->ifirst; s->icount = x->icount;
            s->nat = !(mats[4 * x->mat + 1] & (USE_ALPHA | ENV_MAP));
            ntri += x->icount / 3;
            if (s->nat) { s->sv = snv; s->si = sni; snv += x->vcount; sni += x->icount; gtri += x->icount / 3; ng++; }
        }
        if (k != nset) ok = 0;
        ndup += dup;
        md->ntri = ok ? ntri : 0;
        md->why = ok ? 1 : 2;
        if (ok && ng && md - L.models < 4096) {   /* native_land_set's tag holds the model's index in 12 bits */
            md->conv = 1; md->mixed = ng < nset; md->sets = sets; sets = NULL;
            md->nset = nset; md->ngroup = ng; md->snv = snv; md->sni = sni; md->gtri = gtri;
            mem.tables += nset * (uint32_t)sizeof *md->sets;
            if (md->mixed) nsplit++; else nwhole++;
        } else if (ok) nnone++;
        else ndiff++;
        free(sets);
        i = j;
    }
    free(mats); free(b);
    uint32_t ncm = 0;
    if (ndir) {                         /* v2: each model's cells and spans (read at its builds) */
        nl_dir *d = malloc(ndir * sizeof *d);
        if (d && sa1_plat_file_read(path, doff, d, ndir * (uint32_t)sizeof *d) == (int)(ndir * sizeof *d)) {
            for (uint32_t i = 0; i < ndir; i++) {
                nl_model *md = model_of(d[i].model);
                if (!md || !md->conv || !d[i].ccount || !d[i].scount || d[i].cfirst + d[i].ccount > ncell || d[i].sfirst + d[i].scount > nspan ||
                    d[i].cfirst + d[i].ccount < d[i].cfirst || d[i].sfirst + d[i].scount < d[i].sfirst) continue;
                md->cfirst = d[i].cfirst; md->ncell = d[i].ccount; md->sfirst = d[i].sfirst; md->nspan = d[i].scount;
                ncm++;
            }
        }
        free(d);
    }
    sa1_log("native land: %s for land table 0x%08x: %u mesh sets, %u vertices, %u triangles; %u cells (%u models)", path, lt, nb, nv, ni / 3, ncell, ncm);
    sa1_log("native land: of %u models %u go natively whole, %u split (opaque sets natively, the rest the TA way), %u have no opaque set, %u sets differ, %u not in the file; %u under several COLs",
            L.nmodels, nwhole, nsplit, nnone, ndiff, L.nmodels - nwhole - nsplit - nnone - ndiff, ndup);
}

/* A model's opaque sets from the file, for its per-set draws and the build:
 * read at its first native draw, freed after its persistent copy is drawn
 * (free_model_src).  0: not read (then the TA way). */
static int load_src(nl_model *md) {
    if (md->sv) return 1;
    const uint32_t vb = md->snv * (uint32_t)sizeof(nl_vert), bytes = vb + md->sni * 2u;
    uint8_t *p = malloc(bytes ? bytes : 1);
    if (!p) return 0;
    nl_vert *v = (nl_vert *)(void *)p;
    uint16_t *ix = (uint16_t *)(void *)(p + vb);
    for (uint32_t s = 0; s < md->nset; s++) {
        const nl_set *x = &md->sets[s];
        if (!x->nat) continue;
        const uint32_t n1 = x->vcount * (uint32_t)sizeof(nl_vert), n2 = x->icount * 2u;
        if ((n1 && sa1_plat_file_read(L.path, L.voff + x->fv * (uint32_t)sizeof(nl_vert), v + x->sv, n1) != (int)n1) ||
            (n2 && sa1_plat_file_read(L.path, L.ioff + x->fi * 2u, ix + x->si, n2) != (int)n2)) { free(p); return 0; }
    }
    md->sv = v; md->six = ix;
    mem.src += bytes; mem.staged++;
    if (mem.src > mem.src_peak) mem.src_peak = mem.src;
    return 1;
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

/* view_out without game_matrix: its plane k, n = (p0 sx, p1 sy, p0 cx +
 * p1 cy + p2) on XMTRX's rows 0..2, is n . (X, Y, Z) at the sphere's centre
 * moved by XMTRX, so the centre is moved once (9 multiplies) and each plane
 * costs three; the plane's length in model space (n on each of XMTRX's
 * three columns) only when the centre is outside it.  The n change with
 * the viewport only (four words, compared per call).  The same half-spaces
 * as view_out, rounded in another order: --land-decide check counts the
 * decisions that differ. */
static void view_normals(void) {
    const uint32_t in[4] = { hle_rd32(0x8c8ffe00u), hle_rd32(0x8c8ffe04u), hle_rd32(0x8c8ffe08u), hle_rd32(0x8c8ffe0cu) };
    if (pc.valid && !memcmp(in, pc.in, sizeof in)) return;
    pc_miss++;
    memcpy(pc.in, in, sizeof in);
    pc.valid = 1;
    float f[4];
    memcpy(f, in, sizeof f);
    const float cx = f[0], cy = f[1], sx = f[2], sy = f[3] * sx;
    static const float pl[5][3] = {   /* view_out's */
        { 0, 0, 1 }, { 1, 0, -(DRAW_WIDE_LEFT - 8.0f) }, { -1, 0, DRAW_WIDE_RIGHT + 8.0f }, { 0, 1, 8.0f }, { 0, -1, 488.0f } };
    for (int k = 0; k < 5; k++) {
        pc.n[k][0] = pl[k][0] * sx;
        pc.n[k][1] = pl[k][1] * sy;
        pc.n[k][2] = pl[k][0] * cx + pl[k][1] * cy + pl[k][2];
    }
}

static int view_out_fast(const sh4_ctx *c, const float *b) {
    view_normals();
    const float *x = c->xf;
    const float X = x[0] * b[0] + x[4] * b[1] + x[8] * b[2] + x[12];
    const float Y = x[1] * b[0] + x[5] * b[1] + x[9] * b[2] + x[13];
    const float Z = x[2] * b[0] + x[6] * b[1] + x[10] * b[2] + x[14];
    for (int k = 0; k < 5; k++) {
        const float *n = pc.n[k];
        const float d = n[0] * X + n[1] * Y + n[2] * Z;
        if (d >= 0.0f) continue;
        const float a0 = n[0] * x[0] + n[1] * x[1] + n[2] * x[2];
        const float a1 = n[0] * x[4] + n[1] * x[5] + n[2] * x[6];
        const float a2 = n[0] * x[8] + n[1] * x[9] + n[2] * x[10];
        if (d * d > b[3] * b[3] * (a0 * a0 + a1 * a1 + a2 * a2)) return 1;
    }
    return 0;
}

/* The texture an opaque set of the model would bind now, as ta_mesh.c's
 * mesh_638 finds it: the current texture list's entry for the material's
 * texture number, its texture pointer and that texture's first word.  Water
 * and shore textures are animated by changing the list's entries (Emerald
 * Coast: one material's texture cycles every few frames), which a persistent
 * copy's states would freeze. */
#define TEXLIST_CUR 0x8c673480u
static void tex_now(uint32_t model, uint32_t set, uint32_t *t) {
    const uint32_t g = model | 0x80000000u;
    const uint32_t mi = (uint32_t)hle_rd16(hle_rd32(g + 12u) + 24u * set) & 0x3FFFu, mat = hle_rd32(g + 16u) + 20u * mi;
    t[0] = t[1] = 0;
    uint32_t fl = hle_rd32(mat + 16u);
    if (hle_rd32(0x8c8ffe1cu)) fl = (fl & hle_rd32(0x8c8ffe20u)) | hle_rd32(0x8c8ffe24u);
    if (!(fl & 0x00200000u)) return;
    const uint32_t tl = hle_rd32(TEXLIST_CUR);
    if (!hle_in_ram(tl) || !hle_in_ram(hle_rd32(tl))) return;
    const uint32_t e = hle_rd32(tl) + 12u * (hle_rd32(mat + 12u) & 0x3fffffffu) + 8u;
    if (!hle_in_ram(e)) return;
    t[0] = hle_rd32(e);
    if (hle_in_ram(t[0])) t[1] = hle_rd32(t[0]);
}

/* From the transform's wrapper, XMTRX set: 1 = not the TA way (the transform
 * is skipped), 0 = the TA way.  The answer stands until the next model's
 * transform (lighting and mesh loop ask native_land_model_ok). */
int native_land_decide(sh4_ctx *c, uint32_t model) {
    dec = NULL; dvar = NULL; dec_kind = DEC_NONE; dec_marked = 0; native_land_all_sets = 0;
    if (!native_land_window) return 0;
    n_decide++;
    nl_model *md = find_model(model);
    if (!md) return 0;
    n_range++;
    if (native_land_noland) { dec = md; dec_kind = DEC_OUT; native_land_all_sets = 1; return 1; }
    /* land_cull opaque: models with a translucent material are not culled (once
     * needed for the console's picture: the GE backend's batching put a
     * translucent run's offset colour after the whole run, so another run
     * order changed pixels; draw_gu.c's strip_emit breaks those runs now) */
    int out = 0;
    if (decide_old) game_matrix(c, dec_m);    /* 62c9300: for every land model */
    if (cull && (!md->alpha || cull_alpha)) {
        if (decide_old) out = view_out(dec_m, md->bound);
        else {
            out = view_out_fast(c, md->bound);
            if (cull_check) {
                float m[16];
                game_matrix(c, m);
                const int old = view_out(m, md->bound);
                chk_dec++;
                if (old != out) { chk_mis++; if (chk_mis_logged++ < 10) sa1_log("land check: f%u model 0x%08x: old %d new %d", hle_frame_count(), md->model, old, out); }
            }
        }
    }
    if (out) { dec = md; dec_kind = DEC_OUT; native_land_all_sets = 1; st.out++; st.out_tris += md->ntri; md->out_draws++; return 1; }
    /* the material override (0x8C8FFE1C: enable, AND, OR; the land draw sets it) may turn
     * USE_ALPHA or the environment map on: then the TA way */
    if (mode != 1 || !md->conv || (hle_rd32(0x8c8ffe1cu) && (hle_rd32(0x8c8ffe24u) & (USE_ALPHA | ENV_MAP)))) {
        const int r = mode != 1 ? NC_MODE : md->conv ? NC_OVERRIDE : md->why == 2 ? NC_DIFF : md->ntri ? NC_NOOPAQUE : NC_FILE;
        st.nc[r]++; st.nc_tris[r] += md->gtri_all;
        st.ta_other++; st.ta_tris += md->gtri_all; md->ta_draws++;
        return 0;
    }
    if (!decide_old) game_matrix(c, dec_m);   /* only the native renderer reads it */
    int p = draw_mesh_test(dec_m, md->bound);
    if (p < 0 && p != DRAW_MESH_NO_OTHER && draw_mesh_ge_whole(dec_m, md->bound)) p = 0;   /* on the GE whole in the near pass or both (draw.h) */
    const int by_cells = p < 0 && p != DRAW_MESH_NO_OTHER && md->ncell;   /* refused whole: cell by cell (CELLS) */
    if (p < 0 && !by_cells) { st.ta_test[-p]++; st.ta_tris += md->gtri_all; md->ta_draws++; return 0; }
    /* the lighting: the light in model space, as the lighting will take it */
    const float lx = hle_rdf(0x8c038f24u), ly = hle_rdf(0x8c038f28u), lz = hle_rdf(0x8c038f2cu);
    const float *x = c->xf;
    const float ml[3] = { lx * x[0] + ly * x[1] + lz * x[2], lx * x[4] + ly * x[5] + lz * x[6], lx * x[8] + ly * x[9] + lz * x[10] };
    /* the textures the opaque sets bind now: water and shore textures are
     * animated by changing the texture list's entries (Emerald Coast), which
     * a persistent copy's states would freeze, so they are part of the variant */
    static uint32_t tnow[2 * MAX_SETS];
    for (uint32_t s = 0, g = 0; s < md->nset; s++) if (md->sets[s].nat) { tex_now(md->model, s, tnow + 2 * g); g++; }
    const size_t tb = 2u * md->ngroup * sizeof *tnow;
    nl_var *v = NULL;
    int same_light = 0;
    for (uint32_t k = 0; k < md->nvar && !v; k++) {
        const float *q = md->var[k].ml;
        if (fabsf(q[0] - ml[0]) <= MODEL_LIGHT_EPS && fabsf(q[1] - ml[1]) <= MODEL_LIGHT_EPS && fabsf(q[2] - ml[2]) <= MODEL_LIGHT_EPS) {
            same_light = 1;
            if (md->var[k].tex && !memcmp(md->var[k].tex, tnow, tb)) v = &md->var[k];
        }
    }
    if (!v) {
        if (!md->var) md->var = calloc(MAX_VARS, sizeof *md->var);
        uint32_t *tx = md->var && md->nvar < MAX_VARS ? malloc(tb ? tb : 1) : NULL;
        if (!tx) { st.ta_var++; st.ta_tris += md->gtri_all; md->ta_draws++; md->why = 3; return 0; }
        memcpy(tx, tnow, tb);
        v = &md->var[md->nvar++];
        memcpy(v->ml, ml, sizeof ml);
        v->tex = tx;
        if (same_light) st.tex_vars++;
    }
    const int kind = v->handle && !check ? DEC_MODEL : DEC_SETS;
    if (kind == DEC_SETS && !load_src(md)) { md->conv = 0; st.ta_other++; st.ta_tris += md->gtri_all; md->ta_draws++; return 0; }
    if (kind == DEC_MODEL && !nopersist) {
        free_var_kept(md, v);
        if (md->nbuilt == md->nvar && !md->failed && (md->sv || md->zoff)) free_model_src(md);
    }
    dec = md;
    dvar = v;
    dec_kind = kind;
    if (dec_kind == DEC_MODEL) { st.model++; st.model_tris += md->gtri; } else st.sets++;
    if (by_cells) { st.cell_test[-p]++; st.cell_tris += md->gtri; }
    /* SPLIT: the game transforms (its strip writers draw the other sets from
     * the transformed points); the opaque sets still go natively */
    if (md->mixed) { st.split++; st.split_ta_tris += md->ntri - md->gtri; return 0; }
    return 1;
}

int native_land_model_ok(uint32_t model) {
    return dec && dec->model == (model & 0x1FFFFFFFu);
}

/* From the lighting's wrapper for a model not drawn the TA way: 1 = skip the
 * lighting, 0 = run it, then call native_land_lit(). */
int native_land_skip_light(uint32_t model) {
    if (!native_land_model_ok(model) || dec->mixed) return 0;   /* SPLIT: the TA sets need the lighting */
    return dec_kind != DEC_SETS || (dvar->col != NULL && !(check && dvar->handle));
}
void native_land_lit(sh4_ctx *c, uint32_t model) {
    if (!native_land_model_ok(model) || dec_kind != DEC_SETS || !dvar) return;
    nl_var *v = dvar;
    if (check && v->col && v->handle) {
        const uint32_t vbuf = hle_rd32(c->gbr + 60), offs = hle_rd32(c->gbr + 64);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < dec->npt; i++)
            if (v->col[i] != hle_rd32(vbuf + 16u * i + 12u) || (offs && v->ocol[i] != hle_rd32(offs + 4u * i))) bad++;
        chk_lit++;
        if (bad) { chk_col++; if (chk_logged++ < 20) sa1_log("native check: model 0x%08x lighting %u: %u of %u kept colours differ", dec->model, (unsigned)(v - dec->var), bad, dec->npt); }
        return;
    }
    if (v->col || !dec->npt) return;
    uint32_t *col = malloc(dec->npt * 4u), *ocol = malloc(dec->npt * 4u);
    if (!col || !ocol) { free(col); free(ocol); return; }
    mem.kept += dec->npt * 8u;
    const uint32_t vbuf = hle_rd32(c->gbr + 60), offs = hle_rd32(c->gbr + 64);
    for (uint32_t i = 0; i < dec->npt; i++) {
        col[i] = hle_rd32(vbuf + 16u * i + 12u);
        ocol[i] = offs ? hle_rd32(offs + 4u * i) : 0;
    }
    v->col = col; v->ocol = ocol;
}

/* One converted vertex with a lighting's kept colours. */
static void vertex(const nl_model *md, const nl_var *lv, const nl_vert *s, int zero_off, draw_mesh_vertex *o) {
    o->x = s->x; o->y = s->y; o->z = s->z; o->u = s->u; o->v = s->v;
    const int k = lv->col && s->pt < md->npt;
    o->color = k ? lv->col[s->pt] : 0xFFFFFFFFu;
    o->ocolor = k && !zero_off ? lv->ocol[s->pt] : 0;
}

/* Mesh set `set` of the model being drawn: returns 1 and the TA pointer
 * after anything written in *ta_out, or 0 (then the caller draws it the TA
 * way; not expected once decided). */
int native_land_set(sh4_ctx *c, uint32_t model, uint32_t set, uint32_t ta, uint32_t *ta_out) {
    if (!native_land_model_ok(model)) return 0;
    if (dec_kind == DEC_OUT) { *ta_out = ta; return 1; }
    /* not one of the model's opaque sets (the material override cleared its
     * USE_ALPHA): not in the persistent copy, so the TA way (SPLIT models
     * only: a model without such sets never has one) */
    if (set >= dec->nset || !dec->sets[set].nat) return 0;
    if (dec_kind == DEC_MODEL) {    /* one marker for the model's opaque sets, at the first */
        uint32_t rec;
        if (!dec_marked && kamui_native_model(dvar->handle, dec_m, &rec)) {
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
    const nl_set *b = &md->sets[set];
    if (!md->sv) return 0;
    if (!md->zoff && (md->zoff = calloc(md->nset, 1)) != NULL) mem.kept += md->nset;
    if (!dvar->st && (dvar->st = calloc(md->nset, sizeof *dvar->st)) != NULL) mem.kept += md->nset * (uint32_t)sizeof *dvar->st;
    const uint32_t ocol = hle_rd32(c->gbr + 56);
    const int zero_off = ocol == ZERO_OFFS || !ocol;
    if (md->zoff) md->zoff[set] = (uint8_t)zero_off;
    uint32_t rec;
    const uint32_t tag = (uint32_t)(md - L.models) << 20 | (uint32_t)(dvar - md->var) << 12 | set;
    draw_mesh_vertex *out = kamui_native_mesh(b->vcount, dec_m, md->bound, md->six + b->si, b->icount, tag, &rec);
    if (!out) return 0;
    for (uint32_t i = 0; i < b->vcount; i++) vertex(md, dvar, &md->sv[b->sv + i], zero_off, &out[i]);
    hle_wr32(ta, 0xC0000001u);       /* kamui: a native mesh record (type 6, kind 1) */
    hle_wr32(ta + 4, rec);
    hle_wr32(ta + 8, 0);
    *ta_out = ta + 32;
    return 1;
}

/* A lighting's persistent copy, once every opaque set has drawn with it. */
static void build(nl_model *md, nl_var *lv) {
    const uint32_t nv = md->snv, ni = md->sni;
    draw_mesh_vertex *v = malloc((nv ? nv : 1) * sizeof *v);
    uint16_t *ix = malloc((ni ? ni : 1) * 2u);
    draw_model_set *sets = malloc(md->ngroup * sizeof *sets);
    if (v && ix && sets && md->sv && lv->st && md->zoff) {
        uint32_t n = 0;
        int off = 0;
        for (uint32_t s = 0; s < md->nset; s++) {
            const nl_set *b = &md->sets[s];
            if (!b->nat) continue;
            for (uint32_t i = 0; i < b->vcount; i++) {
                vertex(md, lv, &md->sv[b->sv + i], md->zoff[s], &v[b->sv + i]);
                if (lv->st[s].texture && (v[b->sv + i].ocolor & 0x00FFFFFFu)) off = 1;
            }
            sets[n].state = lv->st[s];
            sets[n].vfirst = b->sv; sets[n].vcount = b->vcount; sets[n].ifirst = b->si; sets[n].icount = b->icount;
            n++;
        }
        memcpy(ix, md->six, ni * 2u);
        /* the cells (file v2): spans to the copy's sets (opaque ones, in order) and indices */
        draw_model_cell *cells = NULL;
        draw_model_span *spans = NULL;
        nl_span *fs = NULL;
        uint32_t ns = 0;
        if (md->ncell) {
            cells = malloc(md->ncell * sizeof *cells);
            spans = malloc(md->nspan * sizeof *spans);
            fs = malloc(md->nspan * sizeof *fs);
            uint32_t *grp = calloc(md->nset, sizeof *grp);
            if (cells && spans && fs && grp &&
                sa1_plat_file_read(L.path, L.coff + md->cfirst * (uint32_t)sizeof *cells, cells, md->ncell * (uint32_t)sizeof *cells) == (int)(md->ncell * sizeof *cells) &&
                sa1_plat_file_read(L.path, L.soff + md->sfirst * (uint32_t)sizeof *fs, fs, md->nspan * (uint32_t)sizeof *fs) == (int)(md->nspan * sizeof *fs)) {
                for (uint32_t s = 0, k = 0; s < md->nset; s++) if (md->sets[s].nat) grp[s] = k++;
                for (uint32_t k = 0; k < md->nspan; k++) {
                    const nl_span *q = &fs[k];
                    if (q->set >= md->nset || !md->sets[q->set].nat || q->cell >= md->ncell || q->ifirst + q->icount > md->sets[q->set].icount) { ns = 0; break; }
                    spans[ns].ifirst = md->sets[q->set].si + q->ifirst; spans[ns].icount = q->icount;
                    spans[ns].cell = q->cell; spans[ns].set = (uint16_t)grp[q->set];
                    ns++;
                }
            }
            free(grp);
        }
        /* another set of textures under a lighting already copied: that copy's
         * vertices with these states (draw_model_variant) */
        const nl_var *base = NULL;
        for (uint32_t k = 0; k < md->nvar && !base; k++) {
            const nl_var *o = &md->var[k];
            if (o != lv && o->handle && fabsf(o->ml[0] - lv->ml[0]) <= MODEL_LIGHT_EPS && fabsf(o->ml[1] - lv->ml[1]) <= MODEL_LIGHT_EPS &&
                fabsf(o->ml[2] - lv->ml[2]) <= MODEL_LIGHT_EPS) base = o;
        }
        if (base) {
            draw_state *sts = malloc(n * sizeof *sts);
            if (sts) {
                for (uint32_t k = 0; k < n; k++) sts[k] = sets[k].state;
                lv->handle = draw_model_variant(base->handle, sts, (int)n);
            }
            free(sts);
            if (lv->handle) { st.built++; st.variants++; md->nbuilt++; mem.psp_sets += n; mem.psp_est += 768u * n + 64u + 48u * n; }
        }
        if (!lv->handle) {
            lv->handle = draw_model_create(v, (int)nv, ix, (int)ni, sets, (int)n, md->bound, cells, ns ? (int)md->ncell : 0, spans, (int)ns);
            if (lv->handle && ns) mem.cells += md->ncell * (uint32_t)sizeof *cells + ns * (uint32_t)sizeof *spans + nv * 8u;   /* draw_gu.c: the colours */
            if (lv->handle) {
                st.built++; md->nbuilt++;
                mem.psp_sets += n;   /* draw_gu.c's layout, 64-byte rounded */
                mem.psp_est += ((nv * 24u + 63u) & ~63u) + (off ? ((nv * 16u + 63u) & ~63u) : 0u) + ((ni * 2u + 63u) & ~63u) + 768u * n + 64u + 48u * n;
            }
        }
        free(cells); free(spans); free(fs);
    }
    if (!lv->handle) { lv->failed = 1; md->failed = 1; }
    free(v); free(ix); free(sets);
}

void native_land_captured(uint32_t tag, const draw_state *s) {
    const uint32_t i = tag >> 20, vi = (tag >> 12) & 0xFFu, set = tag & 0xFFFu;
    if (i >= L.nmodels) return;
    nl_model *md = &L.models[i];
    if (vi >= md->nvar || set >= md->nset || !md->sets[set].nat || nopersist) return;
    nl_var *v = &md->var[vi];
    if (!v->st) return;
    if (check && v->handle) {       /* the states kept with this variant's copy */
        chk_sets++;
        if (memcmp(&v->st[set], s, sizeof *s)) {
            chk_state++;
            const draw_state *k = &v->st[set];
            if (chk_logged++ < 40)
                sa1_log("native check: model 0x%08x set %u: state differs: texture %u/%u blend %u,%u/%u,%u fog %u/%u shade %u/%u filter %u/%u clamp %u%u/%u%u flip %u%u/%u%u alpha %u/%u layer %u/%u cull %u/%u",
                        md->model, set, k->texture, s->texture, k->blend_src, k->blend_dst, s->blend_src, s->blend_dst, k->fog, s->fog,
                        k->shade, s->shade, k->filter, s->filter, k->clamp_u, k->clamp_v, s->clamp_u, s->clamp_v,
                        k->flip_u, k->flip_v, s->flip_u, s->flip_v, k->use_alpha, s->use_alpha, k->layer, s->layer, k->cull, s->cull);
        }
    } else v->st[set] = *s;
    if (v->handle || v->failed) return;
    if (++v->nst == md->ngroup) build(md, v);
}

/* --strip-census: which 0x8C037990 corners f7 slice 1 could take (textured,
 * opaque list, not environment-mapped, a dynamic node whose own sphere
 * passes draw_mesh_test), and the points of nodes made only of such sets
 * (the transform slice 1 skips).  A measurement aid: the marker's fourth
 * word carries the tag (draw scratch only). */
int hle_strip_census;
#define CN_MAX 1024u
#define CN_MAGIC 0x5C000000u
enum { CN_SEEN = 1, CN_BAD = 2, CN_LAND = 4, CN_PASS = 8 };
static uint32_t cn_seq, cn_cur = CN_MAX, cn_pts[CN_MAX];
static uint8_t cn_st[CN_MAX];
static uint64_t cn_nodes, cn_nodes_land, cn_nodes_lnat, cn_res[5], cn_over, cn_frames;
static uint64_t cn_wr_all, cn_wr_env, cn_wr_land, cn_wr_lnat, cn_op, cn_op_tex, cn_elig, cn_untagged, cn_pts_dyn, cn_pts_whole, cn_pts_pass;
void census_node(sh4_ctx *c, uint32_t model, int land_native) {
    if (land_native) { cn_cur = CN_MAX; cn_nodes_lnat++; return; }     /* land on the native path: its TA-way sets */
    if (cn_seq >= CN_MAX) { cn_over++; cn_cur = CN_MAX - 1u; return; }
    const uint32_t s = cn_seq++;
    cn_cur = s;
    cn_nodes++;
    const int land = find_model(model) != NULL;
    float m[16], b[4] = { hle_rdf(model + 24), hle_rdf(model + 28), hle_rdf(model + 32), hle_rdf(model + 36) };
    game_matrix(c, m);
    const int p = draw_mesh_test(m, b);
    cn_res[p >= 0 ? 0 : -p <= 4 ? -p : 1]++;
    cn_pts[s] = hle_rd32(model + 8);
    cn_st[s] = (uint8_t)((land ? CN_LAND : 0) | (p >= 0 ? CN_PASS : 0));
    if (land) cn_nodes_land++;
}
uint32_t census_writer(uint32_t corners, int env) {
    cn_wr_all += corners;
    if (env) cn_wr_env += corners;
    if (cn_cur == CN_MAX) { cn_wr_lnat += corners; return CN_MAGIC | (env ? 0x1000u : 0) | 0x3FFu; }
    if (cn_st[cn_cur] & CN_LAND) cn_wr_land += corners;
    return CN_MAGIC | (env ? 0x1000u : 0) | cn_cur;
}
void census_marker(uint32_t tag, uint32_t n, int list, int textured, int sorted) {
    if ((tag & 0xFF000000u) != CN_MAGIC) { cn_untagged += n; return; }
    const uint32_t s = tag & 0x3FFu;
    const int env = (tag & 0x1000u) != 0, node = s < cn_seq && s < CN_MAX;
    if (node) cn_st[s] |= CN_SEEN;
    if (list == 0 && !sorted) {
        cn_op += n;
        if (textured) cn_op_tex += n;
        if (textured && !env && node && (cn_st[s] & (CN_LAND | CN_PASS)) == CN_PASS) { cn_elig += n; return; }
    }
    if (node) cn_st[s] |= CN_BAD;
}
void census_frame(void) {
    if (!hle_strip_census) return;
    for (uint32_t s = 0; s < cn_seq; s++) {
        if (cn_st[s] & CN_LAND) continue;
        cn_pts_dyn += cn_pts[s];
        if (cn_st[s] & CN_PASS) cn_pts_pass += cn_pts[s];
        if ((cn_st[s] & (CN_SEEN | CN_BAD | CN_PASS)) == (CN_SEEN | CN_PASS)) cn_pts_whole += cn_pts[s];
    }
    cn_seq = 0; cn_cur = CN_MAX;
    cn_frames++;
}
static void census_report(void) {
    if (!hle_strip_census || !cn_frames) return;
    const double f = (double)cn_frames;
    sa1_log("strip census: %llu frames; per frame: nodes %.1f (TA-way land %.1f, native land %.1f), sphere pass %.1f / near %.1f / guard %.1f / split %.1f / other %.1f; %llu over the node cap",
            (unsigned long long)cn_frames, cn_nodes / f, cn_nodes_land / f, cn_nodes_lnat / f,
            cn_res[0] / f, cn_res[2] / f, cn_res[3] / f, cn_res[4] / f, cn_res[1] / f, (unsigned long long)cn_over);
    const double dyn = (double)(cn_wr_all - cn_wr_land - cn_wr_lnat);
    sa1_log("strip census: writer corners/frame %.0f (env %.0f, TA-way land %.0f, native-land leftovers %.0f, dynamic %.0f); opaque-list corners %.0f (textured %.0f); ELIGIBLE %.0f = %.1f %% of dynamic, %.1f %% of all; untagged %.0f",
            cn_wr_all / f, cn_wr_env / f, cn_wr_land / f, cn_wr_lnat / f, dyn / f, cn_op / f, cn_op_tex / f, cn_elig / f,
            dyn > 0 ? 100.0 * cn_elig / dyn : 0.0, cn_wr_all ? 100.0 * cn_elig / cn_wr_all : 0.0, cn_untagged / f);
    sa1_log("strip census: dynamic points/frame %.0f, in sphere-passing nodes %.0f, in whole-eligible nodes %.0f = %.1f %%",
            cn_pts_dyn / f, cn_pts_pass / f, cn_pts_whole / f, cn_pts_dyn ? 100.0 * cn_pts_whole / cn_pts_dyn : 0.0);
}

void native_land_init(void) {
    if (hle_strip_census && hle_strict) { sa1_log("strip census: off (strict run)"); hle_strip_census = 0; }
    if (hle_strip_census) sa1_log("strip census: on (a measurement aid)");
    int on = hle_renderer_cli >= 0 ? hle_renderer_cli : hle_renderer_native;
    const int lc = hle_land_cull_cli >= 0 ? hle_land_cull_cli : hle_land_cull;
    cull = lc != 0;
    cull_alpha = lc == 1;
    decide_old = hle_land_decide_cli == 1;
    cull_check = hle_land_decide_cli == 2;
    if (lc == 2) sa1_log("native land: land_cull opaque (translucent models never culled)");
    if (decide_old) sa1_log("native land: land decision the old way (game_matrix, view_out, binary search)");
    if (cull_check) sa1_log("native land: land decision checked against the old way");
    if (on && hle_strict && !hle_renderer_force) { sa1_log("native land: renderer off (strict run)"); on = 0; }
    if (!on) {
        native_land_window = cull;
        sa1_log("native land: land_cull %s, renderer ta", cull ? "on (models wholly out of view are not drawn)" : "off");
        return;
    }
    if (on == 4) { on = 1; nopersist = 1; }
    if (on == 1 && !draw_mesh_supported()) { sa1_log("native land: this backend has no draw_mesh yet; the TA way"); on = 0; native_land_window = cull; return; }
    mode = on;
    if (on == 1) draw_set_native(1);   /* the backend may put land on the GE in its near pass too (draw.h) */
    { const char *e = getenv("SA1_NATIVE_CHECK"); check = e && *e == '1'; }
    native_land_window = 1;
    native_land_noland = on == 2;
    sa1_log("native land: land_cull %s, renderer %s", cull ? "on" : "off",
            on == 2 ? "noland (the land's models are not drawn; a measurement aid)" : "native (converted models on the GE)");
}

void native_land_report(void) {
    census_report();
    report_ta();
    if (cull_check) sa1_log("land check: %u of %u cull decisions differ from view_out's", chk_mis, chk_dec);
    if (n_decide) sa1_log("native land: %u transforms asked, %u in the land range, %u land models (%u by the hint); plane normals made %u times", n_decide, n_inr, n_range, n_hint, pc_miss);
    if (check) sa1_log("native check: %u of %u set draws with another state than kept, %u of %u lightings with other colours", chk_state, chk_sets, chk_col, chk_lit);
    const uint32_t ta = st.ta_other + st.ta_var + st.ta_test[0] + st.ta_test[1] + st.ta_test[2] + st.ta_test[3] + st.ta_test[4];
    if (st.out || st.model || st.sets || ta)
    {   /* three lines: sa1_log cuts a line at 255 characters */
        sa1_log("native land: model draws: out of view %u (%u tris), persistent %u (%u tris), per-set %u; split %u (their translucent or env-mapped sets the TA way: %u tris)",
                st.out, st.out_tris, st.model, st.model_tris, st.sets, st.split, st.split_ta_tris);
        sa1_log("native land: model draws the TA way %u (%u tris): GE refused near plane %u, guard band %u, depth split %u, other %u; not converted %u; past %u lightings %u",
                ta, st.ta_tris, st.ta_test[2], st.ta_test[3], st.ta_test[4], st.ta_test[1] + st.ta_test[0], st.ta_other, MAX_VARS, st.ta_var);
        sa1_log("native land: %u persistent copies made (a model's, per variant; %u of them sharing another's vertices), %u variants for another set of textures (animated)", st.built, st.variants, st.tex_vars);
        sa1_log("native land: not converted %u: no file or not in it %u (%u tris), no opaque set %u (%u), sets differ %u (%u), material override %u (%u)",
                st.ta_other, st.nc[NC_FILE], st.nc_tris[NC_FILE], st.nc[NC_NOOPAQUE], st.nc_tris[NC_NOOPAQUE], st.nc[NC_DIFF], st.nc_tris[NC_DIFF],
                st.nc[NC_OVERRIDE], st.nc_tris[NC_OVERRIDE]);
        draw_cell_stats cs;
        draw_cell_stats_take(&cs);
        sa1_log("native land: refused whole, drawn by cells %u (near plane %u, guard band %u, depth split %u; %u opaque tris): cells out %u / GE far %u / GE near %u / GE both %u / CPU %u, "
                "tris out %u / GE far %u / GE near %u / GE both %u / CPU %u; per-set draws the CPU way %u tris; refused but on the GE whole: near pass %u, both passes %u",
                st.cell_test[2] + st.cell_test[3] + st.cell_test[4], st.cell_test[2], st.cell_test[3], st.cell_test[4], st.cell_tris,
                cs.cells[0], cs.cells[1], cs.cells[3], cs.cells[4], cs.cells[2], cs.tris[0], cs.tris[1], cs.tris[3], cs.tris[4], cs.tris[2], cs.mesh_cpu_tris,
                cs.whole[0], cs.whole[1]);
    }
    if (mode == 1)
        sa1_log("native land: memory: file tables %u KB, staging %u KB now (%u KB at most; %u models read, %u freed), kept colours and states %u KB; "
                "persistent models %u sets, %u KB in the PSP backend's layout (vertices, indices, call lists), cells %u KB of it",
                mem.tables / 1024u, mem.src / 1024u, mem.src_peak / 1024u, mem.staged, mem.freed, mem.kept / 1024u, mem.psp_sets, (mem.psp_est + mem.cells) / 1024u, mem.cells / 1024u);
}
