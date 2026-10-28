/* me_psp.c - the Media Engine from the game's side (see me_psp.h).
 *
 * sa1me.prx (me/sa1me.c, MODE 3) puts a boot stub at the ME's reset vector
 * that clears the ME's caches and calls a function of this image with a
 * stack and $gp of ours.  From there the ME runs the game's own code at
 * its own addresses: main RAM is the same for both processors, but each
 * has its own caches and nothing keeps them coherent, so
 *  - what the two sides exchange goes through uncached aliases (the game
 *    ORs 0x40000000 into a user address, the ME uses kseg1), or through
 *    cached memory that the writer wrote back and the reader invalidated;
 *  - code that runs on the ME makes no system calls and uses no
 *    interrupts (it has neither), only memory it was handed.
 *
 * Part of sa1-psp (MIT). */
#include "me_psp.h"
#include "hle.h"
#include "platform_psp.h"

#include <pspkernel.h>
#include <kubridge.h>
#include <string.h>

/* sa1me.c's struct me_test (the start argument). */
struct me_test {
    volatile uint32_t magic, mode, done, step;
    volatile uint32_t stub[3][2];
    volatile int32_t r_reset, r_clock, r_release, r_hold;
    volatile uint32_t readback_ok, count1, count2, ack, us1, us2;
    volatile uint32_t kc[3][16];
    volatile uint32_t fn, arg, sp, gp;
    volatile uint32_t expect;
    volatile uint32_t regs[19];
};

#define ME_STACK (32u * 1024u)
static uint8_t me_stack[ME_STACK] __attribute__((aligned(64)));
static SceUID me_mod = -1;

