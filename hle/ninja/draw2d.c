/* draw2d.c - the Ninja draw entries that build TA parameters natively:
 * sprites, 2D and 3D polygon lists, textured quads.  Each writes the same
 * 32-byte headers and vertices the SDK wrote (docs/formats/ninja.md), into
 * the KAMUI buffers kamui.c parses at the end of the frame, so they sort
 * with everything the game draws itself.
 *
 * Conventions recovered from the SDK bodies (US v1.005):
 *   2D depth: the TA z (1/w) is 1 / max(1, -pri); pri is the Ninja priority.
 *   Sprites (0x8C63F838): NJS_SPRITE {p[3], sx, sy, ang, texlist, tanim},
 *   NJS_TEXANIM {sx, sy, cx, cy, u1, v1, u2, v2, texid, attr} (16-bit; uv in
 *   1/256), attr: 1 rotate, 2 constant material, 4 flip u, 8 flip v,
 *   0x20 translucent.  Vertex type 7 (textured, intensity 1.0).
 *   njDrawPolygon (0x8C63EC58): NJS_POLYGON_VTX {x, y, z, col}, type 0.
 *   njDrawTexture (0x8C63ED08): NJS_TEXTURE_VTX {x, y, z, u, v, col}, type 3.
 *   njDrawPolygon2D (0x8C63EBC8): NJS_POINT2COL {p, col, tex, num}, attr
 *   0x20 polygon (the points are the outline in ring order: the SDK's
 *   writer 0x8C63E834 reorders them 0, n-1, 1, n-2, ... into a strip),
 *   0x40 translucent (the header writer 0x8C63E258 takes attr & 0x40),
 *   0x80000000 textured.  Without 0x20 the points are a strip as given.
 *   The event scripts draw the letterbox bars and fades with 0x20.
 *   njDrawPolygon3D (0x8C63CE24): NJS_POINT3COL through XMTRX, attr 0x40
 *   translucent, 0x80000000 textured.
 *
 * Part of sa1-psp (MIT). */
#include "ninja.h"

#include <math.h>

#define FARG(i) (c->fr[4 + (i)])
#define PCW_VTX 0xE0000000u
#define PCW_VTX_END 0xF0000000u

static float pri_to_z(float pri) {
    float d = -pri;
    return d > 1.0f ? 1.0f / d : 1.0f;
}

static int put_vertex(int list, uint32_t pcw, float x, float y, float z, float u, float v, uint32_t c6, uint32_t c7) {
    uint32_t w[8] = {pcw, sh4_f2u(x), sh4_f2u(y), sh4_f2u(z), sh4_f2u(u), sh4_f2u(v), c6, c7};
    return kamui_append(list, w);
}

/* ---- sprites --------------------------------------------------------------- */
static void sprite_quad(int list, float x0, float y0, float x1, float y1, float z, float u0, float v0, float u1, float v1,
                        const float *rot /* cx, cy, s, c or NULL */) {
    float xs[4] = {x0, x0, x1, x1}, ys[4] = {y0, y1, y0, y1};
    float us[4] = {u0, u0, u1, u1}, vs[4] = {v0, v1, v0, v1};
    for (int i = 0; i < 4; i++) {
        float x = xs[i], y = ys[i];
        if (rot) { float dx = x - rot[0], dy = y - rot[1]; x = rot[0] + dx * rot[3] - dy * rot[2]; y = rot[1] + dx * rot[2] + dy * rot[3]; }
        put_vertex(list, i == 3 ? PCW_VTX_END : PCW_VTX, x, y, z, us[i], vs[i], sh4_f2u(1.0f), sh4_f2u(1.0f));
    }
}

