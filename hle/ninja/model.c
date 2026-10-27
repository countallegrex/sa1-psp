/* model.c - njDrawModel / njDrawObject (basic models) and njCnkDrawModel /
 * njCnkDrawObject (chunk models) done natively: transform through XMTRX,
 * light with the first Ninja light, project with the screen globals and
 * write TA strips into the KAMUI buffers, one header per material.
 *
 * The game draws its levels through its own writer (kamui.c); these
 * entries draw characters, menu models and a few objects.  Formats as SA
 * Tools documents them (docs/formats/ninja.md):
 *
 *   NJS_OBJECT  +0 evalflags +4 model +8 pos[3] +20 ang[3] +32 scl[3]
 *               +44 child +48 sibling
 *   NJS_MODEL   +0 points +4 normals +8 nbPoint +12 meshsets +16 mats
 *               +20 nbMeshset(16) +22 nbMat(16) +24 center[3] +36 r
 *   NJS_MESHSET +0 type|matId(16) +2 nbMesh(16) +4 meshes +8 attrs
 *               +12 normals +16 vertcolor +20 vertuv
 *   NJS_MATERIAL +0 diffuse(ARGB) +4 specular +8 exponent +12 attr_texId
 *               +16 attrflags (the TSP bit layout: clamp V 15, clamp U 16,
 *               flip V 17, flip U 18, ignore specular 19, use alpha 20,
 *               use texture 21, use env 22, double side 23, flat 24,
 *               ignore light 25, dst blend 26-28, src blend 29-31)
 *   NJS_CNK_MODEL +0 vlist +4 plist +8 center[3] +20 r
 *
 * Part of sa1-psp (MIT). */
#include "ninja.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
static int model_debug = -1;
static int mdbg(void) { if (model_debug < 0) model_debug = getenv("SA1_MODEL_DEBUG") != NULL; return model_debug; }

#define FARG(i) (c->fr[4 + (i)])
#define LIGHT_LIST  0x8C8EA3E0u
#define LIGHT_COUNT (LIGHT_LIST + 0x80)
#define VIEW_MATRIX 0x8C8E9F20u
#define PCW_VTX 0xE0000000u
#define PCW_VTX_END 0xF0000000u

#define FLAG_CLAMP_V 0x00008000u
#define FLAG_CLAMP_U 0x00010000u
#define FLAG_FLIP_V  0x00020000u
#define FLAG_FLIP_U  0x00040000u
#define FLAG_IGN_SPEC 0x00080000u
#define FLAG_USE_ALPHA 0x00100000u
#define FLAG_USE_TEXTURE 0x00200000u
#define FLAG_USE_ENV 0x00400000u
#define FLAG_DOUBLE 0x00800000u
#define FLAG_FLAT 0x01000000u
#define FLAG_IGN_LIGHT 0x02000000u

/* ---- the per-draw environment ------------------------------------------------ */
typedef struct {
    float m[16];              /* XMTRX */
    float cx, cy, dist, ay, clipn;
    int lit;                  /* a light exists */
    float ldir[3];            /* light direction in XMTRX space (unit) */
    float lcol[3], amb, dif;
    uint32_t ctrl;
} env_t;

typedef struct { float sx, sy, w; float z; int ok; float nx, ny, nz; } tv_t;   /* transformed vertex */

static void env_setup(sh4_ctx *c, env_t *e) {
    for (int i = 0; i < 16; i++) e->m[i] = c->xf[i];
    e->cx = hle_rdf(NJ_G_CX); e->cy = hle_rdf(NJ_G_CY); e->dist = hle_rdf(NJ_G_DIST);
    e->ay = hle_rdf(NJ_G_AY); e->clipn = hle_rdf(NJ_G_CLIPN);
    e->ctrl = hle_rd32(NJ_G_CTRL3D);
    e->lit = 0;
    uint32_t n = hle_rd32(LIGHT_COUNT);
    if (n > 0 && n <= 32) {
        uint32_t l = hle_rd32(LIGHT_LIST);
        if (l && hle_in_ram(l)) {
            float d[3] = {hle_rdf(l + 0x4C), hle_rdf(l + 0x50), hle_rdf(l + 0x54)};
            /* world -> the camera space XMTRX produces (slot 0 of the matrix stack) */
            float v[3];
            for (int i = 0; i < 3; i++) v[i] = hle_rdf(VIEW_MATRIX + 4u * (uint32_t)i) * d[0] + hle_rdf(VIEW_MATRIX + 16 + 4u * (uint32_t)i) * d[1] + hle_rdf(VIEW_MATRIX + 32 + 4u * (uint32_t)i) * d[2];
            float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len > 0.0f) { e->ldir[0] = v[0] / len; e->ldir[1] = v[1] / len; e->ldir[2] = v[2] / len; e->lit = 1; }
            e->lcol[0] = hle_rdf(l + 0x14C); e->lcol[1] = hle_rdf(l + 0x150); e->lcol[2] = hle_rdf(l + 0x154);
            e->amb = hle_rdf(l + 0x128); e->dif = hle_rdf(l + 0x12C);
        }
    }
    if (!e->lit) { e->amb = 1.0f; e->dif = 0.0f; e->lcol[0] = e->lcol[1] = e->lcol[2] = 1.0f; }
}

