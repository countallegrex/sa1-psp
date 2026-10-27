/* frame_skip.c - game logic every frame, the picture only when there is time.
 *
 * At the end of each frame (kmFlip, or the game's own kmSkipFrame;
 * hle/ninja/kamui.c) hle_frame_skip_decide() sets hle_draw_skip for the next
 * one.  On a skipped frame the model pipeline returns at once (the vertex
 * transform 0x8C037294, the lighting 0x8C037350 and the mesh-set loop
 * 0x8C0376D0, hle/ninja/ta_stubs.c), kmRender neither parses the TA lists nor
 * draws, and kmFlip keeps the last picture.  Everything else runs: every
 * object's MainSub and display code (matrices, timers, motion), the other TA
 * writers (callers use the TA pointer they return), kmFlip's buffer reset and
 * the frame counter.  The pipeline writes only draw scratch that the next
 * drawn frame rewrites (the transformed vertices at 0x8C7AC8E0, the offset
 * colours at 0x8C03D760, its own stack), so the game runs exactly as without
 * the skip: the replay tapes and guest-RAM compares at the drawn frames
 * check that.  (The game has no separate display pass in normal play: its
 * DisplaySubs run inside each MainSub.)
 *
 * Modes (sa1psp.ini frame_skip = auto | off | N, or the backend's
 * --frame-skip, which wins):
 *   auto  (default) skip the picture of a turn when the game is a whole
 *         turn behind the backend's 60 Hz pacing (sa1_plat_pace_late, the
 *         pacer's own schedule and re-base, so a load forgiven by the pacer
 *         is forgiven here too), at most MAX_RUN in a row.  A turn is the
 *         game's frame interval in vblanks (TURN_VBLANKS: 2 in play).
 *         Unpaced runs never skip.
 *   off   draw every frame
 *   N     draw one frame in N (a fixed pattern, for tests)
 * Never in strict runs (tapes, the oracle) unless forced (--frame-skip
 * force:auto|force:N), which the equality tests use.  The maintainer, 2026-10-22: a
 * slow-motion game is worse than an inaccurate one; this one is not even
 * inaccurate, only fewer pictures.
 *
 * Part of sa1-psp (MIT). */
#include "hle.h"

#define MAX_RUN 3
#define TURN_VBLANKS 0x8C754EBCu   /* the game's frame interval (set by 0x8C051760) */

int hle_frame_skip = -1;          /* sa1psp.ini: -1 auto, 0 off, N >= 2 */
int hle_frame_skip_cli = -2;      /* backend option, wins: -2 unset, else as above */
int hle_frame_skip_force;         /* backend: also in strict runs (tests) */

int hle_draw_skip;                /* this frame draws nothing (set by the previous frame's end) */

static uint32_t run, n_skipped, n_drawn;

static int decide(void) {
    int mode = hle_frame_skip_cli != -2 ? hle_frame_skip_cli : hle_frame_skip;
    if (!mode || (hle_strict && !hle_frame_skip_force)) return 0;
    uint32_t f = hle_frame_count();
    int skip;
    if (mode >= 2) skip = (f % (uint32_t)mode) != 0;
    else {
        uint32_t turn = hle_rd32(TURN_VBLANKS);
        if (turn < 1 || turn > 4) turn = 2;
        /* late counts from the vblank this turn started on: the next turn
         * starts a whole turn late once it reaches two turns */
        skip = sa1_plat_pace_late(f) >= 2 * (int)turn && run < MAX_RUN;
    }
    run = skip ? run + 1 : 0;
    if (skip) n_skipped++; else n_drawn++;
    return skip;
}

void hle_frame_skip_decide(void) { hle_draw_skip = decide(); }

void hle_frame_skip_report(void) {
    int mode = hle_frame_skip_cli != -2 ? hle_frame_skip_cli : hle_frame_skip;
    sa1_log("frame skip: %s%s; %u frames drawn, %u pictures skipped",
            !mode ? "off" : mode < 0 ? "auto" : "1 in N", (hle_strict && !hle_frame_skip_force && mode) ? " (off: strict run)" : "",
            n_drawn, n_skipped);
}
