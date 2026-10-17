/* kamui.c - the PowerVR layer under Ninja, replaced: render context,
 * tile-accelerator parameter buffers and the frame.
 *
 * How SA1 draws (docs/formats/ninja.md): the game does its own transform
 * and lighting (recompiled) and writes PowerVR TA parameters, 32 bytes at
 * a time, through the SH-4 store queues into KAMUI's vertex buffers in
 * main RAM.  It takes the write pointer from the Ninja context copy at
 * 0x8C890104 (+16 opaque, +20 translucent, +24 punch-through), copies the
 * four compiled header words from the same place, and hands the advanced
 * pointer back.  The store-queue stores land in guest RAM already (the
 * RAM macro masks the 0xE0000000 alias to the same offset), so nothing in
 * the writers needs changing: this file owns the context, keeps the three
 * buffers, and at the end of the frame parses what the game wrote and
 * hands it to backend/draw.h: opaque in order, punch-through in order
 * with alpha test, translucent sorted back to front.
 *
 * The context: the KAMUI strip head the Ninja setters write (pointer at
 * 0x8C88FCB4; fields from the disassembly of kmCommit 0x8C654E60), its
 * compiled PCW/ISP/TSP/TCW at +0x90..+0x9C, the vertex buffer descriptor
 * (pointer at 0x8C88FCD4: +0 / +8 / +16 the three write pointers) and the
 * game-visible copy at 0x8C890104 that njSaveContext / njRestoreContext
 * sync.  All of it lives in guest RAM because game code reads and edits
 * the copy directly (14 sites on the PCW, 12 on the TSP).
 *
 * Part of sa1-psp (MIT). */
#include "ninja.h"
#include "../shinobi/shinobi.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>


/* ---- guest globals ------------------------------------------------------- */
#define G_SH_PTR     0x8C88FCB4u   /* -> strip head (0x8C88FB34, static in the SDK) */
#define G_DESC_PTR   0x8C88FCD4u   /* -> vertex buffer descriptor */
#define G_LAST_ATTR  0x8C88FCD0u
#define G_CTX_TOP    0x8C890128u   /* top bits of the last buffer pointer (game-visible) */
#define G_CTX_SLOT   0x8C89012Cu   /* -> the context slot that holds the pointer */
#define G_SCREEN_W   0x8C88F644u
#define G_SCREEN_H   0x8C88F648u
#define G_PREINIT_SIZE 0x8C673454u
#define G_TEXBUF     0x8C88F7C8u
#define G_TEXBUF_SIZE 0x8C88F7CCu
#define G_VBUF       0x8C88F64Cu
#define G_VBUF_N     0x8C88F650u
#define G_DEPTHQ_NEAR 0x8C88F680u
#define G_DEPTHQ_FAR  0x8C88F684u
#define G_DEPTHQ_INV  0x8C88F688u

/* strip head fields (kmCommit 0x8C654E60) */
#define SH_FLAGS   0
#define SH_TYPE    4      /* 0/1 polygon, 2 sprite */
#define SH_LIST    8      /* list type 0..4 */
#define SH_UV16    16
#define SH_DEPTH   20
#define SH_CULL    24
#define SH_TEXGOU  32     /* bit1 texture, bit0 gouraud */
#define SH_COLTYPE 36
#define SH_ZWDIS   40
#define SH_SRC     44
#define SH_DST     48
#define SH_SRCSEL  52
#define SH_DSTSEL  56
#define SH_FOG     60
#define SH_OFFSET  64
#define SH_USEALPHA 68
#define SH_IGNTEXA 72
#define SH_CLAMP   76
#define SH_FLIP    80
#define SH_FILTER  84
#define SH_SUPER   88
#define SH_MIPADJ  92
#define SH_SHADE   96
#define SH_COLCLAMP 100
#define SH_FACECOL 112    /* 4 floats a,r,g,b (intensity mode) */
#define SH_PCW     0x90
#define SH_ISP     0x94
#define SH_TSP     0x98
#define SH_TCW     0x9C
#define SH_DCALC   0xB4
#define SH_SIZE    0x100

static uint32_t sh_addr, desc_addr;
static struct { uint32_t base, size; } bufs[3];
static const char *list_name[3] = {"opaque", "translucent", "punch-through"};
static uint32_t buf_high[3];              /* bytes used, high-water mark */
static uint32_t clear_color = 0xFF000000u;
static float fog_near = 1.0f, fog_far = 65535.0f;
static uint32_t fog_color = 0xFF808080u;
static const char *screenshot_path;
static uint32_t dump_frame;        /* SA1_KAMUI_DUMP=<frame>: log every header, pointer handoff and writer call of that frame */
static int ptr_trace(void) { return dump_frame && hle_frame_count() >= dump_frame && hle_frame_count() < dump_frame + 2; }
int kamui_ptr_trace(void) { return ptr_trace(); }
static int frame_rendered;
static uint32_t stat_frames, stat_polys[3], stat_strips[3], stat_modvols, stat_overflow, stat_sprites;
static uint32_t last_tris, last_calls, frame_rendered_count, last_polys[3], frame_polys[3];

/* ---- strip head access --------------------------------------------------- */
static uint32_t sh_rd(uint32_t off) { return hle_rd32(sh_addr + off); }
static void sh_wr(uint32_t off, uint32_t v) { hle_wr32(sh_addr + off, v); }
static const uint32_t filter_code[4] = {0, 2, 1, 3};   /* table at 0x8C671D68 */

