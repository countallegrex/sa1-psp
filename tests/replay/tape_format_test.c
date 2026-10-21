/* tape_format_test.c - backend/desktop/input_tape.c without the game: a
 * synthetic run records a tape and a watch trace, a second process replays
 * the tape and checks it against a check file.  tests/replay/run_synthetic.py
 * drives it (CI).
 *
 *   tape_format_test record TITLE [input_tape options...]
 *   tape_format_test replay TITLE [input_tape options...]
 *
 * Both phases generate the same pad sequence and the same guest-memory
 * schedule (synthetic values at the addresses in tests/replay/watch.txt).
 * Replay compares every frame's pad with the sequence, with a --press-like
 * Start on frames 10-11 ORed on top.  Exit: input_tape_finish's code, or 10
 * if a replayed pad differs.
 *
 * Part of sa1-psp (MIT). */
#include "hle.h"
#include "input_tape.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the recompiled game and the HLE would provide. */
const sa1_fn_entry sa1_fn_table[] = {{0x0C010000u, NULL}};
const uint32_t sa1_fn_count = 0;
const sa1_fn_entry sa1_seam_table[] = {{0x0C5FA000u, NULL}};
const uint32_t sa1_seam_count = 0;
hle_pad hle_pad_state;
int hle_strict;
int hle_aspect_wide = 1;   /* input_tape.c writes it into a recording (kamui.c owns it in the build) */
int64_t hle_rtc_epoch;
static uint32_t frame_now;
uint32_t hle_frame_count(void) { return frame_now; }

#define FRAMES 1000u

static uint32_t lcg = 12345;
static uint32_t rnd(void) { lcg = lcg * 1103515245u + 12345u; return lcg >> 8; }

static hle_pad pad_for(uint32_t f) {
    static hle_pad cur;
    static uint32_t made;
    for (; made < f; made++)                  /* runs of identical pads, as a player makes */
        if (rnd() % 8 == 0) {
            cur.buttons = rnd() & 0xFFFu;
            cur.x = (int8_t)(rnd() & 0xFF); cur.y = (int8_t)(rnd() & 0xFF);
            cur.ltrig = (uint8_t)rnd(); cur.rtrig = (uint8_t)rnd();
        }
    return cur;
}

static void guest_state(uint32_t f) {
    uint32_t mode = f < 100 ? 1 : f < 400 ? 11 : 5;
    if (f == 98 || f == 99) mode = 8;         /* a transient tape_expect must ignore */
    hle_wr16(0x8C760970u, mode);
    hle_wr16(0x8C7493BAu, f < 400 ? 0 : 26);
    hle_wr16(0x8C7493BCu, f < 400 ? 0 : 3);
    hle_wr32(0x8C7A64E4u, f / 2);
    hle_wr16(0x8C752BE4u, (f / 50) % 7);
    hle_wr8(0x8C752BE0u, 4);
    hle_wrf(0x8C6BAD10u, (float)f * 0.5f);
    hle_wrf(0x8C6BAD14u, -(float)f);
    hle_wrf(0x8C6BAD18u, 3.25f);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: tape_format_test record|replay TITLE [options]\n"); return 2; }
    int replay = !strcmp(argv[1], "replay");
    for (int i = 3; i < argc; i++)
        if (!input_tape_arg(argc, argv, &i)) { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    sa1_ram = calloc(SA1_RAM_ALLOC, 1);
    if (!sa1_ram || input_tape_start(argv[2]) != 0) return 2;
    if (!hle_strict) { fprintf(stderr, "strict mode not set\n"); return 2; }
    uint32_t end = replay ? input_tape_frames() : FRAMES;
    int bad = 0;
    for (frame_now = 1; frame_now <= end; frame_now++) {
        hle_pad want = pad_for(frame_now);
        int press = frame_now >= 10 && frame_now < 12;
        if (replay) {
            memset(&hle_pad_state, 0, sizeof hle_pad_state);
            uint32_t b = input_tape_pad(frame_now, press ? 8u : 0u, press);
            uint32_t wb = want.buttons | (press ? 8u : 0u);
            if (b != wb || hle_pad_state.x != want.x || hle_pad_state.y != want.y ||
                hle_pad_state.ltrig != want.ltrig || hle_pad_state.rtrig != want.rtrig) {
                if (bad++ < 5) fprintf(stderr, "frame %u: replayed %x %d %d %u %u, want %x %d %d %u %u\n", frame_now, b,
                                       hle_pad_state.x, hle_pad_state.y, hle_pad_state.ltrig, hle_pad_state.rtrig,
                                       wb, want.x, want.y, want.ltrig, want.rtrig);
            }
        } else {
            hle_pad_state = want;
            hle_pad_state.buttons = input_tape_pad(frame_now, want.buttons | (press ? 8u : 0u), press);
        }
        guest_state(frame_now);
        input_tape_frame_end(frame_now);
    }
    frame_now = end;
    int code = input_tape_finish(0, NULL);
    if (replay) printf("replayed %u frames, %d mismatched\n", end, bad);
    return bad ? 10 : code;
}
