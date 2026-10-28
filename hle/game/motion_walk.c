/* motion_walk.c - native version of 0x8C040784, the motion model walker.
 *
 * The walker takes an object tree (NJS_OBJECT: +0 flags, +4 model, +8 the
 * position, +20 the angles, +32 the scale, +44 the child, +48 the sibling)
 * and for each node pushes the matrix, applies the node's transform, draws
 * the model, walks the child and pops.  The transform comes from the motion
 * callback in 0x8C88FE24 (flag 0x40 clear) or from the node itself (flag
 * 0x40 set: the static path).  The callbacks the game installs there
 * (0x8C040612, 0x8C0406A0, 0x8C0406E0: two, three or four key streams per
 * node) set up the motion work area at 0x8C88FE40 (+0 the cursor into the
 * motion data, +4 the key pointers, +8 the key counts, +12 the frame as a
 * float, +20 the stream index) and call three element callbacks through
 * 0x8C88FE2C / FE30 / FE34: the position, the rotation and the scale, each
 * either from the keys (binary search for the key at the frame, then a
 * linear blend with the next key; the rotation keys are integers) or from
 * the node.
 *
 * mw_walk is the recompiled body with the callbacks the game uses run in
 * line, the key search and blends folded in, and the Ninja matrix steps
 * done by the same helpers math.c uses (math_inl.h).  The registers it
 * hands to every guest call and leaves at its exit are those of the
 * recompiled body, including the ones the callees leave behind; it writes
 * the walker's and the callbacks' guest stack frames as they do (the
 * element callbacks' frames only when mw_mirror is set, the default: they
 * are dead once a node is done, but then a --dump compares byte for byte).
 * Every float step is the body's, in its order (no contraction: this file
 * is built with -ffp-contract=off): the blend weight is (frame - k0) times
 * fsrra(d) squared when the key gap d is positive, each component
 * k0 * (1 - w) + k1 * w, the rotation through ftrc (sh4_ftrc).  Any other
 * callback, a draw callback in 0x8C890074, and the static path's rotation
 * go through sa1_call from the same register state as the recompiled call
 * site, so the walker is the body in every case.
 *
 * The mode is hle_math_mode_exact_ok(0x8c040784): native unless the
 * routine is listed in sa1psp.ini math_exact / --math-exact 8c040784 or
 * --math exact is given, strict runs included (math = native is bit-exact
 * here).  math = check (or SA1_WALK_MODE=check on the desktop) runs, for
 * each node with a known motion callback, the recompiled callback first
 * and then the native one from the same state, compares the context (all
 * of sh4_ctx, XMTRX included), the work area 0x8C88FE40..5B and the guest
 * stack below r15, logs the first differences and keeps the recompiled
 * result; the matrix pushes and pops are checked the same way against
 * math.c's njPushMatrix / njPopMatrix.  The wrapper is compiled only when
 * the emitted game.h has the renamed body (recomp/sa1_us_1005/hle_wraps.tsv).
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "../seam.h"
#include "../ninja/math_inl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mw_check_report(void);

#ifdef SA1_WRAP_8C040784

void fn_8c040784_orig(sh4_ctx *c);
void fn_8c037098(sh4_ctx *c);

#define MW_KEY      0x8c040784u
#define FE_CB       0x8c88fe24u         /* the motion callback */
#define FE_POS      0x8c88fe2cu         /* the element callbacks */
#define FE_ANG      0x8c88fe30u
#define FE_SCL      0x8c88fe34u
#define FE_CUR      0x8c88fe40u         /* the work area */
#define FE_KEYS     0x8c88fe44u
#define FE_NB       0x8c88fe48u
#define FE_FRAME    0x8c88fe4cu
#define FE_IDX      0x8c88fe54u
#define DRAW_CB     0x8c890074u
#define A_ROTXYZ    0x8c639cf4u         /* njRotateXYZ */
#define A_ROTALT    0x8c639ff8u         /* the alternative rotation (a seam stub today) */
#define A_PUSH      0x8c639c70u
#define A_TRANS     0x8c63a878u
#define A_SCALE     0x8c63a69cu
#define A_DRAW      0x8c037098u