/* kmCommit: compile the strip head into PCW / ISP / TSP / TCW. */
static void sh_commit(void) {
    uint32_t tex = (sh_rd(SH_TEXGOU) >> 1) & 1, gou = sh_rd(SH_TEXGOU) & 1;
    uint32_t isp = (sh_rd(SH_DEPTH) & 7) << 29 | (sh_rd(SH_CULL) & 3) << 27 | (sh_rd(SH_ZWDIS) ? 1u << 26 : 0)
                 | tex << 25 | (sh_rd(SH_OFFSET) & 1) << 24 | gou << 23 | (sh_rd(SH_UV16) & 1) << 22
                 | (sh_rd(SH_DCALC) ? 1u << 20 : 0);
    /* blend: Ninja's njColorBlendingMode indices -> PowerVR instructions
     * (kmCommit tables; modes 0 and 1 set both factors, 0xFF = not expressible) */
    static const uint8_t src_tab[12] = {5, 4, 6, 2, 7, 3, 5, 0xFF, 4, 0xFF, 1, 0};
    static const uint8_t dst_tab[12] = {4, 5, 6, 0xFF, 7, 0xFF, 5, 3, 4, 2, 1, 0};
    uint32_t si = sh_rd(SH_SRC), di = sh_rd(SH_DST);
    uint32_t tsp_old = sh_rd(SH_TSP);
    uint32_t src = (tsp_old >> 29) & 7, dst = (tsp_old >> 26) & 7;
    if (si < 12) {
        if (si < 2) { src = src_tab[si]; dst = dst_tab[si]; }
        else if (src_tab[si] != 0xFF) src = src_tab[si];
    }
    if (di < 12) {
        if (di < 2) { src = src_tab[di]; dst = dst_tab[di]; }
        else if (dst_tab[di] != 0xFF) dst = dst_tab[di];
    }
    uint32_t tsp = src << 29 | dst << 26 | (sh_rd(SH_SRCSEL) & 1) << 25 | (sh_rd(SH_DSTSEL) & 1) << 24
                 | (sh_rd(SH_FOG) & 3) << 22 | (sh_rd(SH_COLCLAMP) ? 1u << 21 : 0)
                 | (sh_rd(SH_USEALPHA) ? 1u << 20 : 0) | (sh_rd(SH_IGNTEXA) ? 1u << 19 : 0)
                 | (sh_rd(SH_FLIP) & 3) << 17 | (sh_rd(SH_CLAMP) & 3) << 15
                 | filter_code[sh_rd(SH_FILTER) & 3] << 13 | (sh_rd(SH_SUPER) ? 1u << 12 : 0)
                 | ((sh_rd(SH_MIPADJ) & 15) ? (sh_rd(SH_MIPADJ) & 15) : 4) << 8
                 | (sh_rd(SH_SHADE) & 3) << 6 | (tsp_old & 63);
    uint32_t type = sh_rd(SH_TYPE);
    uint32_t pcw = (type == 2 ? 0xA0000000u : 0x80000000u) | (sh_rd(SH_LIST) & 7) << 24 | 0x00800000u
                 | (sh_rd(SH_COLTYPE) & 3) << 4 | tex << 3 | (sh_rd(SH_OFFSET) & 1) << 2 | gou << 1 | (sh_rd(SH_UV16) & 1);
    sh_wr(SH_PCW, pcw); sh_wr(SH_ISP, isp); sh_wr(SH_TSP, tsp);
}

/* A setter: write the field, flag it, recompile (what every Ninja state
 * entry does: field store, kmCommit, kmChangeStripHead). */
static void sh_set(uint32_t off, uint32_t flag, uint32_t v) {
    sh_wr(SH_FLAGS, flag);
    sh_wr(off, v);
    sh_commit();
}

/* ---- buffers --------------------------------------------------------------- */
uint32_t kamui_buffer_ptr(int list) { return hle_rd32(desc_addr + 8u * (uint32_t)list); }
void kamui_buffer_set(int list, uint32_t ptr) { hle_wr32(desc_addr + 8u * (uint32_t)list, ptr); }

int kamui_append(int list, const uint32_t w[8]) {
    uint32_t p = kamui_buffer_ptr(list) & 0x1FFFFFFFu;
    uint32_t end = bufs[list].base + bufs[list].size;
    if (!bufs[list].base || p + 32 > end) { stat_overflow++; return 0; }
    for (int i = 0; i < 8; i++) hle_wr32(p + 4u * (uint32_t)i, w[i]);
    kamui_buffer_set(list, p + 32);
    return 1;
}

static void buffers_reset(void) {
    for (int i = 0; i < 3; i++) if (bufs[i].base) kamui_buffer_set(i, bufs[i].base);
    /* the game-visible copies too, so a stale pointer is never reused */
    hle_wr32(KM_CTX_PTR_OP, bufs[0].base); hle_wr32(KM_CTX_PTR_TR, bufs[1].base); hle_wr32(KM_CTX_PTR_PT, bufs[2].base);
}

static void buffers_alloc(void) {
    if (bufs[0].base) return;
    /* The three lists live in the extra guest area above the console's
     * 16 MB (sh4_rt.h): the SDK took them from the game heap, which is
     * too small for what Station Square writes per frame (~400 KB). */
    static const uint32_t sizes[3] = {1536 * 1024, 384 * 1024, 128 * 1024};
    uint32_t at = HLE_EXTRA_BASE;
    for (int i = 0; i < 3; i++) {
        bufs[i].size = sizes[i];
        bufs[i].base = at;
        at += sizes[i];
    }
    if (at - HLE_EXTRA_BASE > SA1_RAM_EXTRA) sa1_log("kamui: TA buffers exceed SA1_RAM_EXTRA");
    sa1_log("kamui: TA buffers op 0x%08x (%u) tr 0x%08x (%u) pt 0x%08x (%u)", bufs[0].base, bufs[0].size,
            bufs[1].base, bufs[1].size, bufs[2].base, bufs[2].size);
    /* descriptor: +0/+8/+16 write pointers, +4/+12 ends (never read by the game) */
    hle_wr32(desc_addr + 4, bufs[0].base + bufs[0].size);
    hle_wr32(desc_addr + 12, bufs[1].base + bufs[1].size);
    buffers_reset();
}

/* ---- init (from njInitSystem) ------------------------------------------- */
/* The strip head is the SDK's static one: the game's 2D writers read its
 * compiled words straight from 0x8C88FBC4 (= 0x8C88FB34 + 0x90), so the
 * address is part of the contract.  The descriptor only ever goes through
 * the pointer at 0x8C88FCD4. */
#define SH_STATIC   0x8C88FB34u
#define DESC_STATIC 0x8C88FC40u
void kamui_init(void) {
    if (getenv("SA1_KAMUI_DUMP")) dump_frame = (uint32_t)strtoul(getenv("SA1_KAMUI_DUMP"), NULL, 0);
    sh_addr = SH_STATIC;
    desc_addr = DESC_STATIC;
    for (uint32_t i = 0; i < SH_SIZE; i += 4) sh_wr(i, 0);
    for (uint32_t i = 0; i < 32; i += 4) hle_wr32(desc_addr + i, 0);
    hle_wr32(G_SH_PTR, sh_addr); hle_wr32(G_DESC_PTR, desc_addr);
    /* defaults: nearer wins, no culling, Z write, gouraud, bilinear, alpha from
     * vertices, source alpha blending, fog from the table, white face colour */
    sh_wr(SH_DEPTH, DRAW_DEPTH_GEQUAL); sh_wr(SH_CULL, 0); sh_wr(SH_TEXGOU, 1);
    sh_wr(SH_SRC, 8); sh_wr(SH_DST, 6); sh_wr(SH_USEALPHA, 1); sh_wr(SH_FILTER, 2);
    sh_wr(SH_SHADE, DRAW_SHADE_MODULATE); sh_wr(SH_FOG, 2); sh_wr(SH_MIPADJ, 4);
    for (int i = 0; i < 4; i++) hle_wrf(sh_addr + SH_FACECOL + 4u * (uint32_t)i, 1.0f);
    sh_wr(SH_TSP, 0); sh_wr(SH_TCW, 0);
    sh_commit();
    /* the game-visible copy */
    hle_wr32(KM_CTX_PCW, sh_rd(SH_PCW)); hle_wr32(KM_CTX_ISP, sh_rd(SH_ISP));
    hle_wr32(KM_CTX_TSP, sh_rd(SH_TSP)); hle_wr32(KM_CTX_TCW, sh_rd(SH_TCW));
    hle_wr32(G_CTX_TOP, 0x8C000000u); hle_wr32(G_CTX_SLOT, KM_CTX_PTR_OP);
    buffers_alloc();
    texture_init();
}

