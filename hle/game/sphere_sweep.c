/* sphere_sweep.c - native version of 0x8C029B00, a sphere swept against the
 * collision hits of one object.
 *
 * r4 is a sphere record: +0 the radius, +4..+12 the centre, +16..+24 the
 * motion this step; the function fills +40 (16-bit contact count), +42
 * (16-bit index of the chosen contact), +52..+80 and +108..+128 (the chosen
 * contact's words) and +84..+104 (two vectors: the chosen contact's, or the
 * average over all contacts).  r5 is an object id: its node in the collision
 * list @0x8C02D548 (collision.c: id at +8, the hits found for it at +28, a
 * list of polygon pointers, and their count at +36) gives the polygons.  A
 * polygon is 64 bytes: +24..+56 its three vertices, +60 flags (0x00800000:
 * two-sided).
 *
 * For each polygon (0x8C029BF6): the vertices relative to the first go to
 * the frame at +0xD0 (zeroed), +0xDC and +0xE8, the centre relative to the
 * first vertex to +0xC4, the motion to +0xB8; 0x8C027360 gives the normal at
 * +0xAC, njScalor / njUnitVector normalise it; the two angles that turn the
 * normal onto +Y (the libm calls 0x8C10D008 and 0x8C10D0F8, njRotateX/Y on a
 * matrix pushed from 0x8C67C640) take the polygon, the centre and the
 * motion into the polygon's plane.  Then the sweep (0x8C029E7C): the point
 * at +0x94 walks from the centre along the motion in steps of radius * k
 * (the step vector at +0xA0) until the motion's length (+0x1C) is covered,
 * the last test at the end point; the first point within the radius of the
 * plane and inside the triangle (three 2D edge signs in x/z) is a contact.
 * A contact is filled in at r15+60 (88 bytes: the angles, the normal, the
 * contact point back in world space, the sphere's displacement) and copied
 * with the polygon into the 16-entry contact array at 0x8C73E404 (stride
 * 0x9C, count at 0x8C73E400); with the debug byte at 0x8C752BDC
 * non-negative the polygon is drawn and a full array is reported.  At the
 * end (0x8C02A416) the contact with the lowest +0x98 is chosen, every
 * contact's 88 bytes are copied to 0x8C6BACF0 (0x8C10CDDC, a block copy),
 * and the sphere record is filled; r0 is 1 when there was a contact.
 *
 * Every guest memory side effect is the recompiled body's, in its order,
 * and every register handed to a callee or back to the caller is the one
 * the recompiled body leaves there (the emitted register syncs are kept as
 * they are), float operations included, in their order.  The speed comes
 * from host pointers: the stack frame (known RAM) is one host pointer for
 * the whole call, reset only where a callee hands back r15; the sphere
 * record, the hit list entry, the polygon, r14's contact record and the
 * fixed variables are converted once per use site instead of once per
 * access, and the sweep reads the frame's address-free words directly.
 * The one read hoisted is the sweep's @r15 (the first vertex's address):
 * the sweep stores only into +0x94..+0x9C and +44 of the frame.  The debug
 * drawing path keeps the emitted accesses.
 *
 * That body is math = exact (sa1psp.ini / --math; strict runs).  The default,
 * math = native, is sph_native below: the same sweep with host math, the
 * game the same within a stated bound; math = check runs both and logs
 * the differences.  SA1_29B00_REC=1 runs the recompiled body instead
 * (native vs recompiled comparisons).  The wrapper is compiled only when the emitted game.h has
 * the renamed body (recomp/sa1_us_1005/hle_wraps.tsv).
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"

#include <stdlib.h>

#ifdef SA1_WRAP_8C029B00

#define HP(a)         ((const uint8_t *)RAMP(a, 0))
#define HPW(a)        ((uint8_t *)RAMPW(a, 0))
#define SP(a)         ((uint8_t *)RAMKW(a, 0))      /* r15: known RAM */
#define H32(p, o)     (*(const sa1_u32a *)(const void *)((p) + (o)))
#define HF(p, o)      (*(const sa1_f32a *)(const void *)((p) + (o)))
#define HW32(p, o, v) (*(sa1_u32a *)(void *)((p) + (o)) = (uint32_t)(v))
#define HWF(p, o, v)  (*(sa1_f32a *)(void *)((p) + (o)) = (v))
#define HW16(p, o, v) (*(sa1_u16a *)(void *)((p) + (o)) = (uint16_t)(v))
#define HS16(p, o)    ((uint32_t)(int32_t)(int16_t)*(const sa1_u16a *)(const void *)((p) + (o)))

/* The frame (r15 after the 0xF4-byte allocation). */
#define SW(o)         H32(S, o)
#define SF(o)         HF(S, o)
#define SWW(o, v)     HW32(S, o, v)
#define SWF(o, v)     HWF(S, o, v)

#define HITS_N     0x8c73e400u      /* contacts this call (0..16) */
#define HITS       0x8c73e404u      /* the contact array, 0x9C-byte entries */
#define HIT_SIZE   0x9cu
#define COLL_LIST  0x8c02d548u      /* the collision list head (collision.c) */
#define DEBUG_BYTE 0x8c752bdcu      /* non-negative: draw and report */

void fn_8c029b00_orig(sh4_ctx *c);
void fn_8c027360(sh4_ctx *c);
void fn_8c10d008(sh4_ctx *c);
void fn_8c10d058(sh4_ctx *c);
void fn_8c10d0f8(sh4_ctx *c);
void fn_8c10cddc(sh4_ctx *c);
void sdk_8c638ecc(sh4_ctx *c);
void sdk_8c638f94(sh4_ctx *c);
void sdk_8c6390b0(sh4_ctx *c);
void sdk_8c639b98(sh4_ctx *c);
void sdk_8c639c70(sh4_ctx *c);
void sdk_8c639ec8(sh4_ctx *c);
void sdk_8c639f5c(sh4_ctx *c);
void sdk_8c63a75c(sh4_ctx *c);
void sdk_8c63a94c(sh4_ctx *c);
void sdk_8c63d66c(sh4_ctx *c);
void sdk_8c640922(sh4_ctx *c);

static int sph_rec = -1;
static inline int sph_use_rec(void) {
    if (sph_rec < 0) sph_rec = getenv("SA1_29B00_REC") != NULL;
    return sph_rec;
}

/* The register reloads the emitted body repeats after the matrix calls. */
#define LD_FR4_11() do { fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6]; fr7 = c->fr[7]; \
                         fr8 = c->fr[8]; fr9 = c->fr[9]; fr10 = c->fr[10]; fr11 = c->fr[11]; } while (0)
/* njCalcPoint(0, r5, r6) with the emitted syncs and reloads. */
#define CALC_POINT(a5, a6) do { r4 = 0; r5 = (a5); r6 = (a6); \
        c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; sdk_8c638ecc(c); \
        r7 = c->r[7]; LD_FR4_11(); } while (0)