static int mw_mode = -1;                /* 0 exact, 1 native, 2 check */
static int mw_mirror = 1;               /* write the element callbacks' dead frames */
static uint32_t mw_walks, mw_nodes, mw_native, mw_fb_motion, mw_fb_elem, mw_static, mw_draw;
static uint32_t mw_ch_n[3];             /* motion callbacks run in line: 2, 3, 4 streams */
static uint32_t mw_el_n[8];             /* element callbacks run in line, by kind (log) */
static uint32_t ck_chains, ck_skip, ck_bad, ck_pp, ck_pp_bad, ck_words, ck_logged, ck_last;

/* ---- the element callbacks ------------------------------------------------
 * Each runs with r4 = the node and r15 = the 0x8C040612-family frame; it
 * pushes r14, r13, r12, r11 and pr (0 under the emitter) and opens 16
 * bytes: [sp] the key index the search finds, [sp+4..12] the blended
 * values.  The search (0x8C03FEB8) pushes r14 (then 0x8C88FE40) below sp. */
static inline __attribute__((always_inline)) void mw_frame(sh4_ctx *c, uint32_t s15) {
    if (!mw_mirror) return;
    WK32(s15, -4, c->r[14]); WK32(s15, -8, c->r[13]); WK32(s15, -12, c->r[12]);
    WK32(s15, -16, c->r[11]); WK32(s15, -20, 0u);
}

/* The key search: the last key whose frame is <= ftrc(frame), compared
 * unsigned; nb = 0 never ends, as in the body.  Leaves r0 (the last key
 * frame read), r1 = ftrc(frame), r2 = 0, r3 = 0, r5 = hi, r7 = the last
 * midpoint, fr3 = the frame, t = 0, fpul in c; most are overwritten by the
 * blend that follows, the body's order kept anyway. */
static inline __attribute__((always_inline)) uint32_t mw_search(sh4_ctx *c, uint32_t keys, uint32_t nb, uint32_t sp) {
    uint32_t r0 = c->r[0], r7 = c->r[7], lo = 0, hi = nb;
    const float fr3 = RDF(FE_FRAME, 0);
    const uint32_t r1 = sh4_ftrc(fr3);
    if (mw_mirror) WK32(sp, -4, FE_CUR);
    while (hi - lo - 1u > 0u) {
        r7 = (hi + lo) >> 1;
        r0 = RD32((r7 << 4) + keys, 0);
        if (r1 >= r0) lo = r7; else hi = r7;
    }
    if (mw_mirror) WR32(sp, 0, lo);
    c->r[0] = r0; c->r[1] = r1; c->r[2] = 0; c->r[3] = 0; c->r[5] = hi; c->r[7] = r7;
    c->fr[3] = fr3; c->t = 0; c->fpul = r1;
    return lo;
}

/* The blend weight shared by both blends: k0 / k1 (k1 wraps to key 0 at
 * the end), w = (frame - k0.frame) * fsrra(d)^2 for d > 0, else unscaled. */
typedef struct { uint32_t k0, k1, kf0, d, t, fpul; float fr3, w; } mw_wt;
static inline __attribute__((always_inline)) void mw_weight(mw_wt *b, uint32_t keys, uint32_t lo, uint32_t nb) {
    const uint32_t k0 = (lo << 4) + keys;
    const uint32_t k1 = lo + 1u >= nb ? keys : k0 + 16u;
    const uint32_t kf0 = RD32(k0, 0);
    const float fr3 = (float)(int32_t)kf0;
    const uint32_t d = RD32(k1, 0) - kf0;
    float w = RDF(FE_FRAME, 0);
    w = w - fr3;
    b->t = (int32_t)d > 0;
    b->fpul = kf0;
    if (b->t) {
        float s = (float)(int32_t)d;
        s = FSRRA(s);
        s = s * s;
        w = w * s;
        b->fpul = d;
    }
    b->k0 = k0; b->k1 = k1; b->kf0 = kf0; b->d = d; b->fr3 = fr3; b->w = w;
}

/* 0x8C0400A0 / 0x8C0400F8: the position / scale element; keyed floats
 * (0x8C03FF2C) or the node's at +off. */
