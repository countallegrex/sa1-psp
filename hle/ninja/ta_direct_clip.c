/* ta_direct_clip.c - native version of the game's near-plane strip writer
 * 0x8C037CCC and of its triangle clipper 0x8C037EE0, on the model of
 * ta_direct.c (docs/ta-direct.md): the vertices go into kamui's host strip
 * store, one marker record per strip into the TA buffer.
 *
 * 0x8C037CCC walks the same mesh as 0x8C037990 (r2: +2 strip count, +4 strip
 * list, +20 UV list; gbr+60 vertices, gbr+56 offset colours, gbr+84 a fixed
 * strip length, gbr+68 the strips left) but tests every triangle's three
 * vertices against 0 < 1/z <= 1.  Clean triangles extend the current strip;
 * the first unclean one ends it (its middle vertex goes out again with the
 * end flag) and goes to the clipper unless all three vertices are outside.
 * gbr+72/76/80 hold the offset colours of the triangle's three vertices.
 *
 * 0x8C037EE0 (the clipper) builds three 40-byte records at 0x8C8FFEC0
 * [1/z, camera y, camera x, z, y, x, colour, offset, u, v], cuts the
 * triangle at 1/z = 1 (the plane through 0x8C8FFE00/04 centre and
 * 0x8C8FFE14/18 scale), writes the result at 0x8C8FFF38 (records from +12)
 * and sends records 0, 1, (3), 2 as one strip.  It leaves r0-r7 as they
 * were (it borrows the other register bank; ta_stubs.c TA_PASS_BANKED).
 *
 * With bit 22 of r6 (environment mapping, 0x8C0382AC) the UVs come from the
 * normals at @(r4+4) through XMTRX, the vertex offsets *12 are kept at
 * 0x8C8FFFD8.., and the clipped triangles go to 0x8C038516, which is called
 * recompiled (through its wrapper): its TA records then sit between the
 * markers, in the order the recompiled writer would have put them.
 *
 * Every float operation is the recompiled body's, in its order (FTRV as
 * sh4_rt.h spells it, with the 0 fourth component), so the vertices are
 * bit-identical.  Guest memory side effects (gbr+68..80, 0x8C8FFFD8..,
 * the clipper records, the stack slots) and the registers left on return
 * are the recompiled ones, except r14/r0 (the TA pointer, 32 bytes per
 * strip instead of per vertex) and an FPUL the clipper loaded from r14.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "ninja.h"

void fn_8c038516(sh4_ctx *c);      /* the environment-map clipper (ta_stubs.c, recompiled) */

#define TA_MARKER 0xC0000000u          /* parameter type 6: reserved on the TA, never written by the game */

static void put_marker(uint32_t *r14, uint32_t first, uint32_t n) {
    WB32(*r14, 0, TA_MARKER);          /* the TA write pointer: bounded (sh4_rt.h sa1_off) */
    WB32(*r14, 4, first);
    WB32(*r14, 8, n);
    *r14 += 32;
}

/* Host pointer for a guest base (as in ta_direct.c): each array read below
 * lies in one RAM run, so indexing from one resolved base reads the bytes
 * the per-access RD32 would. */
#define HP(a)       ((const uint8_t *)RAMP(a, 0))
#define H16S(p, o)  ((uint32_t)(int32_t)*(const int16_t *)(const void *)((p) + (o)))
#define H32(p, o)   (*(const sa1_u32a *)(const void *)((p) + (o)))
#define HF(p, o)    (*(const sa1_f32a *)(const void *)((p) + (o)))
#define HW32(p, o, v) (*(sa1_u32a *)(void *)((p) + (o)) = (uint32_t)(v))
#define HWF(p, o, v)  (*(sa1_f32a *)(void *)((p) + (o)) = (v))

/* ---- 0x8C037EE0 ---------------------------------------------------------- */

#define CLIP_BASE 0x8c8ffe00u          /* scale/centre at +0x00/+0x14, records at +0xC0, output at +0x138 */

