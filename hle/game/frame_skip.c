/* frame_skip.c - game logic every frame, the picture only when there is time.
 *
 * kmRender (hle/ninja/kamui.c) asks hle_render_skip() once a frame; on a
 * skipped frame it neither parses the frame's TA lists nor draws, and kmFlip
 * keeps the last picture.  Nothing the game reads comes from there (the
 * lists are read, never written, and kmFlip still resets the buffers), so the
 * game runs exactly as without the skip: the replay tapes and a guest-RAM
 * compare check that (tests/replay/README.md).
 *
 * Modes (sa1psp.ini frame_skip = auto | off | N, or the backend's
 * --frame-skip, which wins):
 *   auto  (default) skip the picture of a frame that starts more than a
 *         frame behind the 60 Hz schedule, at most MAX_RUN in a row; a stall
 *         of a second or more (a load) restarts the schedule
 *   off   draw every frame
 *   N     draw one frame in N (a fixed pattern, for tests)
 * Never in strict runs (tapes, the oracle) unless forced (--frame-skip
 * force:auto|force:N), which the equality tests use.  The maintainer, 2026-10-22: a
 * slow-motion game is worse than an inaccurate one; this one is not even
 * inaccurate, only fewer pictures.
 *
 * Part of sa1-psp (MIT). */
#include "hle.h"

#define FRAME_US 16667
#define MAX_RUN 3

int hle_frame_skip = -1;          /* sa1psp.ini: -1 auto, 0 off, N >= 2 */
int hle_frame_skip_cli = -2;      /* backend option, wins: -2 unset, else as above */
int hle_frame_skip_force;         /* backend: also in strict runs (tests) */

static uint64_t sched_t0;
static uint32_t sched_f0, run, n_skipped, n_drawn;
static int sched_on;

int hle_render_skip(void) {
    int mode = hle_frame_skip_cli != -2 ? hle_frame_skip_cli : hle_frame_skip;
    if (!mode || (hle_strict && !hle_frame_skip_force)) return 0;
    uint32_t f = hle_frame_count();
    int skip;
    if (mode >= 2) skip = (f % (uint32_t)mode) != 0;
    else {
        uint64_t now = sa1_plat_time_us();
        if (!sched_on || f < sched_f0) { sched_on = 1; sched_t0 = now; sched_f0 = f; }
        int64_t behind = (int64_t)(now - sched_t0) - (int64_t)(f - sched_f0) * FRAME_US;
        if (behind > 1000000 || behind < -4 * FRAME_US) { sched_t0 = now; sched_f0 = f; behind = 0; }
        skip = behind > FRAME_US && run < MAX_RUN;
    }
    run = skip ? run + 1 : 0;
    if (skip) n_skipped++; else n_drawn++;
    return skip;
}

void hle_frame_skip_report(void) {
    int mode = hle_frame_skip_cli != -2 ? hle_frame_skip_cli : hle_frame_skip;
    sa1_log("frame skip: %s%s; %u frames drawn, %u pictures skipped",
            !mode ? "off" : mode < 0 ? "auto" : "1 in N", (hle_strict && !hle_frame_skip_force && mode) ? " (off: strict run)" : "",
            n_drawn, n_skipped);
}