static inline __attribute__((always_inline)) void mw_el_vec(sh4_ctx *c, uint32_t off, int scale) {
    const uint32_t node = c->r[4], s15 = c->r[15], sp = s15 - 36u;
    mw_frame(c, s15);
    const uint32_t idx = RD32(FE_IDX, 0);
    const uint32_t r5 = idx << 2;
    const uint32_t p = RD32(RD32(FE_KEYS, 0) + r5, 0);
    float x, y, z;
    if (p == 0) {
        const uint32_t a = node + off;
        x = RDF(a, 0); y = RDF(a + 4u, 0); z = RDF(a + 8u, 0);
        c->r[5] = r5;
    } else {
        const uint32_t nb = RD32(RD32(FE_NB, 0) + r5, 0);
        const uint32_t lo = mw_search(c, p, nb, sp);
        mw_wt b;
        mw_weight(&b, p, lo, nb);
        const float w = b.w;
        float u = 1.0f;
        u = u - w;
        float f8 = RDF(b.k0 + 4u, 0), f6 = RDF(b.k1 + 4u, 0);
        f8 = f8 * u;
        float f7 = RDF(b.k1 + 8u, 0);
        f6 = f6 * w;
        f8 = f8 + f6;
        f6 = RDF(b.k0 + 8u, 0);
        f6 = f6 * u;
        f7 = f7 * w;
        f6 = f6 + f7;
        f7 = RDF(b.k0 + 12u, 0);
        f7 = f7 * u;
        float f5 = RDF(b.k1 + 12u, 0);
        f5 = f5 * w;
        f7 = f7 + f5;
        if (mw_mirror) { WRF(sp + 12u, 0, f7); WRF(sp + 8u, 0, f6); WRF(sp + 4u, 0, f8); }
        x = f8; y = f6; z = f7;
        c->r[0] = b.k0; c->r[2] = FE_FRAME; c->r[3] = b.kf0; c->r[5] = b.k0 + 12u; c->r[6] = nb; c->r[7] = sp + 4u;
        c->fr[3] = b.fr3; c->fr[7] = f7; c->fr[8] = f8; c->t = b.t; c->fpul = b.fpul;
    }
    c->r[4] = 0;
    c->fr[4] = x; c->fr[5] = y; c->fr[6] = z;
    if (scale) m_scale((sa1_f32a *)c->xf, x, y, z);
    else { const float v[3] = {x, y, z}; m_translate((sa1_f32a *)c->xf, v); }
    WR32(FE_IDX, 0, idx + 1u);
}

/* One integer component of the rotation blend (0x8C03FF90 / 0x8C040000). */
static inline __attribute__((always_inline)) uint32_t mw_iblend(uint32_t a, uint32_t b, float u, float w, float *f2, float *f3) {
    float x = (float)(int32_t)a;
    x = x * u;
    float y = (float)(int32_t)b;
    y = y * w;
    x = x + y;
    *f2 = y; *f3 = x;
    return sh4_ftrc(x);
}

/* 0x8C040150 / 0x8C0401A8: the rotation element (njRotateXYZ, or the
 * alternative rotation through its symbol); keyed integers or the node's
 * angles at +20. */
static inline __attribute__((always_inline)) void mw_el_rot(sh4_ctx *c, int alt) {
    const uint32_t node = c->r[4], s15 = c->r[15], sp = s15 - 36u;
    mw_frame(c, s15);
    const uint32_t idx = RD32(FE_IDX, 0);
    const uint32_t r5 = idx << 2;
    const uint32_t p = RD32(RD32(FE_KEYS, 0) + r5, 0);
    uint32_t ax, ay, az;
    if (p == 0) {
        const uint32_t a = node + 20u;
        ax = RD32(a, 0); ay = RD32(a, 4); az = RD32(a, 8);
    } else {
        const uint32_t nb = RD32(RD32(FE_NB, 0) + r5, 0);
        const uint32_t lo = mw_search(c, p, nb, sp);
        mw_wt b;
        mw_weight(&b, p, lo, nb);
        const float w = b.w;
        float u = 1.0f, f2, f3;
        u = u - w;
        ax = mw_iblend(RD32(b.k0, 4), RD32(b.k1, 4), u, w, &f2, &f3);
        if (mw_mirror) WR32(sp + 4u, 0, ax);
        ay = mw_iblend(RD32(b.k0, 8), RD32(b.k1, 8), u, w, &f2, &f3);
        if (mw_mirror) WR32(sp + 4u, 4, ay);
        az = mw_iblend(RD32(b.k0, 12), RD32(b.k1, 12), u, w, &f2, &f3);
        if (mw_mirror) WR32(sp + 4u, 8, az);
        c->r[0] = b.k0; c->r[2] = FE_FRAME; c->r[3] = az;
        c->fr[2] = f2; c->fr[3] = f3; c->fr[4] = w; c->fr[5] = u; c->t = b.t; c->fpul = az;
    }
    c->r[4] = 0; c->r[5] = ax; c->r[6] = ay; c->r[7] = az;
    if (!alt) nj_rot_xyz((sa1_f32a *)c->xf, ax, ay, az);
    else { c->r[15] = sp; sdk_8c639ff8(c); c->r[15] = s15; }
    WR32(FE_IDX, 0, idx + 1u);
}

