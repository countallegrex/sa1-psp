/* pvr_decode_test.c - hle/ninja/pvr_decode.c on synthetic textures: the
 * twiddle order, the mipmap chain offsets (the sky-dome bug of M2: the
 * largest level of a twiddled 16-bit mipmapped texture starts one dummy
 * texel into the chain, not three), VQ with and without mipmaps, the
 * rectangle formats and the global-index chunk.  No game data.
 *
 *   cc -std=c11 -Ihle/ninja tests/ninja/pvr_decode_test.c hle/ninja/pvr_decode.c -o pvr_decode_test && ./pvr_decode_test
 *
 * Part of sa1-psp (MIT). */
#include "pvr_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* The reference Morton order: y in the even bits, x in the odd bits. */
static uint32_t morton(uint32_t x, uint32_t y) {
    uint32_t r = 0;
    for (int b = 0; b < 10; b++) r |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
    return r;
}
/* A texel value that identifies its position: RGB565 with x in red, y in green. */
static uint32_t texel565(uint32_t x, uint32_t y) { return (x & 31) << 11 | (y & 63) << 5 | ((x + y) & 31); }
/* The same value in PSP bit order (what the decoder outputs for pixel format 1). */
static uint32_t psp565(uint32_t x, uint32_t y) { uint32_t v = texel565(x, y); return (v & 0xF800) >> 11 | (v & 0x07E0) | (v & 0x001F) << 11; }

/* Builds a PVR-T chunk: header (16 bytes) + data; with_gbix adds a global-index chunk in front. */
static uint8_t *chunk(int pixfmt, int datafmt, uint32_t w, uint32_t h, const uint8_t *data, uint32_t dn, int with_gbix, uint32_t *total) {
    uint32_t pre = with_gbix ? 12 : 0;
    uint8_t *p = calloc(pre + 16 + dn, 1);
    if (with_gbix) { memcpy(p, "GBI" "X", 4); put32(p + 4, 4); put32(p + 8, 12345); }
    memcpy(p + pre, "PVR" "T", 4); put32(p + pre + 4, 8 + dn);
    p[pre + 8] = (uint8_t)pixfmt; p[pre + 9] = (uint8_t)datafmt;
    put16(p + pre + 12, w); put16(p + pre + 14, h);
    memcpy(p + pre + 16, data, dn);
    *total = pre + 16 + dn;
    return p;
}

static void expect_image(const char *what, const uint16_t *img, uint32_t w, uint32_t h) {
    int bad = 0;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++)
        if (img[y * w + x] != psp565(x, y)) { if (bad < 3) printf("  %s: texel (%u,%u) = 0x%04x, want 0x%04x\n", what, x, y, img[y * w + x], (unsigned)psp565(x, y)); bad++; }
    CHECK(bad == 0, "%s: %d texels wrong", what, bad);
}

static void test_twiddled(void) {
    enum { W = 16, H = 16 };
    uint8_t data[W * H * 2];
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) put16(data + morton(x, y) * 2, texel565(x, y));
    uint32_t n; uint8_t *p = chunk(1, 1, W, H, data, sizeof data, 1, &n);
    pvr_info info; uint16_t img[W * H]; int fmt = -1;
    CHECK(pvr_parse(p, n, &info) == PVR_OK, "twiddled: parse");
    CHECK(info.gbix == 12345 && info.w == W && info.h == H && info.pixfmt == 1 && info.datafmt == 1, "twiddled: header fields");
    CHECK(pvr_decode(&info, NULL, img, &fmt) == PVR_OK && fmt == PVR_OUT_5650, "twiddled: decode");
    expect_image("twiddled", img, W, H);
    free(p);
}

/* Mipmapped: the chain 1x1, 2x2, 4x4, 8x8, 16x16 after one dummy texel.
 * Every smaller level is filled with a marker so a wrong base shows. */
static void test_twiddled_mips(void) {
    enum { W = 16, H = 16 };
    uint32_t top = pvr_mip_offset16(4);
    CHECK(top == 0xAC, "mip offset for 16x16 is 0xAC, got 0x%x", top);
    CHECK(pvr_mip_offset16(3) == 0x2C && pvr_mip_offset16(6) == 0xAAC && pvr_mip_offset16(8) == 0xAAAC, "mip offsets for 8, 64 and 256");
    uint32_t dn = top + W * H * 2;
    uint8_t *data = calloc(dn, 1);
    for (uint32_t i = 0; i < top; i += 2) put16(data + i, 0xDEAD);           /* dummy + smaller levels */
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) put16(data + top + morton(x, y) * 2, texel565(x, y));
    uint32_t n; uint8_t *p = chunk(1, 2, W, H, data, dn, 0, &n);
    pvr_info info; uint16_t img[W * H]; int fmt = -1;
    CHECK(pvr_parse(p, n, &info) == PVR_OK && info.gbix == PVR_NO_GINDEX, "mips: parse");
    CHECK(pvr_decode(&info, NULL, img, &fmt) == PVR_OK && fmt == PVR_OUT_5650, "mips: decode");
    expect_image("twiddled+mips", img, W, H);
    /* the chain is exactly as long as the chunk says: nothing read past it */
    CHECK(info.data_len == dn, "mips: data length %u, want %u", info.data_len, dn);
    free(p); free(data);
}

