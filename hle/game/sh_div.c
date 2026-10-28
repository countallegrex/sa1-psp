/* sh_div.c - native versions of 0x8C10CA5C and 0x8C10CB10, the SHC
 * run-time library's 32-bit divide helpers (signed and unsigned).
 *
 * Game code divides by calling these with the divisor in r0 and the
 * dividend in r1; the recompiled bodies run the console's bit-serial
 * division, about 100 guest instructions per call.  The natives
 * (sh_div_inl.h) take one host divide and leave the same registers, flags
 * and memory as the recompiled bodies on every path, so they are bit-exact
 * and the game cannot tell them apart.  Overlays reach them through the
 * same symbols, sa1_call through game_table.c.
 *
 * The mode, per routine, is hle_math_mode_exact_ok(address): native unless
 * the routine is listed in sa1psp.ini math_exact / --math-exact 8c10ca5c,
 * 8c10cb10 or --math exact is given, strict runs included.  math = check
 * (or SA1_DIV_MODE=exact|native|check on the desktop) runs the recompiled
 * body first and then the native one from the same state, compares the
 * whole sh4_ctx, the 8 bytes below r15 and the error word 0x8C7AC108,
 * logs the first differences and keeps the recompiled result.  Check mode
 * (or SA1_DIV_COUNT=1 on the desktop) also counts the calls per frame and
 * per calling function: the host return address is mapped to the guest
 * function whose recompiled code holds it (the main table and the
 * overlays loaded at the time); a call from native code (sa1_call, an HLE
 * body) shows up under the nearest recompiled function before it, so the
 * report names the host address count too.  The wrappers are compiled only
 * when the emitted game.h has the renamed bodies
 * (recomp/sa1_us_1005/hle_wraps.tsv).
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "sh_div_inl.h"

#include <stdlib.h>
#include <string.h>

void dv_check_report(void);

#if defined(SA1_WRAP_8C10CA5C) || defined(SA1_WRAP_8C10CB10)

#define DV_S 0                          /* index: signed / */
#define DV_U 1                          /* unsigned / */
static const uint32_t dv_key[2] = {0x8c10ca5cu, 0x8c10cb10u};
static const char *const dv_name[2] = {"signed", "unsigned"};

static int dv_mode[2] = {-1, -1};       /* 0 exact, 1 native, 2 check */
static int dv_count = 0;                /* per-frame and per-caller counting */
static uint32_t dv_calls[2], dv_zero[2];
static uint32_t ck_calls[2], ck_bad[2], ck_logged;

/* ---- check mode ----------------------------------------------------------- */
typedef struct { sh4_ctx c; uint32_t s4, s8, err; } dv_state;

static void dv_snap(dv_state *s, const sh4_ctx *c, uint32_t sp) {
    s->c = *c;
    s->s4 = RK32(sp, -4); s->s8 = RK32(sp, -8);
    s->err = RD32(SH_DIV_ERR_ADDR, 0);
}
static void dv_put(const dv_state *s, sh4_ctx *c, uint32_t sp) {
    WR32(SH_DIV_ERR_ADDR, 0, s->err);
    WK32(sp, -8, s->s8); WK32(sp, -4, s->s4);
    *c = s->c;
}

static void dv_check(int k, sh4_ctx *c, void (*orig)(sh4_ctx *), void (*nat)(sh4_ctx *)) {
    const uint32_t sp = c->r[15];
    dv_state in, ref, got;
    dv_snap(&in, c, sp);
    orig(c);
    dv_snap(&ref, c, sp);
    dv_put(&in, c, sp);
    nat(c);
    dv_snap(&got, c, sp);
    ck_calls[k]++;
    if (memcmp(&ref, &got, sizeof ref)) {
        ck_bad[k]++;
        if (ck_logged < 20) {
            ck_logged++;
            const uint32_t *a = (const uint32_t *)&ref, *b = (const uint32_t *)&got;
            int first = -1, nd = 0;
            for (unsigned i = 0; i < sizeof ref / 4; i++) if (a[i] != b[i]) { if (first < 0) first = (int)i; nd++; }
            sa1_log("check: f%u divide %s n %08x d %08x r15 %08x: %d words differ, first at word %d (ref %08x native %08x)",
                    hle_frame_count(), dv_name[k], in.c.r[1], in.c.r[0], sp, nd, first, a[first], b[first]);
        }
    }
    dv_put(&ref, c, sp);
}

