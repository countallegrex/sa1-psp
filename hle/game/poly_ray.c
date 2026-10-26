/* poly_ray.c - native version of 0x8C029400, the nearest polygon along each
 * axis from a point, over the collision hits of one object.
 *
 * r4 is a record: +0..+8 the point P, then six 32-byte slots from +12, two
 * per axis (x: 0 and 1, y: 2 and 3, z: 4 and 5; the first for a polygon
 * below P on that axis, the second for one above).  A slot is +0 a flag
 * (1: filled), +4 the owner word of the object's node, +8 and +12 two
 * 16-bit angles of the polygon's unit normal (asin of n.z, minus atan2 of
 * n.x and n.y, times the constant at 0x8C0294D4), +16 the polygon's
 * coordinate on that axis at P, +20..+28 the unit normal.  r5 is an object
 * id: its node in the collision list @0x8C02D548 (collision.c: +8 the id,
 * +12 the owner word, +16 a linked list of polygons: +0 next, +24..+56
 * three vertices).
 *
 * The function (0x8C029400) clears the six flags and seeds each slot's +16
 * with P +- a constant, then for each polygon whose first vertex is within
 * the distance @0x8C029644 (squared) of P tests the axes in the order z, x,
 * y: P inside the triangle projected along the axis (three 2D edge signs),
 * then 0x8C027360's normal n (not unit), the plane's coordinate h at P
 * (skipped when n's axis component squared is below @0x8C0294D0), and the
 * slot on P's side of h is replaced when it is empty or h is nearer; the
 * normal is made unit (njScalor / njUnitVector, (0, 1, 0) for a zero one)
 * and its angles come from the game's libm (0x8C10D0F8 atan2, 0x8C10D058
 * asin).  r0 is 1 when any slot was filled.
 *
 * pr_native is that with host pointers and no guest stack frame: the same
 * float operations in the same order (fmac through FMAC_F, the normal as
 * 0x8C027360 computes it, njScalor / njUnitVector as math.c does), and the
 * libm calls are the game's own bodies, so math = native is bit-exact with
 * the recompiled body (math = exact, strict runs); math = check runs both
 * and compares the record bit for bit.  sa1psp.ini math_exact / --math-exact
 * 8c029400 keeps this routine exact under math = native (A/B timing).  The
 * wrapper is compiled only when the emitted game.h has the renamed body
 * (recomp/sa1_us_1005/hle_wraps.tsv).
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"

#include <stdlib.h>

#ifdef SA1_WRAP_8C029400

#define HP(a)         ((const uint8_t *)RAMP(a, 0))
#define HPW(a)        ((uint8_t *)RAMPW(a, 0))
#define H32(p, o)     (*(const sa1_u32a *)(const void *)((p) + (o)))
#define HF(p, o)      (*(const sa1_f32a *)(const void *)((p) + (o)))
#define HW32(p, o, v) (*(sa1_u32a *)(void *)((p) + (o)) = (uint32_t)(v))
#define HWF(p, o, v)  (*(sa1_f32a *)(void *)((p) + (o)) = (v))

#define COLL_LIST 0x8c02d548u           /* the collision list head (collision.c) */
#define REC_SIZE  204u                  /* P and the six slots */

void fn_8c029400_orig(sh4_ctx *c);
void fn_8c10d058(sh4_ctx *c);
void fn_8c10d0f8(sh4_ctx *c);

/* The game's libm: atan2 takes fr4, fr5 (as the body loads them: n.x, n.y),
 * asin fr4; the result is fr0. */
static inline float pr_atan2(sh4_ctx *c, float a, float b) {
    c->fr[4] = a; c->fr[5] = b; fn_8c10d0f8(c); return c->fr[0];
}
static inline float pr_asin(sh4_ctx *c, float a) {
    c->fr[4] = a; fn_8c10d058(c); return c->fr[0];
}

