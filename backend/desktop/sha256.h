/* sha256.h - SHA-256 (FIPS 180-4) of a file: the tape's title check
 * (input_tape.c) and the save state's build check (savestate.c).
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include <stdint.h>

/* 0 = ok, -1 = the file could not be read. */
int sha256_file(const char *path, uint8_t out[32]);
/* Lower-case hex, 64 chars and a NUL. */
void sha256_hex(const uint8_t digest[32], char out[65]);