/* ---- call counting -------------------------------------------------------- */
#define DV_SITES 1024
static struct { const void *ret; uint32_t fn; const char *ovl; uint32_t n[2]; } dv_site[DV_SITES];
static uint32_t dv_nsites, dv_site_full;
static uint32_t dv_cur_frame = UINT32_MAX, dv_cur_n, dv_frames, dv_max, dv_first = UINT32_MAX, dv_last;
static const sa1_fn_entry **dv_sorted;  /* the main table sorted by host address */

static int dv_cmp(const void *a, const void *b) {
    const uintptr_t x = (uintptr_t)(*(const sa1_fn_entry *const *)a)->fn;
    const uintptr_t y = (uintptr_t)(*(const sa1_fn_entry *const *)b)->fn;
    return x < y ? -1 : x > y;
}

/* The guest function whose host code holds `ret`: the one with the
 * greatest entry address not above it, over the main table and the
 * overlays loaded now. */
static void dv_resolve(const void *ret, uint32_t *fn, const char **ovl) {
    const uintptr_t r = (uintptr_t)ret;
    uintptr_t best = 0;
    *fn = 0; *ovl = NULL;
    if (!dv_sorted) {
        dv_sorted = malloc(sa1_fn_count * sizeof *dv_sorted);
        if (!dv_sorted) return;
        for (uint32_t i = 0; i < sa1_fn_count; i++) dv_sorted[i] = &sa1_fn_table[i];
        qsort(dv_sorted, sa1_fn_count, sizeof *dv_sorted, dv_cmp);
    }
    uint32_t lo = 0, hi = sa1_fn_count;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if ((uintptr_t)dv_sorted[mid]->fn <= r) lo = mid + 1; else hi = mid;
    }
    if (lo) { best = (uintptr_t)dv_sorted[lo - 1]->fn; *fn = dv_sorted[lo - 1]->addr; }
    for (int s = 0; s < SA1_SLOT_COUNT; s++) {
        const sa1_overlay *ov = sa1_overlay_get(s);
        if (!ov) continue;
        for (uint32_t i = 0; i < ov->count; i++) {
            const uintptr_t f = (uintptr_t)ov->fns[i].fn;
            if (f <= r && f > best) { best = f; *fn = ov->fns[i].addr; *ovl = ov->name; }
        }
    }
}

static void dv_note(int k, const void *ret) {
    const uint32_t f = hle_frame_count();
    if (f != dv_cur_frame) {
        if (dv_cur_n) { dv_frames++; if (dv_cur_n > dv_max) dv_max = dv_cur_n; }
        dv_cur_frame = f; dv_cur_n = 0;
    }
    dv_cur_n++;
    if (dv_first == UINT32_MAX) dv_first = f;
    dv_last = f;
    uint32_t i = (uint32_t)(((uintptr_t)ret >> 1) * 2654435761u) % DV_SITES;
    for (uint32_t probe = 0; probe < DV_SITES; probe++, i = (i + 1) % DV_SITES) {
        if (dv_site[i].ret == ret) { dv_site[i].n[k]++; return; }
        if (!dv_site[i].ret) {
            if (dv_nsites + 1 >= DV_SITES) break;
            dv_nsites++;
            dv_site[i].ret = ret;
            dv_resolve(ret, &dv_site[i].fn, &dv_site[i].ovl);
            dv_site[i].n[k]++;
            return;
        }
    }
    dv_site_full++;
}

static void dv_init(void) {
    for (int k = 0; k < 2; k++) dv_mode[k] = hle_math_mode_exact_ok(dv_key[k]);
#ifndef __psp__
    const char *e = getenv("SA1_DIV_MODE");
    if (e) {
        const int m = !strcmp(e, "exact") ? 0 : !strcmp(e, "check") ? 2 : 1;
        dv_mode[0] = dv_mode[1] = m;
    }
    const char *n = getenv("SA1_DIV_COUNT");
    if (n && n[0] == '1') dv_count = 1;
#endif
    if (dv_mode[0] == 2 || dv_mode[1] == 2) dv_count = 1;
    static const char *const mn[3] = {"exact", "native", "check"};
    sa1_log("SHC divide: math signed %s, unsigned %s%s", mn[dv_mode[0]], mn[dv_mode[1]], dv_count ? " (counting calls)" : "");
}

typedef struct { uint32_t fn; const char *ovl; uint32_t n[2]; } dv_row;
static int dv_row_cmp(const void *a, const void *b) {
    const dv_row *x = a, *y = b;
    const uint32_t nx = x->n[0] + x->n[1], ny = y->n[0] + y->n[1];
    return nx > ny ? -1 : nx < ny;
}