/* One axis (a; u and w the other two, in the order the body uses them). */
static inline __attribute__((always_inline)) int pr_axis(sh4_ctx *c, uint8_t *R, const float *P,
        const float *v0, const float *v1, const float *v2, int a, int u, int w,
        float lim, float K, uint32_t owner) {
    const float e0 = (v1[u] - v0[u]) * (P[w] - v0[w]) - (v1[w] - v0[w]) * (P[u] - v0[u]);
    const float e1 = (v2[u] - v1[u]) * (P[w] - v1[w]) - (v2[w] - v1[w]) * (P[u] - v1[u]);
    const float e2 = (v0[u] - v2[u]) * (P[w] - v2[w]) - (v0[w] - v2[w]) * (P[u] - v2[u]);
    if (0.0f > e0 * e1) return 0;
    if (0.0f > e1 * e2) return 0;
    if (0.0f > e0 * e2) return 0;
    /* 0x8C027360(v0, v1, v2) */
    const float d9 = v1[0] - v0[0], d4 = v0[0] - v2[0];
    const float d8 = v1[1] - v0[1], d6 = v0[1] - v2[1];
    const float d7 = v1[2] - v0[2], d5 = v0[2] - v2[2];
    const float n[3] = {d7 * d6 - d8 * d5, d9 * d5 - d7 * d4, d8 * d4 - d9 * d6};
    const float D = ((-(n[0] * v0[0])) - n[1] * v0[1]) - n[2] * v0[2];
    if (lim > n[a] * n[a]) return 0;
    const float h = ((-(n[u] * P[u])) - n[w] * P[w] - D) / n[a];
    uint8_t *s;
    if (P[a] > h) {
        s = R + 12 + 64 * a;
        if (H32(s, 0) && !(h > HF(s, 16))) return 0;
    } else {
        s = R + 44 + 64 * a;
        if (H32(s, 0) && !(HF(s, 16) > h)) return 0;
    }
    float nn[3];
    const float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l == 0.0f) { nn[0] = 0.0f; nn[1] = 1.0f; nn[2] = 0.0f; }
    else { const float r = 1.0f / l; nn[0] = n[0] * r; nn[1] = n[1] * r; nn[2] = n[2] * r; }
    HW32(s, 0, 1);
    HW32(s, 4, owner);
    HWF(s, 16, h);
    HWF(s, 20, nn[0]); HWF(s, 24, nn[1]); HWF(s, 28, nn[2]);
    HW32(s, 12, 0u - FTRC(pr_atan2(c, nn[0], nn[1]) * K));
    HW32(s, 8, FTRC(pr_asin(c, nn[2]) * K));
    return 1;
}

/* R: the record's bytes (the guest record itself, or a copy under check).
 * st (check mode): polygons walked, near ones, fills. */
static uint32_t pr_native(sh4_ctx *c, uint8_t *R, uint32_t *st) {
    const float c1 = RDF(0x0c0294c4u, 0), c2 = RDF(0x0c0294c8u, 0);
    const float lim = RDF(0x0c0294d0u, 0), K = RDF(0x0c0294d4u, 0);
    const float far2 = RDF(0x0c029644u, 0);
    for (int k = 0; k < 6; k++) HW32(R, 12 + 32 * k, 0);
    for (int k = 0; k < 3; k++) {
        HWF(R, 28 + 64 * k, HF(R, 4 * k) + c1);
        HWF(R, 60 + 64 * k, HF(R, 4 * k) + c2);
    }
    uint32_t node = H32(HP(COLL_LIST), 0);
    while (node && H32(HP(node), 8) != c->r[5]) node = H32(HP(node), 0);
    if (!node) return 0;
    const uint32_t owner = H32(HP(node), 12);
    const float P[3] = {HF(R, 0), HF(R, 4), HF(R, 8)};
    uint32_t r0 = 0;
    for (uint32_t pa = H32(HP(node), 16); pa; ) {
        const uint8_t *Q = HP(pa);
        const float v0[3] = {HF(Q, 24), HF(Q, 28), HF(Q, 32)};
        const float v1[3] = {HF(Q, 36), HF(Q, 40), HF(Q, 44)};
        const float v2[3] = {HF(Q, 48), HF(Q, 52), HF(Q, 56)};
        pa = H32(Q, 0);
        const float dy = v0[1] - P[1], dx = v0[0] - P[0], dz = v0[2] - P[2];
        float d2 = dy * dy;
        d2 = FMAC_F(dx, dx, d2);
        d2 = FMAC_F(dz, dz, d2);
        if (st) st[0]++;
        if (d2 > far2) continue;
        if (st) st[1]++;
        const uint32_t f = pr_axis(c, R, P, v0, v1, v2, 2, 0, 1, lim, K, owner)
                         + pr_axis(c, R, P, v0, v1, v2, 0, 1, 2, lim, K, owner)
                         + pr_axis(c, R, P, v0, v1, v2, 1, 2, 0, lim, K, owner);
        if (f) r0 = 1;
        if (st) st[2] += f;
    }
    return r0;
}