/* One byte lane at a time: a + (int16)((b - a) * k >> 16), the lanes ORed
 * together unmasked, exactly as the dmuls.l / swap.w / exts.w sequence. */
static uint32_t lerp_argb(uint32_t r4, uint32_t r5, uint32_t r14, uint32_t *mach, uint32_t *macl) {
    uint32_t out = 0;
    for (int k = 0; k < 4; k++) {
        uint32_t a = (r4 >> (8 * k)) & 0xFFu, b = (r5 >> (8 * k)) & 0xFFu, d = b - a;
        int64_t p = (int64_t)(int32_t)d * (int64_t)(int32_t)r14;
        *macl = (uint32_t)p; *mach = (uint32_t)((uint64_t)p >> 32);
        a += (uint32_t)(int32_t)(int16_t)(*macl >> 16);
        out |= a << (8 * k);
    }
    return out;
}

/* The clipper on the registers the caller left in c.  Writes c->r[8..15],
 * the floats, t, mach/macl and FPUL as the recompiled body (r0-r7 kept, as
 * the banked wrapper does).  *cp / *cfirst: where its strip went in the
 * host store (NULL when it wrote none), for the caller's own pointer. */
static void clip_ee0(sh4_ctx *c, draw_vertex **cp, uint32_t *cfirst) {
    uint32_t r8 = c->r[8], r9 = c->r[9], r10 = c->r[10], r11 = c->r[11], r12 = c->r[12], r13 = c->r[13];
    uint32_t r14 = c->r[14], r15 = c->r[15], gbr = c->gbr, fpul = c->fpul, mach = c->mach, macl = c->macl;
    const uint32_t r1 = c->r[1];
    float fr0, fr1, fr2, fr3, fr4, fr5, fr6, fr7, fr8, fr9, fr10, fr12, fr13, fr14, fr15;
    const float fr11 = c->fr[11];
    uint8_t *S = (uint8_t *)RAMPW(CLIP_BASE, 0);
    *cp = NULL;
    WR32(r15 - 4u, 0, r12);            /* mov.l r12,@-r15 */
    fr9 = 1.0f; fr10 = 1.0f;
    fr14 = HF(S, 0x14); fr9 = fr9 / fr14;
    fr15 = HF(S, 0x18); fr10 = fr10 / fr15;
    fr12 = HF(S, 0x00); fr13 = HF(S, 0x04);
    const uint8_t *uv = HP(r11);
    const uint32_t vreg[3] = { r8, r9, r10 };
    float w[3];
    for (int k = 0; k < 3; k++) {
        uint8_t *R = S + 0xC0 + 40 * k;
        uint32_t su = H16S(uv, 4 * k), sv = H16S(uv, 4 * k + 2);
        fr3 = (float)(int32_t)su;
        fpul = sv;
        fr4 = (float)(int32_t)sv;
        HWF(R, 36, fr4); HWF(R, 32, fr3);
        HW32(R, 28, RD32(gbr, 72 + 4 * k));
        const uint8_t *V = HP(vreg[k]);
        fr3 = HF(V, 0); fr4 = HF(V, 4); fr5 = HF(V, 8);
        HW32(R, 24, H32(V, 12));
        HWF(R, 20, fr3); HWF(R, 16, fr4); HWF(R, 12, fr5);
        float wk = 1.0f; wk = wk / fr5;
        fr3 = fr3 - fr12; fr3 = fr3 * wk; fr3 = fr3 * fr9; HWF(R, 8, fr3);
        fr4 = fr4 - fr13; fr4 = fr4 * wk; fr4 = fr4 * fr10; HWF(R, 4, fr4);
        HWF(R, 0, wk);
        w[k] = wk;
    }
    fr6 = w[0]; fr7 = w[1]; fr8 = w[2];
    r11 += 12;
    fr0 = 1.0f;
    if (!(fr6 > fr0) && !(fr7 > fr0) && !(fr8 > fr0)) {
        c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11; c->r[12] = r12; c->r[15] = r15;
        c->fr[0] = fr0; c->fr[3] = fr3; c->fr[4] = fr4; c->fr[5] = fr5; c->fr[6] = fr6; c->fr[7] = fr7;
        c->fr[8] = fr8; c->fr[9] = fr9; c->fr[10] = fr10; c->fr[12] = fr12; c->fr[13] = fr13;
        c->fr[14] = fr14; c->fr[15] = fr15;
        c->t = 0;
        c->fpul = fpul;
        return;
    }
    WR32(r15 - 8u, 0, r11);            /* mov.l r11,@-r15; mov.l r12 (= r1),@-r15; mov.l r13,@-r15 */
    WR32(r15 - 12u, 0, r1);
    WR32(r15 - 16u, 0, r13);
    uint32_t r6 = 0, cur = 0xC0, prev = 0x110, out = 0x138;
    const float k20 = RDF(0x8c038f20u, 0);
    for (int r7 = 3; ; ) {
        fr0 = 1.0f;
        fr1 = HF(S, prev);
        uint32_t r0 = fr0 > fr1;
        fr2 = HF(S, cur);
        uint32_t r4 = fr0 > fr2;
        if (r0 ^ r4) {
            fr0 = fr0 - fr1;
            fr2 = fr2 - fr1;
            fr0 = fr0 / fr2;
            fr1 = HF(S, prev + 4); fr2 = HF(S, cur + 4);
            fr5 = fr2; fr5 = fr5 - fr1; fr5 = fr5 * fr0; fr5 = fr5 + fr1;
            fr1 = HF(S, prev + 8); fr2 = HF(S, cur + 8);
            fr4 = fr2; fr4 = fr4 - fr1; fr4 = fr4 * fr0; fr4 = fr4 + fr1;
            fr4 = fr4 * fr14; fr4 = fr4 + fr12;
            fr5 = fr5 * fr15; fr5 = fr5 + fr13;
            fr9 = k20; fr9 = fr9 * fr0;
            uint32_t kf = FTRC(fr9);
            uint32_t col = lerp_argb(H32(S, prev + 24), H32(S, cur + 24), kf, &mach, &macl);
            fr9 = U2F(col);
            uint32_t ocl = lerp_argb(H32(S, prev + 28), H32(S, cur + 28), kf, &mach, &macl);
            fr10 = U2F(ocl);
            fpul = r14;                /* flds fr8,fpul: the saved TA pointer */
            fr1 = HF(S, prev + 32); fr2 = HF(S, cur + 32);
            fr7 = fr2; fr7 = fr7 - fr1; fr7 = fr7 * fr0; fr7 = fr7 + fr1;
            fr1 = HF(S, prev + 36); fr2 = HF(S, cur + 36);
            fr8 = fr2; fr8 = fr8 - fr1; fr8 = fr8 * fr0; fr8 = fr8 + fr1;
            HWF(S, out + 36, fr8); HWF(S, out + 32, fr7);
            HW32(S, out + 28, ocl); HW32(S, out + 24, col);
            HWF(S, out + 20, fr4); HWF(S, out + 16, fr5);
            fr6 = 1.0f; HWF(S, out + 12, fr6);
            out += 40; r6++;
        }
        fr0 = 1.0f;
        fr2 = HF(S, cur);
        if (fr2 > fr0) {
            uint32_t z = H32(S, cur + 12), y = H32(S, cur + 16), x = H32(S, cur + 20);
            uint32_t col = H32(S, cur + 24), ocl = H32(S, cur + 28), u = H32(S, cur + 32), v = H32(S, cur + 36);
            fr6 = U2F(z); fr5 = U2F(y); fr4 = U2F(x); fr9 = U2F(col); fr10 = U2F(ocl); fr7 = U2F(u); fr8 = U2F(v);
            HW32(S, out + 36, v); HW32(S, out + 32, u); HW32(S, out + 28, ocl); HW32(S, out + 24, col);
            HW32(S, out + 20, x); HW32(S, out + 16, y); HW32(S, out + 12, z);
            out += 40; r6++;
        }
        prev = cur;
        if (--r7 == 0) break;
        cur += 40;
    }
    /* the strip: records 0, 1, (3), 2 from +12 */
    static const uint16_t order3[3] = { 0x144, 0x16C, 0x194 }, order4[4] = { 0x144, 0x16C, 0x1BC, 0x194 };
    const uint16_t *ord = r6 == 3 ? order3 : order4;
    uint32_t nv = r6 == 3 ? 3 : 4, first;
    draw_vertex *o = kamui_direct_strip(nv, &first);
    for (uint32_t j = 0; j < nv; j++) {
        const uint8_t *q = S + ord[j];
        uint32_t col = H32(q, 12), ocl = H32(q, 16);
        fr2 = HF(q, 0); fr1 = HF(q, 4); fr0 = HF(q, 8); fr3 = U2F(col); fr4 = U2F(ocl);
        fr5 = HF(q, 20); fr6 = HF(q, 24);
        fr5 = fr5 * fr11; fr6 = fr6 * fr11;
        if (o) {
            draw_vertex *d = &o[j];
            d->x = fr0; d->y = fr1; d->w = fr2; d->u = fr5; d->v = fr6; d->color = col; d->ocolor = ocl;
        }
    }
    if (o) { put_marker(&r14, first, nv); *cp = o; *cfirst = first; }
    c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11; c->r[12] = r12; c->r[13] = r13;
    c->r[14] = r14; c->r[15] = r15;
    c->fr[0] = fr0; c->fr[1] = fr1; c->fr[2] = fr2; c->fr[3] = fr3; c->fr[4] = fr4; c->fr[5] = fr5;
    c->fr[6] = fr6; c->fr[7] = fr7; c->fr[8] = fr8; c->fr[9] = fr9; c->fr[10] = fr10;
    c->fr[12] = fr12; c->fr[13] = fr13; c->fr[14] = fr14; c->fr[15] = fr15;
    c->t = r6 == 3;
    c->mach = mach; c->macl = macl;
    c->fpul = fpul;
}