static void xform(const env_t *e, float x, float y, float z, tv_t *o) {
    float qx = e->m[0] * x + e->m[4] * y + e->m[8] * z + e->m[12];
    float qy = e->m[1] * x + e->m[5] * y + e->m[9] * z + e->m[13];
    float qz = e->m[2] * x + e->m[6] * y + e->m[10] * z + e->m[14];
    o->z = qz;
    o->ok = qz > e->clipn;
    float iz = o->ok ? 1.0f / qz : 0.0f;
    o->sx = e->cx + qx * e->dist * iz;
    o->sy = e->cy + qy * e->dist * e->ay * iz;
    o->w = iz;
}
static void xform_n(const env_t *e, float x, float y, float z, tv_t *o) {
    float nx = e->m[0] * x + e->m[4] * y + e->m[8] * z;
    float ny = e->m[1] * x + e->m[5] * y + e->m[9] * z;
    float nz = e->m[2] * x + e->m[6] * y + e->m[10] * z;
    float l = sqrtf(nx * nx + ny * ny + nz * nz);
    if (l > 0.0f) { nx /= l; ny /= l; nz /= l; }
    o->nx = nx; o->ny = ny; o->nz = nz;
}

/* Lit colour: material diffuse x light colour x (ambient + diffuse x max(0, n.l)). */
static uint32_t shade(const env_t *e, uint32_t diffuse, const tv_t *v, int ignore_light) {
    float k = 1.0f;
    if (e->lit && !ignore_light) {
        float d = -(v->nx * e->ldir[0] + v->ny * e->ldir[1] + v->nz * e->ldir[2]);
        if (d < 0.0f) d = 0.0f;
        k = e->amb + e->dif * d;
        if (k > 1.0f) k = 1.0f;
        if (k < 0.0f) k = 0.0f;
    }
    float r = ((diffuse >> 16) & 255) * k * (ignore_light ? 1.0f : e->lcol[0]);
    float g = ((diffuse >> 8) & 255) * k * (ignore_light ? 1.0f : e->lcol[1]);
    float b = (diffuse & 255) * k * (ignore_light ? 1.0f : e->lcol[2]);
    if (r > 255.0f) r = 255.0f;
    if (g > 255.0f) g = 255.0f;
    if (b > 255.0f) b = 255.0f;
    return (diffuse & 0xFF000000u) | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* ---- strip emission with near clipping ----------------------------------------- */
typedef struct { float x, y, w, u, v; uint32_t col; int ok; } sv_t;

/* Emits a strip, splitting it at vertices behind the near plane; a
 * restart at an odd position repeats the first vertex to keep the
 * winding. */
static void emit_strip(int list, const sv_t *v, int n) {
    int i = 0;
    while (i < n) {
        while (i < n && !v[i].ok) i++;
        int j = i;
        while (j < n && v[j].ok) j++;
        int len = j - i;
        if (len >= 3) {
            if (i & 1) kamui_put_vertex(list, PCW_VTX, v[i].x, v[i].y, v[i].w, v[i].u, v[i].v, v[i].col, 0);
            for (int k = i; k < j; k++)
                kamui_put_vertex(list, k + 1 == j ? PCW_VTX_END : PCW_VTX, v[k].x, v[k].y, v[k].w, v[k].u, v[k].v, v[k].col, 0);
        }
        i = j;
    }
}

/* Header for a material: texture, blending, culling, wrap modes. */
static int begin_material(sh4_ctx *c, uint32_t flags, uint32_t texid, int textured) {
    uint32_t ctrl = hle_rd32(NJ_G_CTRL3D);
    if (ctrl & 0x4) flags = (flags & hle_rd32(NJ_G_CONST_AND)) | hle_rd32(NJ_G_CONST_OR);
    kamui_draw_save(c);
    int tex = textured && (flags & FLAG_USE_TEXTURE);
    if (tex) kamui_select_texture(texid);
    int alpha = (flags & FLAG_USE_ALPHA) != 0;
    uint32_t attr = tex ? (alpha ? KM_ATTR_TRANS_TEX : KM_ATTR_OPAQUE_TEX) : (alpha ? KM_ATTR_TRANS_FLAT : KM_ATTR_OPAQUE_FLAT);
    /* material bits map straight onto the TSP: blend, clamp, flip */
    uint32_t tsp_mask = 0xFC000000u | 0x00078000u;
    uint32_t tsp_val = flags & (0xFC000000u | 0x00078000u);
    if (((flags >> 29) & 7) == 0 && ((flags >> 26) & 7) == 0) tsp_mask &= ~0xFC000000u;   /* no blend given: keep the context's */
    uint32_t isp_mask = 3u << 27, isp_val = (flags & FLAG_DOUBLE) ? 0 : hle_rd32(KM_CTX_ISP) & (3u << 27);
    kamui_ctx_patch(isp_mask, isp_val, tsp_mask, tsp_val);
    return kamui_draw_header(c, attr, 0);
}

/* ---- basic models ----------------------------------------------------------------- */
#define MAX_VERTS 4096
static tv_t tvs[MAX_VERTS];
static sv_t svs[MAX_VERTS];

static void draw_basic_model(sh4_ctx *c, uint32_t model) {
    env_t e; env_setup(c, &e);
    uint32_t points = hle_rd32(model), normals = hle_rd32(model + 4), np = hle_rd32(model + 8);
    uint32_t meshsets = hle_rd32(model + 12), mats = hle_rd32(model + 16);
    uint32_t nms = hle_rd16(model + 20), nmat = hle_rd16(model + 22);
    if (mdbg()) sa1_log("model 0x%08x: points 0x%08x normals 0x%08x np %u meshsets 0x%08x (%u) mats 0x%08x (%u) r %.1f", model, points, normals, np, meshsets, nms, mats, nmat, hle_rdf(model + 36));
    if (!points || np == 0 || np > MAX_VERTS || !meshsets) return;
    /* bounding sphere against the near plane and the far clip */
    { tv_t cen; xform(&e, hle_rdf(model + 24), hle_rdf(model + 28), hle_rdf(model + 32), &cen);
      float r = hle_rdf(model + 36);
      if (mdbg()) sa1_log("  centre view z %.1f (near %.1f far %.1f) screen %.0f,%.0f lit %d", cen.z, e.clipn, hle_rdf(NJ_G_CLIPF), cen.sx, cen.sy, e.lit);
      if (cen.z + r < e.clipn) return;
      if (cen.z - r > hle_rdf(NJ_G_CLIPF)) return; }
    /* frame skip (hle/game/frame_skip.c): on a turn without a picture only the
     * geometry is dropped (transforms, shading, vertices: TA list scratch that
     * kmFlip resets); the sphere test and every material's save / texture /
     * header / restore still run, since the restore leaves the strip head as
     * the last material set it */
    if (!hle_draw_skip) for (uint32_t i = 0; i < np; i++) {
        xform(&e, hle_rdf(points + 12 * i), hle_rdf(points + 12 * i + 4), hle_rdf(points + 12 * i + 8), &tvs[i]);
        if (normals) xform_n(&e, hle_rdf(normals + 12 * i), hle_rdf(normals + 12 * i + 4), hle_rdf(normals + 12 * i + 8), &tvs[i]);
        else { tvs[i].nx = 0; tvs[i].ny = 0; tvs[i].nz = -1; }
    }
    for (uint32_t s = 0; s < nms && s < 1024; s++) {
        uint32_t ms = meshsets + s * 24;
        uint32_t tm = hle_rd16(ms), nmesh = hle_rd16(ms + 2);
        uint32_t meshes = hle_rd32(ms + 4), vcol = hle_rd32(ms + 16), vuv = hle_rd32(ms + 20);
        uint32_t type = tm >> 14, matid = tm & 0x3FFF;
        uint32_t diffuse = 0xFFFFFFFFu, flags = 0, texid = 0;
        if (mats && matid < nmat) {
            uint32_t m = mats + matid * 20;
            diffuse = hle_rd32(m); flags = hle_rd32(m + 16); texid = hle_rd32(m + 12) & 0x3FFF;
        }
        if (e.ctrl & 0x2) {        /* constant material */
            diffuse = nj_pack_argb(hle_rdf(NJ_G_CONST_MAT) / 255.0f, hle_rdf(NJ_G_CONST_MAT + 4) / 255.0f, hle_rdf(NJ_G_CONST_MAT + 8) / 255.0f, hle_rdf(NJ_G_CONST_MAT + 12) / 255.0f);
        }
        int textured = vuv != 0;
        int ignore_light = (flags & FLAG_IGN_LIGHT) != 0;
        if (mdbg()) sa1_log("  meshset %u: type %u mat %u nmesh %u flags 0x%08x texid %u uv %d col %d", s, type, matid, nmesh, flags, texid, vuv != 0, vcol != 0);
        if (!hle_draw_skip && kamui_ptr_trace()) {   /* SA1_KAMUI_DUMP: which model wrote which strips, and its worst vertex */
            uint32_t worst = 0; float far = 0.0f; int bad = 0;
            for (uint32_t i = 0; i < np; i++) if (tvs[i].ok) { float d = fabsf(tvs[i].sx - 320.0f) + fabsf(tvs[i].sy - 240.0f); if (d > far) { far = d; worst = i; } }
            { uint32_t p2 = meshes; for (uint32_t mi = 0; mi < nmesh && mi < 4096; mi++) { int cnt = (type == 3 || type == 2) ? (int)(hle_rd16(p2) & 0x7FFF) : type == 0 ? 3 : 4; if (type == 3 || type == 2) p2 += 2; for (int q = 0; q < cnt; q++) if (hle_rd16(p2 + 2u * (uint32_t)q) >= np) bad++; p2 += 2u * (uint32_t)cnt; } }
            sa1_log("model: 0x%08x meshset %u type %u nmesh %u np %u points 0x%08x flags 0x%08x tex %u -> list %s at 0x%08x; worst vertex %u at (%.0f, %.0f) 1/w %.5f; %d indices >= np",
                    model, s, type, nmesh, np, points, flags, texid, (flags & FLAG_USE_ALPHA) ? "tr" : "op", kamui_buffer_ptr((flags & FLAG_USE_ALPHA) ? 1 : 0), worst, tvs[worst].sx, tvs[worst].sy, tvs[worst].w, bad);
        }
        int list = begin_material(c, flags, texid, textured);
        uint32_t p = meshes, k = 0;    /* k: running index into the per-mesh-vertex arrays */
        if (!hle_draw_skip) for (uint32_t mi = 0; mi < nmesh && mi < 4096; mi++) {
            int count, strip_dir = 0;
            if (type == 3 || type == 2) { int16_t len = (int16_t)hle_rd16(p); p += 2; count = len & 0x7FFF; strip_dir = (len < 0); }
            else count = type == 0 ? 3 : 4;
            if (count <= 0 || count > MAX_VERTS) break;
            int n = 0;
            if (type == 1) {           /* quad -> strip order 0,1,3,2 */
                static const int order[4] = {0, 1, 3, 2};
                for (int q = 0; q < 4; q++) {
                    uint32_t idx = hle_rd16(p + 2u * (uint32_t)order[q]);
                    if (idx >= np) idx = 0;
                    sv_t *o = &svs[n++];
                    o->x = tvs[idx].sx; o->y = tvs[idx].sy; o->w = tvs[idx].w; o->ok = tvs[idx].ok;
                    o->u = vuv ? (float)(int16_t)hle_rd16(vuv + 4 * (k + (uint32_t)order[q])) / 255.0f : 0;
                    o->v = vuv ? (float)(int16_t)hle_rd16(vuv + 4 * (k + (uint32_t)order[q]) + 2) / 255.0f : 0;
                    o->col = vcol ? hle_rd32(vcol + 4 * (k + (uint32_t)order[q])) : shade(&e, diffuse, &tvs[idx], ignore_light);
                }
                p += 8; k += 4;
            } else {
                for (int q = 0; q < count; q++) {
                    uint32_t idx = hle_rd16(p + 2u * (uint32_t)q);
                    if (idx >= np) idx = 0;
                    sv_t *o = &svs[n++];
                    o->x = tvs[idx].sx; o->y = tvs[idx].sy; o->w = tvs[idx].w; o->ok = tvs[idx].ok;
                    o->u = vuv ? (float)(int16_t)hle_rd16(vuv + 4 * (k + (uint32_t)q)) / 255.0f : 0;
                    o->v = vuv ? (float)(int16_t)hle_rd16(vuv + 4 * (k + (uint32_t)q) + 2) / 255.0f : 0;
                    o->col = vcol ? hle_rd32(vcol + 4 * (k + (uint32_t)q)) : shade(&e, diffuse, &tvs[idx], ignore_light);
                }
                p += 2u * (uint32_t)count; k += (uint32_t)count;
                if (type == 2) {       /* n-sided polygon as a fan -> strip reorder */
                    sv_t tmp[64]; int m = count < 64 ? count : 64, t = 0;
                    for (int q = 0; q < m; q++) tmp[q] = svs[q];
                    int lo = 1, hi = m - 1; svs[t++] = tmp[0];
                    while (lo <= hi) { svs[t++] = tmp[lo++]; if (lo <= hi) svs[t++] = tmp[hi--]; }
                    n = t;
                }
            }
            if (strip_dir && n >= 3) {     /* reversed winding: insert a degenerate first vertex */
                for (int q = n; q > 0; q--) svs[q] = svs[q - 1];
                n++;
            }
            emit_strip(list, svs, n);
        }
        kamui_draw_end(c);
    }
}

/* ---- objects ----------------------------------------------------------------------- */
static void m_load_xf(sh4_ctx *c, float *m) { for (int i = 0; i < 16; i++) m[i] = c->xf[i]; }
static void m_store_xf(sh4_ctx *c, const float *m) { for (int i = 0; i < 16; i++) c->xf[i] = m[i]; }
static void m_rot(float *m, int axis, uint32_t a) {
    float s = sh4_sin16(a), co = sh4_cos16(a);   /* fsca, as the console (sh4_trig.h) */
    for (int i = 0; i < 4; i++) {
        if (axis == 0) { float c1 = m[4 + i], c2 = m[8 + i]; m[4 + i] = co * c1 + s * c2; m[8 + i] = co * c2 - s * c1; }
        else if (axis == 1) { float c0 = m[i], c2 = m[8 + i]; m[i] = co * c0 - s * c2; m[8 + i] = s * c0 + co * c2; }
        else { float c0 = m[i], c1 = m[4 + i]; m[i] = co * c0 + s * c1; m[4 + i] = co * c1 - s * c0; }
    }
}

typedef void (*model_fn)(sh4_ctx *c, uint32_t model);

static void draw_object(sh4_ctx *c, uint32_t obj, model_fn draw, int depth) {
    while (obj && hle_in_ram(obj) && depth < 64) {
        uint32_t flags = hle_rd32(obj), model = hle_rd32(obj + 4);
        float saved[16]; m_load_xf(c, saved);
        float m[16]; m_load_xf(c, m);
        if (!(flags & 1)) { float x = hle_rdf(obj + 8), y = hle_rdf(obj + 12), z = hle_rdf(obj + 16);
            float t[3] = {m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13], m[2] * x + m[6] * y + m[10] * z + m[14]};
            m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; }
        if (!(flags & 2)) {
            uint32_t ax = hle_rd32(obj + 20), ay = hle_rd32(obj + 24), az = hle_rd32(obj + 28);
            if (flags & 0x20) { m_rot(m, 2, az); m_rot(m, 0, ax); m_rot(m, 1, ay); }   /* ZXY */
            else { m_rot(m, 2, az); m_rot(m, 1, ay); m_rot(m, 0, ax); }                /* XYZ as njRotateXYZ applies it */
        }
        if (!(flags & 4)) { float sx = hle_rdf(obj + 32), sy = hle_rdf(obj + 36), sz = hle_rdf(obj + 40);
            for (int i = 0; i < 4; i++) { m[i] *= sx; m[4 + i] *= sy; m[8 + i] *= sz; } }
        m_store_xf(c, m);
        if (!(flags & 8) && model) draw(c, model);
        if (!(flags & 0x10)) draw_object(c, hle_rd32(obj + 44), draw, depth + 1);
        m_store_xf(c, saved);
        obj = hle_rd32(obj + 48);
    }
}

