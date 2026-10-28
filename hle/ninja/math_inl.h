/* math_inl.h - the Ninja matrix helpers shared by hle/ninja/math.c and the
 * native routines that redo its steps inline (hle/game/motion_walk.c): the
 * matrix copies (VFPU on the PSP), the transforms, rotations, translate and
 * scale, and the matrix stack push and pop.  Moved out of math.c unchanged;
 * the same operations in the same order, so the bits are the bits.
 *
 * Part of sa1-psp (MIT). */
#ifndef SA1_NJ_MATH_INL_H
#define SA1_NJ_MATH_INL_H
#include "../hle.h"
#include <string.h>

#define NJ_G_DEPTH  0x8C88F69Cu         /* matrix stack depth */
#define NJ_G_MAX    0x8C88F698u         /* its limit */
#define NJ_G_PTR    0x8C88F5F8u         /* the current slot */

/* Sine and cosine of a Ninja angle: njSin and njCos are fsca on the
 * console, so they read the same table (sh4_trig.h), bit for bit; it also
 * replaced a 64 KB cache of host sinf/cosf (2.3 % of a PSP-3001 Station
 * Square frame, 2026-10-21). */
static inline __attribute__((always_inline)) void ang_sc(uint32_t a, float *s, float *c) {
    *s = sh4_sin16(a); *c = sh4_cos16(a);
}

/* ---- matrix access: XMTRX or guest memory ---------------------------------
 * Each guest block (a 64-byte matrix, a 12-byte vector) is resolved to a host
 * pointer once and then indexed: RAMP masks the base and adds the
 * displacement after, so RAMP(a, 0) + 4 * i is the byte RDF(a, 4 * i) reads,
 * for every address (the null page included), without the area test per
 * float.  Debug builds (SA1_RAM_CHECK) check the base only. */
static inline const sa1_f32a *g_rd(uint32_t addr) { return (const sa1_f32a *)(const void *)RAMP(addr, 0); }
static inline sa1_f32a *g_wr(uint32_t addr) { return (sa1_f32a *)(void *)RAMPW(addr, 0); }
/* A matrix argument as a host pointer: XMTRX (the context's back bank) for
 * 0, else the guest block.  The njXxx bodies work on it in place instead of
 * copying it into a local and back (2026-10-21: those copies were 5 % of a
 * Station Square frame on the PSP-3001, about 9,000 a frame).  In place is
 * the same: every helper below reads what it needs of a column (or of the
 * whole matrix, m_mul and m_invert through a temporary) before it writes
 * there, and a word it leaves alone was only ever rewritten with its own
 * bits. */
static inline const sa1_f32a *m_rp(sh4_ctx *c, uint32_t addr) { return addr ? g_rd(addr) : (const sa1_f32a *)c->xf; }
static inline sa1_f32a *m_wp(sh4_ctx *c, uint32_t addr) { return addr ? g_wr(addr) : (sa1_f32a *)c->xf; }
/* 16 words, all read before any is written (a load into a local and a store
 * back, as before, also for overlapping blocks); as words, so the bits are
 * the bits, and with no load-use stall between a load and its store. */
static inline void m_copy(void *d, const void *s) {
    const sa1_u32a *p = (const sa1_u32a *)s; sa1_u32a *q = (sa1_u32a *)d;
    /* Sixteen scalars, not a loop through a local array: GCC made that a
     * byte-aligned block move (lwl/lwr) through a spilled pointer. */
    uint32_t t0 = p[0], t1 = p[1], t2 = p[2], t3 = p[3], t4 = p[4], t5 = p[5], t6 = p[6], t7 = p[7];
    uint32_t t8 = p[8], t9 = p[9], t10 = p[10], t11 = p[11], t12 = p[12], t13 = p[13], t14 = p[14], t15 = p[15];
    q[0] = t0; q[1] = t1; q[2] = t2; q[3] = t3; q[4] = t4; q[5] = t5; q[6] = t6; q[7] = t7;
    q[8] = t8; q[9] = t9; q[10] = t10; q[11] = t11; q[12] = t12; q[13] = t13; q[14] = t14; q[15] = t15;
}
/* The copies on the VFPU (PSP): math = native (the default) moves an
 * aligned 64-byte block (XMTRX, the matrix stack, most game matrices) with
 * four lv.q and four sv.q, all loads before any store as m_copy does; an
 * unaligned one takes m_copy.  The bits are the bits.  math = check counts
 * the VFPU copies and compares each destination with its source (the
 * summary at the end, kamui_report); math = exact, a strict run, or
 * sa1psp.ini math_exact / --math-exact 8c639c70 keep m_copy.  The desktop
 * always runs m_copy.  (VFPU arithmetic was tried for the rotations,
 * products and transforms: vmul.q / vadd.q differ from the FPU in the last
 * bit or two in most calls, 2026-10-26, so the arithmetic stays scalar.)
 * VFPU registers: C000-C030 are scratch here, as in draw_gu.c. */
