/* ta_mesh.c - a native version of the model renderer's mesh-set loop
 * 0x8C0376D0 (docs/ta-direct.md, stage 2).
 *
 * r4 is the model: @(r4+12) the mesh sets (24 bytes each, a 16-bit word
 * whose low 14 bits index the materials and whose top two bits pick the
 * strip length table entry at 0x8C037980), @(r4+16) the materials (20
 * bytes: +3 the alpha byte, +12 the texture id, +16 the flags), @(r4+20)
 * the set count.  Per set the material flags, after the global override at
 * 0x8C8FFE1C (enable, AND mask, OR mask), go into the polygon header words
 * at 0x8C890104.. (0x8C037618), and to gbr+44; with bit 21 the texture
 * (0x8C037638) and, with bit 20, the material alpha into the vertex
 * colours (0x8C037872); 0x8C03767C sets the remaining header bits, one of
 * the four header writers (0x8C037460/74D4/7538/75B0) writes the header
 * into the opaque or translucent list, then a strip writer draws the set:
 * 0x8C037990 (textured) or 0x8C037C3C, or with r13 != 0 (some vertex at or
 * before the near plane) 0x8C037CCC or 0x8C038668; 0x8C0376B0 stores the
 * TA pointer it hands back into the list's descriptor.
 *
 * The small helpers and the header writers run inline here, the strip
 * writers are called directly (the native 0x8C037990 and 0x8C037CCC of
 * ta_direct.c and ta_direct_clip.c; the recompiled 0x8C037C3C and
 * 0x8C038668 through their wrappers).  Every guest memory side effect is
 * the recompiled body's (the stack slot below r15, the gbr fields, the
 * header words, the vertex colours), and every register handed to a
 * callee or back to the caller is the one the recompiled body leaves
 * there, float operations included, in their order.  The wrapper in
 * ta_stubs.c runs this only with hle_ta_direct (the native strip writers)
 * and outside a traced frame.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "ninja.h"

#define HP(a)         ((const uint8_t *)RAMP(a, 0))
#define HPW(a)        ((uint8_t *)RAMPW(a, 0))
#define H32(p, o)     (*(const sa1_u32a *)(const void *)((p) + (o)))
#define HW32(p, o, v) (*(sa1_u32a *)(void *)((p) + (o)) = (uint32_t)(v))

/* The polygon header words and list descriptors 0x8C890104..0x8C89012F. */
#define KB            0x8c890104u
#define KR(a)         H32(K, (a) - KB)
#define KW(a, v)      HW32(K, (a) - KB, v)

void fn_8c037c3c(sh4_ctx *c);
void fn_8c038668(sh4_ctx *c);

/* 0x8C037618: the material flags (r0) into the header's third word. */
static inline __attribute__((always_inline)) void mesh_618(sh4_ctx *c, uint8_t *K, uint32_t r0) {
    uint32_t r9 = r0 & 0xfc07ff00u;
    uint32_t r8 = (KR(0x8c89010cu) & 0x03f800ffu) | r9;
    KW(0x8c89010cu, r8);
    c->r[1] = KB; c->r[8] = r8; c->r[9] = r9;
}

/* 0x8C037638: texture r0 of the current texture list; a change of texture
 * sets the header's texture bits and its texture control word. */
static inline __attribute__((always_inline)) void mesh_638(sh4_ctx *c, uint8_t *K, uint32_t r0) {
    uint32_t r1 = RD32(RD32(0x8c673480u, 0), 0);
    r0 <<= 2;
    uint32_t r8 = r0 << 1;
    r0 += r8;
    r1 += r0;
    r1 = RD32(r1, 8);
    r0 = RD32(r1, 0);
    r8 = 0x8c67348cu;
    uint32_t r9 = RD32(0x8c67348cu, 0);
    uint32_t t = r0 == r9;
    if (!t) {
        WR32(0x8c67348cu, 0, r0);
        r9 = KB;
        r8 = (KR(0x8c89010cu) & 0xffffffc0u) | (RD32(r1, 8) & 0x3fu);
        KW(0x8c89010cu, r8);
        r0 = RD32(r1, 12);
        KW(0x8c890110u, r0);
    }
    c->r[0] = r0; c->r[1] = r1; c->r[8] = r8; c->r[9] = r9; c->t = t;
}

/* 0x8C03767C: the header bits in r0's masks into the first and third words. */
static inline __attribute__((always_inline)) void mesh_67c(sh4_ctx *c, uint8_t *K, uint32_t r0) {
    uint32_t r10 = r0 & 0x0200003fu, r11 = r0 & 0x001800c0u;
    uint32_t r8 = (KR(0x8c890104u) & 0xfdffff00u) | r10;
    uint32_t r9 = (KR(0x8c89010cu) & 0xffe7ff3fu) | r11;
    KW(0x8c890104u, r8);
    KW(0x8c89010cu, r9);
    c->r[1] = KB; c->r[8] = r8; c->r[9] = r9; c->r[10] = r10; c->r[11] = r11;
}

