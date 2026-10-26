/* set_scan.c - native version of 0x8C091380, the SET range-in scan.
 *
 * Every game frame (in the mode where the task at 0x8C0915A2 picks it; the
 * other mode runs 0x8C091148) the SET manager walks the act's SET table:
 * the count at 0x8C79A3DC (16-bit), 16-byte entries from 0x8C79A3E0 (+0 a
 * spawn count byte, +2 flags: bit 15 active, bit 0 loaded, bit 1 position
 * from the task at +12; +4 the task, +8 the SET record, +12 the task or,
 * once loaded, a float), the object list at *0x8C79A3CC +4 (20-byte
 * entries: +0 and +1 the arguments, +2 flags: bit 0 the radius at +4,
 * bit 1 always in range, bit 2 always in range while 0x8C1C0ED8 is 0; +8
 * a word copied to the task; +12 the LoadSub).  For each active entry not
 * yet loaded it tests the squared distance from the player (0x8C78C608, or
 * 0x8C18B7EC in the modes the prologue picks) against the radius with the
 * leaf 0x8C09111A, and when in range creates the task (0x8C09852E), copies
 * position, rotation and scale into its data block, and marks the entry.
 *
 * ss_native is the recompiled body with the leaf inlined: the same float
 * operations in the same order (fmac through FMAC_F), the same guest
 * stack frame and the same registers handed to every guest call and left
 * at the exit, so math = native is bit-exact with the recompiled body.  The
 * leaf's register outputs (fr0, fr3-fr10, t) live in a shadow that is
 * written to the context before each guest call and at the exit, as the
 * leaf would have left them.  math = check runs a dry walk (no writes, no
 * calls) before the recompiled body and compares each entry's decision
 * and the stored radius bit for bit.  sa1psp.ini math_exact / --math-exact
 * 8c091380 keeps this routine exact under math = native (A/B timing).  The
 * wrapper is compiled only when the emitted game.h has the renamed body
 * (recomp/sa1_us_1005/hle_wraps.tsv).
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"

#include <stdlib.h>
#include <string.h>

#ifdef SA1_WRAP_8C091380

#define HP(a)       ((const uint8_t *)RAMP(a, 0))
#define H16S(p, o)  ((uint32_t)(int32_t)*(const int16_t *)(const void *)((p) + (o)))
#define H32(p, o)   (*(const sa1_u32a *)(const void *)((p) + (o)))
#define HF(p, o)    (*(const sa1_f32a *)(const void *)((p) + (o)))
#define SET_COUNT   0x8c79a3dcu         /* entries (16-bit) */
#define SET_TABLE   0x8c79a3e0u         /* 16-byte entries */
#define SET_OBJLIST 0x8c79a3ccu         /* -> +4 the object list */
#define PLAYER      0x8c78c608u
#define CAM_TASK    0x8c18b7ecu
#define FREE_LOAD   0x8c1c0ed8u
#define K_RAD_A     0x0c0914a8u         /* literal pool of the body */
#define K_RAD_B     0x0c0914acu
#define K_STORE     0x0c091648u

void fn_8c091380_orig(sh4_ctx *c);
void fn_8c04f7e0(sh4_ctx *c);
void fn_8c04f698(sh4_ctx *c);
void fn_8c09852e(sh4_ctx *c);
void fn_8c098b42(sh4_ctx *c);

/* What the leaf 0x8C09111A leaves in the context. */
typedef struct { float f0, f3, f4, f5, f6, f7, f8, f9, f10; uint32_t t; } ss_shadow;
static inline void sh_load(ss_shadow *s, const sh4_ctx *c) {
    s->f0 = c->fr[0]; s->f3 = c->fr[3]; s->f4 = c->fr[4]; s->f5 = c->fr[5]; s->f6 = c->fr[6];
    s->f7 = c->fr[7]; s->f8 = c->fr[8]; s->f9 = c->fr[9]; s->f10 = c->fr[10]; s->t = c->t;
}
static inline void sh_store(const ss_shadow *s, sh4_ctx *c) {
    c->fr[0] = s->f0; c->fr[3] = s->f3; c->fr[4] = s->f4; c->fr[5] = s->f5; c->fr[6] = s->f6;
    c->fr[7] = s->f7; c->fr[8] = s->f8; c->fr[9] = s->f9; c->fr[10] = s->f10; c->t = s->t;
}

/* 0x8C09111A: r4 -> position, fr4..fr6 the player, fr7 the squared radius;
 * r0 = 1 when the squared distance stays <= fr7. */