/* check mode: the exact body's per-polygon angles and normal (taps) */
static int sph_tap;
static uint32_t tap_k, tap_ax[256], tap_ay[256];
static float tap_ny[256], tap_acos[256], tap_nx[256], tap_nz[256];
static inline __attribute__((always_inline)) void sph_8c029b00(sh4_ctx *c) {
    uint32_t r0, r1 = 0, r2 = c->r[2], r3 = c->r[3], r4 = c->r[4], r5 = c->r[5], r6 = c->r[6], r7 = c->r[7];
    uint32_t r8 = c->r[8], r9 = c->r[9], r10 = c->r[10], r11 = c->r[11], r12 = c->r[12];
    uint32_t r13 = c->r[13], r14 = c->r[14], r15 = c->r[15], t, macl = c->macl, fpul = c->fpul;
    float fr0 = 0, fr1 = 0, fr2, fr3, fr4 = c->fr[4], fr5 = c->fr[5], fr6 = c->fr[6], fr7 = c->fr[7];
    float fr8 = c->fr[8], fr9 = c->fr[9], fr10 = c->fr[10], fr11 = c->fr[11];
    float fr12 = c->fr[12], fr13 = c->fr[13], fr14 = c->fr[14], fr15 = c->fr[15];
    uint8_t *S, *O, *Q;
    const uint8_t *HL, *P;
    uint8_t *cnt = HPW(HITS_N);

    /* 0x8C029B00: the frame */
    r15 -= 48u;
    WK32(r15, 44, r14); WK32(r15, 40, r13); WK32(r15, 36, r12); WK32(r15, 32, r11);
    WK32(r15, 28, r10); WK32(r15, 24, r9); WK32(r15, 20, r8);
    WKF(r15, 16, fr15); WKF(r15, 12, fr14); WKF(r15, 8, fr13); WKF(r15, 4, fr12);
    WK32(r15, 0, 0u);                      /* sts.l pr,@-r15: the emitted pr is 0 */
    r15 += 0xffffff0cu;
    S = SP(r15);
    r13 = r4;
    O = HPW(r13);
    fr3 = 0.0f;
    SWW(0, r5);
    fr2 = HF(O, 0);
    t = fr2 > fr3;
    if (!t) goto ret0;

    r3 = HITS_N; r2 = 0; r4 = r13;
    HW32(cnt, 0, r2);
    r2 = 0x8c63a75cu; r4 += 16u;           /* njScalor(motion) */
    c->r[2] = r2; c->r[4] = r4; c->r[15] = r15; c->fr[2] = fr2;
    sdk_8c63a75c(c);
    r2 = c->r[2]; r7 = c->r[7];
    fr0 = c->fr[0]; fr2 = c->fr[2]; fr4 = c->fr[4]; fr10 = c->fr[10]; fr11 = c->fr[11];
    macl = c->macl; fpul = c->fpul;

    /* 0x8C029B3A: the length, the step, the object's node */
    SWF(0x1c, fr0);
    fr3 = RDF(0x0c029b8cu, 0);
    r5 = 0;
    fr13 = HF(O, 0);
    SWW(16, HITS);
    fr13 = fr13 * fr3;
    r6 = SW(0);
    r3 = COLL_LIST;
    r4 = H32(HP(COLL_LIST), 0);
    t = r4 == 0;
    r10 = 1;
    if (!t) {
        for (;;) {
            const uint8_t *n = HP(r4);
            r3 = H32(n, 8);
            t = r3 == r6;
            if (t) { r5 = r10; break; }
            r4 = H32(n, 0);
            t = r4 == 0;
            if (t) break;
        }
    }
    t = r5 == 0;
    if (t) goto ret0;

    {                                       /* 0x8C029BAE */
        const uint8_t *n = HP(r4);
        r2 = H32(n, 36);
        SWW(20, r2);
        r12 = H32(n, 28);
    }
    r4 = r15 + 0xd0u;
    r3 = r4; r8 = r4; r9 = r3;
    SWW(0, r3);
    r8 += 12u;
    SWW(40, 0x8c67c640u);
    r4 = 0x8c73edc8u;
    r2 = r4;
    r1 = r2;
    SWW(12, r2);
    r4 += 24u;
    r3 = 0x8c73edf8u;
    r2 = r3;
    r1 += 12u;
    SWW(4, r1); SWW(8, r4); SWW(36, r3); SWW(32, r2);
    r3 = SW(20);
    t = (int32_t)r3 > 0;
    r9 += 24u;
    if (!t) goto done;

    r14 = r15 + 60u;
    fr14 = RDF(0x0c029cdcu, 0);
    r11 = 0x8c638eccu;
    fr15 = 0.0f;

poly:                                      /* 0x8C029BF6: polygon @r12 */
    {
        const uint32_t s0 = SW(0);
        uint8_t *v0 = HPW(s0);
        uint8_t *e1 = HPW(r8), *e2 = HPW(r9);
        HWF(v0, 8, fr15); HWF(v0, 4, fr15); HWF(v0, 0, fr15);
        r2 = s0;
        O = HPW(r13);
        HL = HP(r12);
        r5 = H32(HL, 0);
        P = HP(r5);
        r5 += 24u;
        fr6 = HF(P, 24); fr9 = HF(P, 36);
        fr4 = HF(P, 28); fr7 = HF(P, 40);
        fr9 = fr9 - fr6;
        fr5 = HF(P, 32); fr8 = HF(P, 44);
        fr7 = fr7 - fr4;
        fr8 = fr8 - fr5;
        HWF(e1, 8, fr8); HWF(e1, 4, fr7); HWF(e1, 0, fr9);
        fr8 = HF(P, 48);
        fr8 = fr8 - fr6;
        fr7 = HF(P, 52);
        fr9 = HF(P, 56);
        fr9 = fr9 - fr5;
        fr7 = fr7 - fr4;
        HWF(e2, 8, fr9); HWF(e2, 4, fr7); HWF(e2, 0, fr8);
        r6 = r5 + 8u;                       /* the emitted post-increments */
        r5 += 32u;
        r4 = r9;
    }
    fr3 = HF(O, 4); fr3 = fr3 - fr6; SWF(0xc4, fr3);
    fr3 = HF(O, 8); fr3 = fr3 - fr4; SWF(0xc8, fr3);
    fr3 = HF(O, 12); fr3 = fr3 - fr5; SWF(0xcc, fr3);
    r2 = H32(O, 16); SWW(0xb8, r2);
    r2 = H32(O, 20); SWW(0xbc, r2);
    r2 = H32(O, 24); SWW(0xc0, r2);
    r4 = SW(40);                            /* njPushMatrix(0x8C67C640) */
    c->r[4] = r4; c->r[5] = r5; c->r[6] = r6;
    c->fr[4] = fr4; c->fr[5] = fr5; c->fr[6] = fr6; c->fr[7] = fr7; c->fr[8] = fr8; c->fr[9] = fr9;
    sdk_8c639c70(c);
    fr10 = c->fr[10]; fr11 = c->fr[11];

    r7 = r15 + 0xacu; r5 = r8; r6 = r9;    /* 0x8C027360: the normal */
    r4 = SW(0);
    c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7;
    fn_8c027360(c);
    r5 = c->r[5]; r6 = c->r[6];
    fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6]; fr7 = c->fr[7]; fr8 = c->fr[8]; fr9 = c->fr[9];

    r2 = 0x8c63a75cu; r4 = r15 + 0xacu;    /* njScalor(normal) */
    c->r[4] = r4;
    sdk_8c63a75c(c);
    r1 = c->r[1]; r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
    fr0 = c->fr[0]; LD_FR4_11(); fpul = c->fpul;
    fr3 = 0.0f;
    t = fr0 == fr3;
    if (t) {                                /* 0x8C029C92: no normal, +Y */
        fr2 = 1.0f;
        r2 = r15 + 0xacu;
        SWF(0xac, fr15); SWF(0xb0, fr2); SWF(0xb4, fr15);
    } else {
        r2 = 0x8c63a94cu; r4 = r15 + 0xacu; /* njUnitVector(normal) */
        c->r[2] = r2; c->r[4] = r4;
        sdk_8c63a94c(c);
        r1 = c->r[1]; r2 = c->r[2];
        fr2 = c->fr[2]; fr6 = c->fr[6]; fr8 = c->fr[8]; fr9 = c->fr[9]; fr10 = c->fr[10]; fr11 = c->fr[11];
        fpul = c->fpul;
    }

    fr4 = SF(0xb0);                         /* 0x8C029D08 */
    c->r[2] = r2; c->r[13] = r13; c->r[14] = r14;
    c->fr[2] = fr2; c->fr[4] = fr4; c->fr[12] = fr12; c->fr[13] = fr13; c->fr[14] = fr14; c->fr[15] = fr15;
    fn_8c10d008(c);
    r1 = c->r[1]; r13 = c->r[13]; r14 = c->r[14]; r15 = c->r[15];
    fr0 = c->fr[0]; fr1 = c->fr[1]; fr2 = c->fr[2]; fr6 = c->fr[6];
    fr12 = c->fr[12]; fr13 = c->fr[13]; fr14 = c->fr[14]; fr15 = c->fr[15];
    S = SP(r15);
    if (sph_tap && tap_k < 256) { tap_acos[tap_k] = fr0; tap_ny[tap_k] = SF(0xb0); tap_nx[tap_k] = SF(0xac); tap_nz[tap_k] = SF(0xb4); }
    fr0 = fr0 * fr14;
    fpul = FTRC(fr0);
    r2 = fpul;
    r2 = 0u - r2;
    if (sph_tap && tap_k < 256) tap_ax[tap_k] = r2;
    t = r2 == 0;
    if (!t) {                               /* 0x8C029D1C: njRotateX */
        fr4 = SF(0xb0);
        c->r[2] = r2; c->fr[0] = fr0; c->fr[4] = fr4; c->fpul = fpul;
        fn_8c10d008(c);
        r6 = c->r[6]; r7 = c->r[7]; r13 = c->r[13]; r14 = c->r[14]; r15 = c->r[15];
        fr0 = c->fr[0]; fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6]; fr7 = c->fr[7];
        fr12 = c->fr[12]; fr13 = c->fr[13]; fr14 = c->fr[14]; fr15 = c->fr[15];
        S = SP(r15);
        fr0 = fr0 * fr14;
        fpul = FTRC(fr0);
        r2 = fpul;
        r5 = 0u - r2;
        r4 = 0;
        c->r[4] = r4; c->r[5] = r5; c->fr[0] = fr0; c->fpul = fpul;
        sdk_8c639ec8(c);
        fr0 = c->fr[0]; fr1 = c->fr[1]; fr2 = c->fr[2]; fr6 = c->fr[6];
        fr8 = c->fr[8]; fr9 = c->fr[9]; fr10 = c->fr[10]; fr11 = c->fr[11]; fpul = c->fpul;
    }

    fr5 = SF(0xb4); fr4 = SF(0xac);         /* 0x8C029D32 */
    c->fr[0] = fr0; c->fr[4] = fr4; c->fr[5] = fr5; c->fpul = fpul;
    fn_8c10d0f8(c);
    r7 = c->r[7]; r13 = c->r[13]; r14 = c->r[14]; r15 = c->r[15];
    fr0 = c->fr[0]; fr1 = c->fr[1]; fr2 = c->fr[2]; fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6];
    fr7 = c->fr[7]; fr12 = c->fr[12]; fr13 = c->fr[13]; fr14 = c->fr[14]; fr15 = c->fr[15];
    S = SP(r15);
    fr0 = fr0 * fr14;
    fpul = FTRC(fr0);
    r2 = fpul;
    r2 = 0u - r2;
    if (sph_tap && tap_k < 256) tap_ay[tap_k++] = r2;
    t = r2 == 0;
    if (!t) {                               /* 0x8C029D4A: njRotateY */
        fr5 = SF(0xb4); fr4 = SF(0xac);
        c->fr[0] = fr0; c->fr[4] = fr4; c->fr[5] = fr5; c->fpul = fpul;
        fn_8c10d0f8(c);
        r6 = c->r[6]; r7 = c->r[7]; r13 = c->r[13]; r14 = c->r[14]; r15 = c->r[15];
        fr0 = c->fr[0]; fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6]; fr7 = c->fr[7];
        fr13 = c->fr[13]; fr14 = c->fr[14]; fr15 = c->fr[15];
        S = SP(r15);
        fr0 = fr0 * fr14;
        fpul = FTRC(fr0);
        r2 = fpul;
        r5 = 0u - r2;
        r4 = 0;
        c->r[4] = r4; c->r[5] = r5;
        sdk_8c639f5c(c);
        r7 = c->r[7]; LD_FR4_11();
    }

    /* 0x8C029D64: the edges, the centre and the motion into the plane */
    CALC_POINT(r8, r8);
    CALC_POINT(r9, r9);
    CALC_POINT(r15 + 0xc4u, r15 + 0xc4u);
    CALC_POINT(r15 + 0xb8u, r15 + 0xb8u);
    r5 = c->r[5]; r6 = c->r[6];

    r3 = H32(HL, 0);                        /* 0x8C029D8C */
    r2 = 0x00800000u;
    r1 = H32(HP(r3), 60);
    O = HPW(r13);
    fr12 = HF(O, 0);
    r1 &= r2;
    t = r1 == 0;
    SWW(24, r1);
    if (!t) {                               /* two-sided, centre below */
        fr3 = 0.0f;
        fr2 = SF(0xc8);
        t = fr3 > fr2;
        if (t) {
            fr2 = RDF(0x0c029e28u, 0);
            fr12 = fr12 * fr2;
        }
    }
    r2 = 0;                                 /* 0x8C029DAC: the step */
    SWW(44, r2);
    r2 = SW(0xb8); SWW(0xa0, r2);
    r2 = SW(0xbc); SWW(0xa4, r2);
    r2 = SW(0xc0);
    r3 = 0x8c63a75cu;
    SWW(0xa8, r2);
    r4 = r15 + 0xa0u;
    c->r[4] = r4;
    sdk_8c63a75c(c);
    r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
    fr0 = c->fr[0]; fr1 = c->fr[1]; LD_FR4_11(); fpul = c->fpul;
    fr3 = 0.0f;
    t = fr0 == fr3;
    if (t) {
        r1 = r15 + 0xa0u; r3 = r1; r2 = r1;
        SWF(0xa8, fr15); SWF(0xa4, fr15); SWF(0xa0, fr15);
    } else {
        r2 = 0x8c63a94cu; r4 = r15 + 0xa0u;
        c->r[4] = r4;
        sdk_8c63a94c(c);
        r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
        fr1 = c->fr[1]; fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6]; fr7 = c->fr[7];
        fr8 = c->fr[8]; fr9 = c->fr[9]; fr10 = c->fr[10]; fpul = c->fpul;
    }
    fr3 = SF(0xa0); fr3 = fr3 * fr13;       /* 0x8C029E48 */
    r2 = r15 + 0x94u;
    fr11 = fr15;
    SWF(0xa0, fr3);
    fr3 = SF(0xa4); fr3 = fr3 * fr13; SWF(0xa4, fr3);
    fr3 = SF(0xa8); fr3 = fr3 * fr13; SWF(0xa8, fr3);
    r1 = SW(0xc4); SWW(0x94, r1);
    r1 = SW(0xc8); SWW(0x98, r1);
    r1 = SW(0xcc); SWW(0x9c, r1);

    {                                       /* 0x8C029E7C: the sweep */
        const uint32_t s0 = SW(0);
        const uint8_t *v0 = HP(s0), *e1 = HP(r8), *e2 = HP(r9);
        for (;;) {
            fr3 = SF(0x1c);
            t = fr3 > fr11;
            r4 = r10;
            if (!t) {                       /* the end point */
                r4 = 0;
                r2 = r15 + 0xc4u;
                r3 = r15 + 0x94u;
                r1 = SW(0xc4); SWW(0x94, r1);
                r1 = SW(0xc8); SWW(0x98, r1);
                r1 = SW(0xcc); SWW(0x9c, r1);
                fr2 = SF(0xb8); fr1 = SF(0xc4); fr1 = fr1 + fr2; SWF(0x94, fr1);
                fr2 = SF(0xbc); fr1 = SF(0xc8); fr1 = fr1 + fr2; SWF(0x98, fr1);
                fr2 = SF(0xc0); fr1 = SF(0xcc); fr1 = fr1 + fr2; SWF(0x9c, fr1);
            }
            fr2 = HF(O, 0);                 /* 0x8C029EC6 */
            fr3 = SF(0x98);
            fr3 = FABS_F(fr3);
            t = fr3 > fr2;
            if (!t) {                       /* 0x8C029ED2: inside the triangle? */
                r3 = s0;
                fr5 = HF(e1, 8);
                fr7 = HF(v0, 8);
                r2 = s0;
                fr3 = fr5;
                fr6 = HF(v0, 0);
                fr3 = fr3 - fr7;
                fr2 = SF(0x94);
                fr2 = fr2 - fr6;
                fr9 = HF(e1, 0);
                fr4 = HF(e2, 8);
                fr1 = fr9;
                fr1 = fr1 - fr6;
                fr3 = fr3 * fr2;
                fr2 = SF(0x9c);
                fr2 = fr2 - fr7;
                fr8 = fr3;
                fr3 = fr4;
                fr3 = fr3 - fr5;
                fr1 = fr1 * fr2;
                fr2 = SF(0x94);
                fr2 = fr2 - fr9;
                fr8 = fr8 - fr1;
                fr3 = fr3 * fr2;
                fr10 = HF(e2, 0);
                fr7 = fr7 - fr4;
                fr2 = SF(0x9c);
                fr6 = fr6 - fr10;
                fr2 = fr2 - fr5;
                fr5 = fr3;
                fr1 = fr10;
                fr3 = SF(0x94);
                fr1 = fr1 - fr9;
                fr3 = fr3 - fr10;
                fr1 = fr1 * fr2;
                fr2 = 0.0f;
                fr7 = fr7 * fr3;
                fr3 = SF(0x9c);
                fr3 = fr3 - fr4;
                fr5 = fr5 - fr1;
                fr4 = fr7;
                fr6 = fr6 * fr3;
                fr3 = fr8;
                fr3 = fr3 * fr5;
                t = fr2 > fr3;
                fr4 = fr4 - fr6;
                r3 = r15 + 0x94u;
                if (!t) {
                    fr3 = fr5;
                    fr3 = fr3 * fr4;
                    t = fr2 > fr3;
                    if (!t) {
                        fr3 = fr8;
                        fr3 = fr3 * fr4;
                        fr2 = 0.0f;
                        t = fr2 > fr3;
                        if (!t) {
                            SWW(44, r10);   /* a contact */
                            break;
                        }
                    }
                }
            }
            t = r4 == 0;                    /* 0x8C029FA0 */
            if (t) break;
            fr3 = SF(0xa0); fr2 = SF(0x94); fr2 = fr2 + fr3; SWF(0x94, fr2);
            fr3 = SF(0xa4); fr2 = SF(0x98); fr2 = fr2 + fr3; SWF(0x98, fr2);
            fr3 = SF(0xa8); fr2 = SF(0x9c); fr2 = fr2 + fr3; SWF(0x9c, fr2);
            fr11 = fr11 + fr13;
        }
    }

    r3 = SW(44);                            /* 0x8C02A000 */
    t = r3 == 0;
    if (t) goto next;
    r2 = DEBUG_BYTE;
    r3 = RD8S(DEBUG_BYTE, 0);
    t = (int32_t)r3 >= 0;
    if (t) {                                /* 0x8C02A012: draw the polygon (debug) */
        r3 = RD32(r12, 0);
        r1 = RK32(r15, 12);
        r0 = RD32(r3, 24); WR32(r1, 0, r0);
        r0 = RD32(r3, 28); WR32(r1, 4, r0);
        r0 = RD32(r3, 32); WR32(r1, 8, r0);
        r1 = RD32(r12, 0);
        r3 = RK32(r15, 4);
        r0 = RD32(r1, 36); WR32(r3, 0, r0);
        r0 = RD32(r1, 40); WR32(r3, 4, r0);
        r0 = RD32(r1, 44); WR32(r3, 8, r0);
        r1 = RD32(r12, 0);
        r3 = RK32(r15, 8);
        r0 = RD32(r1, 48); WR32(r3, 0, r0);
        r0 = RD32(r1, 52); WR32(r3, 4, r0);
        r0 = RD32(r1, 56); WR32(r3, 8, r0);
        r3 = RK32(r15, 12);
        fr3 = RDF(r15 + 0xacu, 0); fr2 = RDF(r3, 0); fr2 = fr2 + fr3; WRF(r3, 0, fr2);
        r3 = RK32(r15, 4);
        fr2 = RDF(r3, 0); fr3 = RDF(r15 + 0xacu, 0); fr2 = fr2 + fr3;
        WRF(r3, 0, fr2);
        r3 = RK32(r15, 8);
        fr3 = RDF(r15 + 0xacu, 0); fr2 = RDF(r3, 0); fr2 = fr2 + fr3; WRF(r3, 0, fr2);
        r3 = RK32(r15, 12);
        fr3 = RDF(r15 + 0xb0u, 0); fr2 = RDF(r3 + 4u, 0); fr2 = fr2 + fr3; WRF(r3 + 4u, 0, fr2);
        r3 = RK32(r15, 4);
        fr3 = RDF(r15 + 0xb0u, 0); fr2 = RDF(r3 + 4u, 0); fr2 = fr2 + fr3; WRF(r3 + 4u, 0, fr2);
        r3 = RK32(r15, 8);
        fr3 = RDF(r15 + 0xb0u, 0); fr2 = RDF(r3 + 4u, 0); fr2 = fr2 + fr3; WRF(r3 + 4u, 0, fr2);
        r3 = RK32(r15, 12);
        fr3 = RDF(r15 + 0xb4u, 0); fr2 = RDF(r3 + 8u, 0); fr2 = fr2 + fr3; WRF(r3 + 8u, 0, fr2);
        r3 = RK32(r15, 4);
        fr3 = RDF(r15 + 0xb4u, 0); fr2 = RDF(r3 + 8u, 0); fr2 = fr2 + fr3; WRF(r3 + 8u, 0, fr2);
        r3 = RK32(r15, 8);
        fr3 = RDF(r15 + 0xb4u, 0); fr2 = RDF(r3 + 8u, 0); fr2 = fr2 + fr3; WRF(r3 + 8u, 0, fr2);
        r0 = 8; r1 = r15 + 0xb4u;
        r3 = 0x8c638f94u; r4 = RK32(r15, 36);   /* njGetMatrix */
        c->r[4] = r4;
        c->fr[4] = fr4; c->fr[5] = fr5; c->fr[6] = fr6; c->fr[7] = fr7;
        c->fr[8] = fr8; c->fr[9] = fr9; c->fr[10] = fr10; c->fr[11] = fr11;
        sdk_8c638f94(c);
        r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7]; LD_FR4_11();
        r2 = 0x8c639b98u; r4 = 1;           /* njPopMatrix(1) */
        c->r[4] = r4;
        sdk_8c639b98(c);
        r7 = c->r[7]; LD_FR4_11();
        r3 = 0x8c63d66cu; r6 = 32; r4 = 0x8c196034u; r5 = 3;
        c->r[4] = r4; c->r[5] = r5; c->r[6] = r6;
        sdk_8c63d66c(c);
        r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7]; LD_FR4_11();
        r3 = 0x8c639c70u; r4 = RK32(r15, 32);   /* njPushMatrix */
        c->r[4] = r4;
        sdk_8c639c70(c);
        fr1 = c->fr[1]; fr6 = c->fr[6]; fr8 = c->fr[8]; fr9 = c->fr[9];
        fr10 = c->fr[10]; fr11 = c->fr[11]; fpul = c->fpul;
    }

    /* 0x8C02A0E2: the contact record at r14 */
    Q = HPW(r14);
    r3 = r15 + 0xacu;
    fr3 = SF(0xc8); HWF(Q, 80, fr3);
    fr3 = SF(0xbc); fr2 = SF(0xc8); fr2 = fr2 + fr3; HWF(Q, 84, fr2);
    r2 = SW(0xac); HW32(Q, 8, r2);
    r2 = SW(0xb0); HW32(Q, 12, r2);
    r2 = SW(0xb4); HW32(Q, 16, r2);
    r1 = SW(24);
    t = r1 == 0;
    {
        int flip = 0;
        if (!t) {                           /* two-sided: the centre below? */
            fr3 = 0.0f;
            fr2 = SF(0xc8);
            t = fr3 > fr2;
            flip = (int)t;
        }
        if (!flip) {                        /* 0x8C02A180: the normal */
            r2 = r15 + 0xacu;
            r3 = SW(0xac); HW32(Q, 20, r3);
            r3 = SW(0xb0); HW32(Q, 24, r3);
            r3 = SW(0xb4); HW32(Q, 28, r3);
        } else {                            /* 0x8C02A118: flipped */
            fr2 = SF(0xac); fr2 = FNEG_F(fr2); HWF(Q, 20, fr2);
            fr2 = SF(0xb0); fr2 = FNEG_F(fr2); HWF(Q, 24, fr2);
            fr2 = SF(0xb4); fr2 = FNEG_F(fr2); HWF(Q, 28, fr2);
        }
    }
    fr5 = HF(Q, 24); fr4 = HF(Q, 20);       /* 0x8C02A190 */
    c->r[1] = r1; c->fr[1] = fr1; c->fr[2] = fr2; c->fr[4] = fr4; c->fr[5] = fr5; c->fr[6] = fr6;
    c->fr[12] = fr12;
    fn_8c10d0f8(c);
    r1 = c->r[1]; r13 = c->r[13]; r14 = c->r[14]; r15 = c->r[15];
    fr0 = c->fr[0]; fr6 = c->fr[6]; fr12 = c->fr[12]; fr13 = c->fr[13]; fr14 = c->fr[14]; fr15 = c->fr[15];
    S = SP(r15); Q = HPW(r14);
    fr0 = fr0 * fr14;
    fpul = FTRC(fr0);
    r2 = fpul;
    r2 = 0u - r2;
    HW32(Q, 4, r2);
    fr4 = HF(Q, 28);
    c->r[2] = r2; c->fr[0] = fr0; c->fr[4] = fr4; c->fpul = fpul;
    fn_8c10d058(c);
    r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7]; r13 = c->r[13]; r14 = c->r[14]; r15 = c->r[15];
    fr0 = c->fr[0]; fr4 = c->fr[4]; fr5 = c->fr[5]; fr6 = c->fr[6]; fr7 = c->fr[7];
    fr12 = c->fr[12]; fr13 = c->fr[13]; fr14 = c->fr[14]; fr15 = c->fr[15];
    S = SP(r15); Q = HPW(r14);
    fr0 = fr0 * fr14;
    fpul = FTRC(fr0);
    r2 = fpul;
    HW32(Q, 0, r2);
    r4 = 0;                                 /* njInvertMatrix(current) */
    c->r[4] = r4; c->fr[8] = fr8; c->fr[9] = fr9; c->fr[10] = fr10; c->fr[11] = fr11;
    sdk_8c6390b0(c);
    r7 = c->r[7]; LD_FR4_11();

    /* 0x8C02A1BC: the contact point and the displacement back in world space */
    fr3 = SF(0xc4); SWF(0xa0, fr3);
    SWF(0xa4, fr15);
    fr3 = SF(0xcc); SWF(0xa8, fr3);
    CALC_POINT(r15 + 0xa0u, r15 + 0x94u);
    P = HP(H32(HL, 0));
    fr3 = HF(P, 24); fr2 = SF(0x94); fr2 = fr2 + fr3; HWF(Q, 32, fr2);
    P = HP(H32(HL, 0));
    fr3 = HF(P, 28); fr2 = SF(0x98); fr2 = fr2 + fr3; HWF(Q, 36, fr2);
    P = HP(H32(HL, 0));
    fr3 = HF(P, 32); fr2 = SF(0x9c); fr2 = fr2 + fr3; HWF(Q, 40, fr2);
    SWF(0xa4, fr12);
    r4 = 0; r5 = r15 + 0xa0u; r6 = r15 + 0x94u;
    c->r[4] = r4; c->r[5] = r5; c->r[6] = r6;
    sdk_8c638ecc(c);
    r7 = c->r[7];
    fr5 = c->fr[5]; fr6 = c->fr[6]; fr7 = c->fr[7]; fr8 = c->fr[8]; fr9 = c->fr[9];
    fr10 = c->fr[10]; fr11 = c->fr[11];
    P = HP(H32(HL, 0));
    r2 = r15 + 48u;
    fr3 = HF(P, 24); fr2 = SF(0x94); fr2 = fr2 + fr3; HWF(Q, 44, fr2);
    P = HP(H32(HL, 0));
    fr3 = HF(P, 28); fr2 = SF(0x98); fr2 = fr2 + fr3; HWF(Q, 48, fr2);
    P = HP(H32(HL, 0));
    fr3 = HF(P, 32);
    fr2 = SF(0x9c); fr2 = fr2 + fr3; HWF(Q, 52, fr2);
    r1 = SW(0xb8); SWW(48, r1);
    r1 = SW(0xbc); SWW(52, r1);
    r1 = SW(0xc0); SWW(56, r1);
    r3 = SW(24);
    fr4 = SF(0xbc);
    t = r3 == 0;
    if (!t) {
        fr2 = 0.0f;
        fr3 = SF(0xc8);
        fr3 = fr3 * fr4;
        t = fr2 > fr3;
    } else {
        fr12 = fr12 * fr4;
        fr3 = 0.0f;
        t = fr3 > fr12;
    }
    if (t) {
        fr4 = FNEG_F(fr4);
        SWF(52, fr4);
    }
    fr3 = SF(0xb8); SWF(48, fr3);           /* 0x8C02A2CE */
    SWF(52, fr15);
    fr3 = SF(0xc0); SWF(56, fr3);
    r4 = 0; r5 = r15 + 48u; r6 = r15 + 0x94u;
    c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->fr[4] = fr4;
    sdk_8c638ecc(c);
    r7 = c->r[7]; LD_FR4_11();
    r2 = r14 + 56u;                         /* 0x8C02A2EE */
    r1 = SW(0x94); HW32(Q, 56, r1);
    r1 = SW(0x98); HW32(Q, 60, r1);
    r1 = SW(0x9c); HW32(Q, 64, r1);
    SWF(48, fr15);
    fr3 = SF(0xbc); SWF(52, fr3);
    SWF(56, fr15);
    r4 = 0; r5 = r15 + 48u; r6 = r15 + 0x94u;
    c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->fr[3] = fr3;
    sdk_8c638ecc(c);
    r4 = c->r[4]; r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
    fr2 = c->fr[2]; fr3 = c->fr[3]; LD_FR4_11(); macl = c->macl; fpul = c->fpul;

    /* 0x8C02A31E: the entry: the polygon at +4, the record at +68 */
    r2 = r14 + 68u;
    r1 = SW(0x94); HW32(Q, 68, r1);
    r1 = SW(0x98); HW32(Q, 72, r1);
    r1 = SW(0x9c); HW32(Q, 76, r1);
    r3 = H32(HL, 0);
    {
        const uint8_t *src = HP(r3);
        uint8_t *dst;
        int k;
        r0 = SW(16);
        r0 += 4u;
        dst = HPW(r0);
        for (k = 0; k < 64; k += 4) { r2 = H32(src, k); HW32(dst, k, r2); }
    }
    r3 = 0x8c10cddcu;
    r2 = r14;
    r1 = SW(16);
    r1 += 68u;
    r0 = 88;
    c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3;
    fn_8c10cddc(c);
    r15 = c->r[15];
    S = SP(r15);
    r1 = HITS_N;                            /* 0x8C02A384 */
    r2 = SW(16);
    r3 = HIT_SIZE;
    r2 += r3;
    SWW(16, r2);
    r0 = H32(cnt, 0);
    r0 += 1u;
    t = r0 == 16u;
    HW32(cnt, 0, r0);
    if (t) {                                /* the array is full */
        r3 = DEBUG_BYTE;
        r1 = RD8S(DEBUG_BYTE, 0);
        t = (int32_t)r1 >= 0;
        if (t) {
            r1 = 0x8c640922u; r4 = 0x00020016u; r5 = 0x8c112508u;
            c->r[2] = r2; c->r[4] = r4; c->r[5] = r5;
            sdk_8c640922(c);
            r2 = c->r[2]; r4 = c->r[4]; r6 = c->r[6]; r7 = c->r[7];
            fr2 = c->fr[2]; fr3 = c->fr[3]; fr4 = c->fr[4]; macl = c->macl; fpul = c->fpul;
        }
        goto done;
    }

