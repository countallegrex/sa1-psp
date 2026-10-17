/* stubs.c - sound: nothing plays this milestone.  These are the few sound
 * entries whose generated stub (return 0) is not enough because the game
 * reads an output argument or treats 0 as a live handle.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "../seam.h"

/* sd: memory block / port creation write a handle through r4; the game
 * checks it for non-NULL. */
void sdk_8c6434e0(sh4_ctx *c) { HLE_ENTER(0x8c6434e0u, "sdMemBlkCreate"); hle_wr32(ARG0, 0x5D000001u); RET(0); }
void sdk_8c643ac0(sh4_ctx *c) { HLE_ENTER(0x8c643ac0u, "sdPortOpen"); hle_wr32(ARG0, 0x5D000002u); RET(0); }

/* ADXT_GetTime(handle, &count, &rate): nothing played. */
void sdk_8c644504(sh4_ctx *c) { HLE_ENTER(0x8c644504u, "ADXT_GetTime"); hle_wr32(ARG1, 0); hle_wr32(ARG2, 1); RET(0); }
/* ADXSTM transferred amount: nothing transferred. */
void sdk_8c65c1ca(sh4_ctx *c) { HLE_ENTER(0x8c65c1cau, "ADXSTM_Tell"); hle_wr32(ARG1, 0); RET(0); }
/* ADXT_Create returns NULL: every game wrapper then skips ADX. */
void sdk_8c64413c(sh4_ctx *c) { HLE_ENTER(0x8c64413cu, "ADXT_Create (off)"); RET(0); }