/* ---- 0x8C037CCC ---------------------------------------------------------- */

/* The vertex flags of one 1/z: bit 0 not > 0, bit 4 > 1 (shifted per slot). */
#define ZFLAGS(z, lo, hi) ((!((z) > 0.0f) ? (lo) : 0u) | (((z) > 1.0f) ? (hi) : 0u))

/* Common strip bookkeeping: the host region for one source strip is taken
 * when its first sub-strip opens, sized for every vertex from there on (the
 * sub-strips never revisit a vertex: an unclean triangle's flagged vertex
 * keeps the next two unclean too). */
#define STRIP_OPEN() do { \
        if (!have) { have = 1; pos = 0; cap = r12 + 2u; \
            out = cap <= 0x8001u ? kamui_direct_strip(cap, &first) : NULL; } \
        sstart = pos; } while (0)
#define STRIP_CLOSE() do { if (out) put_marker(&r14, first + sstart, pos - sstart); } while (0)
#define PUT(X, Y, W, U, V, COL, OC) do { \
        if (out && pos < cap) { draw_vertex *o_ = &out[pos]; \
            o_->x = (X); o_->y = (Y); o_->w = (W); o_->u = (U); o_->v = (V); o_->color = (COL); o_->ocolor = (OC); } \
        pos++; } while (0)