next:                                      /* 0x8C02A400: njPopMatrix(1) */
    r2 = 0x8c639b98u; r4 = 1;
    c->r[2] = r2; c->r[4] = r4;
    c->fr[2] = fr2; c->fr[3] = fr3; c->fr[4] = fr4; c->fr[5] = fr5; c->fr[6] = fr6;
    c->fr[7] = fr7; c->fr[8] = fr8; c->fr[9] = fr9; c->fr[10] = fr10; c->fr[11] = fr11;
    sdk_8c639b98(c);
    r2 = c->r[2]; r4 = c->r[4]; r6 = c->r[6]; r7 = c->r[7];
    fr2 = c->fr[2]; fr3 = c->fr[3]; fr4 = c->fr[4]; fr10 = c->fr[10]; fr11 = c->fr[11];
    macl = c->macl; fpul = c->fpul;
    r3 = SW(20);
    r3 += 0xffffffffu;
    t = (int32_t)r3 > 0;
    SWW(20, r3);
    r12 += 4u;
    if (t) goto poly;

done:                                      /* 0x8C02A416 */
    r3 = HITS_N;
    r5 = H32(cnt, 0);
    t = (int32_t)r5 >= (int32_t)r10;
    if (!t) goto ret0;
    r4 = r10;                               /* 0x8C02A460: the lowest +0x98 */
    r12 = HITS;
    t = (int32_t)r4 >= (int32_t)r5;
    r6 = HIT_SIZE;
    r7 = 0;
    fr4 = RDF(HITS + 0x98u, 0);
    r6 += r12;
    while (!t) {
        const uint8_t *e = HP(r6);
        fr3 = HF(e, 0x98);
        t = fr4 > fr3;
        if (t) { fr4 = HF(e, 0x98); r7 = r4; }
        r3 = HIT_SIZE;
        r4 += 1u;
        t = (int32_t)r4 >= (int32_t)r5;
        r6 += r3;
    }
    r6 = 0x8c6bacf0u;                       /* 0x8C02A486: the records out */
    r4 = r5;
    t = (int32_t)r4 > 0;
    r14 = r12;
    while (t) {
        r3 = 0x8c10cddcu;
        r2 = r14;
        r1 = r6;
        r2 += 68u;
        r0 = 88;
        c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3;
        fn_8c10cddc(c);
        r15 = c->r[15];
        r2 = HIT_SIZE;
        r4 += 0xffffffffu;
        t = (int32_t)r4 > 0;
        r14 += r2;
        r6 += 88u;
    }
    /* 0x8C02A4A8: the sphere record */
    O = HPW(r13);
    r4 = HIT_SIZE;
    r3 = HITS_N;
    macl = (uint32_t)((int32_t)(int16_t)r7 * (int32_t)(int16_t)r4);
    r2 = HS16(cnt, 0);
    HW16(O, 40, r2);
    r4 = macl;
    r2 = 0x8c73e448u;
    HW16(O, 42, r7);
    r4 = (uint32_t)(int32_t)(int16_t)r4;
    r4 += r2;
    {
        const uint8_t *e = HP(r4);
        r3 = H32(e, 0); HW32(O, 52, r3);
        r1 = H32(e, 4); HW32(O, 56, r1);
        r1 = H32(e, 8); HW32(O, 60, r1);
        r1 = H32(e, 12); HW32(O, 64, r1);
        r1 = H32(e, 16); HW32(O, 68, r1);
        r1 = H32(e, 20); HW32(O, 72, r1);
        r1 = H32(e, 24); HW32(O, 76, r1);
        r1 = H32(e, 28); HW32(O, 80, r1);
        r0 = H32(e, 56); HW32(O, 108, r0);
        r0 = H32(e, 60); HW32(O, 112, r0);
        r0 = H32(e, 64); HW32(O, 116, r0);
        r0 = H32(e, 68); HW32(O, 120, r0);
        r0 = H32(e, 72); HW32(O, 124, r0);
        r0 = H32(e, 76);
        r3 = HITS_N;
        HW32(O, 128, r0);
        r0 = H32(cnt, 0);
        t = r0 == 1u;
        if (t) {                            /* one contact: its vectors */
            r1 = H32(e, 32); HW32(O, 84, r1);
            r1 = H32(e, 36); HW32(O, 88, r1);
            r1 = H32(e, 40); HW32(O, 92, r1);
            r0 = H32(e, 44); HW32(O, 96, r0);
            r0 = H32(e, 48); HW32(O, 100, r0);
            r0 = H32(e, 52); HW32(O, 104, r0);
            r1 = r13 + 96u;
            goto ret1;
        }
    }
    {                                       /* 0x8C02A560: the average */
        const uint8_t *h = HP(HITS);
        r4 = HITS;
        r2 = H32(h, 100); HW32(O, 84, r2);
        r2 = H32(h, 104); HW32(O, 88, r2);
        r2 = H32(h, 108); HW32(O, 92, r2);
        r2 = r4 + 112u;
        r1 = r13 + 96u;
        r0 = H32(h, 112); HW32(O, 96, r0);
        r0 = H32(h, 116); HW32(O, 100, r0);
        r0 = H32(h, 120);
        r2 = HIT_SIZE;
        HW32(O, 104, r0);
        r5 = H32(cnt, 0);
        r5 += 0xffffffffu;
        t = (int32_t)r5 > 0;
        r4 += r2;
        while (t) {
            const uint8_t *e = HP(r4);
            fr3 = HF(e, 100); fr2 = HF(O, 84); fr2 = fr2 + fr3; HWF(O, 84, fr2);
            fr2 = HF(O, 88); fr3 = HF(e, 104); fr2 = fr2 + fr3; HWF(O, 88, fr2);
            fr2 = HF(O, 92); fr3 = HF(e, 108); fr2 = fr2 + fr3; HWF(O, 92, fr2);
            fr2 = HF(O, 96); fr3 = HF(e, 112); fr2 = fr2 + fr3; HWF(O, 96, fr2);
            fr3 = HF(e, 116); fr2 = HF(O, 100);
            r1 = r4 + 120u;
            r3 = HIT_SIZE;
            fr2 = fr2 + fr3;
            r5 += 0xffffffffu;
            t = (int32_t)r5 > 0;
            HWF(O, 100, fr2);
            fr2 = HF(O, 104); fr3 = HF(e, 120); fr2 = fr2 + fr3; HWF(O, 104, fr2);
            r4 += r3;
        }
        r2 = HITS_N;                        /* 0x8C02A5F4 */
        fr4 = 1.0f;
        r3 = H32(cnt, 0);
        fpul = r3;
        fr3 = (float)(int32_t)fpul;
        fr4 = fr4 / fr3;
        fr3 = HF(O, 84); fr3 = fr3 * fr4; HWF(O, 84, fr3);
        fr2 = HF(O, 88); fr2 = fr2 * fr4; HWF(O, 88, fr2);
        fr3 = HF(O, 92); fr3 = fr3 * fr4; HWF(O, 92, fr3);
        fr2 = HF(O, 96); fr2 = fr2 * fr4; HWF(O, 96, fr2);
        fr3 = HF(O, 100); fr3 = fr3 * fr4; HWF(O, 100, fr3);
        fr2 = HF(O, 104); fr2 = fr2 * fr4; HWF(O, 104, fr2);
    }
