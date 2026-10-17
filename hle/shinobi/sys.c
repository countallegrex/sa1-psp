/* sys.c - Shinobi system layer: the start-up checks, hardware init no-ops,
 * the frame counter and the shared Shinobi state.
 *
 * Entry addresses and roles: recomp/sa1_us_1005/sdk_entry_points.tsv and
 * docs/m1-boot-report.md.  Each body states the guest-visible effect it
 * preserves.
 *
 * Part of sa1-psp (MIT). */
#include "shinobi.h"

static uint32_t frame_count;

uint32_t hle_frame_count(void) { return frame_count; }

/* One guest frame has passed: the backend hook runs first (it may stop the
 * run), then the registered frame callbacks (hle/shinobi/chain.c). */
void shinobi_frame(sh4_ctx *c) {
    frame_count++;
    if (hle_frame_hook) hle_frame_hook(frame_count);
    shinobi_run_vblank(c);
    shinobi_run_vblank_callbacks(c);
}

void hle_shinobi_init(void) {
    shinobi_hw_init();
    shinobi_memory_init();
    shinobi_gdfs_init();
}

/* 0x8C5FF8B8 / 0x8C5FF936: library-handle checks the loader stub runs
 * before and after main (they compare the library-handle marker
 * strings that bracket the handle table, using the strncmp at 0x8C10EB1C).  r0 = 0 means
 * every handle matched. */
void sdk_8c5ff8b8(sh4_ctx *c) { HLE_ENTER(0x8c5ff8b8u, "syLibHandleCheck(start)"); RET(0); }
void sdk_8c5ff936(sh4_ctx *c) { HLE_ENTER(0x8c5ff936u, "syLibHandleCheck(end)"); RET(0); }