void me_dcache_wbinv_all(void) {
    /* sceKernelDcacheWritebackInvalidateAll's loop (its code, read through
     * sa1me.c's --me-test dump): op 0x14 twice per line over the size
     * Config gives, 2 KB << field. */
    uint32_t cfg;
    __asm__ volatile("mfc0 %0, $16" : "=r"(cfg));
    uint32_t size = 0x800u << ((cfg >> 6) & 7);
    for (uint32_t a = 0; a < size; a += 64)
        __asm__ volatile("cache 0x14, 0(%0)\n cache 0x14, 0(%0)" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
}

int me_psp_boot(const char *prx_path, void (*fn)(void *), void *arg) {
    static struct me_test t __attribute__((aligned(64)));
    if (me_mod >= 0) return -1;
    memset((void *)&t, 0, sizeof t);
    t.magic = 0x31454D53u; t.mode = 3;
    uint32_t gp;
    __asm__ volatile("move %0, $gp" : "=r"(gp));
    t.fn = (uint32_t)fn; t.arg = (uint32_t)arg; t.sp = (uint32_t)(me_stack + ME_STACK - 64); t.gp = gp;
    t.expect = *(volatile uint32_t *)fn;
    sa1_log("me: boot fn 0x%08x (first word %08x) arg 0x%08x sp 0x%08x gp 0x%08x", (unsigned)t.fn, (unsigned)t.expect,
            (unsigned)t.arg, (unsigned)t.sp, (unsigned)t.gp);
    plat_log_flush();               /* on the host before the ME runs (a wedge loses the buffer) */
    /* the ME reads what we wrote only from RAM */
    sceKernelDcacheWritebackInvalidateAll();
    struct me_test *a = &t;
    SceUID m = kuKernelLoadModule(prx_path, 0, NULL);
    int st = 0, r = m >= 0 ? sceKernelStartModule(m, sizeof a, &a, &st, NULL) : m;
    sa1_log("me: sa1me.prx load 0x%08x, start 0x%08x (status %d), step %u, boot code %u words; reset 0x%x clock 0x%x release 0x%x",
            (unsigned)m, (unsigned)r, st, (unsigned)t.step, (unsigned)t.readback_ok, (unsigned)t.r_reset,
            (unsigned)t.r_clock, (unsigned)t.r_release);
    if (m < 0) return -1;
    sa1_log("me: protection 0xBC000000-44 was %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x; bus clocks 0x%08x (main side; the ME opens its own)",
            (unsigned)t.regs[0], (unsigned)t.regs[1], (unsigned)t.regs[2], (unsigned)t.regs[3], (unsigned)t.regs[4], (unsigned)t.regs[5],
            (unsigned)t.regs[6], (unsigned)t.regs[7], (unsigned)t.regs[8], (unsigned)t.regs[9], (unsigned)t.regs[10], (unsigned)t.regs[11],
            (unsigned)t.regs[12], (unsigned)t.regs[13], (unsigned)t.regs[14], (unsigned)t.regs[15], (unsigned)t.regs[16], (unsigned)t.regs[17],
            (unsigned)t.regs[18]);
    plat_log_flush();
    if (r < 0 || t.step != 3) { sceKernelUnloadModule(m); return -1; }
    me_mod = m;
    return 0;
}

void me_psp_halt(void) {
    if (me_mod < 0) return;
    int st = 0;
    int sr = sceKernelStopModule(me_mod, 0, NULL, &st, NULL);
    int ur = sceKernelUnloadModule(me_mod);
    sa1_log("me: held in reset (stop 0x%08x, unload 0x%08x)", (unsigned)sr, (unsigned)ur);
    me_mod = -1;
}

int me_psp_exception(uint32_t *cause, uint32_t *epc, uint32_t *badvaddr) {
    if (kuKernelPeekw((void *)0xBFC00700u) != 0x45584321u) return 0;
    *cause = kuKernelPeekw((void *)0xBFC00704u);
    *epc = kuKernelPeekw((void *)0xBFC00708u);
    *badvaddr = kuKernelPeekw((void *)0xBFC0070Cu);
    return 1;
}

/* ---- --me-test 3: the probe ------------------------------------------------ */
typedef struct probe_box {          /* uncached on both sides */
    volatile uint32_t state, quit, sum, q_lo, q_hi, spins;
    volatile uint32_t n_hi, n_lo, d;
    volatile uint32_t cfg, status;
} probe_box;
static probe_box pbox __attribute__((aligned(64)));
static uint32_t probe_in[256] __attribute__((aligned(64)));
static uint32_t probe_out[256] __attribute__((aligned(64)));
static uint32_t probe_unc[16] __attribute__((aligned(64)));

/* Runs on the ME. */
static void probe_me(void *arg) {
    probe_box *b = arg;                     /* kseg1 */
    b->state = 1;
    uint32_t cfg, sr;
    __asm__ volatile("mfc0 %0, $16" : "=r"(cfg));
    __asm__ volatile("mfc0 %0, $12" : "=r"(sr));
    b->cfg = cfg; b->status = sr;
    uint32_t s = 0;
    for (int i = 0; i < 256; i++) s += probe_in[i];          /* cached reads of the game's BSS */
    b->sum = s;
    for (int i = 0; i < 128; i++) probe_out[i] = (uint32_t)i * 3u + 1u;
    memset(probe_out + 128, 0x5A, 128 * 4);                  /* newlib on the ME */
    uint64_t q = ((uint64_t)b->n_hi << 32 | b->n_lo) / b->d; /* libgcc */
    b->q_lo = (uint32_t)q; b->q_hi = (uint32_t)(q >> 32);
    me_dcache_wbinv_all();
    b->state = 2;
    volatile uint32_t *u = ME_UNCACHED_MAIN(probe_unc);     /* the game's uncached alias, from the ME */
    for (int i = 0; i < 16; i++) u[i] = 0xC0DE0000u | (uint32_t)i;
    b->state = 3;
    uint32_t n = 0;
    while (!b->quit) n++;
    b->spins = n;
    b->state = 4;
}

void me_psp_probe(const char *prx_path) {
    probe_box *b = ME_UNCACHED_MAIN(&pbox);
    uint32_t *unc = ME_UNCACHED_MAIN(probe_unc);
    uint32_t want = 0;
    for (int i = 0; i < 256; i++) { probe_in[i] = (uint32_t)i * 0x01010101u ^ 0x5A5A0000u; want += probe_in[i]; }
    memset(probe_out, 0, sizeof probe_out);
    memset((void *)&pbox, 0, sizeof pbox);
    sceKernelDcacheWritebackInvalidateAll();
    for (int i = 0; i < 16; i++) unc[i] = 0;
    b->n_hi = 0x12345678u; b->n_lo = 0x9ABCDEF0u; b->d = 1000003u;
    uint64_t wq = 0x123456789ABCDEF0ull / 1000003u;
    if (me_psp_boot(prx_path, probe_me, ME_UNCACHED_ME(&pbox)) != 0) { sa1_log("me-test 3: boot failed"); return; }
    uint64_t t0 = sa1_plat_time_us();
    while (b->state < 3 && sa1_plat_time_us() - t0 < 500000) sceKernelDelayThread(1000);
    uint32_t st = b->state;
    sa1_log("me-test 3: state %u after %u us; ME Config 0x%08x Status 0x%08x", (unsigned)st,
            (unsigned)(sa1_plat_time_us() - t0), (unsigned)b->cfg, (unsigned)b->status);
    sa1_log("me-test 3: cached read of the game's BSS: sum 0x%08x (want 0x%08x) %s", (unsigned)b->sum, (unsigned)want,
            b->sum == want ? "ok" : "WRONG");
    sa1_log("me-test 3: 64-bit divide 0x%08x%08x (want 0x%08x%08x) %s", (unsigned)b->q_hi, (unsigned)b->q_lo,
            (unsigned)(wq >> 32), (unsigned)wq, ((uint64_t)b->q_hi << 32 | b->q_lo) == wq ? "ok" : "WRONG");
    sceKernelDcacheInvalidateRange(probe_out, sizeof probe_out);
    int bad = 0;
    for (int i = 0; i < 128; i++) if (probe_out[i] != (uint32_t)i * 3u + 1u) bad++;
    for (int i = 128; i < 256; i++) if (probe_out[i] != 0x5A5A5A5Au) bad++;
    sa1_log("me-test 3: cached writes + ME writeback: %d of 256 words wrong", bad);
    int ubad = 0;
    for (int i = 0; i < 16; i++) if (unc[i] != (0xC0DE0000u | (uint32_t)i)) ubad++;
    sa1_log("me-test 3: writes through the game's uncached alias: %d of 16 wrong", ubad);
    sceKernelDelayThread(100000);
    uint32_t s1 = b->state;
    b->quit = 1;
    t0 = sa1_plat_time_us();
    while (b->state < 4 && sa1_plat_time_us() - t0 < 100000) sceKernelDelayThread(100);
    sa1_log("me-test 3: state %u -> %u at quit; %u spins of an uncached poll in ~100 ms", (unsigned)s1, (unsigned)b->state,
            (unsigned)b->spins);
    uint32_t ca, ep, bv;
    if (me_psp_exception(&ca, &ep, &bv)) sa1_log("me-test 3: ME EXCEPTION cause 0x%08x epc 0x%08x badvaddr 0x%08x", (unsigned)ca, (unsigned)ep, (unsigned)bv);
    else sa1_log("me-test 3: no ME exception; return flag %u", (unsigned)kuKernelPeekw((void *)0xBFC00610u));
    sa1_log("me-test 3: boot stage %u (2 check, 3 mismatch, 4 called); fn's first word read by the ME as given %08x, kseg1 %08x, kseg0 %08x",
            (unsigned)kuKernelPeekw((void *)0xBFC00618u), (unsigned)kuKernelPeekw((void *)0xBFC00620u),
            (unsigned)kuKernelPeekw((void *)0xBFC00624u), (unsigned)kuKernelPeekw((void *)0xBFC00628u));
    me_psp_halt();
}