static void draw_sprite(sh4_ctx *c, uint32_t sp, uint32_t n, uint32_t attr, float pri, int is3d) {
    uint32_t tlist = hle_rd32(sp + 24), tanim = hle_rd32(sp + 28);
    if (!tanim) return;
    uint32_t an = tanim + n * 20;
    float asx = (float)(int16_t)hle_rd16(an), asy = (float)(int16_t)hle_rd16(an + 2);
    float acx = (float)(int16_t)hle_rd16(an + 4), acy = (float)(int16_t)hle_rd16(an + 6);
    float u1 = (float)(int16_t)hle_rd16(an + 8) / 256.0f, v1 = (float)(int16_t)hle_rd16(an + 10) / 256.0f;
    float u2 = (float)(int16_t)hle_rd16(an + 12) / 256.0f, v2 = (float)(int16_t)hle_rd16(an + 14) / 256.0f;
    uint32_t texid = hle_rd16(an + 16);
    if (attr & 4) { float t = u1; u1 = u2; u2 = t; }
    if (attr & 8) { float t = v1; v1 = v2; v2 = t; }
    float sx = hle_rdf(sp + 12), sy = hle_rdf(sp + 16);
    float px = hle_rdf(sp), py = hle_rdf(sp + 4), z;
    float scale = 1.0f;
    if (is3d) {
        /* place the sprite's point through XMTRX, then project */
        float p[3] = {px, py, hle_rdf(sp + 8)}, q[3];
        for (int i = 0; i < 3; i++) q[i] = c->xf[i] * p[0] + c->xf[4 + i] * p[1] + c->xf[8 + i] * p[2] + c->xf[12 + i];
        float dist = hle_rdf(NJ_G_DIST), depth = q[2];
        if (depth < hle_rdf(NJ_G_CLIPN)) return;
        scale = dist / depth;
        px = hle_rdf(NJ_G_CX) + q[0] * scale;
        py = hle_rdf(NJ_G_CY) + q[1] * scale * hle_rdf(NJ_G_AY);
        z = 1.0f / depth;
    } else z = pri_to_z(pri);
    if (tlist) hle_wr32(NJ_CUR_TEXLIST, tlist);
    uint32_t mat = (attr & 2) ? NJ_G_CONST_MAT : 0;
    int list = kamui_draw_begin(c, (attr & 0x20) ? KM_ATTR_TRANS_INT : KM_ATTR_OPAQUE_INT, mat, tlist ? KM_TEX_NUM : KM_TEX_NONE, texid);
    float x0 = px - acx * sx * scale, y0 = py - acy * sy * scale;
    float x1 = x0 + asx * sx * scale, y1 = y0 + asy * sy * scale;
    float rot[4];
    const float *r = NULL;
    if (attr & 1) {
        float a = (float)(hle_rd32(sp + 20) & 0xFFFF) * (float)(6.283185307179586 / 65536.0);
        rot[0] = px; rot[1] = py; rot[2] = sinf(a); rot[3] = cosf(a); r = rot;
    }
    sprite_quad(list, x0, y0, x1, y1, z, u1, v1, u2, v2, r);
    kamui_draw_end(c);
}

void sdk_8c63eeb4(sh4_ctx *c) { HLE_ENTER(0x8c63eeb4u, "njDrawSprite2D"); kamui_mark2d_begin(); draw_sprite(c, ARG0, ARG1, ARG2, FARG(0), 0); kamui_mark2d_end(); }
void sdk_8c63ef74(sh4_ctx *c) { HLE_ENTER(0x8c63ef74u, "njDrawSprite3D"); draw_sprite(c, ARG0, ARG1, ARG2, 0.0f, 1); }

/* ---- njDrawPolygon(vtx, n, trans): untextured, z given as 1/w --------------- */
void sdk_8c63ec58(sh4_ctx *c) {
    HLE_ENTER(0x8c63ec58u, "njDrawPolygon");
    uint32_t v = ARG0, n = ARG1;
    if (!v || n < 3) return;
    kamui_mark2d_begin();
    int list = kamui_draw_begin(c, ARG2 ? KM_ATTR_TRANS_FLAT : KM_ATTR_OPAQUE_FLAT, 0, KM_TEX_NONE, 0);
    for (uint32_t i = 0; i < n; i++, v += 16)
        put_vertex(list, i + 1 == n ? PCW_VTX_END : PCW_VTX, hle_rdf(v), hle_rdf(v + 4), hle_rdf(v + 8), 0, 0, hle_rd32(v + 12), 0);
    kamui_draw_end(c);
    kamui_mark2d_end();
}