static inline __attribute__((always_inline)) uint32_t ss_leaf(ss_shadow *s, const uint8_t *p,
        float px, float py, float pz, float rad) {
    float fr4 = px, fr5 = py, fr6 = pz, fr0 = s->f0, fr3;
    const float fr8 = HF(p, 0), fr9 = HF(p, 4), fr10 = HF(p, 8);
    uint32_t t, r0;
    fr4 = fr4 - fr8;
    fr3 = fr4;
    fr4 = fr4 * fr3;
    t = fr4 > rad;
    if (t) goto out;
    fr5 = fr5 - fr9;
    fr0 = fr5;
    fr4 = FMAC_F(fr0, fr5, fr4);
    t = fr4 > rad;
    if (t) goto out;
    fr6 = fr6 - fr10;
    fr0 = fr6;
    fr4 = FMAC_F(fr0, fr6, fr4);
    t = fr4 > rad;
out:
    r0 = t ? 0u : 1u;
    s->f0 = fr0; s->f3 = fr3; s->f4 = fr4; s->f5 = fr5; s->f6 = fr6; s->f7 = rad;
    s->f8 = fr8; s->f9 = fr9; s->f10 = fr10; s->t = t;
    return r0;
}

/* The radius the body picks for an object-list entry; *always = 1 when the
 * leaf is skipped (always in range). */
static inline __attribute__((always_inline)) float ss_radius(const uint8_t *o, uint32_t fl, int *always) {
    float fr12;
    *always = 0;
    if (fl & 1u) {
        fr12 = HF(o, 4);
    } else if (fl & 2u) {
        *always = 1;
        return RDF(K_RAD_A, 0);
    } else {
        fr12 = RDF(K_RAD_B, 0);
    }
    if ((fl & 4u) && RD32(FREE_LOAD, 0) == 0) *always = 1;
    return fr12;
}

/* The prologue: picks the player record (0 = no scan).  Calls the two guest
 * helpers exactly as the body does. */
static uint32_t ss_player(sh4_ctx *c, uint32_t *r0p, uint32_t *r1p, uint32_t *r2p, uint32_t *r3p, uint32_t *tp,
                          int *done) {
    uint32_t r0 = *r0p, r1, r2 = *r2p, r3, r4, t;
    *done = 0;
    r1 = PLAYER;
    r3 = RD32(PLAYER, 0);
    t = r3 == 0;
    if (t) goto use_cam;
    r3 = 0x8c19b468u;
    r0 = RD16S(0x8c19b468u, 0);
    t = r0 == 1u;
    if (t) goto use_cam;
    c->r[0] = r0; c->r[1] = r1;
    fn_8c04f7e0(c);
    r0 = c->r[0]; r1 = c->r[1];
    r3 = 0x0000ff00u;
    r2 = 0x2au << 8;
    r0 &= 0xffffu;
    r0 &= r3;
    t = r0 == r2;
    if (t) goto use_cam;
    r1 = 0x8c04f698u;
    c->r[0] = r0; c->r[2] = r2;
    fn_8c04f698(c);
    r0 = c->r[0]; r2 = c->r[2];
    t = r0 == 7u;
    if (!t) goto use_player;
    r3 = 0x8c7a40e8u;
    r0 = RD16S(0x8c7a40e8u, 0) & 0xffffu;
    t = (r0 & 0x40u) == 0;
    if (t) goto use_player;
use_cam:
    r3 = CAM_TASK;
    r4 = RD32(CAM_TASK, 0);
    t = r4 == 0;
    if (t) *done = 1;
    goto out;
use_player:
    r4 = RD32(PLAYER, 0);
out:
    *r0p = r0; *r1p = r1; *r2p = r2; *r3p = r3; *tp = t;
    return r4;
}

/* math = native: the body with host-side locals.  The same guest stack
 * frame (12 saved words, one scratch word) and the same register hand-over
 * at each guest call. */