/* check mode: the exact body's record against native's, bit for bit */
static uint32_t ck_calls, ck_diff, ck_r0, ck_logged, ck_ang, ck_last;
static float ck_hmax, ck_nmax;
static uint32_t ck_st[3];
static int pr_mode = -1;
static void pr_summary(int force) {
    const uint32_t f = hle_frame_count();
    if (!force && f / 600 == ck_last / 600) return;
    ck_last = f;
    sa1_log("check: poly-ray summary f%u calls %u differing-words %u r0-diffs %u max angle-step %u max h-diff %.3g max normal-diff %.3g; polygons %u near %u fills %u",
            f, ck_calls, ck_diff, ck_r0, ck_ang, ck_hmax, ck_nmax, ck_st[0], ck_st[1], ck_st[2]);
}
void pr_check_report(void) { if (pr_mode == 2) pr_summary(1); }
static void pr_compare(const uint8_t *nat, const uint8_t *ref, uint32_t r0n, uint32_t r0r) {
    ck_calls++;
    if (r0n != r0r) {
        ck_r0++;
        if (ck_logged < 200) { ck_logged++; sa1_log("check: f%u poly-ray r0 ref %u native %u", hle_frame_count(), r0r, r0n); }
    }
    for (uint32_t o = 12; o < REC_SIZE; o += 4) {
        const uint32_t a = H32(nat, o), b = H32(ref, o);
        if (a == b) continue;
        ck_diff++;
        const uint32_t f = (o - 12) % 32;
        if (f == 8 || f == 12) {
            const int32_t d = (int16_t)(a - b);
            const uint32_t m = (uint32_t)(d < 0 ? -d : d);
            if (m > ck_ang) ck_ang = m;
        } else if (f >= 16) {
            const float d = fabsf(HF(nat, o) - HF(ref, o));
            if (f == 16) { if (!(d <= ck_hmax)) ck_hmax = d; } else if (!(d <= ck_nmax)) ck_nmax = d;
        }
        if (ck_logged < 200) {
            ck_logged++;
            sa1_log("check: f%u poly-ray slot %u +%u ref %08x native %08x", hle_frame_count(), (o - 12) / 32, f, b, a);
        }
    }
    pr_summary(0);
}

void fn_8c029400(sh4_ctx *c) {
    HLE_ENTER(0x8c029400u, "poly ray test fn_8c029400");
    if (pr_mode < 0) {
        pr_mode = hle_math_mode_for(0x8c029400u);
        sa1_log("poly ray test: math %s", pr_mode == 1 ? "native" : pr_mode == 2 ? "check" : "exact");
    }
    if (pr_mode == 0) { fn_8c029400_orig(c); return; }
    const uint32_t ra = c->r[4];
    if (pr_mode == 1) { c->r[0] = pr_native(c, HPW(ra), NULL); return; }
    uint8_t nat[REC_SIZE];
    uint32_t sr[16];
    float sf[16];
    memcpy(nat, HP(ra), REC_SIZE);
    memcpy(sr, c->r, sizeof sr); memcpy(sf, c->fr, sizeof sf);
    const uint32_t fpul = c->fpul;
    const uint32_t r0n = pr_native(c, nat, ck_st);
    memcpy(c->r, sr, sizeof sr); memcpy(c->fr, sf, sizeof sf); c->fpul = fpul;
    fn_8c029400_orig(c);
    pr_compare(nat, HP(ra), r0n, c->r[0]);
}
#else
void pr_check_report(void) {}
#endif
