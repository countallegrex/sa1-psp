/* math.c - Ninja matrix, vector and camera math.  Not graphics: these
 * results feed positions, collision and culling in game logic, and the game
 * reads the same guest globals directly (it even has an inlined
 * njPushMatrix), so the state has to live where the SDK kept it.
 *
 * Conventions (docs/m1-boot-report.md, "Ninja math"): a matrix is 16
 * floats, column j = m[4j..4j+3], x' = m0*x + m4*y + m8*z + m12*w.  The
 * current matrix XMTRX is the back FP bank, c->xf[0..15], same indexing.
 * A NULL matrix argument means XMTRX.  Angles: 65536 = one turn, only the
 * low 16 bits count.  Rotate/Translate/Scale post-multiply in place.
 *
 * Guest globals: matrix depth 0x8C88F69C, max 0x8C88F698, stack ptr
 * 0x8C88F5F8, stack base 0x8C88F5FC, unit-normal flag 0x8C890070, the stack
 * itself where njInitMatrix puts it (0x8C7EC8E0, 64-byte slots); screen
 * dist 0x8C88F5E4, w/h 0x8C88F5E8/EC, cx/cy 0x8C88F5F0/F4, clip 0x8C88F610/
 * 614, aspect 0x8C88F618/61C; current camera 0x8C8E9F00, its view copy
 * 0x8C8E9F20.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "../seam.h"

#include <math.h>

#define G_DEPTH  0x8C88F69Cu
#define G_MAX    0x8C88F698u
#define G_PTR    0x8C88F5F8u
#define G_BASE   0x8C88F5FCu
#define G_UNITN  0x8C890070u
#define G_DIST   0x8C88F5E4u
#define G_W      0x8C88F5E8u
#define G_H      0x8C88F5ECu
#define G_CX     0x8C88F5F0u
#define G_CY     0x8C88F5F4u
#define G_CLIPN  0x8C88F610u
#define G_CLIPF  0x8C88F614u
#define G_AX     0x8C88F618u
#define G_AY     0x8C88F61Cu
#define G_CAMERA 0x8C8E9F00u
#define G_CAMVIEW 0x8C8E9F20u
#define G_SCREEN_W 0x8C88F644u
#define G_SCREEN_H 0x8C88F648u

#define FR0(v) (c->fr[0] = (v))
#define FARG(i) (c->fr[4 + (i)])

static const float TURN = (float)(6.283185307179586 / 65536.0);
static inline float ang_rad(uint32_t a) { return (float)(a & 0xFFFFu) * TURN; }
static inline uint32_t rad_ang(float r) { return (uint32_t)(int32_t)(r * (65536.0f / 6.2831853f)) & 0xFFFFu; }

/* ---- matrix access: XMTRX or guest memory --------------------------------- */
static void m_load(sh4_ctx *c, uint32_t addr, float *m) {
    if (addr == 0) for (int i = 0; i < 16; i++) m[i] = c->xf[i];
    else for (int i = 0; i < 16; i++) m[i] = RDF(addr, 4 * i);
}
static void m_store(sh4_ctx *c, uint32_t addr, const float *m) {
    if (addr == 0) for (int i = 0; i < 16; i++) c->xf[i] = m[i];
    else for (int i = 0; i < 16; i++) WRF(addr, 4 * i, m[i]);
}
static void v_load(uint32_t addr, float *v) { for (int i = 0; i < 3; i++) v[i] = RDF(addr, 4 * i); }
static void v_store(uint32_t addr, const float *v) { for (int i = 0; i < 3; i++) WRF(addr, 4 * i, v[i]); }