#ifdef __psp__
#define NJ_VFPU 1
#define NJ_KEY 0x8c639c70u
extern int nj_mode;                     /* math.c */
static inline int nj_m(void) {
    if (__builtin_expect(nj_mode < 0, 0)) {
        nj_mode = hle_math_mode_for(NJ_KEY);
        sa1_log("ninja math: copies %s", nj_mode == 1 ? "on the VFPU" : nj_mode == 2 ? "on the VFPU, checked" : "scalar (exact)");
    }
    return nj_mode;
}
#define VF_AL(p) ((((uintptr_t)(const void *)(p)) & 15u) == 0)
extern uint32_t njc_unal;               /* math.c */
/* d = s, 64 bytes, both aligned. */
static inline void vf_copy16(void *d, const void *s) {
    __asm__ volatile(
        "lv.q C000, 0(%1)\n lv.q C010, 16(%1)\n lv.q C020, 32(%1)\n lv.q C030, 48(%1)\n"
        "sv.q C000, 0(%0)\n sv.q C010, 16(%0)\n sv.q C020, 32(%0)\n sv.q C030, 48(%0)\n"
        : : "r"(d), "r"(s) : "memory");
}
/* d1 = d2 = s (njPushMatrix(0) pushes XMTRX and keeps it): one load. */
static inline void vf_copy16x2(void *d1, void *d2, const void *s) {
    __asm__ volatile(
        "lv.q C000, 0(%2)\n lv.q C010, 16(%2)\n lv.q C020, 32(%2)\n lv.q C030, 48(%2)\n"
        "sv.q C000, 0(%0)\n sv.q C010, 16(%0)\n sv.q C020, 32(%0)\n sv.q C030, 48(%0)\n"
        "sv.q C000, 0(%1)\n sv.q C010, 16(%1)\n sv.q C020, 32(%1)\n sv.q C030, 48(%1)\n"
        : : "r"(d1), "r"(d2), "r"(s) : "memory");
}
void njc_cmp(const void *d, const void *s);   /* math.c */
#endif
static inline __attribute__((always_inline)) void nj_copy(void *d, const void *s) {
#ifdef NJ_VFPU
    const int md = nj_m();
    if (md && VF_AL(d) && VF_AL(s)) {
        vf_copy16(d, s);
        if (md == 2) njc_cmp(d, s);
        return;
    }
    if (md == 2) njc_unal++;
#endif
    m_copy(d, s);
}

/* The helpers below read every word they use into locals before they write
 * any: sa1_f32a may alias anything, so a loop that multiplies a word and
 * stores it back made GCC reload the operands after each store and wait for
 * each product (qa, 2026-10-27: njScale, the transforms and the rotations
 * were 5 % of a Chaos 0 turn on the PSP-3001).  Each result is the same
 * operations in the same order as before, so the bits are the bits. */
