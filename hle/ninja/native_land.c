/* native_land.c - the land table drawn from converted meshes with hardware
 * transform (docs/native-renderer.md, the Station Square slice).
 *
 * renderer = native (sa1psp.ini, or --renderer native|ta|force:native) turns
 * it on where the backend has draw_mesh; never in strict runs unless forced.
 * The game's own land draw runs unchanged (its cull, visible list, matrices,
 * njSetTexture; the land task's MainSub 0x8C0519C0 calls the cull and the
 * draw directly).  For a model of the current land table (0x8C7596F4) whose
 * every mesh set has converted triangles in native/LAND_<level>_<act>.BIN
 * (tools/land_convert.py, loaded when the land table changes) and is opaque,
 * wherever it is drawn (the game's matrix and header make that equivalent):
 *   - the vertex transform 0x8C037294 returns at once (ta_stubs.c), only
 *     pointing gbr+60 at the vertex buffer as it would;
 *   - the lighting 0x8C037350 runs as always, so each point's base and
 *     offset colours are the game's;
 *   - the mesh-set loop (ta_mesh.c) writes each set's polygon header as
 *     always, then native_land_set() hands the set's triangles to kamui with
 *     those colours and the game's matrix for the COL (XMTRX) times the
 *     screen scale and centre, instead of calling a strip writer: kamui draws
 *     it with the header's state, in list order (draw_mesh).
 * Models with a translucent set (kamui sorts those per strip) or an
 * environment-mapped one (UVs from the normals) keep the strip writers.
 * The guest state differs from the TA path's only in draw scratch, as on a
 * skipped frame (hle/game/frame_skip.c): tapes and RAM compares check it.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "ninja.h"
#include "draw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hle_renderer_native;            /* sa1psp.ini renderer = native */
int hle_renderer_cli = -1;          /* backend option, wins: 0 ta, 1 native */
int hle_renderer_force;             /* also in strict runs (tests) */
int native_land_window;             /* on: the backend can draw meshes and the option is set */

#define USE_ALPHA 0x00100000u
#define ENV_MAP   0x00400000u       /* UVs from the normals (not converted yet): the TA way */
#define LT_CUR    0x8c7596f4u
#define LV_ACT    0x8c7493bau          /* level (16 bits), then act at +2 */

typedef struct { uint32_t model; uint16_t set, col, mat, pad; uint32_t vfirst, vcount, ifirst, icount; } nl_batch;
typedef struct { float x, y, z, nx, ny, nz, u, v; uint32_t pt; } nl_vert;
typedef struct { uint32_t model; int ok; } nl_model;

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
static uint32_t stat_sets, stat_tris;

static int cmp_batch(const void *a, const void *b) {
    const nl_batch *x = a, *y = b;
    if (x->model != y->model) return x->model < y->model ? -1 : 1;
    return (int)x->set - (int)y->set;
}

static void unload(void) {
    free(L.file); free(L.models);
    memset(&L, 0, sizeof L);
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
    /* per model: native only if every set is converted and opaque */
    L.models = malloc((L.nb ? L.nb : 1) * sizeof *L.models);
    for (uint32_t i = 0; i < L.nb; ) {
        uint32_t m = L.b[i].model, j = i;
        int ok = 1;
        for (; j < L.nb && L.b[j].model == m; j++)
            if (L.b[j].mat >= L.nmat || (L.mats[4 * L.b[j].mat + 1] & (USE_ALPHA | ENV_MAP))) ok = 0;
        uint32_t nset = hle_rd16((m | 0x80000000u) + 20u);
        if (j - i != nset) ok = 0;
        L.models[L.nmodels].model = m; L.models[L.nmodels].ok = ok; L.nmodels++;
        i = j;
    }
    L.have = 1;
    uint32_t nok = 0;
    for (uint32_t k = 0; k < L.nmodels; k++) nok += L.models[k].ok;
    sa1_log("native land: %s for land table 0x%08x: %u mesh sets, %u vertices, %u triangles; %u of %u models native (the rest translucent or environment-mapped)",
            path, lt, L.nb, L.nv, L.ni / 3, nok, L.nmodels);
}