static void ss_native(sh4_ctx *c) {
    const uint32_t sp0 = c->r[15];
    const uint32_t e8 = c->r[8], e9 = c->r[9], e10 = c->r[10], e11 = c->r[11], e12 = c->r[12],
                   e13 = c->r[13], e14 = c->r[14];
    const float ef12 = c->fr[12], ef13 = c->fr[13], ef14 = c->fr[14], ef15 = c->fr[15];
    uint32_t r0 = c->r[0], r1 = 0, r2 = c->r[2], r3 = 0, r4, r8, r9, r10, r11 = e11, r12, r13 = e13, r14 = e14,
             r15, t = 0;
    float fr12 = ef12, fr13, fr14, fr15, fr3;
    ss_shadow s;
    int done;

    r15 = sp0;
    r15 -= 4; WK32(r15, 0, e14);
    r15 -= 4; WK32(r15, 0, e13);
    r15 -= 4; WK32(r15, 0, e12);
    r15 -= 4; WK32(r15, 0, e11);
    r15 -= 4; WK32(r15, 0, e10);
    r15 -= 4; WK32(r15, 0, e9);
    r15 -= 4; WK32(r15, 0, e8);
    r15 -= 4; WKF(r15, 0, ef15);
    r15 -= 4; WKF(r15, 0, ef14);
    r15 -= 4; WKF(r15, 0, ef13);
    r15 -= 4; WKF(r15, 0, ef12);
    r15 -= 4; WK32(r15, 0, 0u);         /* pr, as the emitter has it */
    r15 -= 4;

    r4 = ss_player(c, &r0, &r1, &r2, &r3, &t, &done);
    sh_load(&s, c);
    fr3 = s.f3;
    if (done) goto out;

    r0 = 40u;
    r3 = SET_COUNT;
    fr14 = RDF(r4, 32);
    fr13 = RDF(r4, 36);
    r9 = RD16S(SET_COUNT, 0);
    fr15 = RDF(r4, 40);
    r12 = SET_TABLE;
    r9 += 0xffffffffu;
    r8 = 1u;
    /* Host pointers: the entry (contiguous, so it steps with r12), the SET
     * record and the object-list entry (its base translated again only
     * when the re-read pointer changes); the reads are the body's, from the
     * same bytes. */
    const uint8_t *E = HP(SET_TABLE), *R, *L, *OB = NULL;
    uint32_t ob = 0;
    for (; t = (int32_t)r9 >= 0, t; r9 += 0xffffffffu, r12 += 16u, E += 16) {
        r0 = H16S(E, 2);
        r4 = r0;
        t = (int32_t)r4 >= 0;
        if (t) continue;
        r2 = 1u;
        t = (r4 & 1u) == 0;
        if (!t) continue;
        r11 = H32(E, 8);
        R = HP(r11);
        r1 = ~(0xffffff80u << 8);
        r3 = SET_OBJLIST;
        r2 = (H16S(R, 0) & 0xffffu) & r1;
        r10 = RD32(RD32(SET_OBJLIST, 0), 4);
        if (!OB || r10 != ob) { ob = r10; OB = HP(r10); }
        r0 = r2;
        r2 = (r2 << 2) + r0;
        r2 <<= 2;
        r10 += r2;
        L = OB + r2;
        r2 = 2u;
        r4 &= 2u;
        WK32(r15, 0, r4);
        r0 = H16S(L, 2);
        r3 = r0;
        r4 = r0;
        /* the radius, then the leaf unless the entry is always in range */
        t = (r3 & r8) == 0;
        if (t) goto no_own;
        r0 = 4u;
        fr12 = HF(L, 4);
        goto free_load;
no_own:
        r2 = 2u;
        t = (r4 & r2) == 0;
        if (t) goto rad_b;
        r0 = K_RAD_A;
        fr12 = RDF(K_RAD_A, 0);
        goto always;
rad_b:
        r0 = K_RAD_B;
        fr12 = RDF(K_RAD_B, 0);
free_load:
        r3 = 4u;
        t = (r4 & r3) == 0;
        if (t) goto test;
        r2 = FREE_LOAD;
        r1 = RD32(FREE_LOAD, 0);
        t = r1 == 0;
        if (!t) goto test;
always:
        r4 = r8;
        goto decided;
test:
        r3 = RK32(r15, 0);
        r0 = ss_leaf(&s, r3 ? HP(H32(E, 12) + 12u) : R + 8, fr14, fr13, fr15, fr12);
        fr3 = s.f3;
        r4 = r0;
decided:
        t = r4 == 0;
        if (t) continue;

        /* in range: create the task */
        r0 = RD8S(r10, 1);
        r4 = RD8S(r10, 0);
        r3 = 0x8c09852eu;
        {
            const uint32_t r5 = r0 & 0xffu, r6 = RD32(r10, 12);
            r4 &= 0xffu;
            sh_store(&s, c);
            c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4; c->r[5] = r5; c->r[6] = r6;
            c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11; c->r[12] = r12; c->r[13] = r13;
            c->r[14] = r14; c->r[15] = r15;
            c->fr[3] = fr3; c->fr[12] = fr12; c->fr[13] = fr13; c->fr[14] = fr14; c->fr[15] = fr15;
        }
        fn_8c09852e(c);
        r0 = c->r[0]; r1 = c->r[1]; r2 = c->r[2]; r3 = c->r[3]; r4 = c->r[4];
        r8 = c->r[8]; r9 = c->r[9]; r10 = c->r[10]; r11 = c->r[11]; r12 = c->r[12]; r14 = c->r[14]; r15 = c->r[15];
        fr12 = c->fr[12]; fr13 = c->fr[13]; fr14 = c->fr[14]; fr15 = c->fr[15];
        sh_load(&s, c);
        fr3 = s.f3;
        r13 = r0;
        t = r13 == 0;
        if (t) continue;
        r3 = RD8S(r12, 0) + 1u;
        WR8(r12, 0, r3);
        r0 = RD16S(r12, 2) | 1u;
        WR16(r12, 2, r0);
        WR32(r13, 28, r12);
        r14 = RD32(r13, 32);
        t = r14 == 0;
        if (!t) {
            r2 = RK32(r15, 0);
            t = r2 == 0;
            if (!t) {
                r3 = RD32(r12, 12);
                r2 = RD32(r3, 12); r1 = RD32(r3, 16);
                WR32(r14, 32, r2); WR32(r14, 36, r1);
                r2 = RD32(r3, 20); WR32(r14, 40, r2);
                r0 = RD32(r12, 12);
                r3 = RD32(r0, 0); r2 = RD32(r0, 4);
                WR32(r14, 20, r3); WR32(r14, 24, r2);
                r3 = RD32(r0, 8); WR32(r14, 28, r3);
                r1 = RD32(r12, 12);
                r3 = RD32(r1, 24); r2 = RD32(r1, 28);
                WR32(r14, 44, r3); WR32(r14, 48, r2);
                r3 = RD32(r1, 32); WR32(r14, 52, r3);
                r3 = 0x8c098b42u;
                r4 = RD32(r12, 12);
                sh_store(&s, c);
                c->r[0] = r0; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4;
                fn_8c098b42(c);
                r2 = c->r[2]; r4 = c->r[4];
                sh_load(&s, c);
                r0 = RD16S(r12, 2);
                r3 = 0xfffffffdu;
                r0 &= r3;
                WR16(r12, 2, r0);
            } else {
                r1 = r11 + 8u;
                r3 = RD32(r1, 0); r2 = RD32(r1, 4);
                WR32(r14, 32, r3); WR32(r14, 36, r2);
                r3 = RD32(r1, 8); WR32(r14, 40, r3);
                r3 = r11 + 20u;
                r0 = RD16S(r11, 2); WR32(r14, 20, r0);
                r0 = RD16S(r11, 4); WR32(r14, 24, r0);
                r0 = RD16S(r11, 6); WR32(r14, 28, r0);
                r2 = RD32(r3, 0); r1 = RD32(r3, 4);
                WR32(r14, 44, r2); WR32(r14, 48, r1);
                r2 = RD32(r3, 8); WR32(r14, 52, r2);
            }
            r0 = RD32(r10, 8);
            WR32(r14, 8, r0);
        }
        r0 = 0x0c091648u;
        fr3 = RDF(K_STORE, 0);
        r0 = 12u;
        fr12 = fr12 + fr3;
        WRF(r12, 12, fr12);
        WR32(r12, 4, r13);
    }

out:
    sh_store(&s, c);
    c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4;
    c->r[8] = e8; c->r[9] = e9; c->r[10] = e10; c->r[11] = e11; c->r[12] = e12; c->r[13] = e13; c->r[14] = e14;
    c->r[15] = sp0;
    c->fr[3] = fr3; c->fr[12] = ef12; c->fr[13] = ef13; c->fr[14] = ef14; c->fr[15] = ef15;
    c->t = t;
}

