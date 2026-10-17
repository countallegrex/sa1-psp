/* hw.c - the few hardware registers game code touches outside the SDK seam
 * (docs/seam.md section 3, docs/m1-boot-report.md), and the A-tail Katana
 * pieces that poll hardware and are wrapped instead of recompiled
 * (recomp/sa1_us_1005/hle_wraps.tsv).
 *
 * Nothing here emulates a device.  Each register is given the value the
 * game's own code expects so that it takes the same path as on the console:
 *  - SH-4 TMU channel 1 (0xFFD80004 TSTR, 0xFFD80014 TCOR1, 0xFFD80018
 *    TCNT1, 0xFFD8001C TCR1): the game's profiling timer (0x8C06C0C0-
 *    0x8C06C240).  TCNT1 counts down from the value written at the rate the
 *    TCR1 prescaler selects (peripheral clock 50 MHz / 4 by default);
 *    reads return that count derived from the host clock.
 *  - PDTRA (0xFF800030): bits 8-9 are the video cable pins.  3 = composite
 *    (NTSC path), 0 would be VGA.
 *  - G2 FIFO status (0xA05F688C): 0 = idle, which is what every poll waits
 *    for.
 *  - CCR (0xFF00001C), QACR0/1 (0xFF000038/3C), SB_G1 (0xA05F78BC): cache
 *    and store-queue configuration; accepted and ignored.
 *
 * Part of sa1-psp (MIT). */
#include "shinobi.h"

#include <time.h>

/* ---- TMU channel 1 ----------------------------------------------------- */
static uint32_t tmu_tcnt1_start, tmu_tcor1 = 0xFFFFFFFFu, tmu_tstr;
static uint32_t tmu_tcr1;
static uint64_t tmu_tcnt1_t0;

static uint32_t tmu_rate_hz(void) {
    /* TCR.TPSC: 0 = P/4, 1 = P/16, 2 = P/64, 3 = P/256, 4 = P/1024; P = 50 MHz. */
    static const uint32_t div[8] = {4, 16, 64, 256, 1024, 1024, 1024, 1024};
    return 50000000u / div[tmu_tcr1 & 7];
}

static uint32_t tmu_tcnt1(void) {
    if (!(tmu_tstr & 2)) return tmu_tcnt1_start;
    uint64_t el = shinobi_time_us() - tmu_tcnt1_t0;
    uint64_t ticks = el * tmu_rate_hz() / 1000000u;
    uint64_t period = (uint64_t)tmu_tcor1 + 1;
    uint64_t start = tmu_tcnt1_start;
    if (ticks <= start) return (uint32_t)(start - ticks);
    ticks -= start + 1;
    return (uint32_t)(tmu_tcor1 - (ticks % period));
}

static uint32_t hw_rd(uint32_t addr, int size, int *handled) {
    (void)size;
    *handled = 1;
    switch (addr) {
    case 0xFFD80004u: return tmu_tstr;
    case 0xFFD80014u: return tmu_tcor1;
    case 0xFFD80018u: return tmu_tcnt1();
    case 0xFFD8001Cu: return tmu_tcr1;
    case 0xFF800030u: return 0x0300u;        /* PDTRA: composite cable */
    case 0xA05F688Cu: return 0;              /* G2 FIFO idle */
    case 0xFF00001Cu: return 0;              /* CCR */
    default: *handled = 0; return 0;
    }
}

static void hw_wr(uint32_t addr, int size, uint32_t v, int *handled) {
    (void)size;
    *handled = 1;
    switch (addr) {
    case 0xFFD80004u:
        if ((v & 2) && !(tmu_tstr & 2)) tmu_tcnt1_t0 = shinobi_time_us();
        else if (!(v & 2) && (tmu_tstr & 2)) tmu_tcnt1_start = tmu_tcnt1();
        tmu_tstr = v;
        break;
    case 0xFFD80014u: tmu_tcor1 = v; break;
    case 0xFFD80018u: tmu_tcnt1_start = v; tmu_tcnt1_t0 = shinobi_time_us(); break;
    case 0xFFD8001Cu: tmu_tcr1 = v; break;
    case 0xFF00001Cu: case 0xFF000038u: case 0xFF00003Cu: case 0xA05F78BCu: break;
    default: *handled = 0; break;
    }
}

void shinobi_hw_init(void) {
    sa1_mmio_hook = hw_rd;
    sa1_mmio_wr_hook = hw_wr;
}

/* ---- real-time clock (A-tail syRtc, wrapped) ---------------------------- */
/* 0x8C10BE0C: reads the AICA RTC (0xA0710000/4) through the G2 bus until two
 * reads agree; r0 = seconds since 1950-01-01 00:00, or -1 on a bus error.
 * The host clock stands in (in strict mode, the fixed epoch plus guest
 * time); the oracle run's clock is not reproduced. */
static uint32_t rtc_seconds(void) {
    time_t now = hle_strict ? (time_t)(hle_rtc_epoch + (int64_t)(shinobi_time_us() / 1000000u)) : time(NULL);
    /* 1950-01-01 is 631152000 s before the Unix epoch (20 years, 5 leap days). */
    return (uint32_t)((int64_t)now + 631152000);
}
void fn_8c10be0c(sh4_ctx *c) {
    HLE_ENTER(0x8c10be0cu, "syRtcRead (A-tail)");
    RET(rtc_seconds());
}

/* 0x8C10BE78: the raw read under 0x8C10BE0C and under the per-frame RTC poll
 * (0x8C10BA80 -> 0x8C10BD96, on the VBlank chain): copies the two RTC
 * registers (high and low 16 bits) through the G2 helper 0x8C10D460 and
 * stores the 32-bit count at r4; r0 = 0, or -1 on a bus error.  The helper
 * suspends the AICA DMA channels first through a control block that the
 * game only sets up with a live sound driver (0x8C10D5A8), so recompiled it
 * dereferences a NULL block and its register writes, masked into guest
 * RAM, corrupted memory at random (the task pool, in the M2 exit test). */
void fn_8c10be78(sh4_ctx *c) {
    HLE_ENTER(0x8c10be78u, "syRtc raw read (A-tail)");
    hle_wr32(ARG0, rtc_seconds());
    RET(0);
}

/* 0x8C10BED4 / 0x8C10BEFC / 0x8C10BF22: the RTC writers (write enable, low
 * half, high half) through the same helper.  The host clock cannot be set
 * from here; accepted and ignored, r0 = 0.  The game only calls them from
 * its clock-setting menu. */
static void rtc_write(sh4_ctx *c, uint32_t addr, const char *name) {
    HLE_ENTER(addr, name);
    static int logged;
    if (!logged) { sa1_log("rtc: the game wrote the clock (0x%08x, value 0x%x); ignored", addr, ARG0); logged = 1; }
    RET(0);
}
void fn_8c10bed4(sh4_ctx *c) { rtc_write(c, 0x8c10bed4u, "syRtc write enable (A-tail)"); }
void fn_8c10befc(sh4_ctx *c) { rtc_write(c, 0x8c10befcu, "syRtc write low (A-tail)"); }
void fn_8c10bf22(sh4_ctx *c) { rtc_write(c, 0x8c10bf22u, "syRtc write high (A-tail)"); }
