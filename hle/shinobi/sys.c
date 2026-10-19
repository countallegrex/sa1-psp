/* sys.c - Shinobi system layer: the start-up checks, hardware init no-ops,
 * the frame counter and the shared Shinobi state.
 *
 * Entry addresses and roles: recomp/sa1_us_1005/sdk_entry_points.tsv and
 * docs/m1-boot-report.md.  Each body states the guest-visible effect it
 * preserves.
 *
 * Part of sa1-psp (MIT). */
#include "shinobi.h"
#include "../sound/snd_host.h"
#include "../state.h"

static uint32_t frame_count;

uint32_t hle_frame_count(void) { return frame_count; }

/* One guest frame has passed: the backend hook runs first (it may stop the
 * run), then the registered frame callbacks (hle/shinobi/chain.c). */
#ifdef SA1_RAMDUMP
/* Debug aid (desktop, -DSA1_RAMDUMP): SA1_RAMDUMP="F1,F2,..." writes guest
 * RAM at those frames to $SA1_RAMDUMP_DIR/ram_<frame>.bin.  The dumps are
 * game state: keep them outside the repository. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void ram_dump(uint32_t frame) {
    const char *list = getenv("SA1_RAMDUMP"), *dir = getenv("SA1_RAMDUMP_DIR");
    if (!list || !dir) return;
    for (const char *p = list; *p; ) {
        char *end;
        unsigned long f = strtoul(p, &end, 0);
        if (end == p) break;
        if (f == frame) {
            char path[1024];
            snprintf(path, sizeof path, "%s/ram_%u.bin", dir, (unsigned)frame);
            FILE *o = fopen(path, "wb");
            if (o) { fwrite(sa1_ram, 1, SA1_RAM_SIZE, o); fclose(o); }
            sa1_log("ramdump: %s", path);
        }
        p = *end == ',' ? end + 1 : end;
    }
}
#endif

void shinobi_frame(sh4_ctx *c) {
    frame_count++;
#ifdef SA1_RAMDUMP
    ram_dump(frame_count);
#endif
    snd_host_frame();                  /* 735 samples of sound per guest frame (hle/sound) */
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

void shinobi_sys_state(hle_state_io *io) { hle_state_blob(io, "sys.frame", &frame_count, sizeof frame_count); }
