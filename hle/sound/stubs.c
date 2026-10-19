/* stubs.c - sound entries that still need no more than a fixed answer.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "../seam.h"

/* ADXSTM transferred amount (the CRI stream layer under ADXF, which the
 * game's partition loader reaches only after a successful file query, and
 * ours always fails: hle/sound/adxt.c). */
void sdk_8c65c1ca(sh4_ctx *c) { HLE_ENTER(0x8c65c1cau, "ADXSTM_Tell"); hle_wr32(ARG1, 0); RET(0); }
