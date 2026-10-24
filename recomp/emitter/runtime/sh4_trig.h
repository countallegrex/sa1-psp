/* sh4_trig.h - fsca: sine and cosine of a 16-bit angle (65536 = one turn)
 * as the SH-4 gives them, for the recompiled code (FSCA, sh4_rt.h) and the
 * Ninja HLE's angle math (njSin/njCos are fsca on the console).
 *
 * The values are the console's own table (the oracle's fsca for every angle,
 * dumped by tools/flycast/fsca_dump.py into the user's data folder as
 * fsca_sh4.bin and loaded at boot with sh4_trig_init); the platform's
 * sinf/cosf differ from it at about 70 % of the angles, and differ between
 * the desktop's libm and newlib (PSP) as well.  The table is exactly
 * cos(a) = sin(a + 0x4000) and sin(a + 0x8000) = -sin(a) (bit for bit), so
 * half a turn of sines is all of it.
 *
 * Without the file sh4_trig_init falls back to libm (sh4_fsca_authentic 0):
 * the backends log it loudly and refuse strict runs.
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include <stdint.h>
#include <string.h>

extern uint32_t sh4_fsca_sin[32768];   /* float bits of sin(a), a = 0..0x7FFF */
extern int sh4_fsca_ready;             /* the table is filled (from the file or libm) */
extern int sh4_fsca_authentic;         /* 1: the console's table */

/* table: fsca_sh4.bin (65536 x (sin, cos) float bits, little-endian, 512 KB),
 * NULL for the libm fallback.  Returns 0, or -1 if the table is not a valid
 * fsca table (then the fallback is in place). */
int sh4_trig_init(const void *table, uint32_t size);

static inline uint32_t sh4_sin16_bits(uint32_t a) {
    if (__builtin_expect(!sh4_fsca_ready, 0)) sh4_trig_init(NULL, 0);
    return sh4_fsca_sin[a & 0x7FFFu] ^ ((a & 0x8000u) << 16);
}
static inline float sh4_sin16(uint32_t a) { uint32_t w = sh4_sin16_bits(a); float f; memcpy(&f, &w, 4); return f; }
static inline float sh4_cos16(uint32_t a) { return sh4_sin16(a + 0x4000u); }
