/* ta_xform.c - native versions of the model renderer's per-vertex transform
 * 0x8C037294 and lighting 0x8C037350 (docs/ta-direct.md, stage 2).
 *
 * 0x8C037294: @(r4+0) the positions (12 bytes each), @(r4+8) their count.
 * Each goes through XMTRX (FTRV, w = 1) and out as a 16-byte record
 * [x * sx / z + cx, y * sy / z + cy, 1/z, -] to njInit3D's vertex buffer
 * (@0x8C88F64C, also stored at gbr+60), written through its store-queue
 * alias 0xE0000000 | (addr & 0x03FFFFFF); the screen centre and scale come
 * from 0x8C8FFE00.., the near plane from 0x8C8FFE10.  r13 counts the
 * vertices with z not beyond the near plane.  Two vertices per pass, so an
 * odd count transforms and writes one record more, and one position past
 * the last pass is read; vertices after the first take w/z, not 1/z (the
 * second fldi1 is not repeated for them).
 *
 * 0x8C037350: the light direction at 0x8C038F24 goes through XMTRX's first
 * three columns (FIPR, fourth component 0); per normal (@(r4+4), 12 bytes)
 * the term (L.n) * k + k (k at 0x8C038F18), truncated and clamped to
 * 0..255, indexes the 8-byte palette at @(gbr+88): its first word goes to
 * the record's colour slot (vbuf + 12), its second (0x800 further when
 * bit 19 of @(gbr+44) is clear and @(@(gbr+52)+4) != 0) to the offset colour
 * table 0x8C03D760 (stored at gbr+64), 4 bytes per vertex.
 *
 * Every float operation is the recompiled body's, in its order (FTRV and
 * FIPR as sh4_rt.h spells them), so the results are bit-identical; guest
 * memory side effects (including the store-queue setup and the extra
 * record) and every register, FPUL and T left on return are the
 * recompiled ones.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "ninja.h"

/* Host pointers for guest bases (as in ta_direct.c): each array lies in one
 * RAM run, so indexing from one resolved base touches the bytes the
 * per-access macros would. */
#define HP(a)         ((const uint8_t *)RAMP(a, 0))
#define HPW(a)        ((uint8_t *)RAMPW(a, 0))
#define H32(p, o)     (*(const sa1_u32a *)(const void *)((p) + (o)))
#define HF(p, o)      (*(const sa1_f32a *)(const void *)((p) + (o)))
#define HW32(p, o, v) (*(sa1_u32a *)(void *)((p) + (o)) = (uint32_t)(v))
#define HWF(p, o, v)  (*(sa1_f32a *)(void *)((p) + (o)) = (v))

/* FTRV xmtrx,fvN exactly as sh4_rt.h's FTRV. */
#define XFTRV(x, v0, v1, v2, v3) do { float a_ = (v0), b_ = (v1), c_ = (v2), d_ = (v3); \
    (v0) = x[0] * a_ + x[4] * b_ + x[8]  * c_ + x[12] * d_; \
    (v1) = x[1] * a_ + x[5] * b_ + x[9]  * c_ + x[13] * d_; \
    (v2) = x[2] * a_ + x[6] * b_ + x[10] * c_ + x[14] * d_; \
    (v3) = x[3] * a_ + x[7] * b_ + x[11] * c_ + x[15] * d_; } while (0)