/* The header writers: list 0x8C890114 (0x8C037460, 0x8C037538: bits 20 and
 * 26-31 of the third word replaced by 0x20080000) or 0x8C890118 (0x8C0374D4,
 * 0x8C0375B0); 0x8C037538 and 0x8C0375B0 also write the fourth word. */
static inline __attribute__((always_inline)) void mesh_hdr(sh4_ctx *c, uint8_t *K, const uint32_t list,
                                                           const int four, const int op) {
    uint32_t r8 = KR(list);
    KW(0x8c89012cu, list);
    KW(0x8c890128u, r8 & 0xfc000000u);
    uint32_t q = (r8 >> 24) & 0x1cu;
    sa1_mmio_wr(0xff000038u, 4, q);
    sa1_mmio_wr(0xff00003cu, 4, q);
    r8 = (r8 & 0x03ffffffu) | 0xe0000000u;
    uint8_t *o = HPW(r8);
    if (four) HW32(o, 12, KR(0x8c890110u));
    uint32_t w = KR(0x8c89010cu);
    if (op) w = (w & 0x03efffffu) | 0x20080000u;
    HW32(o, 8, w);
    HW32(o, 4, KR(0x8c890108u));
    HW32(o, 0, KR(0x8c890104u));
    r8 += 32u;
    c->r[0] = r8; c->r[1] = KB; c->r[8] = r8;
    if (op) c->r[9] = 0x20080000u;
}

/* 0x8C037872: the material alpha (@(r5+3) / 255, or the fade at 0x8C88F668
 * by the sign of gbr+40) into the vertex colours' alpha, as a byte or, with
 * bit 25 of gbr+44, as the whole word alpha << 24 | 0xFFFFFF; skipped when
 * the first vertex already has it.  Four vertices per pass. */
static inline __attribute__((always_inline)) void mesh_872(sh4_ctx *c, const uint8_t *G, uint32_t r5) {
    const uint32_t r4 = c->r[4];
    uint32_t r0 = H32(G, 44), r1, r8, r9, t, fpul;
    const int word = (r0 & 0x02000000u) != 0;
    float fr0, fr1;
    r0 = RD8S(r5, 3) & 0xFFu;
    fpul = r0;
    fr0 = (float)(int32_t)fpul;
    fr1 = RDF(0x8c038f10u, 0);
    fr0 = fr0 * fr1;
    r0 = H32(G, 40);
    if (r0 != 0) {
        if ((int32_t)r0 > 0) fr0 = RDF(0x8c88f668u, 0);
        if (!((int32_t)r0 >= 0)) {
            fr1 = RDF(0x8c88f668u, 0);
            fr0 = fr0 + fr1;
            fr1 = 1.0f;
            if (fr0 > fr1) fr0 = fr1;
            fr1 = 0.0f;
            if (!(fr0 > fr1)) fr0 = fr1;
        }
    }
    r1 = H32(G, 60);
    fr1 = RDF(0x8c038f1cu, 0);
    fr0 = fr0 * fr1;
    fpul = FTRC(fr0);
    r0 = fpul;
    r9 = RD32(r4, 8);
    if (!word) {
        r1 += 15u;
        uint8_t *p = HPW(r1);
        r8 = (uint32_t)(int32_t)(int8_t)p[0];
        t = r0 == r8;
        if (!t) {
            do {
                p[0] = (uint8_t)r0; p[16] = (uint8_t)r0; p[32] = (uint8_t)r0; p[48] = (uint8_t)r0;
                p += 64; r1 += 64u;
                r9 += 0xfffffffcu;
                t = (int32_t)r9 > 0;
            } while (t);
        }
    } else {
        r0 <<= 24;
        r0 |= 0x00ffffffu;
        r1 += 12u;
        uint8_t *p = HPW(r1);
        r8 = H32(p, 0);
        t = r0 == r8;
        if (!t) {
            do {
                HW32(p, 0, r0); HW32(p, 16, r0); HW32(p, 32, r0); HW32(p, 48, r0);
                p += 64; r1 += 64u;
                r9 += 0xfffffffcu;
                t = (int32_t)r9 > 0;
            } while (t);
        }
    }
    c->r[0] = r0; c->r[1] = r1; c->r[8] = r8; c->r[9] = r9;
    c->fr[0] = fr0; c->fr[1] = fr1; c->t = t; c->fpul = fpul;
}