ret1:
    r0 = 1;
    goto out;
ret0:                                      /* 0x8C02A41E */
    r0 = 0;
out:                                       /* 0x8C02A632 */
    r1 = 0xf4u;
    r15 += r1;
    fr12 = RKF(r15, 4); fr13 = RKF(r15, 8); fr14 = RKF(r15, 12); fr15 = RKF(r15, 16);
    r8 = RK32(r15, 20); r9 = RK32(r15, 24); r10 = RK32(r15, 28); r11 = RK32(r15, 32);
    r12 = RK32(r15, 36); r13 = RK32(r15, 40); r14 = RK32(r15, 44);
    r15 += 48u;
    c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4; c->r[5] = r5;
    c->r[6] = r6; c->r[7] = r7; c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11;
    c->r[12] = r12; c->r[13] = r13; c->r[14] = r14; c->r[15] = r15;
    c->fr[2] = fr2; c->fr[3] = fr3; c->fr[4] = fr4; c->fr[12] = fr12; c->fr[13] = fr13;
    c->fr[14] = fr14; c->fr[15] = fr15;
    c->t = t; c->macl = macl; c->fpul = fpul;
}

/* ---- math = native: the same sweep with host math ----------------------
 * The algorithm is the console's: the same polygons in the same order, the
 * same march (steps of radius * 0.5 along the motion, the end point last,
 * the first point within the radius of the plane and inside the triangle
 * is the contact), the same 16-contact cap, the same choice and average.
 * Only the arithmetic changes: host acosf/atan2f/asinf instead of the
 * game's SHC libm (one acosf where the game calls it twice), the rotation
 * into the polygon's plane built from the same 16-bit angles and the same
 * sin/cos table on three host vectors instead of the matrix stack, the
 * game's inverse on a host copy, no guest stack frame and no callee trampolines.  With the
 * same host libm the angles almost always quantize to the same 16-bit
 * value, so the results are the console's within rounding; the stated
 * bound (check mode) is one angle unit (1/65536 turn), normals 1e-5 and
 * positions 2e-4 * (1 + |centre - v0| + |motion| + radius) + 1e-4.
 *
 * sph_native computes into a host record (no guest write) and
 * sph_commit writes what the game would: the contact count, the contact
 * array (polygon + 88-byte record), the copies at 0x8C6BACF0 and the
 * sphere record.  A full array (16 contacts) leaves the game's matrix
 * stack one deeper with the inverted rotation on top: the commit does
 * the same through the SDK calls.  With the debug byte set the exact
 * body runs (it draws).
 *
 * math = check: the exact body drives the game; sph_native runs first on
 * the same inputs and its record is compared with what the exact body
 * wrote.  Logged per call: values outside the bound, and contact flips
 * (a polygon hit by one side only) with the polygon's margin (the
 * smallest distance of a tested point to the radius slab or a triangle
 * edge); a flip with a margin above the bound is a failure. */
