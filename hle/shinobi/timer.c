/* timer.c - syTmr: the free-running timer the game stamps frames with.
 *
 * syHwInit programs TMU0 at Pphi/64 (50 MHz / 64 = 781.25 kHz, 1.28 counts
 * per microsecond) and syTmrGetCount returns 0xFFFFFFFF - TCNT0, an
 * up-counter; the SDK's count-to-microseconds conversion is count*100/128.
 * The host clock stands in.  0x8C60260E syTmrGetCount.
 *
 * Part of sa1-psp (MIT). */
#include "shinobi.h"
#include "../state.h"

/* Guest time is virtual: 1/60 s per frame, plus the host time elapsed since
 * the frame began (capped below a frame) so that a busy-wait on the timer
 * inside one frame still sees it move.  The run is then deterministic
 * frame for frame, whatever the host speed, like the oracle's 60 Hz. */
static uint64_t frame_t0;
static uint32_t frame_seen, reads_in_frame;

/* Strict mode drops the host clock: each read inside a frame advances the
 * clock by STRICT_STEP_US instead (capped like the host time), which keeps
 * busy-waits moving and depends only on what the guest code did. */
#define STRICT_STEP_US 10u

uint64_t shinobi_time_us(void) {
    uint32_t f = hle_frame_count();
    if (hle_strict) {
        if (f != frame_seen) { frame_seen = f; reads_in_frame = 0; }
        uint64_t in_frame = (uint64_t)reads_in_frame++ * STRICT_STEP_US;
        return (uint64_t)f * 16667u + (in_frame > 16000 ? 16000 : in_frame);
    }
    uint64_t now = sa1_plat_time_us();
    if (f != frame_seen || !frame_t0) { frame_seen = f; frame_t0 = now; }
    uint64_t in_frame = now - frame_t0;
    if (in_frame > 16000) in_frame = 16000;
    return (uint64_t)f * 16667u + in_frame;
}

uint32_t shinobi_timer_count(void) {
    return (uint32_t)(shinobi_time_us() * 128u / 100u);
}

void sdk_8c60260e(sh4_ctx *c) {
    HLE_ENTER(0x8c60260eu, "syTmrGetCount");
    RET(shinobi_timer_count());
}

/* Save states: the strict-mode sub-frame clock.  frame_t0 is host time and
 * re-bases itself on the first read after a load. */
void shinobi_timer_state(hle_state_io *io) {
    hle_state_blob(io, "timer.seen", &frame_seen, sizeof frame_seen);
    hle_state_blob(io, "timer.reads", &reads_in_frame, sizeof reads_in_frame);
}
