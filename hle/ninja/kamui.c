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
#include "../state.h"
#include "../shinobi/shinobi.h"

#include <stdio.h>
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
static char mark_path[512];               /* the mark key: screenshot + bound-texture list */
static int mark_active;
static uint32_t mark_tex[512], mark_ntex;
static uint32_t dump_frames[16], ndump_frames;   /* SA1_KAMUI_DUMP=<frame>[,<frame>...]: log every header, pointer handoff and writer call of those frames */
static int dump_read;              /* the variable is read on first use: a --load run never passes through kamui_init */
static void dump_setup(void) {
    if (dump_read) return;
    dump_read = 1;
    const char *e = getenv("SA1_KAMUI_DUMP");
    while (e && *e && ndump_frames < 16) {
        char *end;
        uint32_t f = (uint32_t)strtoul(e, &end, 0);
        if (end == e) break;
        if (f) dump_frames[ndump_frames++] = f;
        e = *end == ',' ? end + 1 : end;
    }
}
static int ptr_trace(void) {
    dump_setup();
    uint32_t f = hle_frame_count();
    for (uint32_t i = 0; i < ndump_frames; i++) if (f >= dump_frames[i] && f < dump_frames[i] + 2) return 1;
    return 0;
}
int kamui_ptr_trace(void) { return ptr_trace(); }
static int frame_rendered;
static uint32_t stat_frames, stat_polys[3], stat_strips[3], stat_modvols, stat_overflow, stat_sprites, stat_fogmode[4];
static uint64_t parse_us; static uint32_t parse_frames;     /* render_lists CPU time since the last report */
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

int hle_aspect_wide = 1;

/* 2D ranges: buffer spans written between kamui_mark2d_begin/end, per list. */
#define RANGES2D_MAX 256
static struct { uint32_t lo, hi; } ranges2d[3][RANGES2D_MAX];
static int nranges2d[3], mark_depth;
static uint32_t mark_start[3];
void kamui_mark2d_begin(void) {
    if (mark_depth++ == 0) for (int i = 0; i < 3; i++) mark_start[i] = kamui_buffer_ptr(i) & 0x1FFFFFFFu;
}
void kamui_mark2d_end(void) {
    if (mark_depth == 0 || --mark_depth != 0) return;
    for (int i = 0; i < 3; i++) {
        uint32_t end = kamui_buffer_ptr(i) & 0x1FFFFFFFu;
        if (end <= mark_start[i]) continue;
        int n = nranges2d[i];
        if (n && ranges2d[i][n - 1].hi == mark_start[i]) { ranges2d[i][n - 1].hi = end; continue; }
        if (n < RANGES2D_MAX) { ranges2d[i][n].lo = mark_start[i]; ranges2d[i][n].hi = end; nranges2d[i] = n + 1; }
    }
}
static int in_range2d(uint32_t p) {
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < nranges2d[i]; k++)
            if (p >= ranges2d[i][k].lo && p < ranges2d[i][k].hi) return 1;
    return 0;
}

static draw_vertex *dverts; static uint32_t ndverts, dverts_cap;   /* the TA bypass's host strips (docs/ta-direct.md) */