/* 0x8C604702: njInitSystem(mode, frame, count).  The display globals keep
 * the 640x480 values the console had: game code projects with them, and
 * the backend rescales. */
void sdk_8c604702(sh4_ctx *c) {
    HLE_ENTER(0x8c604702u, "njInitSystem");
    hle_wr32(G_SCREEN_W, 640); hle_wr32(G_SCREEN_H, 480);
    hle_wrf(NJ_G_W, 640.0f); hle_wrf(NJ_G_H, 480.0f);
    hle_wrf(NJ_G_CX, 320.0f); hle_wrf(NJ_G_CY, 240.0f);
    hle_wrf(NJ_G_DIST, 320.0f);
    hle_wrf(NJ_G_AX, 1.0f); hle_wrf(NJ_G_AY, 1.0f);
    hle_wrf(NJ_G_CLIPN, 1.0f); hle_wrf(NJ_G_CLIPF, 65535.0f);
    /* the screen clip rectangle the game's own culling (0x8C038D00) tests
     * projected bounding spheres against: left, top, right, bottom */
    hle_wrf(0x8C88F600u, 0.0f); hle_wrf(0x8C88F604u, 0.0f); hle_wrf(0x8C88F608u, 640.0f); hle_wrf(0x8C88F60Cu, 480.0f);
    sa1_log("njInitSystem: mode 0x%x, frame %u, count %u", ARG0, ARG1, ARG2);
    kamui_init();
}
void sdk_8c6049c0(sh4_ctx *c) { HLE_ENTER(0x8c6049c0u, "njInitSystem (display re-init)"); (void)c; }
void sdk_8c604510(sh4_ctx *c) { HLE_ENTER(0x8c604510u, "nj pre-init buffer size"); hle_wr32(G_PREINIT_SIZE, ARG0); }
void sdk_8c608c14(sh4_ctx *c) { HLE_ENTER(0x8c608c14u, "njInitTextureBuffer"); hle_wr32(G_TEXBUF, ARG0); hle_wr32(G_TEXBUF_SIZE, ARG1 & ~0x7FFu); }
void sdk_8c604dce(sh4_ctx *c) {
    HLE_ENTER(0x8c604dceu, "njInitVertexBuffer");
    sa1_log("njInitVertexBuffer(%d, %d, 0x%x, %d, 0x%x): KAMUI lists stay at their HLE sizes", (int)ARG0, (int)ARG1, ARG2, (int)ARG3, STACK_ARG(0));
    buffers_alloc();
}
void sdk_8c60edb0(sh4_ctx *c) {         /* njInit3D(vbuf, n) */
    HLE_ENTER(0x8c60edb0u, "njInit3D");
    hle_wr32(G_VBUF, ARG0); hle_wr32(G_VBUF_N, ARG1);
    hle_wr32(NJ_G_CTRL3D, 0); hle_wr32(NJ_G_CONST_AND, 0xFFFFFFFFu); hle_wr32(NJ_G_CONST_OR, 0);
    hle_wrf(NJ_G_CONST_MAT, 255.0f); hle_wrf(NJ_G_CONST_MAT + 4, 0); hle_wrf(NJ_G_CONST_MAT + 8, 0); hle_wrf(NJ_G_CONST_MAT + 12, 0);
}
void sdk_8c60ede0(sh4_ctx *c) { HLE_ENTER(0x8c60ede0u, "njControl3D"); hle_wr32(NJ_G_CTRL3D, ARG0); }
void sdk_8c60ede6(sh4_ctx *c) { HLE_ENTER(0x8c60ede6u, "njSetConstantAttr"); hle_wr32(NJ_G_CONST_AND, ARG0); hle_wr32(NJ_G_CONST_OR, ARG1); }
void sdk_8c60edf0(sh4_ctx *c) { HLE_ENTER(0x8c60edf0u, "njSetConstantMaterial"); for (int i = 0; i < 4; i++) hle_wr32(NJ_G_CONST_MAT + 4u * (uint32_t)i, hle_rd32(ARG0 + 4u * (uint32_t)i)); }
void sdk_8c60ee0a(sh4_ctx *c) {         /* depth queue near/far */
    HLE_ENTER(0x8c60ee0au, "njSetDepthQueue");
    float n = c->fr[4], f = c->fr[5];
    hle_wrf(G_DEPTHQ_NEAR, -n); hle_wrf(G_DEPTHQ_FAR, -f); hle_wrf(G_DEPTHQ_INV, (f - n) != 0.0f ? 1.0f / (f - n) : 0.0f);
}

/* ---- render-state setters (Ninja -> strip head) --------------------------- */
void sdk_8c605480(sh4_ctx *c) {         /* texture filter: 0 point, 1 bilinear, 2 trilinear */
    HLE_ENTER(0x8c605480u, "njSetTextureFilter");
    static const uint32_t map[3] = {0, 2, 1};
    sh_set(SH_FILTER, 0x4000, ARG0 < 3 ? map[ARG0] : 2);
}
void sdk_8c6054c6(sh4_ctx *c) { HLE_ENTER(0x8c6054c6u, "njSetTextureSupersample"); sh_set(SH_SUPER, 0x8000, ARG0 == 1); }
void sdk_8c60551e(sh4_ctx *c) { HLE_ENTER(0x8c60551eu, "njSetTextureClamp"); sh_set(SH_CLAMP, 0x1000, ARG0 & 3); }
void sdk_8c60556c(sh4_ctx *c) { HLE_ENTER(0x8c60556cu, "njSetTextureFlip"); sh_set(SH_FLIP, 0x2000, ARG0 & 3); }
void sdk_8c6055d4(sh4_ctx *c) { HLE_ENTER(0x8c6055d4u, "njSetCullingMode"); sh_set(SH_CULL, 2, ARG0 & 3); }
void sdk_8c6055ba(sh4_ctx *c) { HLE_ENTER(0x8c6055bau, "nj float render setting (ignored)"); (void)c; }
void sdk_8c605628(sh4_ctx *c) {         /* njColorBlendingMode(target, mode) */
    HLE_ENTER(0x8c605628u, "njColorBlendingMode");
    uint32_t mode = ARG1 < 12 ? ARG1 : 11;
    if (ARG0 == 0) sh_set(SH_SRC, 0x40, mode);
    else if (ARG0 == 1) sh_set(SH_DST, 0x80, mode);
}
void sdk_8c63fb18(sh4_ctx *c) { HLE_ENTER(0x8c63fb18u, "njFogEnable"); sh_set(SH_FOG, 0x100, 0); }
void sdk_8c63fae8(sh4_ctx *c) { HLE_ENTER(0x8c63fae8u, "njFogDisable"); sh_set(SH_FOG, 0x100, 2); }

