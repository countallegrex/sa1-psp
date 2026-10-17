/* timer.c - syTmr: the free-running timer the game stamps frames with.
 *
 * syHwInit programs TMU0 at Pphi/64 (50 MHz / 64 = 781.25 kHz, 1.28 counts
 * per microsecond) and syTmrGetCount returns 0xFFFFFFFF - TCNT0, an
 * up-counter; the SDK's count-to-microseconds conversion is count*100/128.
 * The host clock stands in.  0x8C60260E syTmrGetCount.
 *
 * Part of sa1-psp (MIT). */
#include "shinobi.h"

static uint64_t t0;

uint32_t shinobi_timer_count(void) {
    uint64_t now = sa1_plat_time_us();
    if (!t0) t0 = now;
    return (uint32_t)((now - t0) * 128u / 100u);
}

void sdk_8c60260e(sh4_ctx *c) {
    HLE_ENTER(0x8c60260eu, "syTmrGetCount");
    RET(shinobi_timer_count());
}
