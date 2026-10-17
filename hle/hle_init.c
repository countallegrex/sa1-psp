/* hle_init.c - shared HLE plumbing: guest string copy, callback entry, and
 * the one-time initialisation that wires the BIOS area and the libraries.
 *
 * Part of sa1-psp (MIT). */
#include "hle.h"

void hle_bios_init(void);     /* hle/bios/syscalls.c */
void hle_shinobi_init(void);  /* hle/shinobi/sys.c */
void hle_movie_init(void);    /* hle/overlay/movie.c */

void (*hle_frame_hook)(uint32_t frame);

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
}