#include <math.h>
#include <string.h>

#define SPH_MAXC 16

typedef struct {
    uint32_t r0;
    int early;                      /* radius <= 0: nothing written */
    uint32_t n, chosen;
    int full;
    uint32_t full_ax, full_ay;      /* the last polygon's angles (full array) */
    uint32_t poly[SPH_MAXC];
    uint32_t rec[SPH_MAXC][22];     /* the 88-byte records, as words */
    float bound[SPH_MAXC];
    uint32_t avg[6];                /* sphere record +84..+104 */
    /* check mode: per polygon of the call, hit and margin */
    uint32_t np;
    uint32_t pp[256];
    float margin[256], pbound[256];
    uint32_t nax[256], nay[256];
    float nny[256], nacos[256];
    uint8_t hit[256];
} sph_res;

static inline uint32_t sph_f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float sph_u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* m = I * RotX(ax) * RotY(ay), all 16 words as math.c's m_rot_x / m_rot_y
 * leave them on the unit matrix (column-major, m[4j+i]). */
static inline void sph_rot(uint32_t ax, uint32_t ay, float m[16]) {
    float s, co;
    for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    if (ax) {
        s = sh4_sin16(ax); co = sh4_cos16(ax);
        for (int i = 0; i < 4; i++) {
            float c1 = m[4 + i], c2 = m[8 + i];
            m[4 + i] = co * c1 + s * c2;
            m[8 + i] = co * c2 - s * c1;
        }
    }
    if (ay) {
        s = sh4_sin16(ay); co = sh4_cos16(ay);
        for (int i = 0; i < 4; i++) {
            float c0 = m[i], c2 = m[8 + i];
            m[i] = co * c0 - s * c2;
            m[8 + i] = s * c0 + co * c2;
        }
    }
}
/* njCalcPoint (math.c m_xform with w = 1). */
static inline void sph_xf(const float m[16], const float v[3], float o[3]) {
    for (int i = 0; i < 3; i++) o[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * 1.0f;
}
void hle_m_invert(float *m);            /* math.c: njInvertMatrix's arithmetic */

static inline uint32_t sph_atan_ang(float x, float y, float K) {
    if (x == 0.0f && y == 0.0f) return 0x80000000u;
    return 0u - FTRC(atan2f(x, y) * K);
}

static void sph_native(sh4_ctx *c, sph_res *res, int check) {
    const uint8_t *O = HP(c->r[4]);
    const float R = HF(O, 0);
    const float K = RDF(0x0c029cdcu, 0);
    res->n = 0; res->chosen = 0; res->full = 0; res->np = 0; res->r0 = 0;
    res->early = !(R > 0.0f);
    if (res->early) return;

    const float mv[3] = {HF(O, 16), HF(O, 20), HF(O, 24)};
    const float len = sqrtf(mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2]);
    const float step = R * RDF(0x0c029b8cu, 0);
    const float kneg = RDF(0x0c029e28u, 0);
    const float mlen = len;

    uint32_t node = H32(HP(COLL_LIST), 0);
    while (node && H32(HP(node), 8) != c->r[5]) node = H32(HP(node), 0);
    if (!node) return;
    const uint8_t *N = HP(node);
    int32_t count = (int32_t)H32(N, 36);
    uint32_t list = H32(N, 28);

    for (; count > 0; count--, list += 4u) {
        const uint32_t pa = H32(HP(list), 0);
        const uint8_t *P = HP(pa);
        const float v0[3] = {HF(P, 24), HF(P, 28), HF(P, 32)};
        const float e1[3] = {HF(P, 36) - v0[0], HF(P, 40) - v0[1], HF(P, 44) - v0[2]};
        const float e2[3] = {HF(P, 48) - v0[0], HF(P, 52) - v0[1], HF(P, 56) - v0[2]};
        const float cw[3] = {HF(O, 4) - v0[0], HF(O, 8) - v0[1], HF(O, 12) - v0[2]};
        /* 0x8C027360 with p0 = 0: a = e1, b = 0 - e2 */
        const float bx = 0.0f - e2[0], by = 0.0f - e2[1], bz = 0.0f - e2[2];
        float n[3] = {e1[2] * by - e1[1] * bz, e1[0] * bz - e1[2] * bx, e1[1] * bx - e1[0] * by};
        const float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl == 0.0f) { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; }
        else { const float r = 1.0f / nl; n[0] *= r; n[1] *= r; n[2] *= r; }
        const uint32_t ax = 0u - FTRC(acosf(n[1]) * K);
        const uint32_t ay = sph_atan_ang(n[0], n[2], K);
        float m[16];
        sph_rot(ax, ay, m);
        float p1[3], p2[3], cc[3], mm[3];
        sph_xf(m, e1, p1); sph_xf(m, e2, p2); sph_xf(m, cw, cc); sph_xf(m, mv, mm);

        const int two = (H32(P, 60) & 0x00800000u) != 0;
        float Reff = R;
        if (two && 0.0f > cc[1]) Reff = R * kneg;

        float sv[3] = {mm[0], mm[1], mm[2]};
        const float sl = sqrtf(sv[0] * sv[0] + sv[1] * sv[1] + sv[2] * sv[2]);
        if (sl == 0.0f) { sv[0] = sv[1] = sv[2] = 0.0f; }
        else { const float r = 1.0f / sl; sv[0] *= r; sv[1] *= r; sv[2] *= r; }
        sv[0] *= step; sv[1] *= step; sv[2] *= step;

        /* the march (0x8C029E7C), v0 at the origin */
        float px = cc[0], py = cc[1], pz = cc[2], walked = 0.0f, marg = 3.4e38f;
        int hit = 0;
        const float l1 = check ? sqrtf(p1[0] * p1[0] + p1[2] * p1[2]) : 0.0f;
        const float l2 = check ? sqrtf((p2[0] - p1[0]) * (p2[0] - p1[0]) + (p2[2] - p1[2]) * (p2[2] - p1[2])) : 0.0f;
        const float l3 = check ? sqrtf(p2[0] * p2[0] + p2[2] * p2[2]) : 0.0f;
        for (;;) {
            const int last = !(mlen > walked);
            if (last) { px = cc[0] + mm[0]; py = cc[1] + mm[1]; pz = cc[2] + mm[2]; }
            const float ay_ = fabsf(py);
            int inside = 0;
            float f8 = 0, f5 = 0, f4 = 0;
            if (!(ay_ > R)) {
                f8 = p1[2] * px - p1[0] * pz;
                f5 = (p2[2] - p1[2]) * (px - p1[0]) - (p2[0] - p1[0]) * (pz - p1[2]);
                f4 = (0.0f - p2[2]) * (px - p2[0]) - (0.0f - p2[0]) * (pz - p2[2]);
                inside = !(0.0f > f8 * f5) && !(0.0f > f5 * f4) && !(0.0f > f8 * f4);
            }
            if (check) {
                float d = fabsf(R - ay_);
                if (!(ay_ > R) || d < marg) {
                    if (ay_ > R) { f8 = p1[2] * px - p1[0] * pz;
                        f5 = (p2[2] - p1[2]) * (px - p1[0]) - (p2[0] - p1[0]) * (pz - p1[2]);
                        f4 = (0.0f - p2[2]) * (px - p2[0]) - (0.0f - p2[0]) * (pz - p2[2]); }
                    /* the point's distance to the slab or an edge it is near */
                    float e = d;
                    if (l1 > 0.0f && fabsf(f8) / l1 < e) e = fabsf(f8) / l1;
                    if (l2 > 0.0f && fabsf(f5) / l2 < e) e = fabsf(f5) / l2;
                    if (l3 > 0.0f && fabsf(f4) / l3 < e) e = fabsf(f4) / l3;
                    if (e < marg) marg = e;
                }
            }
            if (inside) { hit = 1; break; }
            if (last) break;
            px += sv[0]; py += sv[1]; pz += sv[2];
            walked += step;
        }
        const float bnd = 2e-4f * (1.0f + sqrtf(cw[0] * cw[0] + cw[1] * cw[1] + cw[2] * cw[2]) + mlen + R) + 1e-4f;
        if (check && res->np < 256) {
            res->pp[res->np] = pa; res->margin[res->np] = marg; res->pbound[res->np] = bnd; res->hit[res->np] = (uint8_t)hit;
            res->nax[res->np] = ax; res->nay[res->np] = ay; res->nny[res->np] = n[1]; res->nacos[res->np] = acosf(n[1]);
            res->np++;
        }
        if (!hit) continue;

        /* the contact record (0x8C02A0E2) */
        uint32_t *q = res->rec[res->n];
        float nf[3] = {n[0], n[1], n[2]};
        if (two && 0.0f > cc[1]) { nf[0] = -n[0]; nf[1] = -n[1]; nf[2] = -n[2]; }
        q[20] = sph_f2u(cc[1]);
        q[21] = sph_f2u(cc[1] + mm[1]);
        q[2] = sph_f2u(n[0]); q[3] = sph_f2u(n[1]); q[4] = sph_f2u(n[2]);
        q[5] = sph_f2u(nf[0]); q[6] = sph_f2u(nf[1]); q[7] = sph_f2u(nf[2]);
        q[1] = sph_atan_ang(nf[0], nf[1], K);
        q[0] = FTRC(asinf(nf[2]) * K);
        float t[3], o[3], mi[16];
        for (int k = 0; k < 16; k++) mi[k] = m[k];
        hle_m_invert(mi);
        t[0] = cc[0]; t[1] = 0.0f; t[2] = cc[2];
        sph_xf(mi, t, o);
        q[8] = sph_f2u(o[0] + v0[0]); q[9] = sph_f2u(o[1] + v0[1]); q[10] = sph_f2u(o[2] + v0[2]);
        t[1] = Reff;
        sph_xf(mi, t, o);
        q[11] = sph_f2u(o[0] + v0[0]); q[12] = sph_f2u(o[1] + v0[1]); q[13] = sph_f2u(o[2] + v0[2]);
        t[0] = mm[0]; t[1] = 0.0f; t[2] = mm[2];
        sph_xf(mi, t, o);
        q[14] = sph_f2u(o[0]); q[15] = sph_f2u(o[1]); q[16] = sph_f2u(o[2]);
        t[0] = 0.0f; t[1] = mm[1]; t[2] = 0.0f;
        sph_xf(mi, t, o);
        q[17] = sph_f2u(o[0]); q[18] = sph_f2u(o[1]); q[19] = sph_f2u(o[2]);
        res->poly[res->n] = pa;
        res->bound[res->n] = bnd;
        res->n++;
        if (res->n == SPH_MAXC) { res->full = 1; res->full_ax = ax; res->full_ay = ay; break; }
    }
    if (res->n < 1) return;
    /* the lowest +84 (entry +0x98) */
    float lo = sph_u2f(res->rec[0][21]);
    for (uint32_t i = 1; i < res->n; i++) {
        const float h = sph_u2f(res->rec[i][21]);
        if (lo > h) { lo = h; res->chosen = i; }
    }
    if (res->n == 1) {
        for (int k = 0; k < 6; k++) res->avg[k] = res->rec[0][8 + k];
    } else {                                /* the average (0x8C02A560) */
        float a[6];
        for (int k = 0; k < 6; k++) a[k] = sph_u2f(res->rec[0][8 + k]);
        for (uint32_t i = 1; i < res->n; i++)
            for (int k = 0; k < 6; k++) a[k] = a[k] + sph_u2f(res->rec[i][8 + k]);
        const float inv = 1.0f / (float)(int32_t)res->n;
        for (int k = 0; k < 6; k++) res->avg[k] = sph_f2u(a[k] * inv);
    }
    res->r0 = 1;
}