/* ---- chunk models ------------------------------------------------------------------ */
typedef struct { float x, y, z, nx, ny, nz; uint32_t col; int has_col; } cv_t;
#define CNK_CACHE 4096
static cv_t cnk_cache[CNK_CACHE];
static tv_t cnk_tv[CNK_CACHE];

static void cnk_vertices(uint32_t vlist) {
    uint32_t p = vlist;
    for (int guard = 0; guard < 256 && p; guard++) {
        uint32_t h0 = hle_rd32(p), type = h0 & 0xFF, size = h0 >> 16;
        if (type == 0xFF) break;
        if (type < 0x20 || type > 0x32) { p += 4 + size * 4; continue; }
        uint32_t h1 = hle_rd32(p + 4), off = h1 & 0xFFFF, n = h1 >> 16;
        uint32_t q = p + 8;
        uint32_t stride;
        int has_n = (type >= 0x29 && type <= 0x2F) || type == 0x21, extra = 0, packed_n = (type >= 0x30);
        switch (type) {
        case 0x20: stride = 16; break;
        case 0x21: stride = 32; break;
        case 0x22: stride = 12; break;
        case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: stride = 16; extra = 1; break;
        case 0x29: stride = 24; break;
        case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: stride = 28; extra = 1; break;
        case 0x30: stride = 16; break;
        case 0x31: case 0x32: stride = 20; extra = 1; break;
        default: stride = 16; break;
        }
        for (uint32_t i = 0; i < n; i++, q += stride) {
            uint32_t k = off + i;
            if (k >= CNK_CACHE) break;
            cv_t *v = &cnk_cache[k];
            v->x = hle_rdf(q); v->y = hle_rdf(q + 4); v->z = hle_rdf(q + 8);
            v->has_col = 0; v->col = 0xFFFFFFFFu;
            uint32_t r = q + 12;
            if (type == 0x20 || type == 0x21) r += 4;
            if (has_n) { v->nx = hle_rdf(r); v->ny = hle_rdf(r + 4); v->nz = hle_rdf(r + 8); r += 12; if (type == 0x21) r += 4; }
            else if (packed_n) { uint32_t pn = hle_rd32(r); r += 4;
                v->nx = (float)((int32_t)(pn << 2) >> 22) / 511.0f; v->ny = (float)((int32_t)(pn << 12) >> 22) / 511.0f; v->nz = (float)((int32_t)(pn << 22) >> 22) / 511.0f; }
            else { v->nx = 0; v->ny = 0; v->nz = 0; }
            if (extra && (type == 0x23 || type == 0x2A || type == 0x31)) { v->col = hle_rd32(r); v->has_col = 1; }
        }
        p += 4 + size * 4;
    }
}