static void m_unit(float *m) { for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
/* d = a * b */
static void m_mul(float *d, const float *a, const float *b) {
    float r[16];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            r[4 * j + i] = a[i] * b[4 * j] + a[4 + i] * b[4 * j + 1] + a[8 + i] * b[4 * j + 2] + a[12 + i] * b[4 * j + 3];
    for (int i = 0; i < 16; i++) d[i] = r[i];
}
static void m_xform(const float *m, const float *v, float w, float *out) {
    for (int i = 0; i < 3; i++) out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * w;
}
static void m_rot_x(float *m, uint32_t a) {
    float s = sinf(ang_rad(a)), co = cosf(ang_rad(a));
    for (int i = 0; i < 4; i++) {
        float c1 = m[4 + i], c2 = m[8 + i];
        m[4 + i] = co * c1 + s * c2;
        m[8 + i] = co * c2 - s * c1;
    }
}
static void m_rot_y(float *m, uint32_t a) {
    float s = sinf(ang_rad(a)), co = cosf(ang_rad(a));
    for (int i = 0; i < 4; i++) {
        float c0 = m[i], c2 = m[8 + i];
        m[i] = co * c0 - s * c2;
        m[8 + i] = s * c0 + co * c2;
    }
}
static void m_rot_z(float *m, uint32_t a) {
    float s = sinf(ang_rad(a)), co = cosf(ang_rad(a));
    for (int i = 0; i < 4; i++) {
        float c0 = m[i], c1 = m[4 + i];
        m[i] = co * c0 + s * c1;
        m[4 + i] = co * c1 - s * c0;
    }
}
static void m_translate(float *m, const float *v) {
    float t[3];
    m_xform(m, v, 1.0f, t);
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2];
}
static void m_scale(float *m, const float *v) {
    for (int i = 0; i < 4; i++) { m[i] *= v[0]; m[4 + i] *= v[1]; m[8 + i] *= v[2]; }
}
static float v_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float v_len(const float *a) { return sqrtf(v_dot(a, a)); }
static void v_cross(const float *a, const float *b, float *d) {
    d[0] = a[1] * b[2] - a[2] * b[1]; d[1] = a[2] * b[0] - a[0] * b[2]; d[2] = a[0] * b[1] - a[1] * b[0];
}

/* General 4x4 inverse by cofactors; det 0 fills with 3.4e38 like the SDK. */
static void m_invert(float *m) {
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) { for (int i = 0; i < 16; i++) m[i] = 3.4e38f; return; }
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) m[i] = inv[i] * det;
}

