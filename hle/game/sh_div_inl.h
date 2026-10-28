/* sh_div_inl.h - the native bodies of the two SHC run-time divide helpers
 * (0x8C10CA5C signed, 0x8C10CB10 unsigned), shared by sh_div.c and the
 * desk oracle that compares them against the recompiled bodies.
 *
 * Both take the divisor in r0 and the dividend in r1 and leave the
 * quotient in r0 and r1.  The recompiled bodies run the bit-serial
 * division (one div0 step, 32 div1 steps interleaved with 33 rotcl, and
 * for the signed one a sign fix with subc/addc); here the quotient comes
 * from one host divide and the flags the chain leaves from closed forms:
 *
 *   unsigned: t = 0, m = 0, q = 1 - (Q & 1).
 *   signed:   sd = sign(n) ^ sign(d), m = sign(d), Q = |n| / |d| (in uint32,
 *             so INT_MIN / -1 is 0x80000000 and no host divide traps),
 *             negated when sd; raw = Q - sd is the value before the final
 *             carry add, t = its carry (sd && Q == 0), q = (raw & 1) ? m : !m.
 *
 * Memory and registers follow the recompiled bodies path by path: each
 * pushes r2 (and the signed one r3 too, after the zero test) below r15 and
 * reloads them from there; a zero divisor stores 0x44E to the library's
 * error word 0x8C7AC108 and leaves r0 = 0, r1 = the word's address (signed)
 * or its value (unsigned), t = 1, with q, m (and r3) untouched.  pr is
 * never written.  The oracle (private desk harness) holds both bodies to
 * the recompiled ones over the whole sh4_ctx, the two stack words and the
 * error word.
 *
 * Part of sa1-psp (MIT). */
#ifndef SA1_SH_DIV_INL_H
#define SA1_SH_DIV_INL_H

#include "sh4_rt.h"

#define SH_DIV_ERR_ADDR 0x8c7ac108u     /* the run-time library's error word */
#define SH_DIV_ERR_ZERO 0x0000044eu     /* the value a zero divisor stores */

static inline void sh_div_s32(sh4_ctx *c) {
    const uint32_t d = c->r[0], n = c->r[1];
    const uint32_t sp = c->r[15] - 4u;
    WK32(sp, 0, c->r[2]);
    if (__builtin_expect(d == 0, 0)) {
        WR32(SH_DIV_ERR_ADDR, 0, SH_DIV_ERR_ZERO);
        c->r[0] = 0;
        c->r[1] = SH_DIV_ERR_ADDR;
        c->r[2] = RK32(sp, 0);
        c->r[15] = sp + 4u;
        c->t = 1;
        return;
    }
    WK32(sp, -4, c->r[3]);
    const uint32_t sn = n >> 31, sm = d >> 31, sd = sn ^ sm;
    const uint32_t un = sn ? 0u - n : n, ud = sm ? 0u - d : d;
    uint32_t qv = un / ud;
    if (sd) qv = 0u - qv;
    const uint32_t raw = qv - sd;
    c->r[0] = qv;
    c->r[1] = qv;
    c->r[3] = RK32(sp, -4);
    c->r[2] = RK32(sp, 0);
    c->r[15] = sp + 4u;
    c->t = sd & (qv == 0);
    c->q = (raw & 1u) ? sm : sm ^ 1u;
    c->m = sm;
}

static inline void sh_div_u32(sh4_ctx *c) {
    const uint32_t d = c->r[0], n = c->r[1];
    const uint32_t sp = c->r[15] - 4u;
    WK32(sp, 0, c->r[2]);
    if (__builtin_expect(d == 0, 0)) {
        WR32(SH_DIV_ERR_ADDR, 0, SH_DIV_ERR_ZERO);
        c->r[0] = 0;
        c->r[1] = SH_DIV_ERR_ZERO;
        c->r[2] = RK32(sp, 0);
        c->r[15] = sp + 4u;
        c->t = 1;
        return;
    }
    const uint32_t qv = n / d;
    c->r[0] = qv;
    c->r[1] = qv;
    c->r[2] = RK32(sp, 0);
    c->r[15] = sp + 4u;
    c->t = 0;
    c->q = (qv & 1u) ^ 1u;
    c->m = 0;
}

#endif