void ta_xform_8c037294(sh4_ctx *c) {
    const uint32_t r4 = c->r[4], gbr = c->gbr;
    uint32_t r13 = c->r[13];
    float x[16];
    for (int i = 0; i < 16; i++) x[i] = c->xf[i];
    const uint32_t r2 = RD32(r4, 0);
    uint32_t r7 = RD32(r4, 8);
    const float fr4 = RDF(0x8c8ffe00u, 0), fr5 = RDF(0x8c8ffe04u, 0), fr6 = RDF(0x8c8ffe08u, 0);
    float fr7 = RDF(0x8c8ffe0cu, 0);
    fr7 = fr7 * fr6;
    const float fr14 = RDF(0x8c8ffe10u, 0);
    const uint32_t vb = RD32(0x8c88f64cu, 0);
    WR32(gbr, 60, vb);
    const uint32_t qacr = (vb >> 24) & 0x1cu;
    sa1_mmio_wr(0xff000038u, 4, qacr);
    sa1_mmio_wr(0xff00003cu, 4, qacr);
    const uint32_t sq = (vb & 0x03ffffffu) | 0xe0000000u;
    uint8_t *out = HPW(sq);
    const uint8_t *src = HP(r2), *src0 = src;
    float fr0 = HF(src, 0), fr1 = HF(src, 4), fr2 = HF(src, 8), fr3 = 1.0f;
    float fr8, fr9, fr10, fr11;
    src += 12;
    XFTRV(x, fr0, fr1, fr2, fr3);
    fr3 = 1.0f;
    fr3 = fr3 / fr2;
    uint32_t o = 0;
    do {
        if (!(fr2 > fr14)) r13++;
        fr8 = HF(src, 0);
        fr0 = fr0 * fr6;
        fr9 = HF(src, 4);
        fr1 = fr1 * fr7;
        fr10 = HF(src, 8);
        src += 12;
        fr11 = 1.0f;
        XFTRV(x, fr8, fr9, fr10, fr11);
        fr11 = 1.0f;
        fr11 = fr11 / fr10;
        if (!(fr10 > fr14)) r13++;
        fr0 = fr0 * fr3;
        HWF(out, o + 8, fr3);
        fr1 = fr1 * fr3;
        fr0 = fr0 + fr4;
        fr1 = fr1 + fr5;
        HWF(out, o + 4, fr1);
        fr8 = fr8 * fr6;
        HWF(out, o + 0, fr0);
        fr9 = fr9 * fr7;
        fr0 = HF(src, 0);
        fr1 = HF(src, 4);
        fr3 = 1.0f;
        fr2 = HF(src, 8);
        src += 12;
        fr9 = fr9 * fr11;
        XFTRV(x, fr0, fr1, fr2, fr3);
        fr3 = fr3 / fr2;                 /* w / z: no fldi1 before this one */
        fr8 = fr8 * fr11;
        fr9 = fr9 + fr5;
        HWF(out, o + 24, fr11);
        fr8 = fr8 + fr4;
        HWF(out, o + 20, fr9);
        r7 -= 2u;
        HWF(out, o + 16, fr8);
        o += 32u;
    } while ((int32_t)r7 > 0);
    c->r[0] = 0xe0000000u;
    c->r[1] = 0x03ffffffu;
    c->r[2] = r2 + (uint32_t)(src - src0);
    c->r[3] = sq - 16u + o;
    c->r[6] = vb + o;
    c->r[7] = r7;
    c->r[13] = r13;
    c->fr[0] = fr0; c->fr[1] = fr1; c->fr[2] = fr2; c->fr[3] = fr3;
    c->fr[4] = fr4; c->fr[5] = fr5; c->fr[6] = fr6; c->fr[7] = fr7;
    c->fr[8] = fr8; c->fr[9] = fr9; c->fr[10] = fr10; c->fr[11] = fr11;
    c->fr[14] = fr14;
    c->t = 0;
}

#define FIPR4(a0, a1, a2, a3, b0, b1, b2, b3) ((a0) * (b0) + (a1) * (b1) + (a2) * (b2) + (a3) * (b3))