static void sph_commit(sh4_ctx *c, uint32_t oa, const sph_res *res) {
    if (res->early) { c->r[0] = 0; return; }
    uint8_t *cnt = HPW(HITS_N);
    HW32(cnt, 0, res->n);
    for (uint32_t i = 0; i < res->n; i++) {
        uint8_t *e = HPW(HITS + i * HIT_SIZE);
        const uint8_t *src = HP(res->poly[i]);
        for (int k = 0; k < 64; k += 4) HW32(e, 4 + k, H32(src, k));
        for (int k = 0; k < 22; k++) HW32(e, 68 + 4 * k, res->rec[i][k]);
    }
    if (res->full) {                        /* the game leaves the rotation pushed, inverted */
        c->r[4] = 0x8c67c640u; sdk_8c639c70(c);
        if (res->full_ax) { c->r[4] = 0; c->r[5] = res->full_ax; sdk_8c639ec8(c); }
        if (res->full_ay) { c->r[4] = 0; c->r[5] = res->full_ay; sdk_8c639f5c(c); }
        c->r[4] = 0; sdk_8c6390b0(c);
    }
    if (res->n < 1) { c->r[0] = 0; return; }
    for (uint32_t i = 0; i < res->n; i++) {
        uint8_t *d = HPW(0x8c6bacf0u + 88u * i);
        for (int k = 0; k < 22; k++) HW32(d, 4 * k, res->rec[i][k]);
    }
    uint8_t *O = HPW(oa);
    const uint32_t *e = res->rec[res->chosen];
    HW16(O, 40, res->n);
    HW16(O, 42, res->chosen);
    for (int k = 0; k < 8; k++) HW32(O, 52 + 4 * k, e[k]);
    for (int k = 0; k < 6; k++) HW32(O, 84 + 4 * k, res->avg[k]);
    for (int k = 0; k < 6; k++) HW32(O, 108 + 4 * k, e[14 + k]);
    c->r[0] = 1;
}