/* 0x8C04057A / 0x8C040588 / 0x8C040596 / 0x8C0405A4: the node's own values
 * (no key, the stream index not advanced); each tail-calls the SDK with r3
 * = its address. */
static inline __attribute__((always_inline)) void mw_el_static(sh4_ctx *c, int kind) {
    const uint32_t node = c->r[4];
    if (kind == 0 || kind == 3) {
        const uint32_t a = node + (kind == 0 ? 8u : 32u);
        const float x = RDF(a, 0), y = RDF(a + 4u, 0), z = RDF(a + 8u, 0);
        c->r[3] = kind == 0 ? A_TRANS : A_SCALE; c->r[4] = 0;
        c->fr[4] = x; c->fr[5] = y; c->fr[6] = z;
        if (kind == 3) m_scale((sa1_f32a *)c->xf, x, y, z);
        else { const float v[3] = {x, y, z}; m_translate((sa1_f32a *)c->xf, v); }
    } else {
        const uint32_t a = node + 20u;
        const uint32_t ax = RD32(a, 0), ay = RD32(a, 4), az = RD32(a, 8);
        c->r[3] = kind == 1 ? A_ROTXYZ : A_ROTALT; c->r[4] = 0;
        c->r[5] = ax; c->r[6] = ay; c->r[7] = az;
        if (kind == 1) nj_rot_xyz((sa1_f32a *)c->xf, ax, ay, az);
        else sdk_8c639ff8(c);
    }
}

/* An element callback by address: the known ones in line, others through
 * sa1_call (the body's call: r1..r4, r12..r15 already in c). */
static inline int mw_el_known(uint32_t a) {
    switch (a & 0x1fffffffu) {
    case 0x0c0400a0u: case 0x0c0400f8u: case 0x0c040150u: case 0x0c0401a8u:
    case 0x0c04057au: case 0x0c040588u: case 0x0c040596u: case 0x0c0405a4u: return 1;
    default: return 0;
    }
}
static inline __attribute__((always_inline)) void mw_el(sh4_ctx *c, uint32_t a) {
    switch (a & 0x1fffffffu) {
    case 0x0c0400a0u: mw_el_n[0]++; mw_el_vec(c, 8u, 0); return;
    case 0x0c0400f8u: mw_el_n[1]++; mw_el_vec(c, 32u, 1); return;
    case 0x0c040150u: mw_el_n[2]++; mw_el_rot(c, 0); return;
    case 0x0c0401a8u: mw_el_n[3]++; mw_el_rot(c, 1); return;
    case 0x0c04057au: mw_el_n[4]++; mw_el_static(c, 0); return;
    case 0x0c040588u: mw_el_n[5]++; mw_el_static(c, 1); return;
    case 0x0c040596u: mw_el_n[6]++; mw_el_static(c, 2); return;
    case 0x0c0405a4u: mw_el_n[7]++; mw_el_static(c, 3); return;
    default: mw_fb_elem++; sa1_call(c, a); return;
    }
}

/* 0x8C040612 / 0x8C0406A0 / 0x8C0406E0: one record of the motion data per
 * node, nb pointers at +nboff, the cursor advanced by stride.  Frame: r14,
 * r13, r12, pr; r12 = the node, r13 = the cursor, r14 = 0x8C88FE40 while
 * the three elements run. */
