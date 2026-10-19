/* hle_init.c - shared HLE plumbing: guest string copy, callback entry, and
 * the one-time initialisation that wires the BIOS area and the libraries.
 *
 * Part of sa1-psp (MIT). */
#include "hle.h"
#include "state.h"

void hle_bios_init(void);     /* hle/bios/syscalls.c */
void hle_shinobi_init(void);  /* hle/shinobi/sys.c */
void hle_movie_init(void);    /* hle/overlay/movie.c */
void hle_settings_load(void); /* hle/settings.c */
#include "sound/snd_host.h"

void (*hle_frame_hook)(uint32_t frame);
int hle_strict;
int64_t hle_rtc_epoch = 946684800;   /* 2000-01-01 00:00 UTC */

void hle_guest_string(uint32_t addr, char *buf, unsigned n) {
    unsigned i = 0;
    for (; i + 1 < n; i++) {
        char ch = (char)RD8(addr, i);
        if (ch == 0) break;
        buf[i] = ch;
    }
    buf[i] = 0;
}

void hle_call_guest(sh4_ctx *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    /* Callbacks run on the current guest stack: the caller's frame is live,
     * and SH code called from a leaf keeps r15 as it found it. */
    c->r[4] = a0; c->r[5] = a1; c->r[6] = a2; c->r[7] = a3;
    sa1_call(c, fn);
}

void hle_init(void) {
    hle_bios_init();
    hle_shinobi_init();
    hle_movie_init();
    hle_settings_load();
    if (snd_voice_lang_cli >= -1) snd_voice_lang = snd_voice_lang_cli;
    snd_host_init();
}

/* Save states: every module's state, in one fixed order (the inventory in
 * docs/savestate.md section 2). */
void hle_state_all(hle_state_io *io) {
    hle_task_state(io);
    shinobi_sys_state(io);
    shinobi_timer_state(io);
    shinobi_hw_state(io);
    shinobi_vsync_state(io);
    shinobi_pad_state(io);
    shinobi_gdfs_state(io);
    shinobi_memory_state(io);
    shinobi_chain_state(io);
    shinobi_cfg_state(io);
    hle_bios_state(io);
    hle_overlay_state(io);
    kamui_state(io);
    texture_state(io);
    snd_host_state(io);
    snd_sd_state(io);
    snd_adxt_state(io);
}
