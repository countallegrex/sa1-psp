/* collision.c - native versions of the collision list builder 0x8C028EC2
 * and its two hot walks, 0x8C028D0A and 0x8C02D2E0.
 *
 * The collision candidates live in a linked list at @0x8C02D548 (nodes
 * linked through +0, the object id at +8, the head and tail of a key-sorted
 * list of collision spheres at +16 and +20 (spheres linked forward through
 * +0, back through +4, the key at +16), the first and count of the object's
 * hits at +28 and +36, a half-height at +40).  The game's id table is at
 * 0x8C754EF4 (12-byte entries: +0 flags, +4 id), its 16-bit count at
 * 0x8C754EF0.
 *
 * 0x8C028EC2 runs once per frame: it collects the table entries that are
 * flagged (0x08000000), the one at @0x8C73EDC4, or not yet in the list
 * (192 at most) into two arrays on its stack, drops the list nodes the
 * table no longer asks for (0x8C02CF48), then makes a node per collected
 * entry (0x8C02CEF4, 0x8C02CFA4, the matrix calls, 0x8C028B00, 0x8C02D00E)
 * and, with the debug byte at 0x8C752BDC non-negative, prints the counts.
 *
 * 0x8C028D0A is the tail of 0x8C028BFE (one object against every node: the
 * sphere list walked from the nearer end, a sphere test per sphere, the
 * hits appended at r14 and counted at @r13, 96 at most), entered by a tail
 * call in the middle of the forward walk with T from the third compare.
 *
 * 0x8C02D2E0 finds node r5 and walks its sphere list around the point at
 * r4, calling 0x8C02D410 for each sphere within reach.
 *
 * Every guest memory side effect is the recompiled body's, in its order,
 * and every register handed to a callee or back to the caller is the one
 * the recompiled body leaves there (the emitted register syncs are kept as
 * they are), float operations included, in their order.  The speed comes
 * from host pointers: one address-area test per node instead of one per
 * access, known RAM (the stack frame, the id table, the fixed variables)
 * without one, and two hoists, both across stores that cannot reach the
 * hoisted words: the object's centre (r12+4..+12) in 0x8C028D0A, read once
 * (the loop stores only into the nodes' +28/+36, the hit list and the hit
 * count), and in 0x8C028EC2's first pass the list's (node, id) pairs, read
 * once into a host array for the per-entry "is it in the list" search (the
 * pass stores only into its stack arrays and @0x8C73EDC4).  Both of its
 * searches by id (entry in the list, node in the table) go through a hash
 * of the first index per id instead of a linear scan; the registers the
 * scan would have left (index, pointer, last id read) are computed from the
 * index found.  The table hash is rebuilt after each 0x8C02CF48 call.
 *
 * SA1_COLL_REC=1 runs the recompiled bodies instead (native vs recompiled
 * comparisons).  Each wrapper is compiled only when the emitted game.h has
 * the renamed body (recomp/sa1_us_1005/hle_wraps.tsv).
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"

#include <stdlib.h>

#if defined(SA1_WRAP_8C028D0A) || defined(SA1_WRAP_8C028EC2) || defined(SA1_WRAP_8C02D2E0)

#define HP(a)         ((const uint8_t *)RAMP(a, 0))
#define HPW(a)        ((uint8_t *)RAMPW(a, 0))
#define H32(p, o)     (*(const sa1_u32a *)(const void *)((p) + (o)))
#define HF(p, o)      (*(const sa1_f32a *)(const void *)((p) + (o)))
#define HW32(p, o, v) (*(sa1_u32a *)(void *)((p) + (o)) = (uint32_t)(v))
#define HS16(p, o)    ((uint32_t)(int32_t)(int16_t)*(const sa1_u16a *)(const void *)((p) + (o)))

#define COLL_LIST  0x8c02d548u      /* the candidate list head */
#define COLL_HITS  0x8c02d54cu      /* 0x8C02D410's hit count */
#define ID_COUNT   0x8c754ef0u      /* the id table's count (16-bit) */
#define ID_TABLE   0x8c754ef4u      /* the id table, 12-byte entries */
#define ID_EXTRA   0x8c73edc4u      /* one more id asked for */
#define DEBUG_BYTE 0x8c752bdcu      /* non-negative: print the counts */

static int coll_rec = -1;
static inline int coll_use_rec(void) {
    if (coll_rec < 0) coll_rec = getenv("SA1_COLL_REC") != NULL;
    return coll_rec;
}

/* ---- 0x8C028D0A ----------------------------------------------------------- */
#ifdef SA1_WRAP_8C028D0A
void fn_8c028d0a_orig(sh4_ctx *c);