static inline __attribute__((always_inline)) void m_xform(const sa1_f32a *m, const float *v, float w, float *out) {
    const float m0 = m[0], m1 = m[1], m2 = m[2], m4 = m[4], m5 = m[5], m6 = m[6];
    const float m8 = m[8], m9 = m[9], m10 = m[10], m12 = m[12], m13 = m[13], m14 = m[14];
    const float x = v[0], y = v[1], z = v[2];
    const float o0 = m0 * x + m4 * y + m8 * z + m12 * w;
    const float o1 = m1 * x + m5 * y + m9 * z + m13 * w;
    const float o2 = m2 * x + m6 * y + m10 * z + m14 * w;
    out[0] = o0; out[1] = o1; out[2] = o2;
}
/* Columns p and q of m become co * p + s * q and co * q - s * p. */
static inline __attribute__((always_inline)) void m_rot2(sa1_f32a *p, sa1_f32a *q, float s, float co) {
    const float p0 = p[0], p1 = p[1], p2 = p[2], p3 = p[3];
    const float q0 = q[0], q1 = q[1], q2 = q[2], q3 = q[3];
    const float r0 = co * p0 + s * q0, r1 = co * p1 + s * q1, r2 = co * p2 + s * q2, r3 = co * p3 + s * q3;
    const float t0 = co * q0 - s * p0, t1 = co * q1 - s * p1, t2 = co * q2 - s * p2, t3 = co * q3 - s * p3;
    p[0] = r0; p[1] = r1; p[2] = r2; p[3] = r3;
    q[0] = t0; q[1] = t1; q[2] = t2; q[3] = t3;
}
static inline void m_rot_x(sa1_f32a *m, uint32_t a) {
    float s, co; ang_sc(a, &s, &co);
    m_rot2(m + 4, m + 8, s, co);
}
static inline void m_rot_y(sa1_f32a *m, uint32_t a) {
    float s, co; ang_sc(a, &s, &co);
    const float p0 = m[0], p1 = m[1], p2 = m[2], p3 = m[3], q0 = m[8], q1 = m[9], q2 = m[10], q3 = m[11];
    const float r0 = co * p0 - s * q0, r1 = co * p1 - s * q1, r2 = co * p2 - s * q2, r3 = co * p3 - s * q3;
    const float t0 = s * p0 + co * q0, t1 = s * p1 + co * q1, t2 = s * p2 + co * q2, t3 = s * p3 + co * q3;
    m[0] = r0; m[1] = r1; m[2] = r2; m[3] = r3;
    m[8] = t0; m[9] = t1; m[10] = t2; m[11] = t3;
}
static inline void m_rot_z(sa1_f32a *m, uint32_t a) {
    float s, co; ang_sc(a, &s, &co);
    m_rot2(m, m + 4, s, co);
}
static inline __attribute__((always_inline)) void m_translate(sa1_f32a *m, const float *v) {
    float t[3];
    m_xform(m, v, 1.0f, t);
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2];
}
static inline __attribute__((always_inline)) void m_scale(sa1_f32a *m, float x, float y, float z) {
    const float a0 = m[0], a1 = m[1], a2 = m[2], a3 = m[3], b0 = m[4], b1 = m[5], b2 = m[6], b3 = m[7];
    const float c0 = m[8], c1 = m[9], c2 = m[10], c3 = m[11];
    m[0] = a0 * x; m[1] = a1 * x; m[2] = a2 * x; m[3] = a3 * x;
    m[4] = b0 * y; m[5] = b1 * y; m[6] = b2 * y; m[7] = b3 * y;
    m[8] = c0 * z; m[9] = c1 * z; m[10] = c2 * z; m[11] = c3 * z;
}

/* ---- the matrix stack (njPushMatrix / njPopMatrix bodies) ---------------
 * nj_push: arg 0 pushes XMTRX and keeps it, else XMTRX = the matrix at arg;
 * at the limit nothing moves.  Returns what the SDK leaves in r0. */
static inline __attribute__((always_inline)) uint32_t nj_push(sh4_ctx *c, uint32_t arg) {
    uint32_t depth = hle_rd32(NJ_G_DEPTH);
    if (depth >= hle_rd32(NJ_G_MAX)) return 0;
    uint32_t ptr = hle_rd32(NJ_G_PTR);
    sa1_f32a *p0 = g_wr(ptr);
    ptr += 64;
    hle_wr32(NJ_G_DEPTH, depth + 1); hle_wr32(NJ_G_PTR, ptr);
#ifdef NJ_VFPU
    if (arg == 0 && nj_m() == 1 && VF_AL(p0) && VF_AL(c->xf)) { vf_copy16x2(p0, p0 + 16, c->xf); return 1; }
#endif
    nj_copy(p0, c->xf);
    if (arg == 0) nj_copy(g_wr(ptr), c->xf);
    else nj_copy(c->xf, g_rd(arg));
    return 1;
}
/* nj_pop: n levels; refused (0) when that would leave the depth below 1. */
static inline __attribute__((always_inline)) uint32_t nj_pop(sh4_ctx *c, uint32_t n) {
    uint32_t depth = hle_rd32(NJ_G_DEPTH);
    if ((int32_t)(depth - n) < 1) return 0;
    uint32_t ptr = hle_rd32(NJ_G_PTR) - 64 * n;
    hle_wr32(NJ_G_DEPTH, depth - n); hle_wr32(NJ_G_PTR, ptr);
    nj_copy(c->xf, g_rd(ptr));
    return 1;
}
/* njRotateXYZ: Z, then Y, then X; an axis is skipped only when its whole register is 0 (tst rN,rN). */
static inline __attribute__((always_inline)) void nj_rot_xyz(sa1_f32a *m, uint32_t ax, uint32_t ay, uint32_t az) {
    if (az) m_rot_z(m, az);
    if (ay) m_rot_y(m, ay);
    if (ax) m_rot_x(m, ax);
}
#endif