/* ---- njDrawTexture(vtx, n, tex, trans): textured, z given as 1/w ----------- */
void sdk_8c63ed08(sh4_ctx *c) {
    HLE_ENTER(0x8c63ed08u, "njDrawTexture");
    uint32_t v = ARG0, n = ARG1;
    if (!v || n < 3) return;
    kamui_mark2d_begin();
    int list = kamui_draw_begin(c, ARG3 ? KM_ATTR_TRANS_TEX : KM_ATTR_OPAQUE_TEX, 0, KM_TEX_GINDEX, ARG2);
    for (uint32_t i = 0; i < n; i++, v += 24)
        put_vertex(list, i + 1 == n ? PCW_VTX_END : PCW_VTX, hle_rdf(v), hle_rdf(v + 4), hle_rdf(v + 8),
                   hle_rdf(v + 12), hle_rdf(v + 16), hle_rd32(v + 20), 0);
    kamui_draw_end(c);
    kamui_mark2d_end();
}

/* ---- 3D point lists: transform, clip each triangle to the near plane, emit --- */
typedef struct { float x, y, z, u, v; float col[4]; } cv_t;
static void cv_lerp(const cv_t *a, const cv_t *b, float t, cv_t *o) {
    o->x = a->x + (b->x - a->x) * t; o->y = a->y + (b->y - a->y) * t; o->z = a->z + (b->z - a->z) * t;
    o->u = a->u + (b->u - a->u) * t; o->v = a->v + (b->v - a->v) * t;
    for (int i = 0; i < 4; i++) o->col[i] = a->col[i] + (b->col[i] - a->col[i]) * t;
}
static uint32_t cv_pack(const float *c) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) { int k = (int)(c[i] + 0.5f); if (k < 0) k = 0; if (k > 255) k = 255; r |= (uint32_t)k << (24 - 8 * i); }
    return r;
}
/* Clips one triangle against z > near; emits the resulting polygon as a strip. */
static void clip_emit_tri(int list, const cv_t *t0, const cv_t *t1, const cv_t *t2, float near, float cx, float cy, float dist, float ax, float ay) {
    const cv_t *in[3] = {t0, t1, t2};
    cv_t out[6]; int n = 0;
    for (int i = 0; i < 3; i++) {
        const cv_t *a = in[i], *b = in[(i + 1) % 3];
        int ain = a->z > near, bin = b->z > near;
        if (ain) out[n++] = *a;
        if (ain != bin) { float t = (near - a->z) / (b->z - a->z); cv_lerp(a, b, t, &out[n++]); }
    }
    if (n < 3) return;
    /* polygon -> strip order 0,1,n-1,2,n-2,... */
    int order[6], k = 0, lo = 1, hi = n - 1;
    order[k++] = 0;
    while (lo <= hi) { order[k++] = lo++; if (lo <= hi) order[k++] = hi--; }
    for (int i = 0; i < n; i++) {
        const cv_t *p = &out[order[i]];
        float iz = 1.0f / p->z;
        kamui_put_vertex(list, i + 1 == n ? PCW_VTX_END : PCW_VTX, cx + p->x * dist * ax * iz, cy + p->y * dist * ay * iz, iz, p->u, p->v, cv_pack(p->col), 0);
    }
}