/* check mode: what the exact body wrote against the native record */
static uint32_t ck_pang, ck_plog;
static uint32_t ck_calls, ck_polys, ck_contacts, ck_flips, ck_bad, ck_ang, ck_logged, ck_last;
static float ck_ratio;
static void ck_line(const char *what, uint32_t poly, float d, float b) {
    if (ck_logged < 2000) {
        ck_logged++;
        sa1_log("check: f%u sweep %s poly %08x diff %.6g bound %.6g%s", hle_frame_count(), what, poly, d, b, d > b ? " OUTSIDE" : "");
    }
}
static void sph_summary(int force) {
    const uint32_t f = hle_frame_count();
    if (!force && f / 600 == ck_last / 600) return;
    ck_last = f;
    sa1_log("check: sweep summary f%u calls %u polys %u contacts %u flips %u angle-steps %u poly-angle-diffs %u outside %u max %.3g of the bound",
            f, ck_calls, ck_polys, ck_contacts, ck_flips, ck_ang, ck_pang, ck_bad, ck_ratio);
}
static void sph_compare(uint32_t oa, const sph_res *res, uint32_t r0) {
    ck_calls++;
    ck_polys += res->np;
    if (res->early) { if (r0 != 0) { ck_bad++; ck_line("early r0", 0, 1, 0); } sph_summary(0); return; }
    const uint32_t nref = H32(HP(HITS_N), 0);
    ck_contacts += nref;
    uint32_t j = 0, i = 0, flips = 0;
    for (uint32_t k = 0; k < res->np; k++) {
        const uint8_t *pk = HP(res->pp[k]);
        const int rh = j < nref && !memcmp(pk, HP(HITS + j * HIT_SIZE + 4u), 64);
        if (k < tap_k && (tap_ax[k] != res->nax[k] || tap_ay[k] != res->nay[k])) {
            ck_pang++;
            if (getenv("SA1_SPH_DETAIL") && ck_plog++ < 200)
                sa1_log("check:   poly %08x ax ref %d native %d ay ref %d native %d ny ref %.9g native %.9g acos ref %.9g native %.9g n ref %.9g %.9g",
                        res->pp[k], (int)tap_ax[k], (int)res->nax[k], (int)tap_ay[k], (int)res->nay[k], tap_ny[k], res->nny[k], tap_acos[k], res->nacos[k], tap_nx[k], tap_nz[k]);
        }
        if (rh != res->hit[k]) {
            flips++; ck_flips++;
            if (res->margin[k] > res->pbound[k]) ck_bad++;
            ck_line(rh ? "flip ref-hit native-miss margin" : "flip ref-miss native-hit margin", res->pp[k], res->margin[k], res->pbound[k]);
        } else if (rh) {
            const uint8_t *q = HP(HITS + j * HIT_SIZE + 68u);
            const uint32_t *w = res->rec[i];
            const float b = res->bound[i];
            for (int a = 0; a < 2; a++) {
                const int16_t d = (int16_t)(H32(q, 4 * a) - w[a]);
                if (d) { ck_ang++; if (d > 1 || d < -1) { ck_bad++; ck_line("angle", res->pp[k], (float)d, 1); } }
            }
            for (int a = 2; a < 22; a++) {
                const float bb = a < 8 ? 1e-5f : b;
                const float d = fabsf(HF(q, 4 * a) - sph_u2f(w[a]));
                if (!(d <= bb)) {
                    ck_bad++; ck_line(a < 8 ? "normal" : "position", res->pp[k], d, bb);
                    if (getenv("SA1_SPH_DETAIL") && ck_logged < 2000)
                        sa1_log("check:   word %d ref %.9g native %.9g angles ref %08x %08x native %08x %08x", a, HF(q, 4 * a), sph_u2f(w[a]), H32(q, 0), H32(q, 4), w[0], w[1]);
                }
                if (d / bb > ck_ratio) ck_ratio = d / bb;
            }
        }
        if (rh) j++;
        if (res->hit[k]) i++;
    }
    if (j != nref) { ck_bad++; ck_line("unmatched contacts", 0, (float)(nref - j), 0); }
    if (!flips && nref) {
        const uint8_t *O = HP(oa);
        const uint32_t ch = HS16(O, 42) & 0xffffu;
        if (ch != res->chosen) {
            const float d = fabsf(sph_u2f(res->rec[ch < res->n ? ch : 0][21]) - sph_u2f(res->rec[res->chosen][21]));
            ck_line("chosen tie", res->poly[res->chosen], d, res->bound[res->chosen]);
            if (!(d <= res->bound[res->chosen])) ck_bad++;
        } else {
            for (int k = 0; k < 6; k++) {
                const float d = fabsf(HF(O, 84 + 4 * k) - sph_u2f(res->avg[k]));
                const float b = res->bound[res->chosen];
                if (!(d <= b)) { ck_bad++; ck_line("average", 0, d, b); }
            }
        }
    }
    if (r0 != res->r0 && !flips) { ck_bad++; ck_line("r0", 0, 1, 0); }
    sph_summary(0);
}
static void sph_atexit(void) { sph_summary(1); }