static void mw_chain(sh4_ctx *c, uint32_t nboff, uint32_t stride) {
    const uint32_t node = c->r[4];
    uint32_t r15 = c->r[15], r12, r13, a;
    r15 -= 4u; WK32(r15, 0, c->r[14]);
    r15 -= 4u; WK32(r15, 0, c->r[13]);
    r15 -= 4u; WK32(r15, 0, c->r[12]);
    r15 -= 4u; WK32(r15, 0, 0u);
    r13 = RD32(FE_CUR, 0);
    WR32(FE_KEYS, 0, r13);
    WR32(FE_NB, 0, r13 + nboff);
    WR32(FE_IDX, 0, 0u);
    a = RD32(FE_POS, 0);
    c->r[1] = FE_POS; c->r[2] = a; c->r[3] = 0; c->r[4] = node;
    c->r[12] = node; c->r[13] = r13; c->r[14] = FE_CUR; c->r[15] = r15;
    mw_el(c, a);
    r12 = c->r[12];
    a = RD32(FE_ANG, 0);
    c->r[2] = FE_ANG; c->r[3] = a; c->r[4] = r12;
    mw_el(c, a);
    r12 = c->r[12];
    a = RD32(FE_SCL, 0);
    c->r[2] = FE_SCL; c->r[3] = a; c->r[4] = r12;
    mw_el(c, a);
    r13 = c->r[13]; r15 = c->r[15];
    WR32(FE_CUR, 0, r13 + stride);
    c->r[12] = RK32(r15, 4); c->r[13] = RK32(r15, 8); c->r[14] = RK32(r15, 12); c->r[15] = r15 + 16u;
}

static inline int mw_chain_kind(uint32_t a, uint32_t *nboff, uint32_t *stride) {
    switch (a & 0x1fffffffu) {
    case 0x0c040612u: *nboff = 8u; *stride = 16u; mw_ch_n[0]++; return 1;
    case 0x0c0406a0u: *nboff = 12u; *stride = 24u; mw_ch_n[1]++; return 1;
    case 0x0c0406e0u: *nboff = 16u; *stride = 32u; mw_ch_n[2]++; return 1;
    default: return 0;
    }
}

/* ---- check mode ----------------------------------------------------------- */
#define CK_STK 128u                     /* guest stack bytes below r15 compared */
#define CK_LOG 200u
static const char *ck_field(unsigned w, char *buf) {
    static const char *const tail[] = {"t", "s", "q", "m", "mach", "macl", "pr", "gbr", "fpul", "fpscr", "vbr"};
    if (w < 16) { snprintf(buf, 16, "r%u", w); return buf; }
    if (w < 32) { snprintf(buf, 16, "fr%u", w - 16); return buf; }
    if (w < 48) { snprintf(buf, 16, "xf%u", w - 32); return buf; }
    return w - 48 < 11 ? tail[w - 48] : "?";
}
static unsigned ck_cmp(const char *what, uint32_t node, const void *ref, const void *nat, unsigned nwords,
                       uint32_t gbase) {
    const uint32_t *a = (const uint32_t *)ref, *b = (const uint32_t *)nat;
    unsigned bad = 0;
    for (unsigned i = 0; i < nwords; i++) {
        if (a[i] == b[i]) continue;
        bad++;
        if (ck_logged < CK_LOG) {
            char buf[16];
            ck_logged++;
            if (gbase) sa1_log("check: walker f%u %s node %08x [%08x] ref %08x native %08x", hle_frame_count(), what,
                               node, gbase + 4u * i, a[i], b[i]);
            else sa1_log("check: walker f%u %s node %08x %s ref %08x native %08x", hle_frame_count(), what, node,
                         ck_field(i, buf), a[i], b[i]);
        }
    }
    return bad;
}

static void ck_summary(int force) {
    const uint32_t f = hle_frame_count();
    if (!force && f / 600 == ck_last / 600) return;
    ck_last = f;
    sa1_log("check: motion walker summary f%u nodes %u chains checked %u (unchecked %u) differing %u words %u; "
            "push/pop checked %u differing %u", f, mw_nodes, ck_chains, ck_skip, ck_bad, ck_words, ck_pp, ck_pp_bad);
}

/* The recompiled callback and then the native chain from the same state;
 * the recompiled result stays. */