/* 0x8C606A08: set polygon attributes straight into the context copy.
 * attr: bits 0..7 = PCW object control (col type, texture, offset,
 * gouraud, uv16), bit 25 = translucent list, bits 19/20 = TSP alpha. */
void sdk_8c606a08(sh4_ctx *c) {
    HLE_ENTER(0x8c606a08u, "njSetPolygonAttr");
    uint32_t attr = ARG0;
    uint32_t pcw = (hle_rd32(KM_CTX_PCW) & 0xFDFFFF00u) | (attr & 0x020000FFu);
    uint32_t tsp = (hle_rd32(KM_CTX_TSP) & 0xFFE7FFFFu) | (attr & 0x00180000u);
    uint32_t ctrl = hle_rd32(NJ_G_CTRL3D);
    if ((ctrl & 0x400) || (!(ctrl & 0x1000) && (attr & 0x02100000u))) pcw |= 0x80; else pcw &= ~0x80u;
    hle_wr32(KM_CTX_PCW, pcw); hle_wr32(KM_CTX_TSP, tsp);
    hle_wr32(G_LAST_ATTR, attr);
}

/* 0x8C6069A4: hand a buffer pointer back: *slot = (p & 0x03FFFFFF) | top bits. */

void sdk_8c6069a4(sh4_ctx *c) {
    HLE_ENTER(0x8c6069a4u, "njSetVertexBufferPointer");
    uint32_t slot = hle_rd32(G_CTX_SLOT);
    if (slot) hle_wr32(slot, (ARG0 & 0x03FFFFFFu) | hle_rd32(G_CTX_TOP));
    if (ptr_trace()) sa1_log("ptr: set slot 0x%08x = 0x%08x (op 0x%08x tr 0x%08x)", slot, ARG0, hle_rd32(KM_CTX_PTR_OP), hle_rd32(KM_CTX_PTR_TR));
}

/* 0x8C605DAC / 0x8C605E0A: sync strip head + descriptor <-> the context copy. */
void sdk_8c605dac(sh4_ctx *c) {
    HLE_ENTER(0x8c605dacu, "njSaveContext");
    if (!sh_addr) return;
    if (ptr_trace()) sa1_log("ptr: save  desc op 0x%08x tr 0x%08x (ctx op 0x%08x tr 0x%08x)", hle_rd32(desc_addr), hle_rd32(desc_addr + 8), hle_rd32(KM_CTX_PTR_OP), hle_rd32(KM_CTX_PTR_TR));
    hle_wr32(KM_CTX_PCW, sh_rd(SH_PCW)); hle_wr32(KM_CTX_ISP, sh_rd(SH_ISP));
    hle_wr32(KM_CTX_TSP, sh_rd(SH_TSP)); hle_wr32(KM_CTX_TCW, sh_rd(SH_TCW));
    hle_wr32(KM_CTX_PTR_OP, hle_rd32(desc_addr + 0));
    hle_wr32(KM_CTX + 28, hle_rd32(desc_addr + 4));
    hle_wr32(KM_CTX + 32, hle_rd32(desc_addr + 12));
    if (hle_rd32(NJ_G_CTRL3D) & 0x4000) { hle_wr32(KM_CTX_PTR_TR, hle_rd32(desc_addr + 16)); hle_wr32(KM_CTX_PTR_PT, hle_rd32(desc_addr + 8)); }
    else { hle_wr32(KM_CTX_PTR_TR, hle_rd32(desc_addr + 8)); hle_wr32(KM_CTX_PTR_PT, hle_rd32(desc_addr + 16)); }
}
void sdk_8c605e0a(sh4_ctx *c) {
    HLE_ENTER(0x8c605e0au, "njRestoreContext");
    if (!sh_addr) return;
    if (ptr_trace()) sa1_log("ptr: restore ctx op 0x%08x tr 0x%08x (desc op 0x%08x tr 0x%08x)", hle_rd32(KM_CTX_PTR_OP), hle_rd32(KM_CTX_PTR_TR), hle_rd32(desc_addr), hle_rd32(desc_addr + 8));
    sh_wr(SH_PCW, hle_rd32(KM_CTX_PCW)); sh_wr(SH_ISP, hle_rd32(KM_CTX_ISP));
    sh_wr(SH_TSP, hle_rd32(KM_CTX_TSP)); sh_wr(SH_TCW, hle_rd32(KM_CTX_TCW));
    hle_wr32(desc_addr + 0, hle_rd32(KM_CTX_PTR_OP));
    hle_wr32(desc_addr + 4, hle_rd32(KM_CTX + 28));
    hle_wr32(desc_addr + 12, hle_rd32(KM_CTX + 32));
    if (hle_rd32(NJ_G_CTRL3D) & 0x4000) { hle_wr32(desc_addr + 16, hle_rd32(KM_CTX_PTR_TR)); hle_wr32(desc_addr + 8, hle_rd32(KM_CTX_PTR_PT)); }
    else { hle_wr32(desc_addr + 8, hle_rd32(KM_CTX_PTR_TR)); hle_wr32(desc_addr + 16, hle_rd32(KM_CTX_PTR_PT)); }
}

/* 0x8C6064B0 / 0x8C6065B0: write a 32-byte polygon header from the context
 * copy plus the face colour at r4 (4 floats) to the opaque / translucent
 * list; returns the advanced pointer.  The opaque one forces the blend to
 * ONE/ZERO and ignores texture alpha, as the SDK does. */
