/* movie.c - the Sofdec movie player (SONICADV/MOVIE.BIN, loaded at
 * 0x8CEB0000) is not recompiled: FMV is out of scope (README section 9,
 * risk 7).  The game's launcher 0x8C10B840 reads the file into place, points
 * the VSync callback at its own movie routine, and calls the player's entry
 * at 0x8CEB0000 with r4 = movie id; the call returns when the movie ends
 * (r0 >= 0) or is cancelled.  This native entry returns at once, so every
 * movie is skipped.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"

#define MOVIE_ENTRY 0x8CEB0000u

static void movie_entry(sh4_ctx *c) {
    HLE_ENTER(MOVIE_ENTRY, "MOVIE.BIN entry (skipped)");
    sa1_log("movie: id %u skipped (no FMV playback)", c->r[4]);
    c->r[0] = 0;
}

void hle_movie_init(void) {
    sa1_register_fn(MOVIE_ENTRY, movie_entry);
}