static inline __attribute__((always_inline)) void coll_8c028d0a(sh4_ctx *c) {
    uint32_t r0, r2 = c->r[2], r3 = c->r[3], r4 = c->r[4], r5 = c->r[5], r6 = c->r[6], r7 = c->r[7];
    const uint32_t r8 = c->r[8], r9 = c->r[9], r10 = c->r[10], r11 = c->r[11], r12 = c->r[12];
    uint32_t r13 = c->r[13], r14 = c->r[14], r15 = c->r[15], t = c->t;
    float fr0 = c->fr[0], fr2 = c->fr[2], fr3 = c->fr[3], fr4 = c->fr[4], fr5 = c->fr[5];
    float fr7 = c->fr[7], fr8 = c->fr[8];
    const float fr6 = c->fr[6], fr14 = c->fr[14], fr15 = c->fr[15];
    const uint8_t *ctr = HP(r12);
    const float c4 = HF(ctr, 4), c8 = HF(ctr, 8), c12 = HF(ctr, 12);   /* the object's centre */
    uint8_t *cnt = HPW(r13);
    const uint8_t *ncount = HP(ID_COUNT), *tab = HP(ID_TABLE);
    uint8_t *p5 = HPW(r5);
    const uint8_t *n = HP(r4);            /* the sphere r4 */

    if (t) goto next_fwd;
    goto hit_fwd;

fwd:                                       /* 0x8C028CCE: sphere r4, from the head */
    r6 = r12; fr3 = HF(n, 20); r7 = r4; fr2 = fr15; r7 += 8; r6 += 4;
    fr2 = fr2 + fr3;
    fr3 = c4; r6 += 4; fr5 = HF(n, 8); r7 += 4;
    fr5 = fr5 - fr3; fr4 = fr2; fr4 = fr4 * fr6; fr7 = fr5; fr7 = fr7 * fr5;
    if (fr7 > fr4) goto next_fwd;
    fr5 = HF(n, 12); r7 += 4; fr3 = c8; r6 += 4;
    fr5 = fr5 - fr3; fr0 = fr5; fr7 = FMAC_F(fr0, fr5, fr7);
    if (fr7 > fr4) goto next_fwd;
    fr5 = HF(n, 16); fr3 = c12;
    fr5 = fr5 - fr3; fr0 = fr5; fr7 = FMAC_F(fr0, fr5, fr7);
    if (fr7 > fr4) goto next_fwd;
hit_fwd:                                   /* 0x8C028D0C */
    r3 = H32(p5, 28);
    if (r3 == 0) HW32(p5, 28, r14);
    r3 = H32(p5, 36); r3 += 1u; HW32(p5, 36, r3);
    WR32(r14, 0, r4); r14 += 4u;
    r0 = H32(cnt, 0); r0 += 1u; t = r0 == 0x60u; HW32(cnt, 0, r0);
    if (!t) goto next_fwd;
    goto full;
next_fwd:                                  /* 0x8C028D80 */
    r4 = H32(n, 0);
test_fwd:                                  /* 0x8C028D82 */
    t = r4 == 0;
    if (t) goto next_obj;
    n = HP(r4);
    fr3 = HF(n, 16);
    t = fr8 > fr3;
    if (t) goto fwd;
    goto next_obj;

bwd:                                       /* 0x8C028D96: sphere r4, from the tail */
    r6 = r12; fr3 = HF(n, 20); r7 = r4; fr2 = fr15; r7 += 8; r6 += 4;
    fr2 = fr2 + fr3;
    fr3 = c4; r6 += 4; fr8 = HF(n, 8); r7 += 4;
    fr8 = fr8 - fr3; fr5 = fr2; fr5 = fr5 * fr6; fr7 = fr8; fr7 = fr7 * fr8;
    if (fr7 > fr5) goto next_bwd;
    fr8 = HF(n, 12); r7 += 4; fr3 = c8; r6 += 4;
    fr8 = fr8 - fr3; fr0 = fr8; fr7 = FMAC_F(fr0, fr8, fr7);
    if (fr7 > fr5) goto next_bwd;
    fr8 = HF(n, 16); fr3 = c12;
    fr8 = fr8 - fr3; fr0 = fr8; fr7 = FMAC_F(fr0, fr8, fr7);
    if (fr7 > fr5) goto next_bwd;
    r3 = H32(p5, 28);                      /* 0x8C028DD4 */
    if (r3 == 0) HW32(p5, 28, r14);
    r3 = H32(p5, 36); r3 += 1u; HW32(p5, 36, r3);
    WR32(r14, 0, r4); r14 += 4u;
    r0 = H32(cnt, 0); r0 += 1u; t = r0 == 0x60u; HW32(cnt, 0, r0);
    if (!t) goto next_bwd;
    goto full;
next_bwd:                                  /* 0x8C028E00 */
    r4 = H32(n, 4);
test_bwd:                                  /* 0x8C028E02: the key is read before the null test */
    n = HP(r4);
    fr3 = HF(n, 16);
    t = fr3 > fr4;
    if (!t) goto next_obj;
    t = r4 == 0;
    if (!t) goto bwd;

next_obj:                                  /* 0x8C028E0E */
    r5 = H32(p5, 0);
    t = r5 == 0;
    if (t) goto out;
    p5 = HPW(r5);                          /* 0x8C028C4E */
    r2 = ID_COUNT; r6 = ID_TABLE;
    HW32(p5, 28, r11); HW32(p5, 36, r11);
    r4 = HS16(ncount, 0); t = (int32_t)r4 > 0; r7 = r11;
    if (t) {                               /* is the object's id in the table? */
        const uint8_t *e = tab;
        r2 = H32(p5, 8);
        for (;;) {
            r3 = H32(e, 4);
            t = r3 == r2;
            if (t) { r7 = 1u; break; }
            r4 -= 1u; t = (int32_t)r4 > 0; r6 += 12u; e += 12;
            if (!t) break;
        }
    }
    t = r7 == 0;
    if (t) goto next_obj;
    r4 = H32(p5, 16);                      /* 0x8C028C7A: the sphere range */
    fr5 = c12;
    fr7 = HF(p5, 40);
    fr3 = fr5; fr3 = fr3 - fr7;
    n = HP(r4);
    fr2 = HF(n, 16);
    fr7 = fr7 + fr14;
    r6 = H32(p5, 20);
    fr4 = fr3; fr4 = fr4 - fr14;
    fr8 = fr7;
    t = fr4 > fr2;
    fr8 = fr8 + fr5;
    if (t) {
        fr3 = RDF(r6, 16);
        t = fr3 > fr8;
        if (t) {
            fr3 = HF(n, 16);
            t = fr3 > fr4;
            if (!t) {
                for (;;) {                 /* 0x8C028CA8: skip the spheres below the range */
                    r4 = H32(n, 0);
                    t = r4 == 0;
                    if (t) goto from_tail;
                    n = HP(r4);
                    fr3 = HF(n, 16);
                    t = fr3 > fr4;
                    if (t) break;
                }
            }
        }
    }
    t = r4 == 0;                           /* 0x8C028CB8 */
    if (t) goto from_tail;
    fr3 = HF(n, 16);
    t = fr3 > fr4;
    if (t) goto test_fwd;
from_tail:                                 /* 0x8C028D92 */
    r4 = r6;
    goto test_bwd;

full:                                      /* 0x8C028D28: 96 hits */
    r2 = RD8S(r9, 0);
    t = (int32_t)r2 >= 0;
    if (t) {
        r5 = 0x8c11248cu;
        r4 = r10;
        c->r[0] = r0; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7;
        c->r[14] = r14;
        c->fr[0] = fr0; c->fr[2] = fr2; c->fr[3] = fr3; c->fr[4] = fr4; c->fr[5] = fr5; c->fr[7] = fr7;
        c->fr[8] = fr8; c->t = t;
        sa1_call(c, r8);
        r2 = c->r[2]; r3 = c->r[3]; r4 = c->r[4]; r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
        r13 = c->r[13]; r15 = c->r[15];
        fr0 = c->fr[0]; fr2 = c->fr[2]; fr3 = c->fr[3]; fr4 = c->fr[4]; fr5 = c->fr[5]; fr7 = c->fr[7];
        fr8 = c->fr[8]; t = c->t;
    }
out:                                       /* 0x8C028E18: 0x8C028BFE's frame */
    r15 += 4u;                             /* lds.l @r15+,pr */
    r0 = RD32(r13, 0);
    c->fr[14] = RKF(r15, 0); c->fr[15] = RKF(r15, 4);
    c->r[8] = RK32(r15, 8); c->r[9] = RK32(r15, 12); c->r[10] = RK32(r15, 16); c->r[11] = RK32(r15, 20);
    c->r[12] = RK32(r15, 24); c->r[13] = RK32(r15, 28); c->r[14] = RK32(r15, 32);
    r15 += 36u;
    c->r[0] = r0; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7;
    c->r[15] = r15;
    c->fr[0] = fr0; c->fr[2] = fr2; c->fr[3] = fr3; c->fr[4] = fr4; c->fr[5] = fr5; c->fr[7] = fr7;
    c->fr[8] = fr8; c->t = t;
}