/* VQ: a codebook whose entry k holds the four texels of block k (blocks
 * numbered in Morton order of the half-size index grid), indices twiddled. */
static void test_vq(int mips) {
    enum { W = 16, H = 16, HW = 8, HH = 8 };
    uint32_t idx_base = mips ? (1 + 1 + 4 + 16) : 0;  /* 1x1, 2x2, 4x4, 8x8 levels: one byte, one byte, four, sixteen */
    uint32_t dn = 256 * 8 + idx_base + HW * HH;
    uint8_t *data = calloc(dn, 1);
    for (uint32_t by = 0; by < HH; by++) for (uint32_t bx = 0; bx < HW; bx++) {
        uint32_t k = morton(bx, by);
        uint8_t *e = data + k * 8;
        put16(e, texel565(2 * bx, 2 * by)); put16(e + 2, texel565(2 * bx, 2 * by + 1));
        put16(e + 4, texel565(2 * bx + 1, 2 * by)); put16(e + 6, texel565(2 * bx + 1, 2 * by + 1));
        data[256 * 8 + idx_base + morton(bx, by)] = (uint8_t)k;
    }
    for (uint32_t i = 0; i < idx_base; i++) data[256 * 8 + i] = 0x3F;     /* smaller levels: a wrong base reads these */
    uint32_t n; uint8_t *p = chunk(1, mips ? 4 : 3, W, H, data, dn, 0, &n);
    pvr_info info; uint16_t img[W * H]; int fmt = -1;
    CHECK(pvr_parse(p, n, &info) == PVR_OK, "vq%s: parse", mips ? "+mips" : "");
    CHECK(pvr_decode(&info, NULL, img, &fmt) == PVR_OK, "vq%s: decode", mips ? "+mips" : "");
    expect_image(mips ? "vq+mips" : "vq", img, W, H);
    free(p); free(data);
}

static void test_rectangle(void) {
    enum { W = 32, H = 8 };
    uint8_t data[W * H * 2];
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) put16(data + (y * W + x) * 2, texel565(x, y));
    uint32_t n; uint8_t *p = chunk(1, 9, W, H, data, sizeof data, 0, &n);
    pvr_info info; uint16_t img[W * H]; int fmt = -1;
    CHECK(pvr_parse(p, n, &info) == PVR_OK, "rect: parse");
    CHECK(pvr_decode(&info, NULL, img, &fmt) == PVR_OK, "rect: decode");
    expect_image("rectangle", img, W, H);
    free(p);
    /* twiddled rectangle: min(w,h) squares along the long side */
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) put16(data + ((x / H) * H * H + morton(x % H, y)) * 2, texel565(x, y));
    p = chunk(1, 0xD, W, H, data, sizeof data, 0, &n);
    CHECK(pvr_parse(p, n, &info) == PVR_OK && pvr_decode(&info, NULL, img, &fmt) == PVR_OK, "twiddled rect: parse+decode");
    expect_image("twiddled rectangle", img, W, H);
    free(p);
}

static void test_errors(void) {
    uint8_t junk[32] = {0};
    pvr_info info;
    CHECK(pvr_parse(junk, sizeof junk, &info) == PVR_ERR_HEADER, "junk is rejected");
    uint8_t data[8] = {0};
    uint32_t n; uint8_t *p = chunk(1, 1, 24, 8, data, sizeof data, 0, &n);   /* 24 is not a power of two */
    CHECK(pvr_parse(p, n, &info) == PVR_ERR_SIZE, "non-power-of-two size is rejected");
    free(p);
    p = chunk(1, 0x7F, 8, 8, data, sizeof data, 0, &n);
    uint16_t img[64]; int fmt;
    CHECK(pvr_parse(p, n, &info) == PVR_OK && pvr_decode(&info, NULL, img, &fmt) == PVR_ERR_FORMAT, "unknown data format is rejected");
    free(p);
    /* a short chunk reads zeros past its end instead of the buffer */
    uint8_t short_data[2 * 2 * 2] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    p = chunk(1, 1, 4, 4, short_data, sizeof short_data, 0, &n);
    CHECK(pvr_parse(p, n, &info) == PVR_OK && pvr_decode(&info, NULL, img, &fmt) == PVR_OK, "short chunk decodes");
    CHECK(img[0] == 0xFFFF && img[15] == 0, "short chunk: texels past the data are zero (got 0x%04x, 0x%04x)", img[0], img[15]);
    free(p);
}

int main(void) {
    test_twiddled();
    test_twiddled_mips();
    test_vq(0);
    test_vq(1);
    test_rectangle();
    test_errors();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("pvr_decode_test: ok\n");
    return 0;
}