static void write_header(sh4_ctx *c, uint32_t slot, int opaque) {
    uint32_t p = hle_rd32(slot);
    hle_wr32(G_CTX_TOP, p & 0xFC000000u);
    hle_wr32(G_CTX_SLOT, slot);
    uint32_t a = p & 0x1FFFFFFFu;
    uint32_t tsp = hle_rd32(KM_CTX_TSP);
    if (opaque) tsp = (tsp & 0x03EFFFFFu) | 0x20080000u;
    if (ptr_trace()) sa1_log("kamui: header to %s at 0x%08x: pcw 0x%08x tsp 0x%08x tcw 0x%08x (cur global index %u)", opaque ? "op" : "tr", a, hle_rd32(KM_CTX_PCW), tsp, hle_rd32(KM_CTX_TCW), hle_rd32(NJ_CUR_GINDEX));
    hle_wr32(a + 0, hle_rd32(KM_CTX_PCW)); hle_wr32(a + 4, hle_rd32(KM_CTX_ISP));
    hle_wr32(a + 8, tsp); hle_wr32(a + 12, hle_rd32(KM_CTX_TCW));
    for (int i = 0; i < 4; i++) hle_wr32(a + 16 + 4u * (uint32_t)i, hle_rd32(ARG0 + 4u * (uint32_t)i));
    RET(((p & 0x03FFFFFFu) | 0xE0000000u) + 32);
}
void sdk_8c6064b0(sh4_ctx *c) { HLE_ENTER(0x8c6064b0u, "kmWriteHeaderOpaque"); write_header(c, KM_CTX_PTR_OP, 1); }
void sdk_8c6065b0(sh4_ctx *c) { HLE_ENTER(0x8c6065b0u, "kmWriteHeaderTranslucent"); write_header(c, KM_CTX_PTR_TR, 0); }

/* 0x8C606B66 and friends (njDrawTexture / njDrawPolygon headers) are
 * served by the same writer: untextured or textured header from the
 * context copy, with the current texture selected from the argument. */

/* ---- texture selection -------------------------------------------------- */
void sdk_8c608ccc(sh4_ctx *c) {         /* njSetTexture(texlist) */
    HLE_ENTER(0x8c608cccu, "njSetTexture");
    if (!ARG0) { RET(-1); return; }
    hle_wr32(NJ_CUR_TEXLIST, ARG0);
    RET(1);
}
/* 0x8C606A88: njSetTextureNum(n): select texture n of the current texlist
 * into the context copy (TSP size bits, TCW).  0x8C608D14 does the same
 * into the strip head. */
static int select_texture(uint32_t n, uint32_t tsp_addr, uint32_t tcw_addr) {
    uint32_t tl = hle_rd32(NJ_CUR_TEXLIST);
    if (!tl) return -1;
    uint32_t names = hle_rd32(tl), count = hle_rd32(tl + 4);
    /* the SDK indexes without a bounds check (the game relies on it: lists
     * laid out back to back are addressed past their own count) */
    (void)count;
    if (!names || !hle_in_ram(names + n * 12)) return -1;
    uint32_t entry = hle_rd32(names + n * 12 + 8);   /* NJS_TEXNAME.texaddr -> NJS_TEXMEMLIST */
    if (!entry || !hle_in_ram(entry)) return -1;
    uint32_t gbix = hle_rd32(entry), bank = hle_rd32(entry + 4);
    /* the SDK skips the reload when the global index is already current; a
     * texture without one (0xFFFFFFFF) must never match that way */
    if (gbix != 0xFFFFFFFFu && gbix == hle_rd32(NJ_CUR_GINDEX) && bank == hle_rd32(NJ_CUR_BANK) && hle_rd32(tcw_addr) == hle_rd32(entry + 12)) return 1;
    hle_wr32(NJ_CUR_GINDEX, gbix); hle_wr32(NJ_CUR_BANK, bank);
    hle_wr32(tsp_addr, (hle_rd32(tsp_addr) & ~63u) | (hle_rd32(entry + 8) & 63));
    uint32_t tcw = hle_rd32(entry + 12);
    if ((tcw & 0x1FFFFFu) > 0x100000u) return -1;
    hle_wr32(tcw_addr, tcw);
    return 1;
}
void sdk_8c606a88(sh4_ctx *c) {
    HLE_ENTER(0x8c606a88u, "njSetTextureNum");
    int r = select_texture(ARG0, KM_CTX_TSP, KM_CTX_TCW);
    if (ptr_trace()) sa1_log("njSetTextureNum(%u) -> %d, tcw 0x%08x", ARG0, r, hle_rd32(KM_CTX_TCW));
    RET(r);
}
void sdk_8c608d14(sh4_ctx *c) {
    HLE_ENTER(0x8c608d14u, "njSetTextureNum (strip head)");
    int r = sh_addr ? select_texture(ARG0, sh_addr + SH_TSP, sh_addr + SH_TCW) : -1;
    if (ptr_trace()) sa1_log("njSetTextureNum/sh(%u) -> %d, tcw 0x%08x", ARG0, r, sh_addr ? sh_rd(SH_TCW) : 0);
    RET(r);
}

/* ---- background / border / fog ---------------------------------------------- */
void sdk_8c60526a(sh4_ctx *c) { HLE_ENTER(0x8c60526au, "njSetBackColor"); clear_color = ARG0; }
void sdk_8c605c10(sh4_ctx *c) { HLE_ENTER(0x8c605c10u, "njSetBorderColor"); (void)c; }
void sdk_8c63fe20(sh4_ctx *c) { HLE_ENTER(0x8c63fe20u, "njFogColor"); fog_color = ARG0; draw_set_fog(fog_color, fog_near, fog_far); }
void sdk_8c63fe3c(sh4_ctx *c) { HLE_ENTER(0x8c63fe3cu, "njSetFogDensity (ignored)"); (void)c; }
void sdk_8c63fdc0(sh4_ctx *c) {         /* njGenerateFogTable(table): the default curve */
    HLE_ENTER(0x8c63fdc0u, "njGenerateFogTable");
    for (uint32_t i = 0; i < 128; i++) hle_wrf(ARG0 + 4 * i, 16.0f / (float)((16 + (i & 15)) << (i >> 4)));
}
void sdk_8c63fb48(sh4_ctx *c) {         /* njGenerateFogTable3(table, near, far): linear */
    HLE_ENTER(0x8c63fb48u, "njGenerateFogTable3");
    float n = c->fr[4], f = c->fr[5];
    if (n < -65535.0f) n = -65535.0f;
    if (f < -65535.0f) f = -65535.0f;
    n = -n; f = -f;
    for (uint32_t i = 0; i < 128; i++) {
        /* entry i covers 1/w = (16 + (i & 15)) / 16 * 2^((i >> 4) - 7): w = ... */
        float invw = (float)(16 + (i & 15)) / 16.0f * ldexpf(1.0f, (int)(i >> 4) - 7);
        float w = 1.0f / invw;
        float d = (f - n) != 0.0f ? (w - n) / (f - n) : 1.0f;
        if (d < 0.0f) d = 0.0f;
        if (d > 1.0f) d = 1.0f;
        hle_wrf(ARG0 + 4 * i, d);
    }
}
void sdk_8c63fe48(sh4_ctx *c) {         /* njSetFogTable(table): derive linear near/far */
    HLE_ENTER(0x8c63fe48u, "njSetFogTable");
    float wn = -1.0f, wf = -1.0f;
    for (int i = 127; i >= 0; i--) {    /* from nearest to farthest */
        float invw = (float)(16 + (i & 15)) / 16.0f * ldexpf(1.0f, (i >> 4) - 7);
        float w = 1.0f / invw, d = hle_rdf(ARG0 + 4u * (uint32_t)i);
        if (wn < 0.0f && d > 0.02f) wn = w;
        if (wf < 0.0f && d > 0.98f) wf = w;
    }
    if (wn < 0.0f) wn = 65535.0f;
    if (wf < 0.0f) wf = 65535.0f;
    if (wf <= wn) wf = wn + 1.0f;
    fog_near = wn; fog_far = wf;
    draw_set_fog(fog_color, fog_near, fog_far);
}