void fn_8c028d0a(sh4_ctx *c) {
    HLE_ENTER(0x8c028d0au, "collision sphere walk fn_8c028d0a");
    if (coll_use_rec()) fn_8c028d0a_orig(c);
    else coll_8c028d0a(c);
}
#endif

/* ---- 0x8C02D2E0 ----------------------------------------------------------- */
#ifdef SA1_WRAP_8C02D2E0
void fn_8c02d2e0_orig(sh4_ctx *c);
void fn_8c02d410(sh4_ctx *c);

static inline __attribute__((always_inline)) void coll_8c02d2e0(sh4_ctx *c) {
    const uint32_t r4 = c->r[4], r5 = c->r[5];
    uint32_t r0, r1, r2, r3, r11, r12, r13, r14, r15, t;
    float fr0, fr1, fr2, fr6, fr7, fr8, fr9, fr10, fr11, fr12, fr13, fr14, fr15;
    uint8_t *p4 = HPW(r4);
    const uint8_t *p1;

    HW32(p4, 12, 0u);
    HW32(p4, 44, 0u);
    r1 = H32(HP(COLL_LIST), 0);
    for (;;) {                             /* node r5 */
        if (r1 == 0) {
            c->r[0] = 0u; c->r[1] = r1; c->t = 1u;
            return;
        }
        p1 = HP(r1);
        if (H32(p1, 8) == r5) break;
        r1 = H32(p1, 0);
    }
    r11 = c->r[11]; r12 = c->r[12]; r13 = c->r[13]; r14 = c->r[14]; r15 = c->r[15];
    fr12 = c->fr[12]; fr13 = c->fr[13]; fr14 = c->fr[14]; fr15 = c->fr[15];
    r15 -= 36u;
    WK32(r15, 32, r14); WK32(r15, 28, r13); WK32(r15, 24, r12); WK32(r15, 20, r11);
    WK32(r15, 16, 0u);                     /* sts.l pr,@-r15: the emitted pr is 0 */
    WKF(r15, 12, fr15); WKF(r15, 8, fr14); WKF(r15, 4, fr13); WKF(r15, 0, fr12);
    r14 = r4;
    r2 = COLL_HITS;
    WR32(COLL_HITS, 0, 0u);
    r12 = H32(p1, 16);
    r3 = H32(p1, 20);
    r13 = H32(p1, 12);
    fr6 = HF(p4, 8);
    fr14 = HF(p1, 40);
    fr7 = fr6;
    fr6 = fr6 - fr14;
    fr7 = fr7 + fr14;
    fr8 = HF(p4, 0); fr9 = HF(p4, 4); fr10 = HF(p4, 8);
    r0 = 0x8c02d96cu;
    fr1 = RDF(0x8c02d96cu, 0);
    fr2 = fr1;
    fr1 = fr1 + fr9;
    fr2 = FNEG_F(fr2);
    fr2 = fr2 + fr9;
    WRF(r14, 28, fr2);
    WRF(r14, 60, fr1);
    fr1 = RDF(r12, 16);
    t = fr6 > fr1;
    if (t) {
        fr2 = RDF(r3, 16);
        t = fr2 > fr7;
        if (!t) goto from_tail;
        do {                               /* skip the spheres below the point */
            r12 = RD32(r12, 0);
            fr1 = RDF(r12, 16);
            t = fr6 > fr1;
        } while (t);
    }
    r0 = 0x8c02d960u;                      /* 0x8C02D364: forward */
    fr11 = RDF(0x8c02d960u, 0);
    for (;;) {
        const uint8_t *n = HP(r12);
        r12 += 20u;
        fr0 = HF(n, 8); fr1 = HF(n, 12); fr2 = HF(n, 16);
        t = fr7 > fr2;
        if (!t) goto out;
        fr15 = HF(n, 20); r12 += 4u;
        fr15 = fr15 + fr11;
        fr0 = fr0 - fr8; fr0 = fr0 * fr0;
        t = fr0 > fr15;
        if (!t) {
            fr1 = fr1 - fr9; fr1 = fr1 * fr1; fr1 = fr1 + fr0;
            t = fr1 > fr15;
            if (!t) {
                fr2 = fr2 - fr10; fr2 = fr2 * fr2; fr2 = fr2 + fr1;
                t = fr2 > fr15;
                if (!t) {
                    c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[12] = r12; c->r[13] = r13;
                    c->r[14] = r14; c->r[15] = r15;
                    c->fr[0] = fr0; c->fr[1] = fr1; c->fr[2] = fr2; c->fr[6] = fr6; c->fr[7] = fr7; c->fr[8] = fr8;
                    c->fr[9] = fr9; c->fr[10] = fr10; c->fr[11] = fr11; c->fr[14] = fr14;
                    fn_8c02d410(c);
                    r0 = c->r[0]; r1 = c->r[1]; r2 = c->r[2]; r3 = c->r[3]; r11 = c->r[11]; r13 = c->r[13];
                    r14 = c->r[14]; r15 = c->r[15];
                    fr0 = c->fr[0]; fr1 = c->fr[1]; fr2 = c->fr[2]; fr6 = c->fr[6]; fr7 = c->fr[7]; fr8 = c->fr[8];
                    fr9 = c->fr[9]; fr10 = c->fr[10]; fr11 = c->fr[11]; fr13 = c->fr[13]; fr14 = c->fr[14];
                }
            }
        }
        r12 -= 24u;                        /* 0x8C02D398 */
        r12 = RD32(r12, 0);
        t = r12 == 0;
        if (t) goto out;
    }
from_tail:                                 /* 0x8C02D3B0: backward */
    r12 = r3;
    r0 = 0x8c02d960u;
    fr11 = RDF(0x8c02d960u, 0);
    for (;;) {
        const uint8_t *n = HP(r12);
        r12 += 20u;
        fr0 = HF(n, 8); fr1 = HF(n, 12); fr2 = HF(n, 16);
        t = fr6 > fr2;
        if (t) goto out;
        fr15 = HF(n, 20); r12 += 4u;
        fr15 = fr15 + fr11;
        fr0 = fr0 - fr8; fr0 = fr0 * fr0;
        t = fr0 > fr15;
        if (!t) {
            fr1 = fr1 - fr9; fr1 = fr1 * fr1; fr1 = fr1 + fr0;
            t = fr1 > fr15;
            if (!t) {
                fr2 = fr2 - fr10; fr2 = fr2 * fr2; fr2 = fr2 + fr1;
                t = fr2 > fr15;
                if (!t) {
                    c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[12] = r12; c->r[13] = r13;
                    c->r[14] = r14; c->r[15] = r15;
                    c->fr[0] = fr0; c->fr[1] = fr1; c->fr[2] = fr2; c->fr[6] = fr6; c->fr[7] = fr7; c->fr[8] = fr8;
                    c->fr[9] = fr9; c->fr[10] = fr10; c->fr[11] = fr11; c->fr[14] = fr14;
                    fn_8c02d410(c);
                    r0 = c->r[0]; r1 = c->r[1]; r2 = c->r[2]; r3 = c->r[3]; r11 = c->r[11]; r13 = c->r[13];
                    r14 = c->r[14]; r15 = c->r[15];
                    fr0 = c->fr[0]; fr1 = c->fr[1]; fr2 = c->fr[2]; fr6 = c->fr[6]; fr7 = c->fr[7]; fr8 = c->fr[8];
                    fr9 = c->fr[9]; fr10 = c->fr[10]; fr11 = c->fr[11]; fr13 = c->fr[13]; fr14 = c->fr[14];
                }
            }
        }
        r12 -= 24u;                        /* 0x8C02D3E6 */
        r12 = RD32(r12, 4);
        t = r12 == 0;
        if (t) goto out;
    }
out:                                       /* 0x8C02D3EE */
    fr12 = RKF(r15, 0); fr13 = RKF(r15, 4); fr14 = RKF(r15, 8); fr15 = RKF(r15, 12);
    r11 = RK32(r15, 20); r12 = RK32(r15, 24); r13 = RK32(r15, 28); r14 = RK32(r15, 32);
    r15 += 36u;
    r0 = RD32(COLL_HITS, 0);
    c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[11] = r11; c->r[12] = r12;
    c->r[13] = r13; c->r[14] = r14; c->r[15] = r15;
    c->fr[0] = fr0; c->fr[1] = fr1; c->fr[2] = fr2; c->fr[6] = fr6; c->fr[7] = fr7; c->fr[8] = fr8;
    c->fr[9] = fr9; c->fr[10] = fr10; c->fr[11] = fr11; c->fr[12] = fr12; c->fr[13] = fr13;
    c->fr[14] = fr14; c->fr[15] = fr15; c->t = t;
}

