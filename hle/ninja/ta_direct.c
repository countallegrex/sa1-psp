/* ta_direct.c - native versions of the game's TA strip writers that put the
 * vertices straight into kamui's host strip store and leave one marker
 * record per strip in the TA buffer (docs/ta-direct.md).
 *
 * 0x8C037990: the model renderer's strip writer.  r2 is the mesh: +2 the
 * strip count, +4 the strip list (16-bit lengths, high bit flags, then
 * 16-bit vertex indices), +20 the UV list (16-bit pairs).  gbr+60 is the
 * transformed vertex array (16 bytes: x, y, 1/z, packed colour), gbr+56
 * the per-vertex offset colours, gbr+84 a fixed strip length (0: read it
 * from the list), gbr+68 the strips left.  With bit 22 of r6 set the UVs
 * come from the normals at @(r4+4) (12 bytes each) through XMTRX,
 * scaled and biased by the float at 0x8C038F14 (environment mapping).
 * Each vertex goes out as [pcw, x, y, z, u, v, colour, offset], pcw
 * 0xE0000000, or 0xF0000000 for a strip's last one.
 *
 * The native body keeps the recompiled one's memory side effects (the
 * gbr+68 counter, the saved r3 below r15) and its registers on return,
 * except r14/r0, the TA pointer it hands back: one 32-byte marker per
 * strip instead of 32 bytes per vertex.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "ninja.h"

int hle_ta_direct = 1;              /* --no-ta-direct: the recompiled writers and the TA bytes */

#define TA_MARKER 0xC0000000u          /* parameter type 6: reserved on the TA, never written by the game */

static void put_marker(uint32_t *r14, uint32_t first, uint32_t n) {
    WR32(*r14, 0, TA_MARKER);
    WR32(*r14, 4, first);
    WR32(*r14, 8, n);
    *r14 += 32;
}

void ta_direct_8c037990(sh4_ctx *c) {
    uint32_t r1, r2 = c->r[2], r3 = c->r[3], r4 = c->r[4], r6 = c->r[6], r8, r9, r12 = 0;
    uint32_t r14 = c->r[14], r15 = c->r[15], gbr = c->gbr, fpul = c->fpul;
    float fr0 = c->fr[0], fr1 = c->fr[1], fr2 = c->fr[2], fr3 = c->fr[3], fr4 = c->fr[4], fr5 = c->fr[5], fr6 = c->fr[6];
    int env = (r6 & 0x00400000u) != 0;
    WR32(r15 - 4u, 0, r3);             /* mov.l r3,@-r15 ... mov.l @r15+,r3 */
    r8 = RD32(r2, 4);
    r9 = RD32(gbr, 60);
    uint32_t strips = RD16S(r2, 2);
    WR32(gbr, 68, strips);
    uint32_t r3a = RD32(gbr, 56);
    float fr11 = c->fr[11], k = 0.0f;
    float x[16];
    if (env) {
        r1 = RD32(r4, 4);
        k = RDF(0x8c038f14u, 0);
        for (int i = 0; i < 16; i++) x[i] = c->xf[i];
    } else {
        r1 = RD32(r2, 20);
        fr11 = RDF(0x8c038f10u, 0);
    }
    do {
        uint32_t n = RD32(gbr, 84);
        if (n == 0) {
            n = RD16S(r8, 0); r8 += 2;
            if ((int32_t)n < 0) n &= 0x7FFFu;
        }
        uint32_t first;
        draw_vertex *out = kamui_direct_strip(n, &first);
        for (uint32_t j = 0; j < n; j++) {
            uint32_t i4 = RD16S(r8, 0) << 2; r8 += 2;
            fr4 = RDF(i4 + r3a, 0);
            if (env) {
                uint32_t nb = r1 + i4 * 3;
                float a = RDF(nb, 0), b = RDF(nb, 4), cc = RDF(nb, 8), d = 0.0f;
                float u = x[0] * a + x[4] * b + x[8] * cc + x[12] * d;
                float v = x[1] * a + x[5] * b + x[9] * cc + x[13] * d;
                fr2 = x[2] * a + x[6] * b + x[10] * cc + x[14] * d;
                u = u * k; u = u + k;
                v = v * k; v = v + k;
                fr5 = u; fr6 = v;
            } else {
                uint32_t su = RD16S(r1, 0), sv = RD16S(r1, 2); r1 += 4;
                fr5 = (float)(int32_t)su * fr11;
                fr6 = (float)(int32_t)sv * fr11;
                fpul = sv;
            }
            uint32_t vb = r9 + (i4 << 2);
            fr0 = RDF(vb, 0); fr1 = RDF(vb, 4); fr2 = RDF(vb, 8); fr3 = RDF(vb, 12);
            if (out) {
                draw_vertex *o = &out[j];
                o->x = fr0; o->y = fr1; o->w = fr2; o->u = fr5; o->v = fr6;
                o->color = RD32(vb, 12); o->ocolor = RD32(i4 + r3a, 0);
            }
        }
        if (out) put_marker(&r14, first, n);
        strips--;
    } while (strips != 0);
    WR32(gbr, 68, 0);
    c->r[0] = r14;
    c->r[1] = r1;
    c->r[3] = r3;
    if (env) c->r[6] = 0x8c038f14u;
    c->r[8] = r8;
    c->r[9] = r9;
    c->r[11] = 0xf0000000u;
    c->r[12] = r12;
    c->r[14] = r14;
    c->r[15] = r15;
    c->fr[0] = fr0; c->fr[1] = fr1; c->fr[2] = fr2; c->fr[3] = fr3;
    c->fr[4] = fr4; c->fr[5] = fr5; c->fr[6] = fr6;
    if (!env) { c->fr[11] = fr11; c->fpul = fpul; }
    c->t = 1;
}