/* ---- the parser: TA parameters in guest RAM -> backend ------------------- */
typedef struct strip_rec { uint32_t off, count; float depth; } strip_rec;   /* translucent sort */
static draw_vertex *verts; static uint32_t nverts, verts_cap;
static strip_rec *strips; static uint32_t nstrips, strips_cap;
static draw_state *strip_states; static uint32_t states_cap;

static int grow(void **p, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap) return 1;
    uint32_t n = *cap ? *cap * 2 : 1024;
    while (n < need) n *= 2;
    void *q = realloc(*p, (size_t)n * elem);
    if (!q) return 0;
    *p = q; *cap = n;
    return 1;
}

static void state_from_header(draw_state *s, uint32_t pcw, uint32_t isp, uint32_t tsp, uint32_t tcw, int list) {
    memset(s, 0, sizeof *s);
    s->texture = (pcw & 8) ? texture_from_tcw(tcw) : 0;
    if ((pcw & 8) && !s->texture) {
        static uint32_t logged[16], nlogged;
        int seen = 0;
        for (uint32_t i = 0; i < nlogged; i++) if (logged[i] == tcw) seen = 1;
        if (!seen && nlogged < 16) { logged[nlogged++] = tcw; sa1_log("kamui: textured polygon with unknown TCW 0x%08x (pcw 0x%08x list %d)", tcw, pcw, list); }
    }
    s->blend_src = (uint8_t)((tsp >> 29) & 7); s->blend_dst = (uint8_t)((tsp >> 26) & 7);
    if (list == 0) { s->blend_src = DRAW_BLEND_ONE; s->blend_dst = DRAW_BLEND_ZERO; }
    s->depth_func = (uint8_t)((isp >> 29) & 7);
    s->depth_write = !(isp & (1u << 26));
    s->cull = (uint8_t)((isp >> 27) & 3);
    s->alpha_test = (list == 4);
    s->use_alpha = (tsp >> 20) & 1;
    s->ignore_tex_alpha = (tsp >> 19) & 1;
    s->flip_u = (tsp >> 18) & 1; s->flip_v = (tsp >> 17) & 1;
    s->clamp_u = (tsp >> 16) & 1; s->clamp_v = (tsp >> 15) & 1;
    s->filter = (uint8_t)((tsp >> 13) & 3);
    s->shade = (uint8_t)((tsp >> 6) & 3);
    s->fog = (uint8_t)((tsp >> 22) & 3);
    s->gouraud = (pcw >> 1) & 1;
    if (list == 0) s->use_alpha = 0;
}

static uint32_t pack_floats(uint32_t a) {   /* 4 floats a,r,g,b in guest RAM */
    return nj_pack_argb(hle_rdf(a), hle_rdf(a + 4), hle_rdf(a + 8), hle_rdf(a + 12));
}
static uint32_t scale_argb(uint32_t face, float k) {
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    uint32_t r = (uint32_t)(((face >> 16) & 255) * k), g = (uint32_t)(((face >> 8) & 255) * k), b = (uint32_t)((face & 255) * k);
    return (face & 0xFF000000u) | r << 16 | g << 8 | b;
}

typedef struct parse_ctx {
    int list;                 /* list type of the current header */
    int modvol;               /* current global is a modifier volume */
    int sprite;
    int coltype, textured, offset, uv16, volume, hdr_vol2;
    uint32_t face, face_off;  /* intensity mode face colours */
    draw_state state;
    uint32_t strip_start;     /* index into verts */
} parse_ctx;

static void flush_strip(parse_ctx *pc, int sorted) {
    uint32_t n = nverts - pc->strip_start;
    if (n < 3) { nverts = pc->strip_start; return; }
    if (!sorted) {
        draw_set_state(&pc->state);
        draw_strip(verts + pc->strip_start, (int)n);
        nverts = pc->strip_start;
        if (pc->list < 3) stat_strips[pc->list == 2 ? 1 : pc->list]++;
        return;
    }
    if (!grow((void **)&strips, &strips_cap, nstrips + 1, sizeof *strips) ||
        !grow((void **)&strip_states, &states_cap, nstrips + 1, sizeof *strip_states)) { nverts = pc->strip_start; return; }
    float sum = 0.0f;
    for (uint32_t i = 0; i < n; i++) sum += verts[pc->strip_start + i].w;
    strips[nstrips].off = pc->strip_start; strips[nstrips].count = n; strips[nstrips].depth = sum / (float)n;
    strip_states[nstrips] = pc->state;
    nstrips++;
    stat_strips[1]++;
    pc->strip_start = nverts;
}

typedef struct sort_key { float d; uint32_t i; } sort_key;
static int cmp_key(const void *a, const void *b) {
    float da = ((const sort_key *)a)->d, db = ((const sort_key *)b)->d;
    return da < db ? -1 : da > db ? 1 : 0;     /* smaller 1/w = farther first */
}

/* Parse one buffer region.  Polygons of list types 0 and 4 draw at once
 * (sorted = 0); translucent ones (2) are collected for sorting. */
