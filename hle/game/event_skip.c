/* event_skip.c - START skips a story event (README section 9, "Skip
 * cutscenes"), through the game's own end-of-event path.
 *
 * The event runner (block A, game_006): 0x8C049882 loads evNNNN.prs and
 * 0x8C0499C0 starts its thread on the coroutine 0x8C049692, which calls the
 * event's header[0](1) (the script), sets 0x8C1BDFDC, calls header[0](2)
 * (the event's end state: flags, positions) and cleans up.  The retail game
 * still holds the developers' skip, 0x8C049A12, which nothing calls: when
 * the running event (*0x8C7492AC) is in thread state 1 (0x8C049676), it
 * restarts the event's thread at 0x8C049744 (0x8C049820), the coroutine
 * without the script: end state and the same cleanup as a watched event.
 * This calls that code when START is newly pressed.
 *
 * Off in strict runs (every tape, record, replay and oracle run: the game
 * must stay the console's) and with "event_skip = off" in sa1psp.ini.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"

#define EVENT_RUNNING  0x8C7492ACu   /* the running event's record, 0 = none */
#define EVENT_ID       0x8C19C0F0u   /* the current event number */
#define CURRENT_TASK   0x8C7A0A08u
#define PAD0_PRESS     0x8C88E9B4u   /* pad record A0 +16: newly pressed buttons */
#define PAD_START      0x08u
#define EV_STATE       0x8C049676u   /* (event) -> r0 thread state */
#define EV_RESTART     0x8C049820u   /* (event, fn): restart the event thread at fn */
#define EV_SKIP_BODY   0x8C049744u   /* the event coroutine without the script */

int hle_event_skip = 1;          /* sa1psp.ini */
int hle_event_skip_cli = -1;     /* backend option, wins: 0 off, 1 on, 2 also in strict runs (tests) */
static uint32_t skipped_record, skipped_id;

/* Called once per game frame from the frame wait (hle/shinobi/vsync.c),
 * after the pad server has run and outside the event's own thread. */
void hle_event_skip_poll(sh4_ctx *c) {
    int mode = hle_event_skip_cli >= 0 ? hle_event_skip_cli : hle_event_skip;
    if (!mode || (hle_strict && mode != 2)) return;
    uint32_t ev = hle_rd32(EVENT_RUNNING);
    if (!ev) { skipped_record = 0; return; }
    if (ev == skipped_record && hle_rd32(EVENT_ID) == skipped_id) return;   /* already ending */
    if (!(hle_rd32(PAD0_PRESS) & PAD_START)) return;
    uint32_t thread = hle_rd32(ev + 4);
    if (!thread || hle_rd32(CURRENT_TASK) == thread) return;
    sh4_ctx saved = *c;
    hle_call_guest(c, EV_STATE, ev, 0, 0, 0);
    uint32_t state = c->r[0];
    if (state == 1) {
        skipped_record = ev;
        skipped_id = hle_rd32(EVENT_ID);
        sa1_log("event skip: event %u (record 0x%08x) ends at START", skipped_id, ev);
        hle_wr32(PAD0_PRESS, hle_rd32(PAD0_PRESS) & ~PAD_START);   /* nobody else sees this press */
        hle_call_guest(c, EV_RESTART, ev, EV_SKIP_BODY, 0, 0);
    }
    *c = saved;
}