/* ---- matrix stack ---------------------------------------------------------- */
static const float DIAG_MM11[16] = {-1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

void sdk_8c638fe4(sh4_ctx *c) {         /* njInitMatrix(base, n, flag) */
    HLE_ENTER(0x8c638fe4u, "njInitMatrix");
    hle_wr32(G_DEPTH, 1); hle_wr32(G_MAX, ARG1); hle_wr32(G_PTR, ARG0); hle_wr32(G_BASE, ARG0);
    hle_wr32(G_UNITN, ARG2);
    m_store(c, ARG0, DIAG_MM11);
    m_store(c, 0, DIAG_MM11);
}

void sdk_8c639c70(sh4_ctx *c) {         /* njPushMatrix(m) */
    HLE_ENTER(0x8c639c70u, "njPushMatrix");
    uint32_t depth = hle_rd32(G_DEPTH);
    if (depth >= hle_rd32(G_MAX)) { RET(0); return; }
    uint32_t ptr = hle_rd32(G_PTR);
    float m[16];
    m_load(c, 0, m);
    m_store(c, ptr, m);
    ptr += 64;
    hle_wr32(G_DEPTH, depth + 1); hle_wr32(G_PTR, ptr);
    if (ARG0 == 0) m_store(c, ptr, m);
    else { m_load(c, ARG0, m); m_store(c, 0, m); }
    RET(1);
}

void sdk_8c639b98(sh4_ctx *c) {         /* njPopMatrix(n) */
    HLE_ENTER(0x8c639b98u, "njPopMatrix");
    uint32_t depth = hle_rd32(G_DEPTH), n = ARG0;
    if ((int32_t)(depth - n) < 1) { RET(0); return; }
    uint32_t ptr = hle_rd32(G_PTR) - 64 * n;
    hle_wr32(G_DEPTH, depth - n); hle_wr32(G_PTR, ptr);
    float m[16];
    m_load(c, ptr, m); m_store(c, 0, m);
    RET(1);
}

void sdk_8c63a8e0(sh4_ctx *c) { HLE_ENTER(0x8c63a8e0u, "njUnitMatrix"); float m[16]; m_unit(m); m_store(c, ARG0, m); }
void sdk_8c63a770(sh4_ctx *c) { HLE_ENTER(0x8c63a770u, "njSetMatrix"); float m[16]; m_load(c, ARG1, m); m_store(c, ARG0, m); }
void sdk_8c638f94(sh4_ctx *c) { HLE_ENTER(0x8c638f94u, "njGetMatrix"); float m[16]; m_load(c, 0, m); m_store(c, ARG0, m); }
void sdk_8c63997c(sh4_ctx *c) {         /* njMultiMatrix(md, ms): Md = Md * Ms */
    HLE_ENTER(0x8c63997cu, "njMultiMatrix");
    float a[16], b[16]; m_load(c, ARG0, a); m_load(c, ARG1, b); m_mul(a, a, b); m_store(c, ARG0, a);
}
void sdk_8c6390b0(sh4_ctx *c) { HLE_ENTER(0x8c6390b0u, "njInvertMatrix"); float m[16]; m_load(c, ARG0, m); m_invert(m); m_store(c, ARG0, m); }

void sdk_8c639ec8(sh4_ctx *c) { HLE_ENTER(0x8c639ec8u, "njRotateX"); float m[16]; m_load(c, ARG0, m); m_rot_x(m, ARG1); m_store(c, ARG0, m); }
void sdk_8c639f5c(sh4_ctx *c) { HLE_ENTER(0x8c639f5cu, "njRotateY"); float m[16]; m_load(c, ARG0, m); m_rot_y(m, ARG1); m_store(c, ARG0, m); }
void sdk_8c63a1cc(sh4_ctx *c) { HLE_ENTER(0x8c63a1ccu, "njRotateZ"); float m[16]; m_load(c, ARG0, m); m_rot_z(m, ARG1); m_store(c, ARG0, m); }
void sdk_8c639cf4(sh4_ctx *c) {         /* njRotateXYZ(m, ax, ay, az): Z, then Y, then X */
    HLE_ENTER(0x8c639cf4u, "njRotateXYZ");
    float m[16]; m_load(c, ARG0, m);
    if (ARG3 & 0xFFFF) m_rot_z(m, ARG3);
    if (ARG2 & 0xFFFF) m_rot_y(m, ARG2);
    if (ARG1 & 0xFFFF) m_rot_x(m, ARG1);
    m_store(c, ARG0, m);
}
void sdk_8c63a878(sh4_ctx *c) {         /* njTranslate(m, x, y, z) */
    HLE_ENTER(0x8c63a878u, "njTranslate");
    float m[16], v[3] = {FARG(0), FARG(1), FARG(2)}; m_load(c, ARG0, m); m_translate(m, v); m_store(c, ARG0, m);
}
void sdk_8c63a804(sh4_ctx *c) {         /* njTranslateV(m, v) */
    HLE_ENTER(0x8c63a804u, "njTranslateV");
    float m[16], v[3]; m_load(c, ARG0, m); v_load(ARG1, v); m_translate(m, v); m_store(c, ARG0, m);
}
void sdk_8c63a69c(sh4_ctx *c) {         /* njScale(m, sx, sy, sz) */
    HLE_ENTER(0x8c63a69cu, "njScale");
    float m[16], v[3] = {FARG(0), FARG(1), FARG(2)}; m_load(c, ARG0, m); m_scale(m, v); m_store(c, ARG0, m);
}
void sdk_8c63a5ec(sh4_ctx *c) {         /* njScaleV(m, v) */
    HLE_ENTER(0x8c63a5ecu, "njScaleV");
    float m[16], v[3]; m_load(c, ARG0, m); v_load(ARG1, v); m_scale(m, v); m_store(c, ARG0, m);
}

/* ---- point and vector transforms ----------------------------------------- */
void sdk_8c638ecc(sh4_ctx *c) {         /* njCalcPoint(m, vs, vd) */
    HLE_ENTER(0x8c638eccu, "njCalcPoint");
    float m[16], v[3], o[3]; m_load(c, ARG0, m); v_load(ARG1, v); m_xform(m, v, 1.0f, o); v_store(ARG2, o);
}
void sdk_8c638f28(sh4_ctx *c) {         /* njCalcVector(m, vs, vd) */
    HLE_ENTER(0x8c638f28u, "njCalcVector");
    float m[16], v[3], o[3]; m_load(c, ARG0, m); v_load(ARG1, v); m_xform(m, v, 0.0f, o);
    if (hle_rd32(G_UNITN)) { float l = v_len(o); if (l != 0.0f) for (int i = 0; i < 3; i++) o[i] /= l; }
    v_store(ARG2, o);
}
void sdk_8c639bdc(sh4_ctx *c) {         /* njProjectScreen(m, p3, p2) */
    HLE_ENTER(0x8c639bdcu, "njProjectScreen");
    float m[16], v[3], q[3]; m_load(c, ARG0, m); v_load(ARG1, v); m_xform(m, v, 1.0f, q);
    float dist = hle_rdf(G_DIST);
    float z = q[2] != 0.0f ? q[2] : 1.0f;
    hle_wrf(ARG2, dist * q[0] / z * hle_rdf(G_AX) + hle_rdf(G_CX));
    hle_wrf(ARG2 + 4, dist * q[1] / z * hle_rdf(G_AY) + hle_rdf(G_CY));
}

/* ---- scalar and vector math ------------------------------------------------ */
void sdk_8c63a9b8(sh4_ctx *c) { HLE_ENTER(0x8c63a9b8u, "njSin"); float a = ang_rad(ARG0); FR0(sinf(a)); c->fr[1] = cosf(a); }
void sdk_8c63a970(sh4_ctx *c) { HLE_ENTER(0x8c63a970u, "njCos"); float a = ang_rad(ARG0); FR0(cosf(a)); c->fr[1] = sinf(a); }
void sdk_8c63a9f4(sh4_ctx *c) { HLE_ENTER(0x8c63a9f4u, "njTan"); float a = ang_rad(ARG0); FR0(sinf(a) / cosf(a)); }
void sdk_8c63a9c4(sh4_ctx *c) { HLE_ENTER(0x8c63a9c4u, "njSqrt"); float x = FARG(0); FR0(x >= 0.0f ? sqrtf(x) : -sqrtf(-x)); }
void sdk_8c63a980(sh4_ctx *c) { HLE_ENTER(0x8c63a980u, "njFraction"); float x = FARG(0); FR0(x - truncf(x)); }
void sdk_8c63a994(sh4_ctx *c) { HLE_ENTER(0x8c63a994u, "njRoundOff"); FR0(truncf(FARG(0))); }
void sdk_8c63a75c(sh4_ctx *c) { HLE_ENTER(0x8c63a75cu, "njScalor"); float v[3]; v_load(ARG0, v); FR0(v_len(v)); }
void sdk_8c63a94c(sh4_ctx *c) {         /* njUnitVector(v): in place, fr0 = old length */
    HLE_ENTER(0x8c63a94cu, "njUnitVector");
    float v[3]; v_load(ARG0, v);
    float l = v_len(v), r = 1.0f / l;    /* zero vector -> inf/NaN as the hardware fsrra gives */
    for (int i = 0; i < 3; i++) v[i] *= r;
    v_store(ARG0, v);
    FR0(l);
}
void sdk_8c639090(sh4_ctx *c) { HLE_ENTER(0x8c639090u, "njInnerProduct"); float a[3], b[3]; v_load(ARG0, a); v_load(ARG1, b); FR0(v_dot(a, b)); }
void sdk_8c639b50(sh4_ctx *c) {         /* njOuterProduct(v1, v2, vd) */
    HLE_ENTER(0x8c639b50u, "njOuterProduct");
    float a[3], b[3], d[3]; v_load(ARG0, a); v_load(ARG1, b); v_cross(a, b, d); v_store(ARG2, d); FR0(v_len(d));
}
void sdk_8c638eac(sh4_ctx *c) { HLE_ENTER(0x8c638eacu, "njAddVector"); float a[3], b[3]; v_load(ARG0, a); v_load(ARG1, b); for (int i = 0; i < 3; i++) a[i] += b[i]; v_store(ARG0, a); }
void sdk_8c63a7e4(sh4_ctx *c) { HLE_ENTER(0x8c63a7e4u, "njSubVector"); float a[3], b[3]; v_load(ARG0, a); v_load(ARG1, b); for (int i = 0; i < 3; i++) a[i] -= b[i]; v_store(ARG0, a); }

/* ---- distances (line = {p[3], d[3]}, plane = {p[3], n[3]}) ------------------ */
void sdk_8c640128(sh4_ctx *c) {         /* njDistanceP2P(p, q) */
    HLE_ENTER(0x8c640128u, "njDistanceP2P");
    float a[3], b[3]; v_load(ARG0, a); v_load(ARG1, b); for (int i = 0; i < 3; i++) a[i] -= b[i]; FR0(v_len(a));
}
void sdk_8c63ff84(sh4_ctx *c) {         /* njDistanceP2L(p, line, foot) */
    HLE_ENTER(0x8c63ff84u, "njDistanceP2L");
    float p[3], lp[3], d[3], w[3], f[3]; v_load(ARG0, p); v_load(ARG1, lp); v_load(ARG1 + 12, d);
    for (int i = 0; i < 3; i++) w[i] = p[i] - lp[i];
    float dd = v_dot(d, d), t = dd != 0.0f ? v_dot(d, w) / dd : 0.0f;
    for (int i = 0; i < 3; i++) { f[i] = lp[i] + t * d[i]; w[i] = p[i] - f[i]; }
    if (ARG2) v_store(ARG2, f);
    FR0(v_len(w));
}
static float dist_p2pl(const float *p, const float *pp, const float *n, float *foot) {
    float w[3]; for (int i = 0; i < 3; i++) w[i] = p[i] - pp[i];
    float k = v_dot(n, w), nn = v_dot(n, n);
    if (foot) for (int i = 0; i < 3; i++) foot[i] = p[i] - (nn != 0.0f ? k / nn : 0.0f) * n[i];
    return nn != 0.0f ? fabsf(k) / sqrtf(nn) : 0.0f;
}
void sdk_8c640080(sh4_ctx *c) {         /* njDistanceP2PL(p, plane, foot) */
    HLE_ENTER(0x8c640080u, "njDistanceP2PL");
    float p[3], pp[3], n[3], f[3]; v_load(ARG0, p); v_load(ARG1, pp); v_load(ARG1 + 12, n);
    float r = dist_p2pl(p, pp, n, f);
    if (ARG2) v_store(ARG2, f);
    FR0(r);
}
void sdk_8c63feac(sh4_ctx *c) {         /* njDistanceL2PL(line, plane, cross) */
    HLE_ENTER(0x8c63feacu, "njDistanceL2PL");
    float lp[3], d[3], pp[3], n[3], x[3]; v_load(ARG0, lp); v_load(ARG0 + 12, d); v_load(ARG1, pp); v_load(ARG1 + 12, n);
    float nd = v_dot(n, d);
    if (nd == 0.0f) { float r = dist_p2pl(lp, pp, n, x); if (ARG2) v_store(ARG2, x); FR0(r); return; }
    float w[3]; for (int i = 0; i < 3; i++) w[i] = lp[i] - pp[i];
    float t = -v_dot(n, w) / nd;
    for (int i = 0; i < 3; i++) x[i] = lp[i] + t * d[i];
    if (ARG2) v_store(ARG2, x);
    FR0(0.0f);
}
void sdk_8c64016c(sh4_ctx *c) {         /* njDistancePL2PL(pl1, pl2, line) */
    HLE_ENTER(0x8c64016cu, "njDistancePL2PL");
    float p1[3], n1[3], p2[3], n2[3], d[3]; v_load(ARG0, p1); v_load(ARG0 + 12, n1); v_load(ARG1, p2); v_load(ARG1 + 12, n2);
    v_cross(n1, n2, d);
    if (v_dot(d, d) == 0.0f) {
        if (ARG2) { float z[3] = {0, 0, 0}; v_store(ARG2, z); v_store(ARG2 + 12, z); }
        FR0(dist_p2pl(p1, p2, n2, NULL));
        return;
    }
    /* a point on both planes: p1 moved along (n2 x d) until it meets plane 2 */
    float u[3], w[3]; v_cross(n2, d, u);
    for (int i = 0; i < 3; i++) w[i] = p2[i] - p1[i];
    float un2 = v_dot(u, n2), t = un2 != 0.0f ? v_dot(w, n2) / un2 : 0.0f;
    float q[3]; for (int i = 0; i < 3; i++) q[i] = p1[i] + t * u[i];
    if (ARG2) { v_store(ARG2, q); v_store(ARG2 + 12, d); }
    FR0(0.0f);
}

/* ---- screen globals ------------------------------------------------------- */
void sdk_8c640528(sh4_ctx *c) {         /* njSetScreen(scr {dist, w, h, cx, cy}) */
    HLE_ENTER(0x8c640528u, "njSetScreen");
    hle_wrf(G_DIST, hle_rdf(ARG0)); hle_wrf(G_CX, hle_rdf(ARG0 + 12)); hle_wrf(G_CY, hle_rdf(ARG0 + 16));
}
void sdk_8c640500(sh4_ctx *c) {         /* njSetPerspective(ang): dist = w / (2 tan(ang/2)) */
    HLE_ENTER(0x8c640500u, "njSetPerspective");
    float half = ang_rad(ARG0) * 0.5f, t = tanf(half);
    if (t != 0.0f) hle_wrf(G_DIST, hle_rdf(G_W) / (2.0f * t));
}
void sdk_8c64036c(sh4_ctx *c) {         /* njClipZ(near, far) */
    HLE_ENTER(0x8c64036cu, "njClipZ");
    float n = FARG(0), f = FARG(1);
    if (n < -65535.0f) n = -65535.0f; if (n > -1.0f) n = -1.0f;
    if (f < -65535.0f) f = -65535.0f; if (f > -1.0f) f = -1.0f;
    hle_wrf(G_CLIPN, -n); hle_wrf(G_CLIPF, -f);
}

/* ---- camera (game-owned struct: matrix +0, pos +64, dir +76, roll +88,
 *      base copies +92 / +104 / +116) ------------------------------------- */
#define CAM_POS 64
#define CAM_DIR 76
#define CAM_ROLL 88
#define CAM_BPOS 92
#define CAM_BDIR 104
#define CAM_BROLL 116

static uint32_t cam_arg(uint32_t a) { return a ? a : hle_rd32(G_CAMERA); }

void sdk_8c6403b0(sh4_ctx *c) {         /* njInitCamera(cam) */
    HLE_ENTER(0x8c6403b0u, "njInitCamera");
    uint32_t cam = ARG0;
    float m[16]; m_unit(m); m_store(c, cam, m);
    float z[3] = {0, 0, 0}, d[3] = {0, 0, -1};
    v_store(cam + CAM_POS, z); v_store(cam + CAM_DIR, d); hle_wr32(cam + CAM_ROLL, 0);
    v_store(cam + CAM_BPOS, z); v_store(cam + CAM_BDIR, d); hle_wr32(cam + CAM_BROLL, 0);
}

void sdk_8c640540(sh4_ctx *c) {         /* njSetCamera(cam) */
    HLE_ENTER(0x8c640540u, "njSetCamera");
    uint32_t cam = ARG0;
    float pos[3], dir[3]; v_load(cam + CAM_POS, pos); v_load(cam + CAM_DIR, dir);
    uint32_t roll = hle_rd32(cam + CAM_ROLL);
    float v[16]; m_unit(v);
    m_rot_z(v, (uint32_t)(-(int32_t)roll));
    float h = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
    float len = sqrtf(dir[1] * dir[1] + h * h);
    if (len > 0.0f) {
        float ca = h / len; if (ca > 1.0f) ca = 1.0f;
        uint32_t a = rad_ang(acosf(ca));
        m_rot_x(v, dir[1] >= 0.0f ? a : (uint32_t)(-(int32_t)a));
    }
    if (h > 0.0f) {
        float cb = dir[2] / h; if (cb > 1.0f) cb = 1.0f; if (cb < -1.0f) cb = -1.0f;
        uint32_t b = rad_ang(acosf(cb));
        m_rot_y(v, dir[0] >= 0.0f ? (uint32_t)(-(int32_t)b) : b);
    }
    float np[3] = {-pos[0], -pos[1], -pos[2]};
    m_translate(v, np);
    m_store(c, cam, v);
    hle_wr32(G_CAMERA, cam);
    float s0[16]; m_mul(s0, DIAG_MM11, v);
    uint32_t base = hle_rd32(G_BASE);
    hle_wr32(G_DEPTH, 1); hle_wr32(G_PTR, base);
    if (base) m_store(c, base, s0);
    m_store(c, G_CAMVIEW, s0);
    m_store(c, 0, s0);
}

void sdk_8c6404e4(sh4_ctx *c) {         /* njCameraRoll(cam, ang): roll += ang */
    HLE_ENTER(0x8c6404e4u, "njCameraRoll");
    uint32_t cam = cam_arg(ARG0); hle_wr32(cam + CAM_ROLL, hle_rd32(cam + CAM_ROLL) + ARG1);
}
void sdk_8c640454(sh4_ctx *c) {         /* njCameraPitch(cam, ang): rotate dir in y/z */
    HLE_ENTER(0x8c640454u, "njCameraPitch");
    uint32_t cam = cam_arg(ARG0); float s = sinf(ang_rad(ARG1)), co = cosf(ang_rad(ARG1));
    float y = hle_rdf(cam + CAM_DIR + 4), z = hle_rdf(cam + CAM_DIR + 8);
    hle_wrf(cam + CAM_DIR + 8, z * co + y * s); hle_wrf(cam + CAM_DIR + 4, y * co - z * s);
}
void sdk_8c64048c(sh4_ctx *c) {         /* njCameraYaw(cam, ang): rotate dir in x/z */
    HLE_ENTER(0x8c64048cu, "njCameraYaw");
    uint32_t cam = cam_arg(ARG0); float s = sinf(ang_rad(ARG1)), co = cosf(ang_rad(ARG1));
    float x = hle_rdf(cam + CAM_DIR), z = hle_rdf(cam + CAM_DIR + 8);
    hle_wrf(cam + CAM_DIR, x * co + z * s); hle_wrf(cam + CAM_DIR + 8, z * co - x * s);
}
void sdk_8c640684(sh4_ctx *c) {         /* njCameraTranslate(cam, x, y, z) */
    HLE_ENTER(0x8c640684u, "njCameraTranslate");
    uint32_t cam = cam_arg(ARG0);
    for (int i = 0; i < 3; i++) hle_wrf(cam + CAM_POS + 4 * i, hle_rdf(cam + CAM_POS + 4 * i) + FARG(i));
}
void sdk_8c6404c8(sh4_ctx *c) {         /* roll = base roll + ang */
    HLE_ENTER(0x8c6404c8u, "njCameraRollFromBase");
    uint32_t cam = cam_arg(ARG0); hle_wr32(cam + CAM_ROLL, hle_rd32(cam + CAM_BROLL) + ARG1);
}
/* dir = base dir pitched about X by ang (the SDK body 0x8C64041C: the
 * same y/z rotation as njCameraPitch, from the base copy).  The event
 * scripts place their cameras with PosFromBase, DirFromBase (pitch),
 * RollFromBase, then njCameraYaw and njSetCamera; the M2 body yawed here,
 * which put the Chaos 0 cutscene camera under the police cars. */
void sdk_8c64041c(sh4_ctx *c) {
    HLE_ENTER(0x8c64041cu, "njCameraDirFromBase");
    uint32_t cam = cam_arg(ARG0); float s = sinf(ang_rad(ARG1)), co = cosf(ang_rad(ARG1));
    float x = hle_rdf(cam + CAM_BDIR), y = hle_rdf(cam + CAM_BDIR + 4), z = hle_rdf(cam + CAM_BDIR + 8);
    hle_wrf(cam + CAM_DIR, x); hle_wrf(cam + CAM_DIR + 4, y * co - z * s); hle_wrf(cam + CAM_DIR + 8, z * co + y * s);
}
void sdk_8c64065c(sh4_ctx *c) {         /* pos = base pos + (x, y, z) */
    HLE_ENTER(0x8c64065cu, "njCameraPosFromBase");
    uint32_t cam = cam_arg(ARG0);
    for (int i = 0; i < 3; i++) hle_wrf(cam + CAM_POS + 4 * i, hle_rdf(cam + CAM_BPOS + 4 * i) + FARG(i));
}
