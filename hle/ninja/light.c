/* light.c - Ninja lights: the NJS_LIGHT records the game owns, filled as
 * the SDK filled them.  The game's own renderer does its lighting from
 * its own tables (no game code reads the Ninja light list at 0x8C8EA3E0),
 * so these entries only keep the records consistent for a native model
 * renderer.  Layout from njCreateLight (0x8C63AA94):
 *   +0 matrix (16 floats)   +0x40..+0x50 zeros, +0x54 -1.0, +0x58 enable
 *   +0x4C..+0x54 direction (set by njSetLightDirection, normalised; copy at +0x9C)
 *   +0x68 near^2, +0x6C far^2, +0x70 cos^2 inner, +0x74 cos^2 outer,
 *   +0x78 1/(far-near), +0x7C far/(far-near), +0x8C inner|outer<<16
 *   +0xB4..+0xF0 premultiplied colours (intensity x colour)
 *   +0x124 type, +0x128 ambient, +0x12C diffuse, +0x130 specular
 *   +0x134 near, +0x138 far, +0x140 inner angle, +0x144 outer angle
 *   +0x14C r, +0x150 g, +0x154 b
 *
 * Part of sa1-psp (MIT). */
#include "ninja.h"

#include <math.h>

#define LIGHT_LIST  0x8C8EA3E0u
#define LIGHT_COUNT (LIGHT_LIST + 0x80)
#define FARG(i) (c->fr[4 + (i)])
static const float TURN = (float)(6.283185307179586 / 65536.0);

static void premultiply(uint32_t l) {
    float r = hle_rdf(l + 0x14C), g = hle_rdf(l + 0x150), b = hle_rdf(l + 0x154);
    float amb = hle_rdf(l + 0x128), dif = hle_rdf(l + 0x12C), spe = hle_rdf(l + 0x130);
    const float k[4] = {amb, dif, spe, amb + dif};
    for (int i = 0; i < 4; i++) {
        uint32_t a = l + 0xB4 + 16u * (uint32_t)i;
        hle_wrf(a, k[i] * r); hle_wrf(a + 4, k[i] * g); hle_wrf(a + 8, k[i] * b); hle_wrf(a + 12, 1.0f);
    }
}
static void ranges(uint32_t l) {
    float n = hle_rdf(l + 0x134), f = hle_rdf(l + 0x138);
    hle_wrf(l + 0x68, n * n); hle_wrf(l + 0x6C, f * f);
    float inv = (f - n) != 0.0f ? 1.0f / (f - n) : 0.0f;
    hle_wrf(l + 0x78, inv); hle_wrf(l + 0x7C, f * inv);
}
static void angles(uint32_t l) {
    uint32_t in = hle_rd32(l + 0x140) & 0xFFFF, out = hle_rd32(l + 0x144) & 0xFFFF;
    float ci = cosf((float)in * TURN), co = cosf((float)out * TURN);
    hle_wrf(l + 0x70, ci * ci); hle_wrf(l + 0x74, co * co);
    hle_wr32(l + 0x8C, in | out << 16);
}