static void ck_chain(sh4_ctx *c, uint32_t tgt, uint32_t nboff, uint32_t stride) {
    if (!mw_el_known(RD32(FE_POS, 0)) || !mw_el_known(RD32(FE_ANG, 0)) || !mw_el_known(RD32(FE_SCL, 0))) {
        ck_skip++;
        mw_chain(c, nboff, stride);
        return;
    }
    const uint32_t stk = c->r[15] - CK_STK, node = c->r[4];
    uint32_t fe0[7], fe1[7], fe2[7], st0[CK_STK / 4], st1[CK_STK / 4], st2[CK_STK / 4];
    const sh4_ctx c0 = *c;
    memcpy(fe0, RAMP(FE_CUR, 0), 28); memcpy(st0, RAMP(stk, 0), CK_STK);
    sa1_call(c, tgt);
    const sh4_ctx c1 = *c;
    memcpy(fe1, RAMP(FE_CUR, 0), 28); memcpy(st1, RAMP(stk, 0), CK_STK);
    *c = c0;
    memcpy(RAMPW(FE_CUR, 0), fe0, 28); memcpy(RAMPW(stk, 0), st0, CK_STK);
    mw_chain(c, nboff, stride);
    memcpy(fe2, RAMP(FE_CUR, 0), 28); memcpy(st2, RAMP(stk, 0), CK_STK);
    unsigned bad = ck_cmp("ctx", node, &c1, c, sizeof(sh4_ctx) / 4, 0);
    bad += ck_cmp("work", node, fe1, fe2, 7, FE_CUR);
    if (mw_mirror) bad += ck_cmp("stack", node, st1, st2, CK_STK / 4, stk);
    ck_chains++;
    if (bad) { ck_bad++; ck_words += bad; }
    *c = c1;
    memcpy(RAMPW(FE_CUR, 0), fe1, 28); memcpy(RAMPW(stk, 0), st1, CK_STK);
}

/* njPushMatrix(0) / njPopMatrix(1): math.c's entry point and the shared
 * body from the same state (depth, slot pointer, the slots, XMTRX). */
static uint32_t ck_pushpop(sh4_ctx *c, int push) {
    const uint32_t d0 = RD32(NJ_G_DEPTH, 0), p0 = RD32(NJ_G_PTR, 0), lo = p0 - 64u;
    uint32_t m0[48], m1[48], m2[48];
    uint32_t w1[2], w2[2];
    const sh4_ctx c0 = *c;
    memcpy(m0, RAMP(lo, 0), 192);
    if (push) sdk_8c639c70(c); else sdk_8c639b98(c);
    const sh4_ctx c1 = *c;
    w1[0] = RD32(NJ_G_DEPTH, 0); w1[1] = RD32(NJ_G_PTR, 0);
    memcpy(m1, RAMP(lo, 0), 192);
    *c = c0;
    WR32(NJ_G_DEPTH, 0, d0); WR32(NJ_G_PTR, 0, p0); memcpy(RAMPW(lo, 0), m0, 192);
    c->r[0] = push ? nj_push(c, 0) : nj_pop(c, 1);
    w2[0] = RD32(NJ_G_DEPTH, 0); w2[1] = RD32(NJ_G_PTR, 0);
    unsigned bad = ck_cmp(push ? "push ctx" : "pop ctx", 0, &c1, c, sizeof(sh4_ctx) / 4, 0);
    bad += ck_cmp(push ? "push depth/ptr" : "pop depth/ptr", 0, w1, w2, 2, NJ_G_PTR);
    memcpy(m2, RAMP(lo, 0), 192);
    bad += ck_cmp(push ? "push slots" : "pop slots", 0, m1, m2, 48, lo);
    ck_pp++;
    if (bad) ck_pp_bad++;
    *c = c1;
    WR32(NJ_G_DEPTH, 0, w1[0]); WR32(NJ_G_PTR, 0, w1[1]); memcpy(RAMPW(lo, 0), m1, 192);
    return c1.r[0];
}