void fn_8c02d2e0(sh4_ctx *c) {
    HLE_ENTER(0x8c02d2e0u, "collision candidate walk fn_8c02d2e0");
    if (coll_use_rec()) fn_8c02d2e0_orig(c);
    else coll_8c02d2e0(c);
}
#endif

/* ---- 0x8C028EC2 ----------------------------------------------------------- */
#ifdef SA1_WRAP_8C028EC2
void fn_8c028ec2_orig(sh4_ctx *c);
void fn_8c052518(sh4_ctx *c);
void fn_8c052e30(sh4_ctx *c);
void fn_8c02cf48(sh4_ctx *c);
void fn_8c02cef4(sh4_ctx *c);
void fn_8c02cfa4(sh4_ctx *c);
void fn_8c028b00(sh4_ctx *c);
void fn_8c02d00e(sh4_ctx *c);
void fn_8c10ca5c(sh4_ctx *c);
void sdk_8c638ecc(sh4_ctx *c);
void sdk_8c639b98(sh4_ctx *c);
void sdk_8c639c70(sh4_ctx *c);
void sdk_8c639ec8(sh4_ctx *c);
void sdk_8c639f5c(sh4_ctx *c);
void sdk_8c63a1cc(sh4_ctx *c);
void sdk_8c63a5ec(sh4_ctx *c);
void sdk_8c63a804(sh4_ctx *c);
void sdk_8c640766(sh4_ctx *c);
void sdk_8c640922(sh4_ctx *c);
void sdk_8c640a80(sh4_ctx *c);

