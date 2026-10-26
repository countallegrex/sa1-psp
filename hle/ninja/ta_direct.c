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

/* On the PSP the strip store is written front to back, a few hundred KB a
 * frame, so nearly every new 64-byte line of it missed the data cache and a
 * store to it first read the whole line from RAM (the vertex stores and the
 * marker stores held most of this function's samples on the PSP-3001,
 * 2026-10-21).  A line every byte of which this call writes is claimed with
 * cache op 0x18, Create Dirty Exclusive (no fill: the bytes are undefined
 * until written, and all of them are), as draw_gu.c's vertex pool does; the
 * marker line, of which only 12 bytes are written, is asked for early with
 * op 0x1E, Fill, while the strip's vertices are written. */
#ifdef __psp__
typedef struct { uint8_t b[64]; } dc_line;      /* the asm "writes" the line: no store to it moves above */
#define DC_CLAIM(p) __asm__ volatile("cache 0x18, 0(%1)" : "=m"(*(dc_line *)(void *)(p)) : "r"(p))
#define DC_FILL(p)  __asm__ volatile("cache 0x1e, 0(%0)" : : "r"(p))
#else
#define DC_CLAIM(p) ((void)(p))
#define DC_FILL(p)  ((void)(p))
#endif

/* Host pointer for a guest base.  The arrays read below (the strip list,
 * UV or normal list, vertex array, offset colours) and the markers written
 * each lie in one RAM or extra-area run, where sa1_off(base + i) ==
 * sa1_off(base) + i, so indexing from one resolved base reads the bytes the
 * per-access RD32 would, without the area test on every access. */
#define HP(a)       ((const uint8_t *)RAMP(a, 0))
#define H16S(p, o)  ((uint32_t)(int32_t)*(const int16_t *)(const void *)((p) + (o)))
#define H32(p, o)   (*(const sa1_u32a *)(const void *)((p) + (o)))
#define HF(p, o)    (*(const sa1_f32a *)(const void *)((p) + (o)))

/* The guest registers the vertex loop leaves behind (those of its last vertex). */
typedef struct tad_regs { float fr0, fr1, fr2, fr3, fr4, fr5, fr6; uint32_t fpul; } tad_regs;

/* One strip of n vertices into out (NULL: the store is full, nothing goes
 * out).  claim: out lies in one block reserved for the whole call, whose
 * lines from *claim up to claim_end are claimed as the stores reach them.
 * env and claim are constants at each call, so each loop is its own copy. */
static inline __attribute__((always_inline)) void tad_strip(const int env, const int claim,
        const uint8_t **s8p, const uint8_t **s1p, uint32_t n, draw_vertex *out,
        uint8_t **clp, const uint8_t *claim_end,
        const uint8_t *vbuf, const uint8_t *ocol, const float *x, float k, float fr11, tad_regs *r) {
    const uint8_t *s8 = *s8p, *s1 = *s1p;
    float fr0 = r->fr0, fr1 = r->fr1, fr2 = r->fr2, fr3 = r->fr3, fr4 = r->fr4, fr5 = r->fr5, fr6 = r->fr6;
    uint32_t fpul = r->fpul;
    uint8_t *cl = claim ? *clp : NULL;
    for (uint32_t j = 0; j < n; j++) {
        uint32_t i4 = H16S(s8, 0) << 2; s8 += 2;
        uint32_t oc = H32(ocol, i4);
        memcpy(&fr4, &oc, 4);
        if (env) {
            const uint8_t *nb = s1 + i4 * 3;
            float a = HF(nb, 0), b = HF(nb, 4), cc = HF(nb, 8), d = 0.0f;
            float u = x[0] * a + x[4] * b + x[8] * cc + x[12] * d;
            float v = x[1] * a + x[5] * b + x[9] * cc + x[13] * d;
            fr2 = x[2] * a + x[6] * b + x[10] * cc + x[14] * d;
            u = u * k; u = u + k;
            v = v * k; v = v + k;
            fr5 = u; fr6 = v;
        } else {
            uint32_t su = H16S(s1, 0), sv = H16S(s1, 2); s1 += 4;
            fr5 = (float)(int32_t)su * fr11;
            fr6 = (float)(int32_t)sv * fr11;
            fpul = sv;
        }
        const uint8_t *vb = vbuf + (i4 << 2);
        uint32_t col = H32(vb, 12);
        fr0 = HF(vb, 0); fr1 = HF(vb, 4); fr2 = HF(vb, 8); memcpy(&fr3, &col, 4);
        if (claim || out) {
            draw_vertex *o = &out[j];
            /* a 28-byte record reaches into at most one line not yet claimed */
            if (claim && (uint8_t *)(o + 1) > cl && cl < claim_end) { DC_CLAIM(cl); cl += 64; }
            o->x = fr0; o->y = fr1; o->w = fr2; o->u = fr5; o->v = fr6;
            o->color = col; o->ocolor = oc;
        }
    }
    *s8p = s8; *s1p = s1;
    if (claim) *clp = cl;
    r->fr0 = fr0; r->fr1 = fr1; r->fr2 = fr2; r->fr3 = fr3; r->fr4 = fr4; r->fr5 = fr5; r->fr6 = fr6;
    r->fpul = fpul;
}