/* ---- njDrawPolygon2D(p, n, pri, attr) and the line family ------------------- */
static void point_list(sh4_ctx *c, uint32_t pl, uint32_t n, float z, uint32_t attr, int is3d, int lines) {
    if (!pl) return;
    uint32_t pts = hle_rd32(pl), cols = hle_rd32(pl + 4), texs = hle_rd32(pl + 8), num = hle_rd32(pl + 12);
    if (!num || num > 4096) num = n;
    if (!pts || num < (lines ? 2u : 3u)) return;
    int textured = (attr & 0x80000000u) && texs != 0;
    int trans = (attr & 0x40) != 0;
    int ring = !is3d && !lines && (attr & 0x20);   /* outline order -> strip, as 0x8C63E834 */
    uint32_t a = textured ? (trans ? KM_ATTR_TRANS_TEX : KM_ATTR_OPAQUE_TEX) : (trans ? KM_ATTR_TRANS_FLAT : KM_ATTR_OPAQUE_FLAT);
    int list = kamui_draw_begin(c, a, 0, KM_TEX_NONE, 0);
    float dist = hle_rdf(NJ_G_DIST), cx = hle_rdf(NJ_G_CX), cy = hle_rdf(NJ_G_CY), ax = hle_rdf(NJ_G_AX), ay = hle_rdf(NJ_G_AY), clipn = hle_rdf(NJ_G_CLIPN);
    uint32_t stride = is3d ? 12 : 8;
    float px = 0, py = 0, pz = z;
    if (is3d) {
        /* through XMTRX into the game's camera space (z grows away from the
         * eye there, as the game's own writer projects it), then clip every
         * triangle of the strip to the near plane */
        cv_t prev[2]; int have = 0;
        for (uint32_t i = 0; i < num; i++) {
            cv_t v;
            float p[3] = {hle_rdf(pts + i * stride), hle_rdf(pts + i * stride + 4), hle_rdf(pts + i * stride + 8)};
            for (int j = 0; j < 3; j++) (&v.x)[j] = c->xf[j] * p[0] + c->xf[4 + j] * p[1] + c->xf[8 + j] * p[2] + c->xf[12 + j];
            v.u = textured ? hle_rdf(texs + i * 8) : 0; v.v = textured ? hle_rdf(texs + i * 8 + 4) : 0;
            uint32_t col = cols ? hle_rd32(cols + i * 4) : 0xFFFFFFFFu;
            v.col[0] = (float)(col >> 24); v.col[1] = (float)((col >> 16) & 255); v.col[2] = (float)((col >> 8) & 255); v.col[3] = (float)(col & 255);
            if (have >= 2) {
                if (i & 1) clip_emit_tri(list, &prev[1], &prev[0], &v, clipn, cx, cy, dist, ax, ay);
                else clip_emit_tri(list, &prev[0], &prev[1], &v, clipn, cx, cy, dist, ax, ay);
            }
            prev[0] = prev[1]; prev[1] = v; if (have < 2) have++;
        }
        kamui_draw_end(c);
        return;
    }
    for (uint32_t i = 0; i < num; i++) {
        uint32_t k = i;
        if (ring) k = (i & 1) ? num - 1 - i / 2 : i / 2;   /* 0, n-1, 1, n-2, 2, ... */
        float x = hle_rdf(pts + k * stride), y = hle_rdf(pts + k * stride + 4);
        float zz = z;
        uint32_t col = cols ? hle_rd32(cols + k * 4) : 0xFFFFFFFFu;
        float u = textured ? hle_rdf(texs + k * 8) : 0, v = textured ? hle_rdf(texs + k * 8 + 4) : 0;
        if (lines) {
            if (i > 0) {   /* a 1-pixel quad from the previous point to this one */
                float dx = x - px, dy = y - py, l = sqrtf(dx * dx + dy * dy);
                float nx = l > 0 ? -dy / l * 0.5f : 0, ny = l > 0 ? dx / l * 0.5f : 0.5f;
                put_vertex(list, PCW_VTX, px + nx, py + ny, pz, 0, 0, col, 0);
                put_vertex(list, PCW_VTX, px - nx, py - ny, pz, 0, 0, col, 0);
                put_vertex(list, PCW_VTX, x + nx, y + ny, zz, 0, 0, col, 0);
                put_vertex(list, PCW_VTX_END, x - nx, y - ny, zz, 0, 0, col, 0);
            }
            px = x; py = y; pz = zz;
        } else put_vertex(list, i + 1 == num ? PCW_VTX_END : PCW_VTX, x, y, zz, u, v, col, 0);
    }
    kamui_draw_end(c);
}
void sdk_8c63ebc8(sh4_ctx *c) { HLE_ENTER(0x8c63ebc8u, "njDrawPolygon2D"); kamui_mark2d_begin(); point_list(c, ARG0, ARG1, pri_to_z(FARG(0)), ARG2, 0, 0); kamui_mark2d_end(); }
void sdk_8c63e1d4(sh4_ctx *c) { HLE_ENTER(0x8c63e1d4u, "njDrawLine2D"); kamui_mark2d_begin(); point_list(c, ARG0, ARG1, pri_to_z(FARG(0)), ARG2, 0, 1); kamui_mark2d_end(); }
void sdk_8c63ce24(sh4_ctx *c) { HLE_ENTER(0x8c63ce24u, "njDrawPolygon3D"); point_list(c, ARG0, ARG1, 1.0f, ARG2, 1, 0); }