static void draw_cnk_model(sh4_ctx *c, uint32_t model) {
    env_t e; env_setup(c, &e);
    uint32_t vlist = hle_rd32(model), plist = hle_rd32(model + 4);
    if (!plist) return;
    { tv_t cen; xform(&e, hle_rdf(model + 8), hle_rdf(model + 12), hle_rdf(model + 16), &cen);
      float r = hle_rdf(model + 20);
      if (r > 0.0f && cen.z + r < e.clipn) return; }
    if (vlist) {
        cnk_vertices(vlist);
        for (int i = 0; i < CNK_CACHE; i++) { /* lazily: only what strips use would be cheaper */ }
    }
    uint32_t diffuse = 0xFFFFFFFFu, texid = 0, texflags = 0, blend = 0;
    uint32_t p = plist;
    int transformed[CNK_CACHE / 32 + 1]; memset(transformed, 0, sizeof transformed);
    for (int guard = 0; guard < 4096; guard++) {
        uint32_t h = hle_rd16(p), type = h & 0xFF, flags = h >> 8;
        if (type == 0xFF) break;
        if (type == 0) { p += 2; continue; }
        if (type <= 5) { if (type == 1) blend = flags; p += 2; continue; }      /* bits chunks */
        if (type == 8 || type == 9) { uint32_t t = hle_rd16(p + 2); texid = t & 0x1FFF; texflags = t; p += 4; continue; }
        if (type >= 0x10 && type <= 0x1F) {                                /* material */
            uint32_t size = hle_rd16(p + 2), q = p + 4;
            if (type & 1) { diffuse = hle_rd32(q); q += 4; }                   /* D */
            blend = flags;
            p += 4 + size * 2;
            continue;
        }
        if (type >= 0x18 && type <= 0x1A) { uint32_t size = hle_rd16(p + 2); p += 4 + size * 2; continue; }  /* volumes */
        if (type >= 0x40 && type <= 0x51) {                                /* strips */
            uint32_t size = hle_rd16(p + 2);
            uint32_t n = hle_rd16(p + 4), nstrips = n & 0x3FFF, uf = n >> 14;
            int has_uv = (type == 0x41 || type == 0x42 || type == 0x44 || type == 0x45 || type == 0x47 || type == 0x48 || type == 0x4A || type == 0x4B || type == 0x4D || type == 0x4E || type == 0x50 || type == 0x51);
            int uvh = (type == 0x42 || type == 0x45 || type == 0x48 || type == 0x4B || type == 0x4E || type == 0x51);
            int has_vn = (type >= 0x43 && type <= 0x45);
            int has_col = (type >= 0x46 && type <= 0x48);
            uint32_t mflags = (flags & 0x01 ? FLAG_IGN_LIGHT : 0) | (flags & 0x08 ? FLAG_USE_ALPHA : 0) | (flags & 0x10 ? FLAG_DOUBLE : 0)
                            | (texflags & 0x8000 ? FLAG_CLAMP_V : 0) | (texflags & 0x4000 ? FLAG_CLAMP_U : 0)
                            | (texflags & 0x2000 ? FLAG_FLIP_V : 0)
                            | ((blend & 0x38) << 26) | ((blend & 7) << 26);
            int textured = has_uv;
            if (textured) mflags |= FLAG_USE_TEXTURE;
            int list = begin_material(c, mflags, texid, textured);
            uint32_t q = p + 6;
            if (!hle_draw_skip) for (uint32_t s = 0; s < nstrips && s < 2048; s++) {   /* frame skip: as draw_basic_model */
                int16_t len = (int16_t)hle_rd16(q); q += 2;
                int count = len & 0x7FFF, rev = len < 0, nv = 0;
                if (count <= 0 || count > MAX_VERTS - 1) break;
                for (int i = 0; i < count; i++) {
                    uint32_t idx = hle_rd16(q); q += 2;
                    float u = 0, v = 0; uint32_t col = 0; int vcol = 0;
                    if (has_uv) { u = (float)(int16_t)hle_rd16(q) / (uvh ? 1023.0f : 255.0f); v = (float)(int16_t)hle_rd16(q + 2) / (uvh ? 1023.0f : 255.0f); q += 4; }
                    if (has_vn) q += 6;
                    if (has_col) { col = hle_rd32(q); vcol = 1; q += 4; }
                    q += 2 * uf;
                    if (idx >= CNK_CACHE) idx = 0;
                    if (!(transformed[idx >> 5] & (1u << (idx & 31)))) {
                        transformed[idx >> 5] |= 1u << (idx & 31);
                        xform(&e, cnk_cache[idx].x, cnk_cache[idx].y, cnk_cache[idx].z, &cnk_tv[idx]);
                        xform_n(&e, cnk_cache[idx].nx, cnk_cache[idx].ny, cnk_cache[idx].nz, &cnk_tv[idx]);
                    }
                    sv_t *o = &svs[nv++];
                    o->x = cnk_tv[idx].sx; o->y = cnk_tv[idx].sy; o->w = cnk_tv[idx].w; o->ok = cnk_tv[idx].ok; o->u = u; o->v = v;
                    if (vcol) o->col = col;
                    else if (cnk_cache[idx].has_col) o->col = cnk_cache[idx].col;
                    else o->col = shade(&e, diffuse, &cnk_tv[idx], (flags & 1) != 0);
                }
                if (rev && nv >= 3) { for (int k = nv; k > 0; k--) svs[k] = svs[k - 1]; nv++; }
                emit_strip(list, svs, nv);
            }
            kamui_draw_end(c);
            p += 4 + size * 2;
            continue;
        }
        /* unknown chunk: stop */
        sa1_log("cnk: unknown poly chunk type 0x%x at 0x%08x", type, p);
        break;
    }
}