#define LIST_CAP 256                       /* list nodes read into the host array */
#define HASH_N   512                       /* id -> first index, open addressing */
#define TAB_CAP  400                       /* table entries hashed; longer tables are scanned */

/* The first index holding each id: idx[] is -1 for an empty slot. */
static inline uint32_t id_slot(uint32_t id) { return (id * 0x9e3779b1u) >> 23; }

static void id_hash(uint32_t *key, int16_t *idx, const uint32_t *ids, int n, int stride) {
    int i;
    for (i = 0; i < HASH_N; i++) idx[i] = -1;
    for (i = 0; i < n; i++) {
        uint32_t id = ids[i * stride], h = id_slot(id);
        while (idx[h] >= 0 && key[h] != id) h = (h + 1) & (HASH_N - 1);
        if (idx[h] < 0) { key[h] = id; idx[h] = (int16_t)i; }
    }
}

static inline int id_find(const uint32_t *key, const int16_t *idx, uint32_t id) {
    uint32_t h = id_slot(id);
    while (idx[h] >= 0) {
        if (key[h] == id) return idx[h];
        h = (h + 1) & (HASH_N - 1);
    }
    return -1;
}

/* The debug lines: a text call, with the registers the emitted one syncs. */
#define TXT(fn) do { c->r[4] = r4; c->r[5] = r5; fn(c); } while (0)
#define NUM(fn) do { c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; fn(c); } while (0)