static int sph_mode = -1;
void fn_8c029b00(sh4_ctx *c) {
    HLE_ENTER(0x8c029b00u, "sphere sweep fn_8c029b00");
    if (sph_use_rec()) { fn_8c029b00_orig(c); return; }
    if (sph_mode < 0) {
        sph_mode = hle_math_mode();
        sa1_log("sphere sweep: math %s", sph_mode == 1 ? "native" : sph_mode == 2 ? "check" : "exact");
        if (sph_mode == 2) atexit(sph_atexit);
    }
    if (sph_mode == 0 || (int32_t)RD8S(DEBUG_BYTE, 0) >= 0) { sph_8c029b00(c); return; }
    static sph_res res;
    const uint32_t oa = c->r[4];
    if (sph_mode == 1) { sph_native(c, &res, 0); sph_commit(c, oa, &res); return; }
    sph_native(c, &res, 1);
    sph_tap = 1; tap_k = 0;
    sph_8c029b00(c);
    sph_tap = 0;
    sph_compare(oa, &res, c->r[0]);
    /* SA1_SPH_DRIVE=native (diagnosis): the native record replaces what the
     * exact body wrote, so native drives and exact is the shadow (a full
     * array keeps the exact body's: its matrix stack is already pushed). */
    static int drive = -1;
    if (drive < 0) drive = getenv("SA1_SPH_DRIVE") != NULL;
    if (drive && !res.full) sph_commit(c, oa, &res);
}
#endif