static void ccc_plain(sh4_ctx *c) {
    const uint32_t r2 = c->r[2], r3_in = c->r[3], r4_in = c->r[4], r7_in = c->r[7], r13 = c->r[13], gbr = c->gbr;
    const uint32_t r15_in = c->r[15];
    uint32_t r0, r1, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r14 = c->r[14], r15 = r15_in, fpul = c->fpul, t;
    float fr0 = 0.0f, fr1 = 0.0f, fr2 = c->fr[2], fr3 = c->fr[3], fr4 = c->fr[4], fr5 = c->fr[5], fr6 = c->fr[6];
    float fr9, fr11, fr12, fr15;
    r15 -= 20u;                        /* sts.l pr (0 here), r13, r7, r4, r3 */
    WR32(r15, 16, 0); WR32(r15, 12, r13); WR32(r15, 8, r7_in); WR32(r15, 4, r4_in); WR32(r15, 0, r3_in);
    r4 = RD32(r2, 4);
    r5 = RD32(gbr, 60);
    WR32(gbr, 68, RD16S(r2, 2));
    r1 = RD32(r2, 20);
    fr11 = RDF(0x8c038f10u, 0);
    r3 = RD32(gbr, 56);
    const uint32_t r4b = r4, r1b = r1;
    const uint8_t *s4 = HP(r4), *s1 = HP(r1), *vbuf = HP(r5), *ocol = HP(r3);
    uint32_t oc72, oc76, oc80;
#define LIST16() (H16S(s4, (int32_t)(r4 - r4b)))
#define VB(r) (vbuf + (int32_t)((r) - r5))
    /* one vertex out: [x, y, 1/z, colour] from the array, the offset colour,
     * the UV pair from the list (r1 += 4) */
#define EMIT(rv, oc) do { const uint8_t *vb_ = VB(rv); uint32_t col_ = H32(vb_, 12), oc_ = (oc); \
        fr0 = HF(vb_, 0); fr1 = HF(vb_, 4); fr2 = HF(vb_, 8); fr3 = U2F(col_); \
        fr4 = U2F(oc_); \
        const uint8_t *uv_ = s1 + (int32_t)(r1 - r1b); uint32_t su_ = H16S(uv_, 0), sv_ = H16S(uv_, 2); r1 += 4; \
        fr5 = (float)(int32_t)su_; fr5 = fr5 * fr11; \
        fpul = sv_; fr6 = (float)(int32_t)sv_; fr6 = fr6 * fr11; \
        PUT(fr0, fr1, fr2, fr5, fr6, col_, oc_); } while (0)
    do {
        draw_vertex *out = NULL;
        uint32_t first = 0, pos = 0, cap = 0, sstart = 0;
        int have = 0;
        r0 = RD32(gbr, 84);
        if (r0 != 0) r12 = r0;
        else { r12 = LIST16(); r4 += 2; if ((int32_t)r12 < 0) r12 &= 0x7FFFu; }
        r12 -= 2u;
        r11 = 0xe0000000u;
        r6 = 0;
        r0 = LIST16() << 2; r4 += 2; oc72 = H32(ocol, (int32_t)r0); r8 = (r0 << 2) + r5;
        r0 = LIST16() << 2; r4 += 2; oc76 = H32(ocol, (int32_t)r0); r9 = (r0 << 2) + r5;
        fr9 = HF(VB(r8), 8);
        fr12 = HF(VB(r9), 8);
        fr0 = 0.0f; fr1 = 1.0f;
        r7 = ZFLAGS(fr9, 0x01u, 0x10u) | ZFLAGS(fr12, 0x02u, 0x20u);
        for (;;) {
            r0 = LIST16() << 2; r4 += 2; oc80 = H32(ocol, (int32_t)r0); r10 = (r0 << 2) + r5;
            fr15 = HF(VB(r10), 8);
            fr0 = 0.0f; fr1 = 1.0f;
            r7 |= ZFLAGS(fr15, 0x04u, 0x40u);
            if (r7 == 0) {
                if (r6 == 0) { STRIP_OPEN(); EMIT(r8, oc72); r6++; }
                EMIT(r9, oc76); r6++;
            } else {
                if (r6 != 0) { EMIT(r9, oc76); r1 -= 4; STRIP_CLOSE(); r6 = 0; }
                else r1 += 4;
                if (!((r7 & 0x11u) && (r7 & 0x22u) && (r7 & 0x44u))) {
                    r11 = r1 - 4;
                    t = 1;
                    WR32(gbr, 72, oc72); WR32(gbr, 76, oc76); WR32(gbr, 80, oc80);
                    c->r[1] = r1; c->r[3] = r3; c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7;
                    c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11; c->r[12] = r12;
                    c->r[14] = r14; c->r[15] = r15;
                    c->fr[0] = fr0; c->fr[1] = fr1; c->fr[2] = fr2; c->fr[3] = fr3; c->fr[4] = fr4;
                    c->fr[5] = fr5; c->fr[6] = fr6; c->fr[9] = fr9; c->fr[11] = fr11; c->fr[12] = fr12;
                    c->fr[15] = fr15; c->t = t; c->fpul = fpul;
                    draw_vertex *cp; uint32_t cf = 0;
                    clip_ee0(c, &cp, &cf);
                    if (out && cp) out = cp - (cf - first);   /* the store may have moved */
                    r14 = c->r[14];
                    fr0 = c->fr[0]; fr1 = c->fr[1]; fr2 = c->fr[2]; fr3 = c->fr[3]; fr4 = c->fr[4];
                    fr5 = c->fr[5]; fr6 = c->fr[6]; fr9 = c->fr[9]; fr12 = c->fr[12]; fr15 = c->fr[15];
                    fpul = c->fpul;
                    r11 = 0xe0000000u;
                }
            }
            r8 = r9; r9 = r10;
            oc72 = oc76; oc76 = oc80;
            r7 = (r7 >> 1) & 0x33u;
            if (--r12 == 0) break;
        }
        if (r6 != 0) { EMIT(r10, oc80); r11 = 0xf0000000u; STRIP_CLOSE(); }
        else r1 += 8;
        r0 = RD32(gbr, 68) - 1u;
        WR32(gbr, 68, r0);
        t = r0 == 0;
    } while (!t);