static inline __attribute__((always_inline)) void coll_8c028ec2(sh4_ctx *c) {
    uint32_t r0, r1 = c->r[1], r2, r3, r4, r5 = 0, r6 = c->r[6], r7 = c->r[7], r8 = c->r[8], r9 = c->r[9];
    uint32_t r10 = c->r[10], r11 = c->r[11], r12 = c->r[12], r13 = c->r[13], r14 = c->r[14], r15 = c->r[15];
    uint32_t t, macl;
    float fr3, fr15 = c->fr[15];

    r15 -= 36u;
    WK32(r15, 32, r14); WK32(r15, 28, r13); WK32(r15, 24, r12); WK32(r15, 20, r11);
    WK32(r15, 16, r10); WK32(r15, 12, r9); WK32(r15, 8, r8); WKF(r15, 4, fr15);
    WK32(r15, 0, 0u);                      /* sts.l pr,@-r15: the emitted pr is 0 */
    r0 = 0xfffff9e8u;
    r15 += r0;
    r2 = 0x8c196044u;
    r3 = RD32(0x8c196044u, 0);
    r14 = 0;
    c->r[0] = r0; c->r[14] = r14; c->r[15] = r15;
    if (r3 == 0) {
        fn_8c052518(c);
        r0 = c->r[0]; r5 = c->r[5]; r14 = c->r[14]; r15 = c->r[15];
    } else {
        fn_8c052e30(c);
        r0 = c->r[0]; r5 = c->r[5]; r14 = c->r[14]; r15 = c->r[15];
        WR32(0x8c196044u, 0, r14);
    }

    /* 0x8C028F4A: the entries to make nodes for, ids at r15+0x318 and flags
     * at r15+24 */
    {
        uint8_t *S = (uint8_t *)RAMKW(r15, 0);
        const uint8_t *tab = HP(ID_TABLE), *head = HP(COLL_LIST);
        uint8_t *extra = HPW(ID_EXTRA);
        uint32_t lp[LIST_CAP], lid[LIST_CAP];
        uint32_t hkey[HASH_N];
        int16_t hidx[HASH_N];
        int ln = -1;                       /* -1: not read yet, LIST_CAP + 1: too long, walk it */
        r3 = ID_COUNT;
        r11 = 1;
        r13 = r14;
        r9 = HS16(HP(ID_COUNT), 0);
        r6 = r15 + 24u;
        r7 = r15 + 0x318u;
        r12 = 0xc0;
        r10 = 0x08000000u;
        if ((int32_t)r9 > (int32_t)r12) r9 = r12;
        r4 = ID_TABLE;
        for (; (int32_t)r9 > 0; r4 += 12u, r9 -= 1u) {
            const uint8_t *e = tab + (r4 - ID_TABLE);
            r2 = H32(e, 0);
            if (r2 & r10) {                /* flagged */
                r1 = H32(e, 4); r13 += 1u;
                HW32(S, r7 - r15, r1); r7 += 4u;
                r3 = H32(e, 0);
                HW32(S, r6 - r15, r3); r6 += 4u;
                continue;
            }
            r1 = ID_EXTRA;
            r3 = H32(e, 4);
            r2 = H32(extra, 0);
            if (r2 == r3) {                /* the extra id */
                r3 = ID_EXTRA; r13 += 1u;
                HW32(extra, 0, r14);
                r2 = H32(e, 4);
                HW32(S, r7 - r15, r2); r7 += 4u;
                r0 = H32(e, 0);
                HW32(S, r6 - r15, r0); r6 += 4u;
                continue;
            }
            r3 = COLL_LIST;
            r5 = H32(head, 0);
            r8 = r14;
            if (r5 != 0) {                 /* already in the list? */
                if (ln < 0 || (ln <= LIST_CAP && lp[0] != r5)) {
                    uint32_t p = r5;
                    ln = 0;
                    while (p != 0 && ln < LIST_CAP) {
                        const uint8_t *q = HP(p);
                        lp[ln] = p; lid[ln] = H32(q, 8); ln++;
                        p = H32(q, 0);
                    }
                    if (p != 0) ln = LIST_CAP + 1;
                    else id_hash(hkey, hidx, lid, ln, 1);
                }
                r1 = H32(e, 4);
                if (ln <= LIST_CAP) {
                    int k = id_find(hkey, hidx, r1);
                    if (k >= 0) { r5 = lp[k]; r2 = lid[k]; r8 = r11; }
                    else { r5 = 0; r2 = lid[ln - 1]; }
                } else {
                    for (;;) {
                        const uint8_t *q = HP(r5);
                        r2 = H32(q, 8);
                        if (r2 == r1) { r8 = r11; break; }
                        r5 = H32(q, 0);
                        if (r5 == 0) break;
                    }
                }
            }
            if (r8 != 0) continue;
            r3 = H32(e, 4); r13 += 1u;     /* 0x8C028FEA: a new one */
            t = r13 == r12;
            HW32(S, r7 - r15, r3); r7 += 4u;
            r2 = H32(e, 0);
            HW32(S, r6 - r15, r2); r6 += 4u;
            if (t) goto made;
        }
        if (r13 == r12) goto made;

        /* 0x8C029008: drop the nodes the table no longer asks for */
        r2 = COLL_LIST;
        r4 = H32(head, 0);
        if (r4 == 0) goto made;
        int tn = -1;                       /* entries in the table hash; -1: rebuild */
        do {
            const uint8_t *p4 = HP(r4);
            r2 = ID_COUNT; r5 = ID_TABLE;
            r7 = HS16(HP(ID_COUNT), 0);
            r6 = r14;
            if ((int32_t)r7 > 0 && (int32_t)r7 <= TAB_CAP) {
                int j;
                r3 = H32(p4, 8);
                if (tn != (int)r7) {
                    tn = (int)r7;
                    id_hash(hkey, hidx, (const uint32_t *)(const void *)(tab + 4), tn, 3);
                }
                j = id_find(hkey, hidx, r3);
                if (j >= 0) {              /* the scan stops at entry j */
                    const uint8_t *e = tab + 12 * j;
                    r7 -= (uint32_t)j; r5 += 12u * (uint32_t)j;
                    r2 = H32(e, 4);
                    r0 = H32(e, 0);
                    if (r0 & r10) r6 = r14;
                    else {
                        r1 = ID_EXTRA;
                        r2 = H32(e, 4);
                        r3 = H32(extra, 0);
                        r6 = r3 == r2 ? r14 : r11;
                    }
                } else {                   /* the scan runs off the end */
                    r2 = H32(tab + 12 * (r7 - 1u), 4);
                    r5 += 12u * r7; r7 = 0;
                }
            } else if ((int32_t)r7 > 0) {
                const uint8_t *e = tab;
                r3 = H32(p4, 8);
                for (;;) {
                    r2 = H32(e, 4);
                    if (r3 == r2) {
                        r0 = H32(e, 0);
                        if (r0 & r10) r6 = r14;
                        else {
                            r1 = ID_EXTRA;
                            r2 = H32(e, 4);
                            r3 = H32(extra, 0);
                            r6 = r3 == r2 ? r14 : r11;
                        }
                        break;
                    }
                    r7 -= 1u; r5 += 12u; e += 12;
                    if ((int32_t)r7 <= 0) break;
                }
            }
            r9 = H32(p4, 0);
            if (r6 == 0) {
                c->r[0] = r0; c->r[4] = r4;
                fn_8c02cf48(c);
                tn = -1;
                r0 = c->r[0];
            }
            r4 = r9;
        } while (r4 != 0);
    }

made:                                      /* 0x8C02907A: a node per collected entry */
    r3 = 0x8c639c70u;
    r4 = 0x8c67c640u;
    c->r[0] = r0; c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7;
    sdk_8c639c70(c);
    r0 = c->r[0]; r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
    r3 = 0x8c67c640u;
    r9 = r15 + 24u;
    r10 = r15 + 0x318u;
    WK32(r15, 4, r13);
    WK32(r15, 8, r3);
    r2 = RK32(r15, 4);
    t = (int32_t)r2 > 0;
    if (t) {
        r8 = 0x8c73edf0u;
        fr15 = 0.0f;
        do {
            r2 = 0x8c02cef4u;
            c->r[0] = r0;
            fn_8c02cef4(c);
            r0 = c->r[0];
            r3 = 0x8c02cfa4u;
            r13 = r0;
            fn_8c02cfa4(c);
            r7 = c->r[7];
            r3 = 0x8c639c70u;
            r4 = RK32(r15, 8);
            c->r[4] = r4;
            sdk_8c639c70(c);
            r6 = c->r[6]; r7 = c->r[7];
            r11 = RK32(r10, 0);
            r2 = 0x8c63a804u;
            r5 = r11 + 8u;
            r4 = 0;
            c->r[4] = r4; c->r[5] = r5;
            sdk_8c63a804(c);
            r6 = c->r[6]; r7 = c->r[7];
            {
                const uint8_t *o = HP(r11);
                r3 = H32(o, 28); t = r3 == 0; WK32(r15, 0, r3);
                if (!t) {
                    r3 = 0x8c63a1ccu; r5 = RK32(r15, 0); r4 = 0;
                    c->r[4] = r4; c->r[5] = r5;
                    sdk_8c63a1cc(c);
                    r6 = c->r[6]; r7 = c->r[7];
                }
                r3 = RD32(r11, 24); t = r3 == 0; WK32(r15, 0, r3);
                if (!t) {
                    r3 = 0x8c639f5cu; r5 = RK32(r15, 0); r4 = 0;
                    c->r[4] = r4; c->r[5] = r5;
                    sdk_8c639f5c(c);
                    r6 = c->r[6]; r7 = c->r[7];
                }
                r3 = RD32(r11, 20); t = r3 == 0; WK32(r15, 0, r3);
                if (!t) {
                    r3 = 0x8c639ec8u; r5 = RK32(r15, 0); r4 = 0;
                    c->r[4] = r4; c->r[5] = r5;
                    sdk_8c639ec8(c);
                    r6 = c->r[6]; r7 = c->r[7];
                }
            }
            r3 = 0x8c63a5ecu;
            r5 = r11 + 32u;
            r4 = 0;
            c->r[4] = r4; c->r[5] = r5;
            sdk_8c63a5ec(c);
            r7 = c->r[7];
            r2 = RD32(r11, 4);             /* 0x8C0290F2: the position, transformed */
            r6 = r15; r5 = r15;
            fr3 = RDF(r2, 24);
            r5 += 12u;
            WKF(r15, 12, fr3);
            r3 = RD32(r11, 4);
            r6 += 12u;
            fr3 = RDF(r3, 28);
            WKF(r15, 16, fr3);
            r3 = RD32(r11, 4);
            fr3 = RDF(r3, 32);
            r3 = 0x8c638eccu;
            WKF(r15, 20, fr3);
            r4 = 0;
            c->r[4] = r4; c->r[5] = r5; c->r[6] = r6;
            sdk_8c638ecc(c);
            r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
            r3 = 0x8c73edecu;
            fr3 = RKF(r15, 20);
            r2 = 0x8c639b98u;
            WRF(0x8c73edecu, 0, fr3);
            {
                uint8_t *nd = HPW(r13);
                fr3 = RKF(r15, 12); HW32(nd, 48, sh4_f2u(fr3));
                fr3 = RKF(r15, 16); HW32(nd, 52, sh4_f2u(fr3));
                fr3 = RKF(r15, 20); HW32(nd, 56, sh4_f2u(fr3));
            }
            r4 = 1;
            c->r[4] = r4;
            sdk_8c639b98(c);
            r7 = c->r[7];
            r3 = 0x8c73edf4u;              /* 0x8C029144: the node's fields */
            r6 = r13;
            WRF(0x8c73edf0u, 0, fr15);
            WRF(0x8c73edf4u, 0, fr15);
            {
                uint8_t *nd = HPW(r13);
                r2 = RK32(r10, 0); HW32(nd, 8, r2);
                r2 = 0;
                r1 = RK32(r9, 0); HW32(nd, 12, r1);
                r1 = r2;
                HW32(nd, 16, r2);
                HW32(nd, 24, r14);
                HW32(nd, 28, r2);
                r2 = 0x8c028b00u;
                HW32(nd, 36, r14);
            }
            r5 = RK32(r9, 0); r9 += 4u;
            r4 = RK32(r10, 0); r10 += 4u;
            c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[8] = r8; c->r[9] = r9; c->r[10] = r10;
            c->r[11] = r11; c->r[12] = r12; c->r[13] = r13; c->fr[15] = fr15;
            fn_8c028b00(c);
            r6 = c->r[6]; r8 = c->r[8]; r9 = c->r[9]; r10 = c->r[10]; r12 = c->r[12]; r13 = c->r[13];
            r14 = c->r[14]; r15 = c->r[15]; fr15 = c->fr[15];
            r3 = 0x8c02d00eu;
            r4 = r13;
            c->r[4] = r4;
            fn_8c02d00e(c);
            r5 = c->r[5]; r7 = c->r[7];
            fr3 = RDF(0x8c73edf0u, 0);
            r3 = 0x8c73edf4u;
            WRF(r13, 40, fr3);
            fr3 = RDF(0x8c73edf4u, 0);
            WRF(r13, 44, fr3);
            r0 = 0x2cu;                    /* the last mov #44,r0, handed to 0x8C02CEF4 */
            r2 = RK32(r15, 4);
            r2 -= 1u;
            t = (int32_t)r2 > 0;
            WK32(r15, 4, r2);
        } while (t);
    }
    r3 = 0x8c639b98u;                      /* 0x8C029186 */
    r4 = 1;
    c->r[4] = r4;
    sdk_8c639b98(c);
    r6 = c->r[6]; r7 = c->r[7];
    r3 = DEBUG_BYTE;
    r2 = RD8S(DEBUG_BYTE, 0);
    t = (int32_t)r2 >= 0;
    if (!t) goto out;

    /* 0x8C029206: the debug counts */
    r4 = 0x0002001bu; r5 = 0x8c1124a8u; r13 = 0x8c640922u;
    c->r[2] = r2;
    TXT(sdk_8c640922);
    r2 = c->r[2];
    r6 = 0x8c6bb27cu;
    r11 = r14;
    r7 = r12;
    WK32(r15, 4, r14);
    WK32(r15, 0, r14);
    do {                                   /* 0x8C02921A: the node pool */
        r3 = RD32(r6, 8);
        if (r3 == 0) r11 += 1u;
        else {
            r4 = RD32(r6, 16);
            r5 = r14;
            while (r4 != 0) { r4 = RD32(r4, 0); r5 += 1u; }
            r2 = RK32(r15, 4);
            if (!((int32_t)r2 >= (int32_t)r5)) WK32(r15, 4, r5);
            r3 = RK32(r15, 0); r3 += r5; WK32(r15, 0, r3);
        }
        r7 -= 1u;
        t = (int32_t)r7 > 0;
        r6 += 64u;
    } while (t);
    r1 = 0x64u;
    r3 = 0x8c10ca5cu;
    macl = r11 * r1;
    r1 = macl;
    r0 = r12;
    c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3;
    fn_8c10ca5c(c);
    r0 = c->r[0]; r15 = c->r[15];
    r4 = 0x000a001au; r9 = 0x64u; r5 = 0x8c1124b0u;
    r9 -= r0;
    c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7;
    sdk_8c640922(c);
    r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
    r14 = 0x8c640a80u; r2 = 0x5fu; r8 = 0x8c1124b8u;
    t = (int32_t)r9 >= (int32_t)r2;
    r10 = 0x8c640766u;
    if (t) {
        r4 = 0xffff8080u; c->r[4] = r4; sdk_8c640766(c); r7 = c->r[7];
        r4 = 0x000f001au; r6 = 3; r5 = r9; NUM(sdk_8c640a80); r6 = c->r[6]; r7 = c->r[7];
        r4 = 0x0012001au; r5 = r8; TXT(sdk_8c640922); r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
        r4 = 0xffffffffu; c->r[4] = r4; sdk_8c640766(c); r7 = c->r[7];
    } else {
        r4 = 0x000f001au; r6 = 3; r5 = r9; NUM(sdk_8c640a80); r6 = c->r[6]; r7 = c->r[7];
        r4 = 0x0012001au; r5 = r8; TXT(sdk_8c640922); r7 = c->r[7];
    }
    r4 = 0x0014001au; r5 = r12; r6 = 5; r5 -= r11;
    NUM(sdk_8c640a80); r6 = c->r[6]; r7 = c->r[7];
    if (r11 == 0) {
        r3 = DEBUG_BYTE;
        r2 = RD8S(DEBUG_BYTE, 0);
        if ((int32_t)r2 >= 0) {
            r4 = 0x00020013u; r5 = 0x8c1124bcu;
            TXT(sdk_8c640922); r6 = c->r[6]; r7 = c->r[7];
        }
    }
    r4 = 0x001b001au; r5 = 0x8c1124d8u;
    TXT(sdk_8c640922); r7 = c->r[7];
    r4 = 0x0023001au; r6 = 4; r5 = RK32(r15, 4);
    NUM(sdk_8c640a80); r6 = c->r[6]; r7 = c->r[7];
    r2 = RK32(r15, 0);
    r3 = 0x64u;
    macl = r2 * r3;
    r12 = macl;
    if (!((int32_t)r12 >= 0)) { r1 = 0x1fffu; r12 += r1; }
    r4 = 0x000a001bu; r1 = 0xfffffff3u; r5 = 0x8c1124e0u;
    SHAD(r12, r1);
    TXT(sdk_8c640922); r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
    r3 = 0x5fu;
    if ((int32_t)r12 >= (int32_t)r3) {
        r4 = 0xffff8080u; c->r[4] = r4; sdk_8c640766(c); r7 = c->r[7];
        r4 = 0x000f001bu; r6 = 3; r5 = r12; NUM(sdk_8c640a80); r6 = c->r[6]; r7 = c->r[7];
        r4 = 0x0012001bu; r5 = r8; TXT(sdk_8c640922); r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
        r4 = 0xffffffffu; c->r[4] = r4; sdk_8c640766(c); r7 = c->r[7];
    } else {
        r4 = 0x000f001bu; r6 = 3; r5 = r12; NUM(sdk_8c640a80); r6 = c->r[6]; r7 = c->r[7];
        r4 = 0x0012001bu; r5 = r8; TXT(sdk_8c640922); r7 = c->r[7];
    }
    r4 = 0x0014001bu; r6 = 5; r5 = RK32(r15, 0);
    NUM(sdk_8c640a80);
    r2 = c->r[2]; r3 = c->r[3]; r6 = c->r[6]; r7 = c->r[7];
    t = r11 == 0;
    if (!t) goto out;
    r3 = DEBUG_BYTE;
    r2 = RD8S(DEBUG_BYTE, 0);
    t = (int32_t)r2 >= 0;
    if (!t) goto out;
    r4 = 0x00020014u; r5 = 0x8c1124e8u;
    c->r[2] = r2; c->r[3] = r3; c->t = t;
    TXT(sdk_8c640922);
    r2 = c->r[2]; r3 = c->r[3]; t = c->t;

out:                                       /* 0x8C029382 */
    r1 = 0x618u;
    r0 = 1;
    r15 += r1;
    fr15 = RKF(r15, 4);
    r8 = RK32(r15, 8); r9 = RK32(r15, 12); r10 = RK32(r15, 16); r11 = RK32(r15, 20);
    r12 = RK32(r15, 24); r13 = RK32(r15, 28); r14 = RK32(r15, 32);
    r15 += 36u;
    c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[8] = r8; c->r[9] = r9;
    c->r[10] = r10; c->r[11] = r11; c->r[12] = r12; c->r[13] = r13; c->r[14] = r14; c->r[15] = r15;
    c->fr[15] = fr15; c->t = t;
}

void fn_8c028ec2(sh4_ctx *c) {
    HLE_ENTER(0x8c028ec2u, "collision list builder fn_8c028ec2");
    if (coll_use_rec()) fn_8c028ec2_orig(c);
    else coll_8c028ec2(c);
}
#endif

#endif