/* ---- the walker ----------------------------------------------------------- */
static void mw_walk(sh4_ctx *c) {
    uint32_t r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15, t;
    float fr4, fr5, fr6;
    r4 = c->r[4]; r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
    r8 = c->r[8]; r9 = c->r[9]; r10 = c->r[10]; r11 = c->r[11];
    r12 = c->r[12]; r13 = c->r[13]; r14 = c->r[14]; r15 = c->r[15];
    fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6];
    (void)r1; (void)r5; (void)r6; (void)r7; (void)fr4; (void)fr5; (void)fr6;
    mw_walks++;
    /* the frame: r14..r8, then pr (0 under the emitter); the static path
     * reads its three angles back from @r15, @(4,r15), @(8,r15) */
    r15 -= 4u; WK32(r15, 0, r14);
    r14 = r4;
    r15 -= 4u; WK32(r15, 0, r13);
    r15 -= 4u; WK32(r15, 0, r12);
    r15 -= 4u; WK32(r15, 0, r11);
    r11 = 0x20u;
    r15 -= 4u; WK32(r15, 0, r10);
    r10 = 0x40u;
    r15 -= 4u; WK32(r15, 0, r9);
    r15 -= 4u; WK32(r15, 0, r8);
    r15 -= 4u; WK32(r15, 0, 0u);
    r8 = A_ROTXYZ;
    r9 = A_PUSH;
    do {
        sa1_depth_probe();
        mw_nodes++;
        r13 = RD32(r14, 0);
        c->r[4] = 0; c->r[15] = r15;
        c->r[0] = mw_mode == 2 ? ck_pushpop(c, 1) : nj_push(c, 0);
        r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
        fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6];
        r3 = r13;
        t = (r3 & r10) == 0;
        if (t) {
            /* the motion callback, r4 = the node */
            uint32_t nboff, stride;
            r2 = FE_CB;
            r1 = RD32(FE_CB, 0);
            r4 = r14;
            c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4;
            c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11;
            c->r[12] = r12; c->r[13] = r13; c->r[14] = r14; c->t = t;
            if (mw_chain_kind(r1, &nboff, &stride)) {
                mw_native++;
                if (mw_mode == 2) ck_chain(c, r1, nboff, stride);
                else mw_chain(c, nboff, stride);
            } else {
                mw_fb_motion++;
                sa1_call(c, r1);
            }
            r1 = c->r[1]; r3 = c->r[3];
            r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
            r8 = c->r[8]; r9 = c->r[9]; r10 = c->r[10]; r11 = c->r[11];
            r13 = c->r[13]; r14 = c->r[14]; r15 = c->r[15];
            fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6];
        } else {
            /* the static path (flag 0x40; reached nowhere the census looked):
             * njTranslate(node+8), the rotation with the angles from the
             * frame, njScale(node+32), each from the body's registers */
            mw_static++;
            r4 = r14 + 8u;
            r3 = A_TRANS;
            fr4 = RDF(r4, 0); r4 += 4u;
            fr5 = RDF(r4, 0); r4 += 4u;
            fr6 = RDF(r4, 0);
            r4 = 0;
            c->r[3] = r3; c->r[4] = r4; c->fr[4] = fr4; c->fr[5] = fr5; c->fr[6] = fr6;
            sdk_8c63a878(c);
            r1 = c->r[1]; r3 = c->r[3];
            fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6];
            r2 = r13;
            t = (r2 & r11) == 0;
            r12 = t ? r8 : A_ROTALT;
            r5 = RK32(r15, 0); r6 = RK32(r15, 4); r7 = RK32(r15, 8);
            r4 = 0;
            c->r[2] = r2; c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7;
            c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11;
            c->r[12] = r12; c->r[13] = r13; c->r[14] = r14; c->t = t;
            sa1_call(c, r12);
            r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
            r8 = c->r[8]; r9 = c->r[9]; r10 = c->r[10]; r11 = c->r[11];
            r13 = c->r[13]; r14 = c->r[14]; r15 = c->r[15];
            r4 = r14 + 32u;
            r3 = A_SCALE;
            fr4 = RDF(r4, 0); r4 += 4u;
            fr5 = RDF(r4, 0); r4 += 4u;
            fr6 = RDF(r4, 0);
            r4 = 0;
            c->r[3] = r3; c->r[4] = r4; c->fr[4] = fr4; c->fr[5] = fr5; c->fr[6] = fr6;
            sdk_8c63a69c(c);
            r1 = c->r[1]; r3 = c->r[3];
            r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
            fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6];
        }
        /* the draw callback (NULL almost everywhere) */
        r2 = DRAW_CB;
        r12 = RD32(DRAW_CB, 0);
        t = r12 == 0;
        if (!t) {
            mw_draw++;
            r4 = r14;
            c->r[2] = r2; c->r[4] = r4; c->r[12] = r12; c->t = t;
            sa1_call(c, r12);
            r1 = c->r[1];
            r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
            r8 = c->r[8]; r9 = c->r[9]; r10 = c->r[10]; r11 = c->r[11];
            r12 = c->r[12]; r13 = c->r[13]; r14 = c->r[14]; r15 = c->r[15];
            fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6];
        }
        /* the model, unless flag 8 hides it */
        r2 = RD32(r14, 4);
        t = r2 == 0;
        if (!t) {
            r3 = 8u;
            t = (r13 & r3) == 0;
            if (t) {
                r3 = A_DRAW;
                r4 = RD32(r14, 4);
                c->r[3] = r3; c->r[4] = r4; c->r[12] = r12;
                fn_8c037098(c);
                r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
                r8 = c->r[8]; r9 = c->r[9]; r10 = c->r[10]; r11 = c->r[11];
                r12 = c->r[12]; r13 = c->r[13]; r14 = c->r[14]; r15 = c->r[15];
                fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6];
            }
        }
        /* the child */
        r2 = RD32(r14, 44);
        t = r2 == 0;
        if (!t) {
            r4 = RD32(r14, 44);
            c->r[4] = r4; c->r[12] = r12;
            mw_walk(c);
            r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
            r8 = c->r[8]; r9 = c->r[9]; r10 = c->r[10]; r11 = c->r[11];
            r12 = c->r[12]; r14 = c->r[14]; r15 = c->r[15];
            fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6];
        }
        r4 = 1u;
        c->r[4] = r4;
        c->r[0] = mw_mode == 2 ? ck_pushpop(c, 0) : nj_pop(c, 1);
        r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
        fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6];
        /* the sibling */
        r14 = RD32(r14, 48);
        t = r14 == 0;
    } while (!t);
    r15 += 4u;                          /* pr */
    r8 = RK32(r15, 0); r15 += 4u;
    r9 = RK32(r15, 0); r15 += 4u;
    r10 = RK32(r15, 0); r15 += 4u;
    r11 = RK32(r15, 0); r15 += 4u;
    r12 = RK32(r15, 0); r15 += 4u;
    r13 = RK32(r15, 0); r15 += 4u;
    r14 = RK32(r15, 0); r15 += 4u;
    c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11;
    c->r[12] = r12; c->r[13] = r13; c->r[14] = r14; c->r[15] = r15;
    c->t = t;
}

