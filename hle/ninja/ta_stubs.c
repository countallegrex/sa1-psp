/* ta_stubs.c - the game functions that write PowerVR tile-accelerator
 * parameters themselves through the SH-4 store queues: the game's own
 * model/landtable renderer (0x8C036F00-0x8C038E00, state in GBR-relative
 * globals), the 2D writers (0x8C041E00-0x8C042C00) and the clusters
 * docs/seam.md section 5 lists.
 *
 * M1 made them no-ops.  M2 lets the recompiled bodies run: their stores
 * to 0xE0000000 | offset land in guest RAM at the same offset (the RAM
 * macro masks the alias), which is exactly where the KAMUI vertex buffer
 * they are aimed at lives (hle/ninja/kamui.c), and `pref` has no effect.
 * The wrappers stay so that the per-writer counts appear in the seam
 * report; recomp/sa1_us_1005/hle_wraps.tsv lists them.
 *
 * Three of them (the strip writers 0x8C038516 and 0x8C038802 and the
 * near-plane clipper 0x8C037EE0) clear SR.RB on entry and set it back
 * before returning: they borrow the other register bank's r0-r7 as
 * scratch, so the caller's r0-r7 survive untouched on the hardware (only
 * r1 is handed back, equal to the caller's).  The emitter lowers `ldc
 * Rn,sr` to the flag bits alone (docs/m2-render-report.md), so those
 * wrappers save r0-r7 around the recompiled body.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"

#define TA_PASS(addr) void fn_##addr##_orig(sh4_ctx *c); \
    void fn_##addr(sh4_ctx *c) { HLE_ENTER(0x##addr##u, "TA writer fn_" #addr); fn_##addr##_orig(c); }

#define TA_PASS_BANKED(addr) void fn_##addr##_orig(sh4_ctx *c);     void fn_##addr(sh4_ctx *c) {         HLE_ENTER(0x##addr##u, "TA writer fn_" #addr " (banked)");         uint32_t saved[8];         for (int i = 0; i < 8; i++) saved[i] = c->r[i];         fn_##addr##_orig(c);         for (int i = 0; i < 8; i++) c->r[i] = saved[i]; }

TA_PASS(8c037294)
TA_PASS(8c037460)
TA_PASS(8c0374d4)
TA_PASS(8c037538)
TA_PASS(8c0375b0)
TA_PASS(8c037990)
TA_PASS(8c037c3c)
TA_PASS(8c037ccc)
TA_PASS_BANKED(8c037ee0)
TA_PASS_BANKED(8c038516)
TA_PASS(8c038668)
TA_PASS_BANKED(8c038802)
TA_PASS(8c041eaa)
TA_PASS(8c042098)
TA_PASS(8c042298)
TA_PASS(8c042416)
TA_PASS(8c0425a0)
TA_PASS(8c042880)
TA_PASS(8c04298e)
TA_PASS(8c042b22)
TA_PASS(8c0522c0)
TA_PASS(8c052518)
TA_PASS(8c0528ae)
TA_PASS(8c07e0ae)
TA_PASS(8c07e480)
TA_PASS(8c07e642)
TA_PASS(8c07e9e6)
TA_PASS(8c080fa2)
TA_PASS(8c09e778)
TA_PASS(8c09e880)
TA_PASS(8c09eb08)
TA_PASS(8c09ee44)