static void buffers_reset(void) {
    nranges2d[0] = nranges2d[1] = nranges2d[2] = 0; mark_depth = 0;
    ndverts = 0;
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
    dump_setup();
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
     * projected bounding spheres against: left, top, right, bottom, the
     * last pixel inclusive (the console holds 0, 0, 639, 479).  In wide
     * mode it is the 16:9 frame in the game's 640x480 space, so the objects
     * that only the wider frame shows are not culled (draw.h).  That is
     * game logic too: objects run parts of their update only while this
     * test passes (Emerald Coast's 0x8C0FDCE0 blends its animation only
     * then), so a wide run plays differently from the console; replays
     * that are compared with it run --aspect 4:3. */
    float left = hle_aspect_wide ? DRAW_WIDE_LEFT : 0.0f;
    hle_wrf(0x8C88F600u, left); hle_wrf(0x8C88F604u, 0.0f); hle_wrf(0x8C88F608u, 639.0f - left); hle_wrf(0x8C88F60Cu, 479.0f);
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

/* Table fog, as the hardware does it (PowerVR FOG_TABLE / FOG_DENSITY, what
 * Flycast's fog_mode2 implements): the 128-entry table is indexed by
 * z = density * (1/w), clamped to [1, 256): entry i covers
 * z from (16 + (i & 15)) / 16 * 2^(i >> 4), so entry 0 is the farthest
 * (w = density) and entry 127 the nearest (w = density / 248); beyond
 * either end the end entry applies.  FOG_DENSITY holds an 8-bit float:
 * mantissa (128..255, /128) in bits 15..8, signed exponent in bits 7..0.
 * The Ninja entries: njSetFogDensity (0x8C63FE3C -> 0x8C652E38) writes
 * that register word as given; njGenerateFogTable3 (0x8C63FB48) computes it
 * from |far| and writes a linear table (below); njGenerateFogTable
 * (0x8C63FDC0) writes the default curve 1/z (fog = w / density); njSetFogTable
 * (0x8C652DD0) uploads the 128 floats as bytes.  The backends have linear
 * fog only (sceGuFog), so a table is reduced to the w where it passes 0.02
 * and 0.98. */
static float fog_density = 1.0f;          /* FOG_DENSITY as a float: the w of entry 0 */
static uint32_t fog_density_reg = 0x8000u;
static float fog_table[128];
static int fog_table_set;

static float fog_entry_z(int i) { return (float)(16 + (i & 15)) / 16.0f * ldexpf(1.0f, i >> 4); }
static void fog_apply(const char *why) {
    if (fog_table_set) {
        float wn = -1.0f, wf = -1.0f;
        for (int i = 127; i >= 0; i--) {    /* from nearest to farthest */
            float w = fog_density / fog_entry_z(i), d = fog_table[i];
            if (wn < 0.0f && d > 0.02f) wn = w;
            if (wf < 0.0f && d > 0.98f) wf = w;
        }
        if (wn < 0.0f) wn = 65535.0f;
        if (wf < 0.0f) wf = 65535.0f;
        if (wf <= wn) wf = wn + 1.0f;
        fog_near = wn; fog_far = wf;
    }
    draw_set_fog(fog_color, fog_near, fog_far);
    /* entry 0 is the far end (w = density), entry 127 the near end (w = density / 248) */
    sa1_log("fog: %s: density %g (reg 0x%04x), colour 0x%08x -> linear from w %.1f (fog 0) to %.1f (fog 1); table far[0] %.3f, [64] %.3f (w %.1f), near[127] %.3f", why,
            fog_density, fog_density_reg, fog_color, fog_near, fog_far, fog_table[0], fog_table[64], fog_density / fog_entry_z(64), fog_table[127]);
}
static void fog_set_density_reg(uint32_t reg) {
    fog_density_reg = reg & 0xFFFFu;
    fog_density = (float)((reg >> 8) & 255) / 128.0f * ldexpf(1.0f, (int)(int8_t)(reg & 255));
}
/* |far| as the FOG_DENSITY word: 8 mantissa bits of far / 2^e with far / 2^e in [1, 2). */
static uint32_t fog_density_word(float afar) {
    int e; float m = frexpf(afar, &e);    /* afar = m * 2^e, m in [0.5, 1) */
    if (afar <= 0.0f) return 0x8000u;
    m *= 2.0f; e -= 1;
    uint32_t mant = (uint32_t)(m * 128.0f);
    if (mant > 255) mant = 255;
    return (mant & 0xFF) << 8 | ((uint32_t)e & 0xFF);
}

void sdk_8c63fe20(sh4_ctx *c) { HLE_ENTER(0x8c63fe20u, "njFogColor"); fog_color = ARG0; fog_apply("njFogColor"); }
void sdk_8c63fe3c(sh4_ctx *c) {         /* njSetFogDensity(register word) -> kmSetFogDensity 0x8C652E38 */
    HLE_ENTER(0x8c63fe3cu, "njSetFogDensity");
    fog_set_density_reg(ARG0);
    fog_apply("njSetFogDensity");
    RET(0);
}
void sdk_8c63fdc0(sh4_ctx *c) {         /* njGenerateFogTable(table): the default curve, 1/z */
    HLE_ENTER(0x8c63fdc0u, "njGenerateFogTable");
    for (int i = 0; i < 128; i++) hle_wrf(ARG0 + 4u * (uint32_t)i, 1.0f / fog_entry_z(i));
}
/* njGenerateFogTable3(table, near, far), near and far negative (view z):
 * density = |far| (so the table spans w up to |far|); with |near| within the
 * table's range the table is 0 at the first entry nearer than |near| and
 * rises linearly in w to 1 at |far|; otherwise it is the default curve. */
void sdk_8c63fb48(sh4_ctx *c) {
    HLE_ENTER(0x8c63fb48u, "njGenerateFogTable3");
    float n = c->fr[4], f = c->fr[5];
    uint32_t reg = n < -65535.0f ? 0x8010u : fog_density_word(-f);
    fog_set_density_reg(reg);
    float afar = -f, anear = -n;
    if (anear > afar / 256.0f) {
        int r7 = 0;
        for (int i = 0; i < 128; i++) if (afar / fog_entry_z(i) > anear) r7++;
        float wr = afar / fog_entry_z(r7 < 128 ? r7 : 127);   /* z of entry 128 would be 256 */
        if (r7 == 128) wr = afar / 256.0f;
        float span = afar - wr;
        for (int i = 0; i < 128; i++) {
            float d = 0.0f;
            if (i < r7) d = 1.0f - (afar - afar / fog_entry_z(i)) / span;
            hle_wrf(ARG0 + 4u * (uint32_t)i, d);
        }
    } else {
        for (int i = 0; i < 128; i++) hle_wrf(ARG0 + 4u * (uint32_t)i, 1.0f / fog_entry_z(i));
    }
    sa1_log("njGenerateFogTable3(0x%08x, near %.1f, far %.1f): density reg 0x%04x = %g", ARG0, n, f, reg, fog_density);
}
void sdk_8c63fe48(sh4_ctx *c) {         /* njSetFogTable(table) -> kmSetFogTable 0x8C652DD0 */
    HLE_ENTER(0x8c63fe48u, "njSetFogTable");
    for (int i = 0; i < 128; i++) fog_table[i] = hle_rdf(ARG0 + 4u * (uint32_t)i);
    fog_table_set = 1;
    fog_apply("njSetFogTable");
}

/* ---- the parser: TA parameters in guest RAM -> backend ------------------- */
/* Game mode 11 (the ADVERTISE overlay: title, main menu, character select)
 * frames its 3D for a 4:3 screen: drawn in the 4:3 area (draw.h). */
static int menu_mode;
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
    if (mark_active && s->texture) {
        uint32_t i = 0;
        while (i < mark_ntex && mark_tex[i] != s->texture) i++;
        if (i == mark_ntex && mark_ntex < 512) mark_tex[mark_ntex++] = s->texture;
    }
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
    stat_fogmode[s->fog]++;
    s->gouraud = (pcw >> 1) & 1;
    s->translucent = (list == 2);
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

draw_vertex *kamui_direct_strip(uint32_t n, uint32_t *first) {
    if (!grow((void **)&dverts, &dverts_cap, ndverts + n, sizeof *dverts)) return NULL;
    *first = ndverts;
    ndverts += n;
    return dverts + *first;
}

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

/* A strip the TA bypass wrote: [x, y, 1/w, u, v, colour, offset] as the
 * writer's vertex records hold them.  Under the header the writer is made
 * for (packed colour, float UV, offset) the host array is the strip; any
 * other header decodes the same words as parse_region decodes a record. */
static __attribute__((noinline)) void direct_strip(parse_ctx *pc, const draw_vertex *sv, uint32_t n, int sorted) {
    if (!sorted && pc->coltype == 0 && pc->textured && !pc->uv16 && pc->offset) {
        if (n < 3) return;
        draw_set_state(&pc->state);
        draw_strip(sv, (int)n);
        if (pc->list < 3) stat_strips[pc->list == 2 ? 1 : pc->list]++;
        return;
    }
    if (!grow((void **)&verts, &verts_cap, nverts + n, sizeof *verts)) return;
    pc->strip_start = nverts;
    for (uint32_t i = 0; i < n; i++) {
        const draw_vertex *s = &sv[i];
        draw_vertex *v = verts + nverts++;
        v->x = s->x; v->y = s->y; v->w = s->w;
        v->u = v->v = 0.0f; v->ocolor = 0;
        if (pc->textured) {
            if (pc->uv16) { uint32_t uv = sh4_f2u(s->u); v->u = sh4_u2f(uv & 0xFFFF0000u); v->v = sh4_u2f(uv << 16); }
            else { v->u = s->u; v->v = s->v; }
        }
        switch (pc->coltype) {
        case 0: v->color = s->color; if (pc->offset) v->ocolor = s->ocolor; break;
        case 1: v->color = nj_pack_argb(s->u, s->v, sh4_u2f(s->color), sh4_u2f(s->ocolor)); break;
        default: v->color = scale_argb(pc->face, sh4_u2f(s->color)); if (pc->offset) v->ocolor = scale_argb(pc->face_off, sh4_u2f(s->ocolor)); break;
        }
    }
    flush_strip(pc, sorted);
}

typedef struct sort_key { float d; uint32_t i; } sort_key;
/* Smaller 1/w = farther first.  Equal depth keeps the order the strips were
 * written in (qsort is not stable): the event scripts draw the subtitle's
 * black band and the text quad at the same 2D depth, and with the order
 * left to qsort it changed from frame to frame, the half-alpha band then
 * darkening the text every other frame (section 10 of the M2 report). */
static int cmp_key(const void *a, const void *b) {
    const sort_key *ka = a, *kb = b;
    if (ka->d != kb->d) return ka->d < kb->d ? -1 : 1;
    return ka->i < kb->i ? -1 : ka->i > kb->i ? 1 : 0;
}

/* The host bytes of a TA region in guest RAM (contiguous there: the buffers
 * sit in the 16 MB or the extra area above it), or NULL if it is not. */
static const uint8_t *ta_host(uint32_t start, uint32_t end) {
    uint32_t off = start & SA1_RAM_MASK;
    if (!hle_in_ram(start) || end < start || off + (end - start) > SA1_RAM_SIZE + SA1_RAM_EXTRA) {
        static int logged;
        if (!logged++) sa1_log("kamui: TA region 0x%08x..0x%08x not in RAM; skipped", start, end);
        return NULL;
    }
    return sa1_ram + off;
}

/* Vertex and global parameter sizes of a polygon or sprite header (pcw). */
static uint32_t ta_vsize(uint32_t pcw) {
    uint32_t list = (pcw >> 24) & 7, coltype = (pcw >> 4) & 3, textured = (pcw >> 3) & 1, volume = (pcw >> 6) & 1;
    if ((pcw >> 29) == 5 || list == 1 || list == 3) return 64;
    return ((textured && coltype == 1) || (volume && textured)) ? 64 : 32;
}
static uint32_t ta_hsize(uint32_t pcw) {
    uint32_t list = (pcw >> 24) & 7, coltype = (pcw >> 4) & 3, offset = (pcw >> 2) & 1, volume = (pcw >> 6) & 1;
    if ((pcw >> 29) == 5 || list == 1 || list == 3) return 32;
    return ((coltype == 2 && offset) || (volume && coltype >= 2)) ? 64 : 32;
}

/* One walk over a buffer that cuts it into runs of consecutive headers of
 * one list type, so that each list's parse visits its own runs only: the
 * three parses (opaque, punch-through, translucent) used to walk every
 * buffer whole, reading each vertex's word through the guest-address
 * helpers, which on the console was two thirds of parse_region (15 % of
 * the frame in Station Square, 2026-10-19). */
typedef struct ta_run { uint32_t lo, hi; int list; } ta_run;
static ta_run *runs[3]; static uint32_t nruns[3], runs_cap[3], ta_lo[3], ta_hi[3];
static void ta_runs(int b, uint32_t start, uint32_t end) {
    nruns[b] = 0; ta_lo[b] = start; ta_hi[b] = end;
    const uint8_t *hb = ta_host(start, end);
    if (!hb) return;
    uint32_t p = start, lo = start, vsize = 32;
    int list = -1;
    while (p + 32 <= end) {
        uint32_t pcw = *(const sa1_u32a *)(hb + (p - start)), type = pcw >> 29;
        if (type == 7) { if (p + vsize > end) break; p += vsize; continue; }
        if (type == 4 || type == 5) {
            int l = (int)((pcw >> 24) & 7);
            if (l != list) {
                if (p > lo && grow((void **)&runs[b], &runs_cap[b], nruns[b] + 1, sizeof (ta_run)))
                    runs[b][nruns[b]++] = (ta_run){lo, p, list};
                lo = p; list = l;
            }
            vsize = ta_vsize(pcw);
            p += ta_hsize(pcw);
            continue;
        }
        p += 32;
    }
    if (end > lo && grow((void **)&runs[b], &runs_cap[b], nruns[b] + 1, sizeof (ta_run)))
        runs[b][nruns[b]++] = (ta_run){lo, end, list};
}
static void parse_region(uint32_t start, uint32_t end, int want_list, int sorted);
static void parse_list(int want, int sorted) {
    if (want == 0 && ptr_trace()) {     /* the header dump logs every list's headers in the opaque parse */
        for (int b = 0; b < 3; b++) if (ta_hi[b] > ta_lo[b]) parse_region(ta_lo[b], ta_hi[b], 0, 0);
        return;
    }
    for (int b = 0; b < 3; b++)
        for (uint32_t k = 0; k < nruns[b]; k++)
            if (runs[b][k].list == want) parse_region(runs[b][k].lo, runs[b][k].hi, want, sorted);
}

/* The TA buffers are a few hundred KB the game wrote this frame, read once,
 * front to back: on the PSP, ask for the line TA_AHEAD bytes on while this one
 * is parsed (cache op 0x1E, Fill).  Bytes past the region are still guest RAM
 * (ta_host), so the hint never faults. */
#ifdef __psp__
#define TA_AHEAD 512
#define TA_FILL(ptr) __asm__ volatile("cache 0x1e, 0(%0)" : : "r"(ptr))
#else
#define TA_AHEAD 0
#define TA_FILL(ptr) ((void)(ptr))
#endif

/* Parse one buffer region.  Polygons of list types 0 and 4 draw at once
 * (sorted = 0); translucent ones (2) are collected for sorting. */
static void parse_region(uint32_t start, uint32_t end, int want_list, int sorted) {
    const uint8_t *hb = ta_host(start, end);
    if (!hb) return;
#define TRD32(a) (*(const sa1_u32a *)(hb + ((a) - start)))
#define TRDF(a)  (*(const sa1_f32a *)(hb + ((a) - start)))
#define TPACK(a) nj_pack_argb(TRDF(a), TRDF((a) + 4), TRDF((a) + 8), TRDF((a) + 12))
    parse_ctx pc; memset(&pc, 0, sizeof pc);
    pc.list = -1; pc.strip_start = nverts;
    uint32_t p = start;
    int in_strip = 0;
    while (p + 32 <= end) {
        if (TA_AHEAD && !((uintptr_t)(hb + (p - start)) & 63) && p + TA_AHEAD < end) TA_FILL(hb + (p - start) + TA_AHEAD);
        uint32_t pcw = TRD32(p);
        uint32_t type = pcw >> 29;
        if (type == 7) {                                   /* vertex */
            uint32_t vsize = pc.modvol ? 64 : ((pc.textured && pc.coltype == 1) || (pc.volume && pc.textured)) ? 64 : 32;
            if (pc.sprite) vsize = 64;
            if (p + vsize > end) break;
            if (pc.list == want_list && !pc.modvol) {
                if (pc.sprite) {
                    /* 4 points a,b,c,d (d has no z: it takes c's) and 3 packed uv pairs */
                    if (grow((void **)&verts, &verts_cap, nverts + 4, sizeof *verts)) {
                        float ax = TRDF(p + 4), ay = TRDF(p + 8), az = TRDF(p + 12);
                        float bx = TRDF(p + 16), by = TRDF(p + 20), bz = TRDF(p + 24);
                        float cx = TRDF(p + 28), cy = TRDF(p + 32), cz = TRDF(p + 36);
                        float dx = TRDF(p + 40), dy = TRDF(p + 44);
                        uint32_t uva = TRD32(p + 52), uvb = TRD32(p + 56), uvc = TRD32(p + 60);
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
                        v->x = TRDF(p + 4); v->y = TRDF(p + 8); v->w = TRDF(p + 12);
                        v->u = v->v = 0.0f; v->ocolor = 0;
                        uint32_t q = p + 16;
                        if (pc.textured) {
                            if (pc.uv16) { uint32_t uv = TRD32(q); v->u = sh4_u2f(uv & 0xFFFF0000u); v->v = sh4_u2f(uv << 16); q += 8; }
                            else { v->u = TRDF(q); v->v = TRDF(q + 4); q += 8; }
                        } else q += 8;
                        switch (pc.coltype) {
                        case 0: v->color = TRD32(q); if (pc.offset) v->ocolor = TRD32(q + 4); break;
                        case 1:
                            if (pc.textured) { v->color = TPACK(p + 32); if (pc.offset) v->ocolor = TPACK(p + 48); }
                            else v->color = TPACK(q);
                            break;
                        default: v->color = scale_argb(pc.face, TRDF(q)); if (pc.offset) v->ocolor = scale_argb(pc.face_off, TRDF(q + 4)); break;
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
        if (type == 6) {                                   /* a TA-bypass strip (ta_direct.c): host vertices */
            uint32_t first = TRD32(p + 4), n = TRD32(p + 8);
            if (pc.list == want_list && !pc.modvol && !pc.sprite && first <= ndverts && n <= ndverts - first)
                direct_strip(&pc, dverts + first, n, sorted);
            p += 32;
            continue;
        }
        if (type == 4 || type == 5) {
            uint32_t isp = TRD32(p + 4), tsp = TRD32(p + 8), tcw = TRD32(p + 12);
            if (ptr_trace() && want_list == 0 && !sorted) {
                /* count the vertices that follow, with their screen extent and 1/w range */
                uint32_t q = p + 32, nv = 0;
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, w0 = 1e9f, w1 = -1e9f;
                uint32_t vs = (type == 5) ? 64 : ((((pcw >> 3) & 1) && ((pcw >> 4) & 3) == 1) || (((pcw >> 6) & 1) && ((pcw >> 3) & 1))) ? 64 : 32;
                while (q + vs <= end && (hle_rd32(q) >> 29) == 7) {
                    float x = hle_rdf(q + 4), y = hle_rdf(q + 8), w = hle_rdf(q + 12);
                    if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; if (w < w0) w0 = w; if (w > w1) w1 = w;
                    nv++; q += vs;
                }
                sa1_log("hdr list %u at 0x%08x: pcw %08x isp %08x tsp %08x tcw %08x verts %u x %.0f..%.0f y %.0f..%.0f 1/w %.5f..%.5f col %08x",
                        (pcw >> 24) & 7, p, pcw, isp, tsp, tcw, nv, x0, x1, y0, y1, w0, w1, nv ? hle_rd32(p + 32 + 24) : 0);
                {   /* the translucency audit: every TA flag decoded, and the texture's alpha */
                    static const char *lname[8] = {"opaque", "opmod", "translucent", "trmod", "punch", "?5", "?6", "?7"};
                    static const char *dname[8] = {"never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"};
                    static const char *bname[8] = {"zero", "one", "othercol", "invothercol", "srca", "invsrca", "dsta", "invdsta"};
                    static const char *fname[4] = {"table", "vertex", "none", "table2"};
                    static const char *cname[4] = {"packed", "float", "intensity", "intensity2"};
                    static const char *sname[4] = {"decal", "modulate", "decalalpha", "modulatealpha"};
                    static const char *tname[8] = {"1555", "565", "4444", "yuv", "bump", "pal4", "pal8", "?"};
                    uint32_t list = (pcw >> 24) & 7;
                    char tex[96] = "none";
                    if (pcw & 8) {
                        uint32_t h = texture_from_tcw(tcw);
                        int amin = 255, ai = texture_alpha_info(h, &amin);
                        snprintf(tex, sizeof tex, "%s h%u %s alpha=%s amin=%d", tname[(tcw >> 27) & 7], h, texture_name(h),
                                 ai == TEX_ALPHA_USED ? "used" : ai == TEX_ALPHA_OPAQUE ? "opaque" : ai == TEX_ALPHA_NONE ? "none" : "?", amin);
                    }
                    uint32_t face = type == 5 ? hle_rd32(p + 16) : ((pcw >> 4) & 3) >= 2 ? pack_floats(p + 16) : 0;
                    sa1_log("  audit: %s%s depth=%s zwrite=%s cull=%u blend=%s/%s usealpha=%u igntexalpha=%u fog=%s clamp=%u%u flip=%u%u col=%s face=%08x shade=%s%s%s tex=%s",
                            lname[list], type == 5 ? " sprite" : "", dname[(isp >> 29) & 7], (isp & (1u << 26)) ? "off" : "on", (isp >> 27) & 3,
                            bname[(tsp >> 29) & 7], bname[(tsp >> 26) & 7], (tsp >> 20) & 1, (tsp >> 19) & 1, fname[(tsp >> 22) & 3],
                            (tsp >> 16) & 1, (tsp >> 15) & 1, (tsp >> 18) & 1, (tsp >> 17) & 1, cname[(pcw >> 4) & 3], face, sname[(tsp >> 6) & 3],
                            (pcw & 4) ? " offset" : "", (pcw & 2) ? " gouraud" : "", tex);
                }
                const char *dv = getenv("SA1_KAMUI_DUMP_VERTS");   /* 1: the odd strips' vertices; 2: every strip of up to 8 vertices too */
                if (dv && nv && nv <= 64 && ((w1 >= 1.0f || x1 > 2000.0f || x0 < -2000.0f || y0 < -2000.0f || y1 > 2000.0f) || (dv[0] == '2' && nv <= 8))) {
                    uint32_t r = p + 32;
                    sa1_log("    header words %08x %08x %08x %08x", hle_rd32(p + 16), hle_rd32(p + 20), hle_rd32(p + 24), hle_rd32(p + 28));
                    for (uint32_t k = 0; k < nv; k++, r += vs)
                        sa1_log("    v%u (%.1f, %.1f) 1/w %.5f +16 %08x +20 %08x +24 %08x +28 %08x", k, hle_rdf(r + 4), hle_rdf(r + 8), hle_rdf(r + 12), hle_rd32(r + 16), hle_rd32(r + 20), hle_rd32(r + 24), hle_rd32(r + 28));
                }
            }
            pc.list = (int)((pcw >> 24) & 7);
            pc.sprite = (type == 5);
            pc.modvol = (pc.list == 1 || pc.list == 3);
            pc.coltype = (int)((pcw >> 4) & 3); pc.textured = (pcw >> 3) & 1; pc.offset = (pcw >> 2) & 1;
            pc.uv16 = pcw & 1; pc.volume = (pcw >> 6) & 1;
            uint32_t hsize = 32;
            if (!pc.sprite && !pc.modvol && ((pc.coltype == 2 && pc.offset) || (pc.volume && pc.coltype >= 2))) hsize = 64;
            if (pc.sprite) { pc.face = TRD32(p + 16); pc.face_off = TRD32(p + 20); }
            else if (pc.coltype >= 2) {
                pc.face = TPACK(p + 16);
                pc.face_off = hsize == 64 ? TPACK(p + 32) : 0;
            } else { pc.face = 0xFFFFFFFFu; pc.face_off = 0; }
            if (pc.list == want_list) {
                if (pc.modvol) stat_modvols++;
                else { state_from_header(&pc.state, pcw, isp, tsp, tcw, pc.list); pc.state.layer = (uint8_t)(in_range2d(p) ? DRAW_LAYER_2D : menu_mode ? DRAW_LAYER_3D_43 : DRAW_LAYER_3D); if (pc.list < 5) { int k = pc.list == 2 ? 1 : pc.list == 4 ? 2 : 0; stat_polys[k]++; frame_polys[k]++; } }
            }
            pc.strip_start = nverts;
            p += hsize;
            continue;
        }
        /* end of list, user clip, object list set: 32 bytes each */
        p += 32;
    }
    if (in_strip) flush_strip(&pc, sorted);
#undef TRD32
#undef TRDF
#undef TPACK
}

static void render_lists(void) {
    uint64_t t_start = sa1_plat_time_us();
    menu_mode = hle_rd16(0x8C760970u) == 11;
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
    for (int i = 0; i < 3; i++) { nruns[i] = 0; ta_lo[i] = ta_hi[i] = 0; if (bufs[i].base) ta_runs(i, bufs[i].base & 0x1FFFFFFFu, ends[i]); }
    /* opaque, then punch-through, in the order written; both buffers may hold either */
    parse_list(0, 0);
    parse_list(4, 0);
    /* translucent: collect from every buffer, sort far to near, draw */
    nverts = 0;
    parse_list(2, 1);
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
    parse_us += sa1_plat_time_us() - t_start; parse_frames++;
}

/* ---- the frame ------------------------------------------------------------- */
static void mark_finish(void);

/* 0x8C642440: kmRender-like, called once per frame after the game drew:
 * render the lists.  0x8C641F00: flip; the buffers restart. */
void sdk_8c642440(sh4_ctx *c) {
    HLE_ENTER(0x8c642440u, "kmRender");
    (void)c;
    if (!bufs[0].base) return;
    if (ptr_trace())
        sa1_log("kamui: kmRender (frame %u) op 0x%08x tr 0x%08x pt 0x%08x", hle_frame_count(), kamui_buffer_ptr(0), kamui_buffer_ptr(1), kamui_buffer_ptr(2));
    render_lists();
}
void sdk_8c641f00(sh4_ctx *c) {
    HLE_ENTER(0x8c641f00u, "kmFlip");
    if (ptr_trace())
        sa1_log("kamui: kmFlip (frame %u, rendered %d) tr 0x%08x", hle_frame_count(), frame_rendered, kamui_buffer_ptr(1));
    if (frame_rendered) {
        if (screenshot_path) {
            if (draw_screenshot(screenshot_path) == 0) sa1_log("screenshot: %s", screenshot_path);
            else sa1_log("screenshot: cannot write %s (headless?)", screenshot_path);
            screenshot_path = NULL;
        }
        mark_finish();
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
    if (ptr_trace())
        sa1_log("kamui: kmSkipFrame (frame %u) tr 0x%08x", hle_frame_count(), kamui_buffer_ptr(1));
    buffers_reset();
}

void kamui_set_screenshot(const char *path) { screenshot_path = path; }

void kamui_mark_frame(const char *dir) {
    snprintf(mark_path, sizeof mark_path, "%s/mark_%u.ppm", dir && *dir ? dir : ".", hle_frame_count());
    screenshot_path = mark_path;
    mark_active = 1; mark_ntex = 0;
}
static void mark_finish(void) {
    if (!mark_active) return;
    mark_active = 0;
    sa1_log("mark: frame %u, %u textures bound (%s):", hle_frame_count(), mark_ntex, mark_path);
    char line[256], one[64]; unsigned used = 0;
    line[0] = 0;
    for (uint32_t i = 0; i < mark_ntex; i++) {
        texture_describe(mark_tex[i], one, sizeof one);
        if (used + strlen(one) + 4 > sizeof line) { sa1_log("mark:   %s", line); line[0] = 0; used = 0; }
        used += (unsigned)snprintf(line + used, sizeof line - used, "%s[%s]", used ? "  " : "", one);
    }
    if (used) sa1_log("mark:   %s", line);
}

void kamui_report(void) {
    sa1_log("render: %u frames presented; last frame %u draw calls, %u triangles, polygons op %u tr %u pt %u; "
            "per frame on average: polygons op %u tr %u pt %u, strips op %u tr %u",
            frame_rendered_count, last_calls, last_tris, last_polys[0], last_polys[1], last_polys[2],
            stat_frames ? stat_polys[0] / stat_frames : 0, stat_frames ? stat_polys[1] / stat_frames : 0, stat_frames ? stat_polys[2] / stat_frames : 0,
            stat_frames ? stat_strips[0] / stat_frames : 0, stat_frames ? stat_strips[1] / stat_frames : 0);
    sa1_log("render: textures %d (%u bytes); TA buffer high-water op %u tr %u pt %u bytes, %u overflows; %u modifier volumes skipped, %u sprites",
            texture_count(), texture_memory_bytes(), buf_high[0], buf_high[1], buf_high[2], stat_overflow, stat_modvols, stat_sprites);
    sa1_log("render: polygon headers by fog mode: table %u, per-vertex %u, none %u, table2 %u", stat_fogmode[0], stat_fogmode[1], stat_fogmode[2], stat_fogmode[3]);
    sa1_log("render: TA parse and record %.2f ms/frame over %u frames", parse_frames ? parse_us / 1000.0 / parse_frames : 0.0, parse_frames);
    parse_us = 0; parse_frames = 0;
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

/* ---- save states (hle/state.h, docs/savestate.md) ----------------------------
 * The context addresses, the buffers, the clear colour, the fog model and
 * the 2D ranges of the frame in progress.  Statistics and the per-render
 * scratch arrays are not state.  After a load the fog is pushed to the
 * backend again (GL fog is native). */
void kamui_state(hle_state_io *io) {
    hle_state_blob(io, "km.sh", &sh_addr, sizeof sh_addr);
    hle_state_blob(io, "km.desc", &desc_addr, sizeof desc_addr);
    hle_state_blob(io, "km.bufs", bufs, sizeof bufs);
    hle_state_blob(io, "km.clear", &clear_color, sizeof clear_color);
    hle_state_blob(io, "km.fog.near", &fog_near, sizeof fog_near);
    hle_state_blob(io, "km.fog.far", &fog_far, sizeof fog_far);
    hle_state_blob(io, "km.fog.color", &fog_color, sizeof fog_color);
    hle_state_blob(io, "km.fog.dens", &fog_density, sizeof fog_density);
    hle_state_blob(io, "km.fog.reg", &fog_density_reg, sizeof fog_density_reg);
    hle_state_blob(io, "km.fog.table", fog_table, sizeof fog_table);
    hle_state_blob(io, "km.fog.set", &fog_table_set, sizeof fog_table_set);
    hle_state_blob(io, "km.r2d", ranges2d, sizeof ranges2d);
    hle_state_blob(io, "km.r2d.n", nranges2d, sizeof nranges2d);
    hle_state_blob(io, "km.mark.depth", &mark_depth, sizeof mark_depth);
    hle_state_blob(io, "km.mark.start", mark_start, sizeof mark_start);
    hle_state_blob(io, "km.rendered", &frame_rendered, sizeof frame_rendered);
    if (hle_state_loading(io) && !hle_state_error(io)) draw_set_fog(fog_color, fog_near, fog_far);
}