#undef EMIT
    WR32(gbr, 72, oc72); WR32(gbr, 76, oc76); WR32(gbr, 80, oc80);
    c->r[0] = r14;
    c->r[1] = r1; c->r[3] = r3_in; c->r[4] = r4_in; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7_in;
    c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11; c->r[12] = r12; c->r[13] = r13;
    c->r[14] = r14; c->r[15] = r15_in;
    c->fr[0] = fr0; c->fr[1] = fr1; c->fr[2] = fr2; c->fr[3] = fr3; c->fr[4] = fr4; c->fr[5] = fr5;
    c->fr[6] = fr6; c->fr[9] = fr9; c->fr[11] = fr11; c->fr[12] = fr12; c->fr[15] = fr15;
    c->t = t;
    c->fpul = fpul;
}

static void ccc_env(sh4_ctx *c) {
    const uint32_t r2 = c->r[2], r3_in = c->r[3], r4_in = c->r[4], r7_in = c->r[7], r13_in = c->r[13], gbr = c->gbr;
    const uint32_t r15_in = c->r[15];
    uint32_t r0, r1, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14 = c->r[14], r15 = r15_in, fpul = c->fpul, t;
    float fr0 = 0.0f, fr1 = 0.0f, fr2 = c->fr[2], fr3 = c->fr[3], fr4 = c->fr[4], fr5 = c->fr[5], fr6 = c->fr[6];
    float fr9, fr11, fr12, fr15;
    float x[16];
    for (int i = 0; i < 16; i++) x[i] = c->xf[i];
    r15 -= 20u;
    WR32(r15, 16, 0); WR32(r15, 12, r13_in); WR32(r15, 8, r7_in); WR32(r15, 4, r4_in); WR32(r15, 0, r3_in);
    r5 = RD32(gbr, 60);
    r1 = RD32(r4_in, 4);
    r4 = RD32(r2, 4);
    WR32(gbr, 68, RD16S(r2, 2));
    r13 = 0x8c8fffd8u;
    fr11 = RDF(0x8c038f10u, 0);
    r3 = RD32(gbr, 56);
    const float k = RDF(0x8c038f14u, 0);
    const uint32_t r4b = r4, r1b = r1;
    const uint8_t *s4 = HP(r4), *s1 = HP(r1), *vbuf = HP(r5), *ocol = HP(r3);
    uint32_t oc72, oc76, oc80, d8, da, dc;      /* d8..dc: the 16-bit words at 0x8C8FFFD8 (sign-extended) */
    /* one vertex out: the UV from the normal at r1 + offset through XMTRX,
     * then [x, y, 1/z, colour] and the offset colour */
#define EMIT(rv, oc, noff) do { const uint8_t *nb_ = s1 + (int32_t)(r1 + (noff) - r1b); \
        fr0 = HF(nb_, 0); fr1 = HF(nb_, 4); fr2 = HF(nb_, 8); fr3 = 0.0f; \
        { float a_ = fr0, b_ = fr1, c_ = fr2, d_ = fr3; \
          fr0 = x[0] * a_ + x[4] * b_ + x[8]  * c_ + x[12] * d_; \
          fr1 = x[1] * a_ + x[5] * b_ + x[9]  * c_ + x[13] * d_; } \
        fr3 = k; fr0 = fr0 * fr3; fr0 = fr0 + fr3; fr1 = fr1 * fr3; fr1 = fr1 + fr3; \
        fr5 = fr0; fr6 = fr1; \
        const uint8_t *vb_ = VB(rv); uint32_t col_ = H32(vb_, 12), oc_ = (oc); \
        fr0 = HF(vb_, 0); fr1 = HF(vb_, 4); fr2 = HF(vb_, 8); fr3 = U2F(col_); \
        fpul = oc_; fr4 = U2F(oc_); \
        PUT(fr0, fr1, fr2, fr5, fr6, col_, oc_); } while (0)
    for (;;) {
        draw_vertex *out = NULL;
        uint32_t first = 0, pos = 0, cap = 0, sstart = 0;
        int have = 0;
        r0 = RD32(gbr, 84);
        if (r0 != 0) r12 = r0;
        else { r12 = LIST16(); r4 += 2; if ((int32_t)r12 < 0) r12 &= 0x7FFFu; }
        r12 -= 2u;
        r11 = 0xe0000000u;
        r6 = 0;
        r0 = LIST16() << 2; r4 += 2; oc72 = H32(ocol, (int32_t)r0); r8 = (r0 << 2) + r5;
        d8 = (uint32_t)(int32_t)(int16_t)(r0 * 3u);
        r0 = LIST16() << 2; r4 += 2; oc76 = H32(ocol, (int32_t)r0); r9 = (r0 << 2) + r5;
        da = (uint32_t)(int32_t)(int16_t)(r0 * 3u);
        fr9 = HF(VB(r8), 8);
        fr12 = HF(VB(r9), 8);
        fr0 = 0.0f; fr1 = 1.0f;
        r7 = ZFLAGS(fr9, 0x01u, 0x10u) | ZFLAGS(fr12, 0x02u, 0x20u);
        for (;;) {
            r0 = LIST16() << 2; r4 += 2; oc80 = H32(ocol, (int32_t)r0); r10 = (r0 << 2) + r5;
            dc = (uint32_t)(int32_t)(int16_t)(r0 * 3u);
            fr15 = HF(VB(r10), 8);
            fr0 = 0.0f; fr1 = 1.0f;
            r7 |= ZFLAGS(fr15, 0x04u, 0x40u);
            if (r7 == 0) {
                if (r6 == 0) { STRIP_OPEN(); EMIT(r8, oc72, d8); r6++; }
                EMIT(r9, oc76, da); r6++;
            } else {
                if (r6 != 0) { EMIT(r9, oc76, da); STRIP_CLOSE(); r6 = 0; }
                if (!((r7 & 0x11u) && (r7 & 0x22u) && (r7 & 0x44u))) {
                    t = 1;
                    WR32(gbr, 72, oc72); WR32(gbr, 76, oc76); WR32(gbr, 80, oc80);
                    WR16(0x8c8fffd8u, 0, d8); WR16(0x8c8fffdau, 0, da); WR16(0x8c8fffdcu, 0, dc);
                    c->r[1] = r1; c->r[3] = r3; c->r[4] = r4; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7;
                    c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11; c->r[12] = r12;
                    c->r[13] = r13; c->r[14] = r14; c->r[15] = r15;
                    c->fr[0] = fr0; c->fr[1] = fr1; c->fr[2] = fr2; c->fr[3] = fr3; c->fr[4] = fr4;
                    c->fr[5] = fr5; c->fr[6] = fr6; c->fr[9] = fr9; c->fr[11] = fr11; c->fr[12] = fr12;
                    c->fr[15] = fr15; c->t = t; c->fpul = fpul;
                    fn_8c038516(c);            /* recompiled: TA records between the markers */
                    r1 = c->r[1]; r3 = c->r[3]; r4 = c->r[4]; r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
                    r9 = c->r[9]; r10 = c->r[10]; r12 = c->r[12]; r13 = c->r[13]; r14 = c->r[14]; r15 = c->r[15];
                    fr0 = c->fr[0]; fr1 = c->fr[1]; fr2 = c->fr[2]; fr3 = c->fr[3]; fr4 = c->fr[4];
                    fr5 = c->fr[5]; fr6 = c->fr[6]; fr9 = c->fr[9]; fr12 = c->fr[12]; fr15 = c->fr[15];
                    fpul = c->fpul;
                    oc72 = RD32(gbr, 72); oc76 = RD32(gbr, 76); oc80 = RD32(gbr, 80);
                    d8 = RD16S(0x8c8fffd8u, 0); da = RD16S(0x8c8fffdau, 0); dc = RD16S(0x8c8fffdcu, 0);
                    r11 = 0xe0000000u;
                }
            }
            r8 = r9; r9 = r10;
            oc72 = oc76; oc76 = oc80;
            d8 = da; da = dc;
            r7 = (r7 >> 1) & 0x33u;
            if (--r12 == 0) break;
        }
        if (r6 != 0) { EMIT(r10, oc80, dc); r11 = 0xf0000000u; STRIP_CLOSE(); }
        r0 = RD32(gbr, 68) - 1u;
        t = (int32_t)r0 > 0;
        if (!t) break;
        WR32(gbr, 68, r0);
    }
#undef EMIT
    WR32(gbr, 72, oc72); WR32(gbr, 76, oc76); WR32(gbr, 80, oc80);
    WR16(0x8c8fffd8u, 0, d8); WR16(0x8c8fffdau, 0, da); WR16(0x8c8fffdcu, 0, dc);
    c->r[0] = r14;
    c->r[1] = r1; c->r[3] = r3_in; c->r[4] = r4_in; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7_in;
    c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11; c->r[12] = r12; c->r[13] = r13_in;
    c->r[14] = r14; c->r[15] = r15_in;
    c->fr[0] = fr0; c->fr[1] = fr1; c->fr[2] = fr2; c->fr[3] = fr3; c->fr[4] = fr4; c->fr[5] = fr5;
    c->fr[6] = fr6; c->fr[9] = fr9; c->fr[11] = fr11; c->fr[12] = fr12; c->fr[15] = fr15;
    c->t = t;
    c->fpul = fpul;
}

void ta_direct_8c037ccc(sh4_ctx *c) {
    if (c->r[6] & 0x00400000u) ccc_env(c);
    else ccc_plain(c);
}