static void parse_region(uint32_t start, uint32_t end, int want_list, int sorted) {
    parse_ctx pc; memset(&pc, 0, sizeof pc);
    pc.list = -1; pc.strip_start = nverts;
    uint32_t p = start;
    int in_strip = 0;
    while (p + 32 <= end) {
        uint32_t pcw = hle_rd32(p);
        uint32_t type = pcw >> 29;
        if (type == 7) {                                   /* vertex */
            uint32_t vsize = pc.modvol ? 64 : ((pc.textured && pc.coltype == 1) || (pc.volume && pc.textured)) ? 64 : 32;
            if (pc.sprite) vsize = 64;
            if (p + vsize > end) break;
            if (pc.list == want_list && !pc.modvol) {
                if (pc.sprite) {
                    /* 4 points a,b,c,d (d has no z: it takes c's) and 3 packed uv pairs */
                    if (grow((void **)&verts, &verts_cap, nverts + 4, sizeof *verts)) {
                        float ax = hle_rdf(p + 4), ay = hle_rdf(p + 8), az = hle_rdf(p + 12);
                        float bx = hle_rdf(p + 16), by = hle_rdf(p + 20), bz = hle_rdf(p + 24);
                        float cx = hle_rdf(p + 28), cy = hle_rdf(p + 32), cz = hle_rdf(p + 36);
                        float dx = hle_rdf(p + 40), dy = hle_rdf(p + 44);
                        uint32_t uva = hle_rd32(p + 52), uvb = hle_rd32(p + 56), uvc = hle_rd32(p + 60);
                        float au = sh4_u2f(uva & 0xFFFF0000u), av = sh4_u2f(uva << 16);
                        float bu = sh4_u2f(uvb & 0xFFFF0000u), bv = sh4_u2f(uvb << 16);
                        float cu = sh4_u2f(uvc & 0xFFFF0000u), cv = sh4_u2f(uvc << 16);
                        draw_vertex *v = verts + nverts;
                        v[0] = (draw_vertex){ax, ay, az, au, av, pc.face, pc.face_off};
                        v[1] = (draw_vertex){bx, by, bz, bu, bv, pc.face, pc.face_off};
                        v[2] = (draw_vertex){dx, dy, cz, au + (cu - bu), av + (cv - bv), pc.face, pc.face_off};
                        v[3] = (draw_vertex){cx, cy, cz, cu, cv, pc.face, pc.face_off};
                        nverts += 4;
                        stat_sprites++;
                        flush_strip(&pc, sorted);
                    }
                } else {
                    if (grow((void **)&verts, &verts_cap, nverts + 1, sizeof *verts)) {
                        draw_vertex *v = verts + nverts;
                        v->x = hle_rdf(p + 4); v->y = hle_rdf(p + 8); v->w = hle_rdf(p + 12);
                        v->u = v->v = 0.0f; v->ocolor = 0;
                        uint32_t q = p + 16;
                        if (pc.textured) {
                            if (pc.uv16) { uint32_t uv = hle_rd32(q); v->u = sh4_u2f(uv & 0xFFFF0000u); v->v = sh4_u2f(uv << 16); q += 8; }
                            else { v->u = hle_rdf(q); v->v = hle_rdf(q + 4); q += 8; }
                        } else q += 8;
                        switch (pc.coltype) {
                        case 0: v->color = hle_rd32(q); if (pc.offset) v->ocolor = hle_rd32(q + 4); break;
                        case 1:
                            if (pc.textured) { v->color = pack_floats(p + 32); if (pc.offset) v->ocolor = pack_floats(p + 48); }
                            else v->color = pack_floats(q);
                            break;
                        default: v->color = scale_argb(pc.face, hle_rdf(q)); if (pc.offset) v->ocolor = scale_argb(pc.face_off, hle_rdf(q + 4)); break;
                        }
                        nverts++;
                        in_strip = 1;
                        if (pcw & 0x10000000u) { flush_strip(&pc, sorted); in_strip = 0; }
                    }
                }
            }
            p += vsize;
            continue;
        }
        /* a global parameter ends any open strip */
        if (in_strip) { flush_strip(&pc, sorted); in_strip = 0; }
        if (type == 4 || type == 5) {
            uint32_t isp = hle_rd32(p + 4), tsp = hle_rd32(p + 8), tcw = hle_rd32(p + 12);
            if (dump_frame && hle_frame_count() >= dump_frame && hle_frame_count() < dump_frame + 2 && want_list == 0 && !sorted) {
                /* count the vertices that follow */
                uint32_t q = p + 32, nv = 0;
                while (q + 32 <= end && (hle_rd32(q) >> 29) == 7) { nv++; q += 32; }
                sa1_log("hdr list %u at 0x%08x: pcw %08x isp %08x tsp %08x tcw %08x verts %u", (pcw >> 24) & 7, p, pcw, isp, tsp, tcw, nv);
            }
            pc.list = (int)((pcw >> 24) & 7);
            pc.sprite = (type == 5);
            pc.modvol = (pc.list == 1 || pc.list == 3);
            pc.coltype = (int)((pcw >> 4) & 3); pc.textured = (pcw >> 3) & 1; pc.offset = (pcw >> 2) & 1;
            pc.uv16 = pcw & 1; pc.volume = (pcw >> 6) & 1;
            uint32_t hsize = 32;
            if (!pc.sprite && !pc.modvol && ((pc.coltype == 2 && pc.offset) || (pc.volume && pc.coltype >= 2))) hsize = 64;
            if (pc.sprite) { pc.face = hle_rd32(p + 16); pc.face_off = hle_rd32(p + 20); }
            else if (pc.coltype >= 2) {
                pc.face = pack_floats(p + 16);
                pc.face_off = hsize == 64 ? pack_floats(p + 32) : 0;
            } else { pc.face = 0xFFFFFFFFu; pc.face_off = 0; }
            if (pc.list == want_list) {
                if (pc.modvol) stat_modvols++;
                else { state_from_header(&pc.state, pcw, isp, tsp, tcw, pc.list); if (pc.list < 5) { int k = pc.list == 2 ? 1 : pc.list == 4 ? 2 : 0; stat_polys[k]++; frame_polys[k]++; } }
            }
            pc.strip_start = nverts;
            p += hsize;
            continue;
        }
        /* end of list, user clip, object list set: 32 bytes each */
        p += 32;
    }
    if (in_strip) flush_strip(&pc, sorted);
}

static void render_lists(void) {
    uint32_t ends[3];
    for (int i = 0; i < 3; i++) {
        uint32_t base = bufs[i].base & 0x1FFFFFFFu;
        ends[i] = bufs[i].base ? (kamui_buffer_ptr(i) & 0x1FFFFFFFu) : 0;
        if (bufs[i].base && (ends[i] < base || ends[i] > base + bufs[i].size)) {
            sa1_log("kamui: %s pointer 0x%08x outside its buffer; frame dropped for that list", list_name[i], ends[i]);
            stat_overflow++;
            ends[i] = base;
        }
        if (bufs[i].base) { uint32_t used = ends[i] - base; if (used > buf_high[i]) buf_high[i] = used; }
    }
    draw_begin_frame(clear_color);
    nverts = 0; nstrips = 0;
    frame_polys[0] = frame_polys[1] = frame_polys[2] = 0;
    /* opaque, then punch-through, in the order written; both buffers may hold either */
    for (int l = 0; l < 2; l++) {
        int want = l == 0 ? 0 : 4;
        for (int i = 0; i < 3; i++) if (bufs[i].base) parse_region(bufs[i].base & 0x1FFFFFFFu, ends[i], want, 0);
    }
    /* translucent: collect from every buffer, sort far to near, draw */
    nverts = 0;
    for (int i = 0; i < 3; i++) if (bufs[i].base) parse_region(bufs[i].base & 0x1FFFFFFFu, ends[i], 2, 1);
    if (nstrips) {
        sort_key *arr = malloc(nstrips * sizeof *arr);
        if (arr) {
            for (uint32_t i = 0; i < nstrips; i++) { arr[i].d = strips[i].depth; arr[i].i = i; }
            qsort(arr, nstrips, sizeof *arr, cmp_key);
            for (uint32_t k = 0; k < nstrips; k++) {
                uint32_t i = arr[k].i;
                draw_set_state(&strip_states[i]);
                draw_strip(verts + strips[i].off, (int)strips[i].count);
            }
            free(arr);
        }
    }
    const draw_stats *ds = draw_get_stats();
    last_tris = ds->triangles; last_calls = ds->draw_calls;
    for (int i = 0; i < 3; i++) last_polys[i] = frame_polys[i];
    stat_frames++;
    frame_rendered = 1;
}