/* math = check: the decisions of a dry walk (no writes, no calls) over a
 * copy of the table taken before the recompiled body, compared with what
 * the body did: an entry it spawned that the walk saw out of range is a
 * flip; for each spawned entry the stored float (radius + K) must match
 * bit for bit.  An entry the walk saw in range that the body did not mark
 * means the task create returned 0 (counted, not a failure). */
#define SS_MAX 4096
static int ss_mode = -1;                /* 0 exact, 1 native, 2 check */
static uint8_t ss_snap[SS_MAX * 16];
static uint32_t ck_calls, ck_entries, ck_tested, ck_in, ck_spawned, ck_flip, ck_fdiff, ck_nospawn, ck_logged, ck_last;

static void ss_summary(int force) {
    const uint32_t f = hle_frame_count();
    if (!force && f / 600 == ck_last / 600) return;
    ck_last = f;
    sa1_log("check: set-scan summary f%u calls %u entries %u tested %u in-range %u spawned %u flips %u float-diffs %u no-spawn %u",
            f, ck_calls, ck_entries, ck_tested, ck_in, ck_spawned, ck_flip, ck_fdiff, ck_nospawn);
}
void ss_check_report(void) { if (ss_mode == 2) ss_summary(1); }

static void ss_check(sh4_ctx *c) {
    /* the player the prologue picks, with its helpers run on a copy */
    sh4_ctx cc = *c;
    uint32_t r0 = cc.r[0], r1, r2 = cc.r[2], r3, t;
    int done;
    const uint32_t pl = ss_player(&cc, &r0, &r1, &r2, &r3, &t, &done);
    const int32_t n = (int32_t)RD16S(SET_COUNT, 0);
    const uint32_t nn = n > 0 ? (uint32_t)(n < SS_MAX ? n : SS_MAX) : 0u;
    if (n > SS_MAX && ck_logged < 200) { ck_logged++; sa1_log("check: set-scan %d entries, checking %d", n, SS_MAX); }
    memcpy(ss_snap, HP(SET_TABLE), nn * 16u);
    fn_8c091380_orig(c);
    ck_calls++;
    if (done) return;
    const float px = RDF(pl, 32), py = RDF(pl, 36), pz = RDF(pl, 40);
    const float k = RDF(K_STORE, 0);
    ss_shadow s;
    sh_load(&s, c);
    for (uint32_t i = 0; i < nn; i++) {
        const uint8_t *e = ss_snap + i * 16u;
        const uint32_t ea = SET_TABLE + i * 16u;
        const int16_t fl = *(const int16_t *)(const void *)(e + 2);
        ck_entries++;
        if (fl >= 0 || (fl & 1)) continue;
        const uint32_t rec = *(const uint32_t *)(const void *)(e + 8);
        const uint32_t id = RD16(rec, 0) & 0x7fffu;
        const uint8_t *ol = HP(RD32(RD32(SET_OBJLIST, 0), 4) + id * 20u);
        const uint32_t ofl = H16S(ol, 2);
        int always;
        const float rad = ss_radius(ol, ofl, &always);
        int in = 1;
        if (!always) {
            ck_tested++;
            const uint8_t *p = HP((fl & 2) ? *(const uint32_t *)(const void *)(e + 12) + 12u : rec + 8u);
            in = (int)ss_leaf(&s, p, px, py, pz, rad);
        }
        const int spawned = RD8(ea, 0) != e[0];
        ck_in += (uint32_t)in;
        ck_spawned += (uint32_t)spawned;
        if (spawned && !in) {
            ck_flip++;
            if (ck_logged < 200) { ck_logged++; sa1_log("check: f%u set-scan entry %u spawned, native out of range", hle_frame_count(), i); }
        } else if (in && !spawned) {
            ck_nospawn++;
        }
        if (spawned) {
            const float st = rad + k;
            const uint32_t a = RD32(ea, 12);
            uint32_t b;
            memcpy(&b, &st, 4);
            if (a != b) {
                ck_fdiff++;
                if (ck_logged < 200) { ck_logged++; sa1_log("check: f%u set-scan entry %u float ref %08x native %08x", hle_frame_count(), i, a, b); }
            }
        }
    }
    ss_summary(0);
}

void fn_8c091380(sh4_ctx *c) {
    HLE_ENTER(0x8c091380u, "SET range-in scan fn_8c091380");
    if (ss_mode < 0) {
        ss_mode = hle_math_mode_for(0x8c091380u);
        sa1_log("SET range-in scan: math %s", ss_mode == 1 ? "native" : ss_mode == 2 ? "check" : "exact");
    }
    if (ss_mode == 1) { ss_native(c); return; }
    if (ss_mode == 2) { ss_check(c); return; }
    fn_8c091380_orig(c);
}
#else
void ss_check_report(void) {}
#endif