/* ---- entries -------------------------------------------------------------------------- */
void sdk_8c611028(sh4_ctx *c) { HLE_ENTER(0x8c611028u, "njDrawModel"); if (ARG0) draw_basic_model(c, ARG0); }
void sdk_8c61103c(sh4_ctx *c) { HLE_ENTER(0x8c61103cu, "njDrawObject"); draw_object(c, ARG0, draw_basic_model, 0); }
void sdk_8c638e4e(sh4_ctx *c) { HLE_ENTER(0x8c638e4eu, "njDrawModel (lit)"); if (ARG0) draw_basic_model(c, ARG0); }
void sdk_8c638dec(sh4_ctx *c) { HLE_ENTER(0x8c638decu, "njDrawObject (lit)"); draw_object(c, ARG0, draw_basic_model, 0); }
/* SA1 (Dreamcast) models are all in the basic format; the 0x8C6215xx
 * family is the lit basic draw (its bodies start by turning the first
 * light into view space).  A model whose first word is not a pointer is
 * taken as a chunk model so that the chunk path stays exercised. */
static int looks_chunk(uint32_t model) {
    uint32_t a = hle_rd32(model), b = hle_rd32(model + 4);
    return hle_in_ram(a) && hle_in_ram(b) && !hle_in_ram(hle_rd32(model + 12)) && hle_rd32(model + 8) > 100000u;
}
static void draw_any_model(sh4_ctx *c, uint32_t model) { if (looks_chunk(model)) draw_cnk_model(c, model); else draw_basic_model(c, model); }
void sdk_8c6215d6(sh4_ctx *c) { HLE_ENTER(0x8c6215d6u, "njDrawModel (lit, 0x8C6215D6)"); if (ARG0) draw_any_model(c, ARG0); }
void sdk_8c621574(sh4_ctx *c) { HLE_ENTER(0x8c621574u, "njDrawObject (lit, 0x8C621574)"); draw_object(c, ARG0, draw_any_model, 0); }