static inline __attribute__((always_inline)) void tad_run(sh4_ctx *c, const int env) {
    uint32_t r1, r2 = c->r[2], r3 = c->r[3], r4 = c->r[4], r8, r9, r12 = 0;
    uint32_t r14 = c->r[14], r15 = c->r[15], gbr = c->gbr;
    tad_regs rg = { c->fr[0], c->fr[1], c->fr[2], c->fr[3], c->fr[4], c->fr[5], c->fr[6], c->fpul };
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
    const uint32_t fixed_n = RD32(gbr, 84);
    const uint8_t *s8 = HP(r8), *s8_0 = s8;      /* strip list */
    const uint8_t *s1 = HP(r1), *s1_0 = s1;      /* UV list, or the normals */
    const uint8_t *vbuf = HP(r9), *ocol = HP(r3a);
    uint8_t *mk = (uint8_t *)RAMPB(r14, 0);      /* the markers, when they all lie in one run (bounded: sh4_rt.h sa1_off) */
    const int mk_run = strips - 1u < 0x7FFFu && sa1_off_full(r14 + 32u * strips - 4u) == sa1_off_full(r14) + 32u * strips - 4u;

    /* One block for all the strips (one call instead of one per strip, and
     * the block's whole lines can be claimed) when that gives the strips
     * the same indices and the same markers the per-strip calls would: a
     * first strip of at least one vertex (so the store is allocated before
     * any later empty strip asks), and a strip count and lengths the loop
     * below ends on.  The lengths read here are the ones it reads.  If the
     * block cannot be had, the per-strip calls run as before (a failed grow
     * changes nothing). */
    draw_vertex *blk = NULL;
    uint32_t blk_first = 0, blk_off = 0;
    uint8_t *cl = NULL;
    const uint8_t *cl_end = NULL;
    if (strips - 1u < 0x7FFFu && fixed_n <= 0xFFFFu) {
        uint32_t total = 0, first_n;
        if (fixed_n) {
            first_n = fixed_n;
            total = fixed_n * strips;
        } else {
            const uint8_t *p = s8;
            first_n = H16S(p, 0) & 0x7FFFu;    /* the same as the loop's: negative lengths lose bit 15 */
            for (uint32_t s = 0; s < strips; s++) {
                uint32_t n = H16S(p, 0);
                if ((int32_t)n < 0) n &= 0x7FFFu;
                total += n;
                p += 2 + 2 * n;
            }
        }
        if (first_n != 0 && (blk = kamui_direct_strip(total, &blk_first)) != NULL) {
            cl = (uint8_t *)(((uintptr_t)blk + 63u) & ~(uintptr_t)63u);
            cl_end = (const uint8_t *)(((uintptr_t)(blk + total)) & ~(uintptr_t)63u);
        }
    }

    do {
        uint32_t n = fixed_n;
        if (n == 0) {
            n = H16S(s8, 0); s8 += 2;
            if ((int32_t)n < 0) n &= 0x7FFFu;
        }
        uint32_t first;
        draw_vertex *out;
        if (mk_run) DC_FILL(mk);
        if (blk) {
            out = blk + blk_off; first = blk_first + blk_off; blk_off += n;
            tad_strip(env, 1, &s8, &s1, n, out, &cl, cl_end, vbuf, ocol, x, k, fr11, &rg);
        } else {
            out = kamui_direct_strip(n, &first);
            tad_strip(env, 0, &s8, &s1, n, out, NULL, NULL, vbuf, ocol, x, k, fr11, &rg);
        }
        if (out) {
            if (mk_run) {
                *(sa1_u32a *)(void *)(mk + 0) = TA_MARKER;
                *(sa1_u32a *)(void *)(mk + 4) = first;
                *(sa1_u32a *)(void *)(mk + 8) = n;
                if (hle_strip_census) *(sa1_u32a *)(void *)(mk + 12) = census_writer(n, env);
            } else {
                WB32(r14, 0, TA_MARKER); WB32(r14, 4, first); WB32(r14, 8, n);
                if (hle_strip_census) WB32(r14, 12, census_writer(n, env));
            }
            mk += 32; r14 += 32;
        }
        strips--;
    } while (strips != 0);
    r8 += (uint32_t)(s8 - s8_0);
    r1 += (uint32_t)(s1 - s1_0);
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
    c->fr[0] = rg.fr0; c->fr[1] = rg.fr1; c->fr[2] = rg.fr2; c->fr[3] = rg.fr3;
    c->fr[4] = rg.fr4; c->fr[5] = rg.fr5; c->fr[6] = rg.fr6;
    if (!env) { c->fr[11] = fr11; c->fpul = rg.fpul; }
    c->t = 1;
}

void ta_direct_8c037990(sh4_ctx *c) {
    if (c->r[6] & 0x00400000u) tad_run(c, 1);
    else tad_run(c, 0);
}
