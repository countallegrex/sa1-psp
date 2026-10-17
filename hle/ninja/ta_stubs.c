/* ta_stubs.c - the game functions that write PowerVR tile-accelerator data
 * themselves, through the SH-4 store queues (0xE0000000) and the TA FIFO
 * (0x10000000): the game's own model/landtable renderer (0x8C036F00-
 * 0x8C038E00, state in GBR-relative globals), the 2D/debug writers
 * (0x8C041E00-0x8C042C00), and the five smaller clusters docs/seam.md
 * section 5 lists.  M1 has no graphics, so each is a native no-op: their
 * stores would otherwise land in low guest RAM (the RAM macro masks the
 * store-queue address), and the strip writer 0x8C038668 spun on a count
 * nothing produced.  M2 replaces them with the TA-vertex-to-GE translator
 * (README section 4, rule 3).  Wrapped through recomp/sa1_us_1005/
 * hle_wraps.tsv; the recompiled bodies (fn_*_orig) stay unused.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"

#define TA_STUB(addr) void fn_##addr(sh4_ctx *c) { HLE_ENTER(0x##addr##u, "TA writer fn_" #addr " (no graphics)"); (void)c; }

TA_STUB(8c037294)
TA_STUB(8c037460)
TA_STUB(8c0374d4)
TA_STUB(8c037538)
TA_STUB(8c0375b0)
TA_STUB(8c037990)
TA_STUB(8c037c3c)
TA_STUB(8c037ccc)
TA_STUB(8c037ee0)
TA_STUB(8c038516)
TA_STUB(8c038668)
TA_STUB(8c038802)
TA_STUB(8c041eaa)
TA_STUB(8c042098)
TA_STUB(8c042298)
TA_STUB(8c042416)
TA_STUB(8c0425a0)
TA_STUB(8c042880)
TA_STUB(8c04298e)
TA_STUB(8c042b22)
TA_STUB(8c0522c0)
TA_STUB(8c052518)
TA_STUB(8c0528ae)
TA_STUB(8c07e0ae)
TA_STUB(8c07e480)
TA_STUB(8c07e642)
TA_STUB(8c07e9e6)
TA_STUB(8c080fa2)
TA_STUB(8c09e778)
TA_STUB(8c09e880)
TA_STUB(8c09eb08)
TA_STUB(8c09ee44)
