/* ninja.h - internal interface of the Ninja/KAMUI HLE (the hle/ninja sources).
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include "../hle.h"
#include "../seam.h"
#include "draw.h"

/* kamui.c: render context, TA parameter buffers, frame */
void kamui_init(void);                        /* called from njInitSystem */
/* The three TA buffer slots the game writes through (opaque, translucent,
 * punch-through); each returns the current write pointer (guest address)
 * and takes the advanced one back.  Native draw entries use them to append
 * parameters exactly as game code does. */
enum { KM_LIST_OP = 0, KM_LIST_TR = 1, KM_LIST_PT = 2 };
uint32_t kamui_buffer_ptr(int list);
void kamui_buffer_set(int list, uint32_t ptr);
/* Appends a 32-byte block of words to a list; returns 0 when full. */
int kamui_append(int list, const uint32_t w[8]);
/* The four compiled header words of the current context (0x8C890104). */
#define KM_CTX 0x8C890104u
#define KM_CTX_PCW (KM_CTX + 0)
#define KM_CTX_ISP (KM_CTX + 4)
#define KM_CTX_TSP (KM_CTX + 8)
#define KM_CTX_TCW (KM_CTX + 12)
#define KM_CTX_PTR_OP (KM_CTX + 16)
#define KM_CTX_PTR_TR (KM_CTX + 20)
#define KM_CTX_PTR_PT (KM_CTX + 24)
void kamui_report(void);
enum { KM_TEX_NONE = 0, KM_TEX_NUM = 1, KM_TEX_GINDEX = 2 };   /* how tex names the texture */
int kamui_draw_begin(sh4_ctx *c, uint32_t attr, uint32_t mat, int tex_kind, uint32_t tex);
void kamui_draw_save(sh4_ctx *c);
int kamui_draw_header(sh4_ctx *c, uint32_t attr, uint32_t mat);
void kamui_ctx_patch(uint32_t isp_mask, uint32_t isp_val, uint32_t tsp_mask, uint32_t tsp_val);
int kamui_put_vertex(int list, uint32_t pcw, float x, float y, float z, float u, float v, uint32_t c6, uint32_t c7);
void kamui_draw_end(sh4_ctx *c);
/* Bracket writes of 2D elements (HUD, menus, 2D sprites): the parameters
 * written in between are drawn in the 4:3 area (draw.h, DRAW_LAYER_2D). */
int kamui_ptr_trace(void);            /* SA1_KAMUI_DUMP: this frame is being traced */
void kamui_mark2d_begin(void);
void kamui_mark2d_end(void);
int kamui_select_texture(uint32_t n);
/* polygon attribute words as 0x8C606A08 takes them */
#define KM_ATTR_OPAQUE_TEX   0x0008000Au
#define KM_ATTR_TRANS_TEX    0x0210000Au
#define KM_ATTR_OPAQUE_FLAT  0x00080002u
#define KM_ATTR_TRANS_FLAT   0x02100002u
#define KM_ATTR_OPAQUE_INT   0x00080028u   /* intensity colour, textured (sprites) */
#define KM_ATTR_TRANS_INT    0x02100028u

/* texture.c */
void texture_init(void);
/* Resolves a TCW to a backend texture handle (0 when none). */
uint32_t texture_from_tcw(uint32_t tcw);
/* The alpha channel of a live texture: TEX_ALPHA_NONE (565/YUV), _OPAQUE
 * (a format with alpha, every texel 255) or _USED; *amin the smallest texel
 * alpha.  -1 for an unknown handle.  For the translucency audit. */
enum { TEX_ALPHA_NONE = 0, TEX_ALPHA_OPAQUE = 1, TEX_ALPHA_USED = 2 };
int texture_alpha_info(uint32_t handle, int *amin);
const char *texture_name(uint32_t handle);     /* the archive entry name of a live texture ("?" if none) */
void texture_set_dump_dir(const char *dir, unsigned min_w);   /* --texdump: every decode as PPM */
int texture_describe(uint32_t handle, char *buf, unsigned n);
/* The mark key (F9): screenshot of the next rendered frame into dir, and
 * the textures bound in it listed in the log. */
void kamui_mark_frame(const char *dir);
int texture_select_gindex(uint32_t gbix);
uint32_t texture_memory_bytes(void);
int texture_count(void);

/* Current Ninja texlist (0x8C673480) and the selected texture (0x8C67348C gbix, 0x8C673490 bank). */
#define NJ_CUR_TEXLIST 0x8C673480u
#define NJ_CUR_GINDEX    0x8C67348Cu
#define NJ_CUR_BANK    0x8C673490u

/* Screen globals shared with math.c */
#define NJ_G_DIST  0x8C88F5E4u
#define NJ_G_W     0x8C88F5E8u
#define NJ_G_H     0x8C88F5ECu
#define NJ_G_CX    0x8C88F5F0u
#define NJ_G_CY    0x8C88F5F4u
#define NJ_G_CLIPN 0x8C88F610u
#define NJ_G_CLIPF 0x8C88F614u
#define NJ_G_AX    0x8C88F618u
#define NJ_G_AY    0x8C88F61Cu
#define NJ_G_CTRL3D 0x8C88F62Cu
#define NJ_G_CONST_AND 0x8C88F660u
#define NJ_G_CONST_OR  0x8C88F664u
#define NJ_G_CONST_MAT 0x8C88F668u

/* packed ARGB helpers */
static inline uint32_t nj_pack_argb(float a, float r, float g, float b) {
    int ia = (int)(a * 255.0f + 0.5f), ir = (int)(r * 255.0f + 0.5f), ig = (int)(g * 255.0f + 0.5f), ib = (int)(b * 255.0f + 0.5f);
    if (ia < 0) ia = 0; if (ia > 255) ia = 255;
    if (ir < 0) ir = 0; if (ir > 255) ir = 255;
    if (ig < 0) ig = 0; if (ig > 255) ig = 255;
    if (ib < 0) ib = 0; if (ib > 255) ib = 255;
    return ((uint32_t)ia << 24) | ((uint32_t)ir << 16) | ((uint32_t)ig << 8) | (uint32_t)ib;
}
