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
 * report; recomp/sa1_us_1005/hle_wraps.tsv lists them.  Eight of the
 * wrapped functions are not writers at all (docs/overlays-sonic.md
 * section 6: three collision functions and five that only test the
 * constant 0x10000000); they run unchanged here too.
 *
 * Three of them (the strip writers 0x8C038516 and 0x8C038802 and the
 * near-plane clipper 0x8C037EE0) clear SR.RB on entry and set it back
 * before returning: they borrow the other register bank's r0-r7 as
 * scratch, so the caller's r0-r7 survive untouched on the hardware (only
 * r1 is handed back, equal to the caller's).  The emitter lowers `ldc
 * Rn,sr` to the flag bits alone (docs/m2-render-report.md), so those
 * wrappers save r0-r7 around the recompiled body.
 *
 * With SA1_KAMUI_DUMP=<frame> set, every call in that frame is logged
 * with its registers (kamui.c's pointer trace).
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "ninja.h"

#include <stdlib.h>

int kamui_ptr_trace(void);
void kamui_mark2d_begin(void);
void kamui_mark2d_end(void);

/* SA1_TA_BT=<guest address>: with SA1_KAMUI_DUMP, the host backtrace (the
 * guest call stack) of the writer call that starts at that translucent
 * buffer address: which game function submitted a strip the dump showed. */
void plat_backtrace(void);
uint32_t kamui_buffer_ptr(int list);     /* kamui.c: the descriptor's write pointer (the writers advance it) */
static uint32_t ta_bt_want, ta_bt_enter_tr, ta_bt_regs[16]; static int ta_bt_read;
static void ta_bt_check(uint32_t tr, const sh4_ctx *c) {       /* at entry: remember where this call starts writing, and its registers */
    if (!ta_bt_read) { ta_bt_read = 1; const char *e = getenv("SA1_TA_BT"); if (e) ta_bt_want = (uint32_t)strtoul(e, NULL, 16) & 0x1FFFFFFFu; }
    ta_bt_enter_tr = tr & 0x1FFFFFFFu;
    for (int i = 0; i < 16; i++) ta_bt_regs[i] = c->r[i];
}
static void ta_bt_leave(uint32_t tr) {       /* at exit: the wanted header lies in what this call wrote */
    tr &= 0x1FFFFFFFu;
    /* the game's writers carry their pointer in r1 as a store-queue address
     * (0xE0000000 | RAM offset) and hand it back only at the end of the model;
     * a call matches when its entry r14 is at most 8 KB below the wanted header */
    uint32_t r14off = ta_bt_regs[14] & 0x01FFFFFFu, want_off = ta_bt_want & 0x01FFFFFFu;
    int by_r1 = (ta_bt_regs[14] >> 28) == 0xEu && r14off <= want_off && want_off < r14off + 0x2000u;
    if (ta_bt_want && ((ta_bt_want >= ta_bt_enter_tr && ta_bt_want < tr) || by_r1)) {
        sa1_log("ta: the writer call covering tr 0x%08x (SA1_TA_BT) wrote 0x%08x..0x%08x; registers at entry r0-r15:", ta_bt_want, ta_bt_enter_tr, tr);
        sa1_log("ta:   %08x %08x %08x %08x %08x %08x %08x %08x", ta_bt_regs[0], ta_bt_regs[1], ta_bt_regs[2], ta_bt_regs[3], ta_bt_regs[4], ta_bt_regs[5], ta_bt_regs[6], ta_bt_regs[7]);
        sa1_log("ta:   %08x %08x %08x %08x %08x %08x %08x %08x", ta_bt_regs[8], ta_bt_regs[9], ta_bt_regs[10], ta_bt_regs[11], ta_bt_regs[12], ta_bt_regs[13], ta_bt_regs[14], ta_bt_regs[15]);
        for (int i = 8; i <= 10; i++) {      /* the clipper 0x8C037EE0 takes its vertex records in r8, r9, r10 */
            uint32_t v = ta_bt_regs[i];
            if (hle_in_ram(v)) sa1_log("ta:   r%d -> (%g, %g, %g) word %08x; next (%g, %g, %g) %08x", i, hle_rdf(v), hle_rdf(v + 4), hle_rdf(v + 8), hle_rd32(v + 12), hle_rdf(v + 16), hle_rdf(v + 20), hle_rdf(v + 24), hle_rd32(v + 28));
        }
        sa1_log("ta:   guest call stack:");
        plat_backtrace();
    }
}

#define TA_PASS(addr) void fn_##addr##_orig(sh4_ctx *c); \
    void fn_##addr(sh4_ctx *c) { \
        HLE_ENTER(0x##addr##u, "TA writer fn_" #addr); \
        int tr_ = kamui_ptr_trace(); \
        if (tr_) sa1_log("ta: enter " #addr " r4 %08x r5 %08x r6 %08x r14 %08x ctx-op %08x tr %08x", c->r[4], c->r[5], c->r[6], c->r[14], hle_rd32(0x8C890114u), hle_rd32(0x8C890118u)); \
        if (tr_) ta_bt_check(kamui_buffer_ptr(1), c); \
        fn_##addr##_orig(c); \
        if (tr_) ta_bt_leave(kamui_buffer_ptr(1)); \
        if (tr_) sa1_log("ta: leave " #addr " r0 %08x r14 %08x ctx-op %08x tr %08x", c->r[0], c->r[14], hle_rd32(0x8C890114u), hle_rd32(0x8C890118u)); }

#define TA_PASS_BANKED(addr) void fn_##addr##_orig(sh4_ctx *c); \
    void fn_##addr(sh4_ctx *c) { \
        HLE_ENTER(0x##addr##u, "TA writer fn_" #addr " (banked)"); \
        uint32_t saved[8]; \
        for (int i = 0; i < 8; i++) saved[i] = c->r[i]; \
        int tr_ = kamui_ptr_trace(); \
        if (tr_) sa1_log("ta: enter " #addr " (banked) r1 %08x r14 %08x tr %08x", c->r[1], c->r[14], hle_rd32(0x8C890118u)); \
        if (tr_) ta_bt_check(kamui_buffer_ptr(1), c); \
        fn_##addr##_orig(c); \
        if (tr_) ta_bt_leave(kamui_buffer_ptr(1)); \
        if (tr_) sa1_log("ta: leave " #addr " (banked) r14 %08x tr %08x", c->r[14], hle_rd32(0x8C890118u)); \
        for (int i = 0; i < 8; i++) c->r[i] = saved[i]; }

/* The 2D writers (HUD, menus): their parameters are marked for the 4:3
 * area (ninja.h, kamui_mark2d_begin). */
#define TA_PASS_2D(addr) void fn_##addr##_orig(sh4_ctx *c); \
    void fn_##addr(sh4_ctx *c) { \
        HLE_ENTER(0x##addr##u, "TA 2D writer fn_" #addr); \
        kamui_mark2d_begin(); \
        fn_##addr##_orig(c); \
        kamui_mark2d_end(); }

/* The per-vertex transform and the lighting (ta_xform.c) natively with
 * --ta-direct too; a traced frame runs the recompiled bodies. */
void fn_8c037294_orig(sh4_ctx *c);
void fn_8c037294_ta(sh4_ctx *c);
void fn_8c037294(sh4_ctx *c) {
    if (hle_draw_skip) return;   /* frame skip: no picture this frame (hle/game/frame_skip.c) */
    if (native_land_window && native_land_model_ok(c->r[4])) {   /* native_land.c: the backend transforms */
        WR32(c->gbr, 60, RD32(0x8c88f64cu, 0));                  /* the vertex buffer, for the lighting */
        return;
    }
    if (hle_ta_direct && !kamui_ptr_trace()) {
        HLE_ENTER(0x8c037294u, "TA writer fn_8c037294");
        ta_xform_8c037294(c);
        return;
    }
    fn_8c037294_ta(c);
}
#define fn_8c037294 fn_8c037294_ta
TA_PASS(8c037294)
#undef fn_8c037294
/* 0x8C037350 is wrapped once recomp/sa1_us_1005/hle_wraps.tsv lists it and
 * recomp/out is regenerated (its body then is fn_8c037350_orig). */
#ifdef SA1_WRAP_8C037350
void fn_8c037350_orig(sh4_ctx *c);
void fn_8c037350(sh4_ctx *c) {
    HLE_ENTER(0x8c037350u, "model lighting fn_8c037350");
    if (hle_draw_skip) return;   /* frame skip */
    if (hle_ta_direct && !kamui_ptr_trace()) ta_xform_8c037350(c);
    else fn_8c037350_orig(c);
}
#endif
TA_PASS(8c037460)
TA_PASS(8c0374d4)
TA_PASS(8c037538)
TA_PASS(8c0375b0)
/* 0x8C0376D0, the mesh-set loop (ta_mesh.c), natively with --ta-direct
 * once hle_wraps.tsv's entry is in the emitted code (as 0x8C037350).
 * SA1_MESH_REC=1 runs the recompiled body instead (native vs recompiled
 * comparisons). */
#ifdef SA1_WRAP_8C0376D0
void fn_8c0376d0_orig(sh4_ctx *c);
static int mesh_rec = -1;
void fn_8c0376d0(sh4_ctx *c) {
    HLE_ENTER(0x8c0376d0u, "model mesh-set loop fn_8c0376d0");
    if (hle_draw_skip) return;   /* frame skip */
    if (mesh_rec < 0) mesh_rec = getenv("SA1_MESH_REC") != NULL;
    if (hle_ta_direct && !mesh_rec && !kamui_ptr_trace()) ta_mesh_8c0376d0(c);
    else fn_8c0376d0_orig(c);
}
#endif
/* With --ta-direct the native body (ta_direct.c) writes the strips into
 * kamui's host store; a traced frame keeps the TA bytes for the dump. */
void fn_8c037990_orig(sh4_ctx *c);
void fn_8c037990_ta(sh4_ctx *c);
void fn_8c037990(sh4_ctx *c) {
    if (hle_ta_direct && !kamui_ptr_trace()) {
        HLE_ENTER(0x8c037990u, "TA writer fn_8c037990");
        ta_direct_8c037990(c);
        return;
    }
    fn_8c037990_ta(c);
}
#define fn_8c037990 fn_8c037990_ta
TA_PASS(8c037990)
#undef fn_8c037990
TA_PASS(8c037c3c)
/* The near-plane writer and its clipper 0x8C037EE0 the same way
 * (ta_direct_clip.c); the clipper's own wrapper stays for the traced path. */
void fn_8c037ccc_orig(sh4_ctx *c);
void fn_8c037ccc_ta(sh4_ctx *c);
void fn_8c037ccc(sh4_ctx *c) {
    if (hle_ta_direct && !kamui_ptr_trace()) {
        HLE_ENTER(0x8c037cccu, "TA writer fn_8c037ccc");
        ta_direct_8c037ccc(c);
        return;
    }
    fn_8c037ccc_ta(c);
}
#define fn_8c037ccc fn_8c037ccc_ta
TA_PASS(8c037ccc)
#undef fn_8c037ccc
TA_PASS_BANKED(8c037ee0)
TA_PASS_BANKED(8c038516)
TA_PASS(8c038668)
TA_PASS_BANKED(8c038802)
TA_PASS_2D(8c041eaa)
TA_PASS_2D(8c042098)
TA_PASS_2D(8c042298)
TA_PASS_2D(8c042416)
TA_PASS_2D(8c0425a0)
TA_PASS_2D(8c042880)
TA_PASS_2D(8c04298e)
TA_PASS_2D(8c042b22)
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