void ta_xform_8c037350(sh4_ctx *c) {
    const uint32_t r4 = c->r[4], gbr = c->gbr;
    uint32_t r5 = RD32(gbr, 52), r10 = RD32(gbr, 44);
    const uint32_t r2 = RD32(r4, 4);
    uint32_t r7 = RD32(r4, 8);
    const uint32_t r8 = RD32(gbr, 60) + 12u;
    WR32(gbr, 64, 0x8c03d760u);
    const uint32_t r9 = 0x8c03d760u;
    float fr12 = RDF(0x8c038f24u, 0), fr13 = RDF(0x8c038f28u, 0), fr14 = RDF(0x8c038f2cu, 0), fr15 = 0.0f;
    float fr0 = c->xf[0], fr1 = c->xf[1], fr2 = c->xf[2], fr3 = c->xf[3];
    fr3 = FIPR4(fr12, fr13, fr14, fr15, fr0, fr1, fr2, fr3);
    const float fr4 = c->xf[4], fr5 = c->xf[5], fr6 = c->xf[6];
    float fr7 = c->xf[7];
    fr7 = FIPR4(fr12, fr13, fr14, fr15, fr4, fr5, fr6, fr7);
    float fr8 = c->xf[8], fr9 = c->xf[9], fr10 = c->xf[10], fr11 = c->xf[11];
    fr11 = FIPR4(fr12, fr13, fr14, fr15, fr8, fr9, fr10, fr11);
    fr12 = fr3; fr13 = fr7; fr14 = fr11; fr15 = 0.0f;
    fr7 = RDF(0x8c038f18u, 0);
    const uint32_t r12 = 0xffu;
    uint32_t r11 = 0;
    if (!(r10 & 0x00080000u) && RD32(r5, 4) != 0) r11 = 0x800u;
    r5 = RD32(gbr, 88);
    r10 = 3;
    const uint8_t *nm = HP(r2), *nm0 = nm, *pal = HP(r5);
    uint8_t *vc = HPW(r8), *oc = HPW(r9);
    uint32_t o8 = 0, o9 = 0, r0, r3, r14, fpul;
    int t;
    r7 -= 1u;
    fr8 = HF(nm, 0); fr9 = HF(nm, 4); fr10 = HF(nm, 8); nm += 12;
    fr11 = 1.0f;
    do {
        fr11 = FIPR4(fr12, fr13, fr14, fr15, fr8, fr9, fr10, fr11);
        fr0 = HF(nm, 0);
        fr11 = fr11 * fr7;
        fr1 = HF(nm, 4);
        fr11 = fr11 + fr7;
        fr2 = HF(nm, 8);
        nm += 12;
        fpul = FTRC(fr11);
        r0 = fpul;
        fr3 = 1.0f;
        fr3 = FIPR4(fr12, fr13, fr14, fr15, fr0, fr1, fr2, fr3);
        if ((int32_t)r0 < 0) r0 = 0;
        fr3 = fr3 * fr7;
        if ((int32_t)r0 > (int32_t)r12) r0 = r12;
        fr3 = fr3 + fr7;
        fpul = FTRC(fr3);
        r14 = fpul;
        r0 <<= 3;
        if ((int32_t)r14 < 0) r14 = 0;
        r3 = H32(pal, r0);
        r0 = H32(pal, r0 + 4u + r11);
        if ((int32_t)r14 > (int32_t)r12) r14 = r12;
        HW32(vc, o8, r3);
        r14 <<= 3;
        HW32(oc, o9, r0);
        o8 += 16u;
        r3 = H32(pal, r14);
        o9 += 4u;
        r14 = H32(pal, r14 + 4u + r11);
        fr8 = HF(nm, 0);
        HW32(vc, o8, r3);
        fr9 = HF(nm, 4);
        HW32(oc, o9, r14);
        fr10 = HF(nm, 8);
        nm += 12;
        o8 += 16u;
        fr11 = 1.0f;
        r7 -= 2u;
        o9 += 4u;
    } while ((int32_t)r7 > 0);
    t = r7 == 0;
    if (t) {
        fr11 = FIPR4(fr12, fr13, fr14, fr15, fr8, fr9, fr10, fr11);
        fr11 = fr11 * fr7;
        fr11 = fr11 + fr7;
        fpul = FTRC(fr11);
        r0 = fpul;
        if ((int32_t)r0 < 0) r0 = 0;
        t = (int32_t)r0 > (int32_t)r12;
        if (t) r0 = r12;
        r0 <<= 3;
        r3 = H32(pal, r0);
        r0 = H32(pal, r0 + 4u + r11);
        HW32(vc, o8, r3);
        HW32(oc, o9, r0);
    }
    c->r[0] = r0;
    c->r[1] = 0x8c038f30u;
    c->r[2] = r2 + (uint32_t)(nm - nm0);
    c->r[3] = r3;
    c->r[5] = r5;
    c->r[7] = r7;
    c->r[8] = r8 + o8;
    c->r[9] = r9 + o9;
    c->r[10] = r10;
    c->r[11] = r11;
    c->r[12] = r12;
    c->r[14] = r14;
    c->fr[0] = fr0; c->fr[1] = fr1; c->fr[2] = fr2; c->fr[3] = fr3;
    c->fr[4] = fr4; c->fr[5] = fr5; c->fr[6] = fr6; c->fr[7] = fr7;
    c->fr[8] = fr8; c->fr[9] = fr9; c->fr[10] = fr10; c->fr[11] = fr11;
    c->fr[12] = fr12; c->fr[13] = fr13; c->fr[14] = fr14; c->fr[15] = fr15;
    c->t = (uint32_t)t;
    c->fpul = fpul;
    /* fpscr: the two fschg cancel */
}