int native_land_model_ok(uint32_t model) {
    if (!native_land_window) return 0;
    uint32_t lt = hle_rd32(LT_CUR);
    if (lt != L.lt) { if (lt) load(lt); else unload(); }
    if (!L.have) return 0;
    model &= 0x1FFFFFFFu;
    uint32_t lo = 0, hi = L.nmodels;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (L.models[mid].model < model) lo = mid + 1; else hi = mid;
    }
    if (lo >= L.nmodels || L.models[lo].model != model || !L.models[lo].ok) return 0;
    /* the material override (0x8C8FFE1C: enable, AND, OR; the land draw sets it) may turn
     * USE_ALPHA on: then the model's sets are not all opaque */
    if (hle_rd32(0x8c8ffe1cu) && (hle_rd32(0x8c8ffe24u) & (USE_ALPHA | ENV_MAP))) return 0;
    return 1;
}

/* Draws mesh set `set` of `model` natively: returns 1 and the TA pointer
 * after the marker in *ta_out, or 0 (then the caller draws it the TA way). */
int native_land_set(sh4_ctx *c, uint32_t model, uint32_t set, uint32_t ta, uint32_t *ta_out) {
    model &= 0x1FFFFFFFu;
    uint32_t lo = 0, hi = L.nb;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (L.b[mid].model < model || (L.b[mid].model == model && L.b[mid].set < set)) lo = mid + 1; else hi = mid;
    }
    if (lo >= L.nb || L.b[lo].model != model || L.b[lo].set != set) return 0;
    const nl_batch *b = &L.b[lo];
    if (b->vfirst + b->vcount > L.nv || b->ifirst + b->icount > L.ni) return 0;
    /* matrix: the fed vector (sx z, sy z, -(z + 1), z) from XMTRX's (x', y', z'):
     * sx = x' Sx / z' + Cx, sy = y' Sy / z' + Cy (ta_xform.c 0x8C037294) */
    const float cx = hle_rdf(0x8c8ffe00u), cy = hle_rdf(0x8c8ffe04u), sx = hle_rdf(0x8c8ffe08u), sy = hle_rdf(0x8c8ffe0cu) * sx;
    const float *x = c->xf;
    float m[16];
    for (int col = 0; col < 4; col++) {
        float x0 = x[4 * col], x1 = x[4 * col + 1], x2 = x[4 * col + 2];
        float w = col == 3 ? 1.0f : 0.0f;
        m[4 * col + 0] = sx * x0 + cx * x2;
        m[4 * col + 1] = sy * x1 + cy * x2;
        m[4 * col + 2] = -x2 - w;
        m[4 * col + 3] = x2;
    }
    uint32_t rec;
    draw_mesh_vertex *out = kamui_native_mesh(b->vcount, m, L.idx + b->ifirst, b->icount, &rec);
    if (!out) return 0;
    const uint32_t vbuf = hle_rd32(c->gbr + 60), ocol = hle_rd32(c->gbr + 56);
    for (uint32_t i = 0; i < b->vcount; i++) {
        const nl_vert *s = &L.v[b->vfirst + i];
        out[i].x = s->x; out[i].y = s->y; out[i].z = s->z; out[i].u = s->u; out[i].v = s->v;
        out[i].color = hle_rd32(vbuf + 16u * s->pt + 12u);
        out[i].ocolor = ocol ? hle_rd32(ocol + 4u * s->pt) : 0;
    }
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
    sa1_log("native land: on");
}

void native_land_report(void) {
    if (stat_sets)
        sa1_log("native land: %u mesh sets (%u triangles) drawn natively", stat_sets, stat_tris);
}