/* ---- the frame ------------------------------------------------------------- */
/* 0x8C642440: kmRender-like, called once per frame after the game drew:
 * render the lists.  0x8C641F00: flip; the buffers restart. */
void sdk_8c642440(sh4_ctx *c) {
    HLE_ENTER(0x8c642440u, "kmRender");
    (void)c;
    if (!bufs[0].base) return;
    render_lists();
}
void sdk_8c641f00(sh4_ctx *c) {
    HLE_ENTER(0x8c641f00u, "kmFlip");
    if (frame_rendered) {
        if (screenshot_path) {
            if (draw_screenshot(screenshot_path) == 0) sa1_log("screenshot: %s", screenshot_path);
            else sa1_log("screenshot: cannot write %s (headless?)", screenshot_path);
            screenshot_path = NULL;
        }
        draw_end_frame();
        frame_rendered = 0;
        frame_rendered_count++;
    }
    buffers_reset();
    RET(0);
}
void sdk_8c6433e0(sh4_ctx *c) {         /* no-draw frame: keep the picture, drop the lists */
    HLE_ENTER(0x8c6433e0u, "kmSkipFrame");
    (void)c;
    buffers_reset();
}

void kamui_set_screenshot(const char *path) { screenshot_path = path; }

void kamui_report(void) {
    sa1_log("render: %u frames presented; last frame %u draw calls, %u triangles, polygons op %u tr %u pt %u; "
            "per frame on average: polygons op %u tr %u pt %u, strips op %u tr %u",
            frame_rendered_count, last_calls, last_tris, last_polys[0], last_polys[1], last_polys[2],
            stat_frames ? stat_polys[0] / stat_frames : 0, stat_frames ? stat_polys[1] / stat_frames : 0, stat_frames ? stat_polys[2] / stat_frames : 0,
            stat_frames ? stat_strips[0] / stat_frames : 0, stat_frames ? stat_strips[1] / stat_frames : 0);
    sa1_log("render: textures %d (%u bytes); TA buffer high-water op %u tr %u pt %u bytes, %u overflows; %u modifier volumes skipped, %u sprites",
            texture_count(), texture_memory_bytes(), buf_high[0], buf_high[1], buf_high[2], stat_overflow, stat_modvols, stat_sprites);
}

/* ---- helpers for the native draw entries (draw2d.c) ------------------------ */
/* Save the context, apply polygon attributes (as 0x8C606A08), write a header
 * carrying the material (4 floats at mat, or white) to the list the
 * attributes select, and return that list.  kamui_draw_end restores. */
void kamui_draw_save(sh4_ctx *c) { sdk_8c605dac(c); }
int kamui_draw_header(sh4_ctx *c, uint32_t attr, uint32_t mat) {
    uint32_t saved_r4 = ARG0;
    ARG0 = attr; sdk_8c606a08(c);
    int list = (attr & 0x02000000u) ? KM_LIST_TR : KM_LIST_OP;
    uint32_t slot = list == KM_LIST_TR ? KM_CTX_PTR_TR : KM_CTX_PTR_OP;
    ARG0 = mat ? mat : 0x8C66CD78u;   /* the SDK's default material: 1,1,1,1 */
    uint32_t saved_r0 = c->r[0];
    write_header(c, slot, list == KM_LIST_OP);
    uint32_t next = (c->r[0] & 0x03FFFFFFu) | hle_rd32(G_CTX_TOP);   /* past the header */
    hle_wr32(slot, next);
    kamui_buffer_set(list, next);
    c->r[0] = saved_r0;
    ARG0 = saved_r4;
    return list;
}
int kamui_draw_begin(sh4_ctx *c, uint32_t attr, uint32_t mat, int tex_kind, uint32_t tex) {
    kamui_draw_save(c);       /* first: it overwrites the context copy, texture word included */
    if (tex_kind == KM_TEX_NUM) select_texture(tex, KM_CTX_TSP, KM_CTX_TCW);
    else if (tex_kind == KM_TEX_GINDEX) texture_select_gindex(tex);
    return kamui_draw_header(c, attr, mat);
}
/* Patch bits of the context copy between save and header (material flags). */
void kamui_ctx_patch(uint32_t isp_mask, uint32_t isp_val, uint32_t tsp_mask, uint32_t tsp_val) {
    hle_wr32(KM_CTX_ISP, (hle_rd32(KM_CTX_ISP) & ~isp_mask) | (isp_val & isp_mask));
    hle_wr32(KM_CTX_TSP, (hle_rd32(KM_CTX_TSP) & ~tsp_mask) | (tsp_val & tsp_mask));
}
int kamui_put_vertex(int list, uint32_t pcw, float x, float y, float z, float u, float v, uint32_t c6, uint32_t c7) {
    uint32_t w[8] = {pcw, sh4_f2u(x), sh4_f2u(y), sh4_f2u(z), sh4_f2u(u), sh4_f2u(v), c6, c7};
    return kamui_append(list, w);
}
void kamui_draw_end(sh4_ctx *c) {
    /* the vertices advanced the descriptor; hand the pointers back through the copy */
    hle_wr32(KM_CTX_PTR_OP, kamui_buffer_ptr(KM_LIST_OP));
    hle_wr32(KM_CTX_PTR_TR, kamui_buffer_ptr(KM_LIST_TR));
    hle_wr32(KM_CTX_PTR_PT, kamui_buffer_ptr(KM_LIST_PT));
    sdk_8c605e0a(c);
}
int kamui_select_texture(uint32_t n) { return select_texture(n, KM_CTX_TSP, KM_CTX_TCW); }
uint32_t kamui_clear_color(void) { return clear_color; }