void ta_mesh_8c0376d0(sh4_ctx *c) {
    const uint32_t gbr = c->gbr;
    uint32_t r2, r3, r5, r6 = 0, r7, r13 = c->r[13], r14, r15 = c->r[15], macl = 0, r0, r9;
    uint8_t *G = HPW(gbr), *K = HPW(KB);
    const uint8_t *ovr = HP(0x8c8ffe1cu), *lens = HP(0x8c037980u);
    r15 -= 4u;
    WK32(r15, 0, 0u);                       /* sts.l pr,@-r15: the emitted pr is 0 */
    const uint32_t model = c->r[4];
    const int native = native_land_window && native_land_model_ok(model);   /* native_land.c */
    {
        const uint32_t r4 = c->r[4];
        r2 = RD32(r4, 12);
        r3 = RD32(r4, 16);
        r7 = RD16S(r4, 20);
    }
    for (;;) {
        r0 = RD16S(r2, 0) & 0x3fffu;
        macl = (20u & 0xFFFFu) * (r0 & 0xFFFFu);
        r5 = macl + r3;
        r0 = RD32(r5, 16);
        if (H32(ovr, 0) != 0) {
            r0 &= H32(ovr, 4);
            r0 |= H32(ovr, 8);
        }
        c->r[0] = r0;
        mesh_618(c, K, r0);
        HW32(G, 44, r0);
        if (!(r0 & 0x00200000u)) {
            HW32(G, 56, H32(G, 64));
            r0 = H32(G, 44);
            if (!(r0 & 0x00100000u)) {
                c->r[0] = 0x00080002u;
                mesh_67c(c, K, 0x00080002u);
                mesh_hdr(c, K, 0x8c890114u, 0, 1);           /* 0x8C037460 */
            } else {
                c->r[5] = r5;
                mesh_872(c, G, r5);
                c->r[0] = 0x02100002u;
                mesh_67c(c, K, 0x02100002u);
                mesh_hdr(c, K, 0x8c890118u, 0, 0);           /* 0x8C0374D4 */
            }
        } else {
            r0 = RD32(r5, 12) & 0x3fffffffu;
            c->r[0] = r0;
            mesh_638(c, K, r0);
            r0 = H32(G, 44);
            if (r0 & 0x00100000u) {
                c->r[5] = r5;
                mesh_872(c, G, r5);
                r0 = H32(G, 44);
                if (r0 & 0x02000000u) HW32(G, 56, 0x8c03cf60u);
                else HW32(G, 56, H32(G, 64));
                c->r[0] = 0x021000ceu;
                mesh_67c(c, K, 0x021000ceu);
                mesh_hdr(c, K, 0x8c890118u, 1, 0);           /* 0x8C0375B0 */
            } else {
                if (r0 & 0x02000000u) { HW32(G, 56, 0x8c03cf60u); r0 = 0x0008000eu; }
                else { HW32(G, 56, H32(G, 64)); r0 = 0x000800ceu; }
                c->r[0] = r0;
                mesh_67c(c, K, r0);
                mesh_hdr(c, K, 0x8c890114u, 1, 1);           /* 0x8C037538 */
            }
        }
        r14 = c->r[0];
        r9 = ((RD16S(r2, 0) & 0xFFFFu) & 0xffffc000u) >> 12;
        r6 = RD32(r5, 16);
        r0 = H32(lens, r9);
        HW32(G, 84, r0);
        uint32_t nta;
        if (native && !(H32(G, 44) & 0x00500000u) && native_land_set(c, model, (r2 - RD32(model, 12)) / 24u, r14, &nta)) {
            r0 = nta;               /* the TA pointer a strip writer would hand back */
        } else if (r13 == 0) {
            if (r6 & 0x00200000u) {
                c->r[0] = r0; c->r[2] = r2; c->r[3] = r3; c->r[6] = r6; c->r[14] = r14; c->r[15] = r15;
                ta_direct_8c037990(c);
                r0 = c->r[0]; r3 = c->r[3]; r6 = c->r[6]; r15 = c->r[15];
            } else {
                c->r[0] = r0; c->r[2] = r2; c->r[3] = r3; c->r[14] = r14; c->r[15] = r15;
                fn_8c037c3c(c);
                r0 = c->r[0]; r3 = c->r[3]; r15 = c->r[15];
            }
        } else {
            c->r[0] = r0; c->r[2] = r2; c->r[3] = r3; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7;
            c->r[14] = r14; c->r[15] = r15; c->macl = macl;
            if (r6 & 0x00200000u) {
                ta_direct_8c037ccc(c);
                r13 = c->r[13];
            } else {
                fn_8c038668(c);
            }
            r0 = c->r[0]; r2 = c->r[2]; r3 = c->r[3]; r5 = c->r[5]; r6 = c->r[6]; r7 = c->r[7];
            r15 = c->r[15]; macl = c->macl;
        }
        r14 = r0;
        c->r[14] = r14;
        {                                                    /* 0x8C0376B0 */
            uint32_t r8 = KR(0x8c890128u);
            r0 = (r14 & 0x03ffffffu) | r8;
            uint32_t r1 = KR(0x8c89012cu);
            WR32(r1, 0, r0);
            c->r[0] = r0; c->r[1] = r1; c->r[8] = r8;
        }
        r2 += 24u;
        r7 -= 1u;
        if (r7 == 0) break;
    }
    r15 += 4u;                                               /* lds.l @r15+,pr */
    c->r[2] = r2; c->r[5] = r5; c->r[6] = r6; c->r[7] = r7; c->r[15] = r15;
    c->t = 1; c->macl = macl;
}