static void mw_init(void) {
    int m = hle_math_mode_exact_ok(MW_KEY);
#ifndef __psp__
    const char *e = getenv("SA1_WALK_MODE");
    if (e) m = !strcmp(e, "exact") ? 0 : !strcmp(e, "check") ? 2 : 1;
    const char *mi = getenv("SA1_WALK_MIRROR");
    if (mi && mi[0] == '0') mw_mirror = 0;
#endif
    mw_mode = m;
    sa1_log("motion walker: math %s%s", m == 1 ? "native" : m == 2 ? "check" : "exact",
            m && !mw_mirror ? " (element frames not mirrored)" : "");
}

void mw_check_report(void) {
    if (mw_mode < 0) return;
    sa1_log("motion walker: walks %u nodes %u native chains %u motion-callback fallbacks %u element fallbacks %u "
            "static %u draw callbacks %u", mw_walks, mw_nodes, mw_native, mw_fb_motion, mw_fb_elem, mw_static, mw_draw);
    sa1_log("motion walker: chains 0x8c040612 %u 0x8c0406a0 %u 0x8c0406e0 %u", mw_ch_n[0], mw_ch_n[1], mw_ch_n[2]);
    sa1_log("motion walker: elements in line: translate keyed %u scale keyed %u rotate keyed %u rotate-alt keyed %u "
            "node translate %u node rotate %u node rotate-alt %u node scale %u", mw_el_n[0], mw_el_n[1], mw_el_n[2],
            mw_el_n[3], mw_el_n[4], mw_el_n[5], mw_el_n[6], mw_el_n[7]);
    if (mw_mode == 2) ck_summary(1);
}

void fn_8c040784(sh4_ctx *c) {
    HLE_ENTER(MW_KEY, "motion walker fn_8c040784");
    if (__builtin_expect(mw_mode < 0, 0)) mw_init();
    if (mw_mode == 0) { mw_walks++; fn_8c040784_orig(c); return; }
    mw_walk(c);
    if (mw_mode == 2) ck_summary(0);
}
#else
void mw_check_report(void) {}
#endif