void sdk_8c63aa94(sh4_ctx *c) {         /* njCreateLight(light, type) */
    HLE_ENTER(0x8c63aa94u, "njCreateLight");
    uint32_t l = ARG0, n = hle_rd32(LIGHT_COUNT);
    if (n >= 32) return;
    hle_wr32(LIGHT_LIST + 4 * n, l); hle_wr32(LIGHT_COUNT, n + 1);
    for (uint32_t i = 0; i < 16; i++) hle_wrf(l + 4 * i, (i % 5 == 0) ? 1.0f : 0.0f);
    for (uint32_t a = 0x40; a <= 0x50; a += 4) hle_wr32(l + a, 0);
    hle_wrf(l + 0x54, -1.0f); hle_wr32(l + 0x58, 1);
    for (uint32_t a = 0x90; a <= 0xA0; a += 4) hle_wr32(l + a, 0);
    hle_wrf(l + 0xA4, -1.0f);
    hle_wr32(l + 0x124, ARG1);
    hle_wrf(l + 0x128, 0.2f); hle_wrf(l + 0x12C, 1.0f); hle_wrf(l + 0x130, 0.3f);
    hle_wrf(l + 0x134, 1.0f); hle_wrf(l + 0x138, 65535.0f);
    hle_wr32(l + 0x140, 0x071C); hle_wr32(l + 0x144, 0x1555); hle_wr32(l + 0x148, 0);
    hle_wrf(l + 0x14C, 1.0f); hle_wrf(l + 0x150, 1.0f); hle_wrf(l + 0x154, 1.0f);
    hle_wrf(l + 0x84, 1.0f); hle_wr32(l + 0x88, 0);
    ranges(l); angles(l); premultiply(l);
}
void sdk_8c63ac14(sh4_ctx *c) {         /* njDeleteLight(light) */
    HLE_ENTER(0x8c63ac14u, "njDeleteLight");
    uint32_t n = hle_rd32(LIGHT_COUNT);
    for (uint32_t i = 0; i < n && i < 32; i++) {
        if (hle_rd32(LIGHT_LIST + 4 * i) != ARG0) continue;
        for (uint32_t j = i; j + 1 < n; j++) hle_wr32(LIGHT_LIST + 4 * j, hle_rd32(LIGHT_LIST + 4 * (j + 1)));
        hle_wr32(LIGHT_COUNT, n - 1);
        return;
    }
}
void sdk_8c63aee8(sh4_ctx *c) {         /* njSetLightIntensity(light, spe, dif, amb) */
    HLE_ENTER(0x8c63aee8u, "njSetLightIntensity");
    hle_wrf(ARG0 + 0x130, FARG(0)); hle_wrf(ARG0 + 0x12C, FARG(1)); hle_wrf(ARG0 + 0x128, FARG(2));
    premultiply(ARG0);
}
void sdk_8c63ae2c(sh4_ctx *c) {         /* njSetLightColor(light, r, g, b) */
    HLE_ENTER(0x8c63ae2cu, "njSetLightColor");
    hle_wrf(ARG0 + 0x14C, FARG(0)); hle_wrf(ARG0 + 0x150, FARG(1)); hle_wrf(ARG0 + 0x154, FARG(2));
    premultiply(ARG0);
}
void sdk_8c63adc0(sh4_ctx *c) {         /* njSetLightAngle(light, inner, outer) */
    HLE_ENTER(0x8c63adc0u, "njSetLightAngle");
    hle_wr32(ARG0 + 0x140, ARG1); hle_wr32(ARG0 + 0x144, ARG2);
    angles(ARG0);
}
void sdk_8c63ae90(sh4_ctx *c) {         /* njSetLightDirection(light, x, y, z) */
    HLE_ENTER(0x8c63ae90u, "njSetLightDirection");
    float x = FARG(0), y = FARG(1), z = FARG(2), l = sqrtf(x * x + y * y + z * z);
    if (l > 0.0f) { x /= l; y /= l; z /= l; }
    hle_wrf(ARG0 + 0x4C, x); hle_wrf(ARG0 + 0x50, y); hle_wrf(ARG0 + 0x54, z);
    hle_wrf(ARG0 + 0x9C, x); hle_wrf(ARG0 + 0xA0, y); hle_wrf(ARG0 + 0xA4, z);
}
void sdk_8c63af48(sh4_ctx *c) {         /* njSetLightRange(light, near, far) */
    HLE_ENTER(0x8c63af48u, "njSetLightRange");
    hle_wrf(ARG0 + 0x134, FARG(0)); hle_wrf(ARG0 + 0x138, FARG(1));
    ranges(ARG0);
}
void sdk_8c63ac78(sh4_ctx *c) { HLE_ENTER(0x8c63ac78u, "njLightOn"); hle_wr32(ARG0 + 0x58, 1); }
void sdk_8c63af98(sh4_ctx *c) {         /* njUnitMatrix on the light's matrix */
    HLE_ENTER(0x8c63af98u, "njLightUnitMatrix");
    for (uint32_t i = 0; i < 16; i++) hle_wrf(ARG0 + 4 * i, (i % 5 == 0) ? 1.0f : 0.0f);
}
void sdk_8c63af80(sh4_ctx *c) {         /* njTranslateV on the light's matrix */
    HLE_ENTER(0x8c63af80u, "njLightTranslateV");
    uint32_t m = ARG0, v = ARG1;
    float x = hle_rdf(v), y = hle_rdf(v + 4), z = hle_rdf(v + 8);
    for (int i = 0; i < 3; i++)
        hle_wrf(m + 48 + 4u * (uint32_t)i, hle_rdf(m + 4u * (uint32_t)i) * x + hle_rdf(m + 16 + 4u * (uint32_t)i) * y + hle_rdf(m + 32 + 4u * (uint32_t)i) * z + hle_rdf(m + 48 + 4u * (uint32_t)i));
}
