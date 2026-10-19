/* crash_psp.c - where the game was, on the PSP: a guest backtrace for traps
 * and stack probes, and a watchdog for hangs.
 *
 * The desktop names recompiled functions from host symbols (fn_8c0xxxxx).
 * The EBOOT is stripped, so the backtrace scans the running coroutine's
 * stack for words that are return addresses (the instruction two before is
 * a jal or jalr) and maps each to the recompiled function that contains it
 * through the function tables (main image and the installed overlays,
 * sorted by host address at start-up).  Words that are code but not in a
 * table (HLE, libc) are printed raw: tools/psp_addr2line.sh resolves them
 * against the unstripped build-psp/sa1psp/sa1psp.elf.
 *
 * Hardware exceptions: a user-mode EBOOT cannot install an exception
 * handler on the PSP (sceKernelRegisterDefaultExceptionHandler, which
 * pspDebugInstallErrorHandler calls, is a kernel export; importing it stops
 * the EBOOT from loading).  PPSSPP reports the faulting PC and RA in its
 * log, psplink's shell prints the registers on hardware, and either maps
 * through the same addr2line script.  The watchdog thread notices a hang
 * (no guest frame for N seconds), logs the frame and flushes the log, so
 * a frozen console still leaves its last lines on the card.
 *
 * Part of sa1-psp (MIT). */
#include "platform_psp.h"
#include "overlay/overlay_loader.h"

#include <pspkernel.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uintptr_t host; uint32_t guest; } host_fn;
static host_fn *byhost;
static uint32_t nbyhost;
static uintptr_t code_lo = ~(uintptr_t)0, code_hi;

static int cmp_host(const void *a, const void *b) {
    uintptr_t x = ((const host_fn *)a)->host, y = ((const host_fn *)b)->host;
    return x < y ? -1 : x > y;
}

static void add_table(const sa1_fn_entry *t, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uintptr_t h = (uintptr_t)t[i].fn;
        byhost[nbyhost].host = h; byhost[nbyhost].guest = t[i].addr | 0x80000000u; nbyhost++;
        if (h < code_lo) code_lo = h;
        if (h > code_hi) code_hi = h;
    }
}

void plat_crash_init(void) {
    uint32_t n = sa1_fn_count;
    for (unsigned i = 0; i < hle_overlay_file_count; i++) if (hle_overlay_files[i].ov) n += hle_overlay_files[i].ov->count;
    byhost = malloc(n * sizeof *byhost);
    if (!byhost) return;
    add_table(sa1_fn_table, sa1_fn_count);
    for (unsigned i = 0; i < hle_overlay_file_count; i++)
        if (hle_overlay_files[i].ov) add_table(hle_overlay_files[i].ov->fns, hle_overlay_files[i].ov->count);
    qsort(byhost, nbyhost, sizeof *byhost, cmp_host);
    code_hi += 256u * 1024u;          /* the last function's body */
    sa1_log("crash: %u recompiled functions mapped, host code 0x%08x-0x%08x", nbyhost, (unsigned)code_lo, (unsigned)code_hi);
}

static const host_fn *lookup(uintptr_t pc) {
    if (!nbyhost || pc < byhost[0].host) return NULL;
    uint32_t lo = 0, hi = nbyhost;
    while (hi - lo > 1) { uint32_t mid = (lo + hi) / 2; if (byhost[mid].host <= pc) lo = mid; else hi = mid; }
    return &byhost[lo];
}

static int is_call_return(uintptr_t ra) {
    if ((ra & 3) || ra < 0x08804000u + 8 || ra >= 0x0C000000u) return 0;   /* user RAM (MEMSIZE=1), where the EBOOT is */
    uint32_t ins = *(const uint32_t *)(ra - 8);
    return (ins >> 26) == 3 || ((ins >> 26) == 0 && (ins & 0x3F) == 9);  /* jal, jalr */
}

void plat_backtrace(void) {
    sa1_coro *co = sa1_plat_coro_current();
    int idx = sa1_plat_coro_index(co);
    uintptr_t lo, hi, sp = (uintptr_t)__builtin_frame_address(0);
    sa1_plat_coro_stack(idx, &lo, &hi);
    if (idx < 0 || sp < lo || sp >= hi) { hi = sp + 16384; }   /* the supervisor: the main thread's stack */
    sa1_log("backtrace: guest frame %u, coroutine slot %d, sp 0x%08x (%u bytes used)", hle_frame_count(), idx, (unsigned)sp, (unsigned)(hi - sp));
    int shown = 0;
    for (uintptr_t p = sp; p + 4 <= hi && shown < 32; p += 4) {
        uintptr_t w = *(const uint32_t *)p;
        if (!is_call_return(w)) continue;
        const host_fn *f = w >= code_lo && w < code_hi ? lookup(w - 8) : NULL;
        if (f) sa1_log("  #%-2d 0x%08x  in fn_%08x + 0x%x (guest)", shown, (unsigned)w, f->guest, (unsigned)(w - f->host));
        else sa1_log("  #%-2d 0x%08x  (host)", shown, (unsigned)w);
        shown++;
    }
    plat_log_flush();
}

/* ---- watchdog ------------------------------------------------------------- */
static uint32_t (*wd_frame)(void);
static unsigned wd_seconds;

static int watchdog_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    uint32_t last = wd_frame(), still = 0;
    int reported = 0;
    for (;;) {
        sceKernelDelayThread(1000000);
        uint32_t f = wd_frame();
        if (f != last) { last = f; still = 0; reported = 0; continue; }
        if (++still >= wd_seconds && !reported) {
            reported = 1;
            sa1_log("watchdog: no guest frame for %u s (stuck in frame %u)", still, f);
            plat_log_flush();
        }
    }
    return 0;
}

void plat_watchdog_start(uint32_t (*frame)(void), unsigned seconds) {
    wd_frame = frame; wd_seconds = seconds;
    /* above the main thread (0x20): a guest loop that spins without a
     * syscall would starve a lower one */
    SceUID th = sceKernelCreateThread("sa1_watchdog", watchdog_thread, 0x18, 0x2000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}