/* 0x8C610CF8 / 0x8C610D30 / 0x8C610D3E: draw parameters the SDK's model
 * renderer kept at 0x8C8901F8 and 0x8C6735A8 (stores only). */
void sdk_8c610cf8(sh4_ctx *c) {
    HLE_ENTER(0x8c610cf8u, "njSetDrawParams");
    uint32_t b = 0x8C8901F8u;
    hle_wrf(b, FARG(0)); hle_wrf(b + 4, FARG(1)); hle_wrf(b + 8, FARG(2));
    hle_wrf(b + 36, hle_rdf(NJ_G_DIST) * hle_rdf(NJ_G_AX)); hle_wrf(b + 40, hle_rdf(NJ_G_DIST) * hle_rdf(NJ_G_AY));
    hle_wrf(b + 44, hle_rdf(NJ_G_CX)); hle_wrf(b + 48, hle_rdf(NJ_G_CY));
}
void sdk_8c610d30(sh4_ctx *c) { HLE_ENTER(0x8c610d30u, "njSetDrawParams2"); hle_wrf(0x8C8901F8u + 12, -FARG(0)); hle_wrf(0x8C8901F8u + 32, FARG(1)); }
/* 0x8C60D1F2 / 0x8C60D1F8 (one store each, from their disassembly): the
 * vertex buffer the game's shape-motion interpolation writes into
 * (0x8C89002C; the game passes its static 0x8C782984 at boot and the
 * walker 0x8C0403C0 lerps every vertex into it, then points the model at
 * it) and the per-node draw callback (0x8C890074) the object walkers
 * 0x8C040784 / 0x8C040880 call for every node (the game registers
 * 0x8C0D215C at frame 692: it turns particular nodes of the player model).
 * Both were seam stubs returning 0: the Emerald Coast dolphins then lerped
 * through a NULL buffer (docs/m2-render-report.md section 10). */
void sdk_8c60d1f2(sh4_ctx *c) { HLE_ENTER(0x8c60d1f2u, "njSetVertexBuffer"); hle_wr32(0x8C89002Cu, ARG0); }
void sdk_8c60d1f8(sh4_ctx *c) { HLE_ENTER(0x8c60d1f8u, "njSetDrawCallback"); hle_wr32(0x8C890074u, ARG0); }
void sdk_8c610d3e(sh4_ctx *c) { HLE_ENTER(0x8c610d3eu, "njSetDrawVector"); hle_wrf(0x8C6735ACu, FARG(0)); hle_wrf(0x8C6735B0u, FARG(1)); hle_wrf(0x8C6735B4u, FARG(2)); }