void dv_check_report(void) {
    if (dv_mode[0] < 0) return;
    sa1_log("SHC divide: calls signed %u (zero divisor %u) unsigned %u (zero divisor %u)",
            dv_calls[0], dv_zero[0], dv_calls[1], dv_zero[1]);
    if (dv_mode[0] == 2 || dv_mode[1] == 2)
        sa1_log("SHC divide summary: checked signed %u differing %u, unsigned %u differing %u",
                ck_calls[0], ck_bad[0], ck_calls[1], ck_bad[1]);
    if (!dv_count) return;
    if (dv_cur_n) { dv_frames++; if (dv_cur_n > dv_max) dv_max = dv_cur_n; dv_cur_n = 0; }
    const uint32_t tot = dv_calls[0] + dv_calls[1];
    const uint32_t span = dv_first == UINT32_MAX ? 0 : dv_last - dv_first + 1;
    sa1_log("SHC divide: frames with calls %u of %u (f%u..f%u), per frame with calls %.1f, per frame of the span %.1f, max %u",
            dv_frames, span, dv_first == UINT32_MAX ? 0 : dv_first, dv_last,
            dv_frames ? (double)tot / dv_frames : 0.0, span ? (double)tot / span : 0.0, dv_max);
    /* Fold the host return addresses into guest functions. */
    static dv_row rows[DV_SITES];
    uint32_t nr = 0;
    for (uint32_t i = 0; i < DV_SITES; i++) {
        if (!dv_site[i].ret) continue;
        uint32_t j = 0;
        while (j < nr && !(rows[j].fn == dv_site[i].fn && rows[j].ovl == dv_site[i].ovl)) j++;
        if (j == nr) { rows[nr].fn = dv_site[i].fn; rows[nr].ovl = dv_site[i].ovl; rows[nr].n[0] = rows[nr].n[1] = 0; nr++; }
        rows[j].n[0] += dv_site[i].n[0]; rows[j].n[1] += dv_site[i].n[1];
    }
    qsort(rows, nr, sizeof rows[0], dv_row_cmp);
    sa1_log("SHC divide: %u host call sites in %u calling functions%s", dv_nsites, nr,
            dv_site_full ? " (site table full, some calls not attributed)" : "");
    for (uint32_t j = 0; j < nr && j < 24; j++)
        sa1_log("SHC divide caller fn_%08x%s%s: signed %u unsigned %u (%.1f %%, %.1f per frame with calls)",
                rows[j].fn | 0x80000000u, rows[j].ovl ? " in " : "", rows[j].ovl ? rows[j].ovl : "",
                rows[j].n[0], rows[j].n[1], tot ? 100.0 * (rows[j].n[0] + rows[j].n[1]) / tot : 0.0,
                dv_frames ? (double)(rows[j].n[0] + rows[j].n[1]) / dv_frames : 0.0);
}

/* The wrappers.  __builtin_return_address is taken here, in the function
 * the game code calls, so it lands in the caller. */
#define DV_WRAP(k, NAME, ORIG, NAT)                                              \
    void ORIG(sh4_ctx *c);                                                       \
    static void NAT##_fn(sh4_ctx *c) { NAT(c); }                                 \
    void NAME(sh4_ctx *c) {                                                      \
        HLE_ENTER(dv_key[k], "SHC divide " #NAME);                               \
        if (__builtin_expect(dv_mode[k] < 0, 0)) dv_init();                      \
        dv_calls[k]++;                                                           \
        dv_zero[k] += c->r[0] == 0;                                              \
        if (__builtin_expect(dv_count, 0)) dv_note(k, __builtin_return_address(0)); \
        if (__builtin_expect(dv_mode[k] == 1, 1)) { NAT(c); return; }            \
        if (dv_mode[k] == 0) { ORIG(c); return; }                                \
        dv_check(k, c, ORIG, NAT##_fn);                                          \
    }

#ifdef SA1_WRAP_8C10CA5C
DV_WRAP(DV_S, fn_8c10ca5c, fn_8c10ca5c_orig, sh_div_s32)
#endif
#ifdef SA1_WRAP_8C10CB10
DV_WRAP(DV_U, fn_8c10cb10, fn_8c10cb10_orig, sh_div_u32)
#endif

#else
void dv_check_report(void) {}
#endif
