/* vsync.c - frame synchronisation.
 *
 * On the console the VBlank interrupt drives two things the game sees: the
 * KAMUI vblank-in callback (the game installs its own, 0x8C06C1D8, which
 * samples TMU1 and then calls the Ninja handler 0x8C604546 that clears the
 * "waiting" flag), and the syChain event 0x900 list, where
 * njSetVSyncFunction put the game's per-frame routine 0x8C051810 (it counts
 * frames down at 0x8C754EC0, reads the pad, runs the VMU server).  The
 * main loop spins in 0x8C051790 until that count reaches zero; the boot
 * logo loop uses njWaitVSync instead.
 *
 * Here there are no interrupts (README section 4): both spins are
 * replaced, and each turn of them is one guest frame: the backend hook
 * runs (input, frame budget), then the vblank-in callback, then the 0x900
 * handlers.
 *
 * Part of sa1-psp (MIT). */
#include "shinobi.h"

void fn_8c051790_orig(sh4_ctx *c);

#define FRAME_COUNTDOWN 0x8C754EC0u   /* frames still to wait, decremented by 0x8C051810 */
#define NJ_VSYNC_WAITING 0x8C88F7D0u  /* njWaitVSync sets, the vblank handler clears */
#define NJ_VSYNC_SEEN    0x8C88F7D8u
#define NJ_FRAME_COUNT   0x8C88F67Cu
#define NJ_VBLANK_HOOK   0x8C673478u  /* optional hook the Ninja handler calls */
#define NJ_STAMP_IN      0x8C88FE1Cu
#define NJ_STAMP_OUT     0x8C88FE20u

static uint32_t vblank_callback;      /* syChain 0x900: the game's per-frame routine */
static uint32_t vblank_in_callback, vblank_in_arg;   /* KAMUI vblank-in */

/* 0x8C605234: njSetVSyncFunction(func): registers func on event 0x900 with
 * priority 88 (0 removes it).  Returns the previous one. */
void sdk_8c605234(sh4_ctx *c) {
    HLE_ENTER(0x8c605234u, "njSetVSyncFunction");
    uint32_t prev = vblank_callback;
    if (ARG0 != vblank_callback) sa1_log("vsync: per-frame callback = 0x%08x", ARG0);
    vblank_callback = ARG0;
    RET(prev);
}

/* 0x8C64133E: KAMUI set vblank-in callback (func, arg).  The game installs
 * 0x8C06C1D8 with its TMU1 state. */
void sdk_8c64133e(sh4_ctx *c) {
    HLE_ENTER(0x8c64133eu, "kmSetVBlankInCallback");
    if (ARG0 != vblank_in_callback) sa1_log("vsync: vblank-in callback = 0x%08x", ARG0);
    vblank_in_callback = ARG0; vblank_in_arg = ARG1;
    RET(0);
}

/* 0x8C604546: the Ninja vblank-in handler: clears the waiting flag, marks a
 * vblank seen, calls the optional hook. */
void sdk_8c604546(sh4_ctx *c) {
    HLE_ENTER(0x8c604546u, "njVBlankHandler");
    hle_wr32(NJ_VSYNC_WAITING, 0);
    hle_wr32(NJ_VSYNC_SEEN, 1);
    uint32_t hook = hle_rd32(NJ_VBLANK_HOOK);
    if (hook) hle_call_guest(c, hook, 0, 0, 0, 0);
}

void shinobi_run_vblank(sh4_ctx *c) {
    sh4_ctx saved = *c;
    if (vblank_in_callback) hle_call_guest(c, vblank_in_callback, vblank_in_arg, 0, 0, 0);
    else sdk_8c604546(c);
    if (vblank_callback) hle_call_guest(c, vblank_callback, 0, 0, 0, 0);
    *c = saved;          /* interrupt context: the caller's registers are preserved */
}

/* 0x8C051790: the game's frame wait (wrapped game function).  Advance
 * frames until the per-frame routine has counted 0x8C754EC0 down to zero,
 * then let the recompiled body run: its own loop exits at once and it does
 * the per-frame bookkeeping (timer stamps, KAMUI flip, pad server). */
void fn_8c051790(sh4_ctx *c) {
    HLE_ENTER(0x8c051790u, "game frame wait (wrapped)");
    int guard = 0;
    while ((int32_t)hle_rd32(FRAME_COUNTDOWN) > 0) {
        shinobi_frame(c);
        if (!vblank_callback && ++guard > 4) {
            hle_wr32(FRAME_COUNTDOWN, 0);       /* nothing can decrement it: do not hang */
            break;
        }
    }
    fn_8c051790_orig(c);
}

/* 0x8C6050B0: njWaitVSync: wait for the next vblank, flip, run the pad
 * server (the boot/logo loop at 0x8C0109F8 uses it). */
void sdk_8c6050b0(sh4_ctx *c) {
    HLE_ENTER(0x8c6050b0u, "njWaitVSync");
    hle_wr32(NJ_STAMP_IN, shinobi_timer_count());
    hle_wr32(NJ_VSYNC_WAITING, 1);
    shinobi_frame(c);                            /* the vblank: handler clears the flag */
    hle_wr32(NJ_VSYNC_WAITING, 1);
    sdk_8c642440(c);                             /* KAMUI begin scene (stub) */
    hle_wr32(NJ_FRAME_COUNT, hle_rd32(NJ_FRAME_COUNT) + 1);
    sdk_8c641f00(c);                             /* KAMUI end scene / flip (stub) */
    sdk_8c601e2a(c);                             /* pad server */
    hle_wr32(NJ_STAMP_OUT, shinobi_timer_count());
}
