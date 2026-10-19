/* adx.c - CRI ADX decoder (see adx.h).
 *
 * Part of sa1-psp (MIT). */
#include "adx.h"
#include <math.h>

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int adx_parse(const uint8_t *h, uint32_t n, adx_info *a) {
    if (n < 0x20 || be16(h) != 0x8000) return -1;
    uint32_t copy = be16(h + 2);
    if (h[4] != 3 || h[5] != ADX_BLOCK || h[6] != 4) return -1;
    if (h[7] < 1 || h[7] > ADX_MAX_CH) return -1;
    uint32_t ver = h[0x12], flags = h[0x13];
    if (flags != 0) return -1;                  /* encrypted streams: not on this disc */
    a->data_off = copy + 4;
    a->channels = h[7];
    a->rate = be32(h + 8);
    a->samples = be32(h + 12);
    uint32_t cutoff = be16(h + 16);
    a->loop = 0; a->loop_start = 0; a->loop_end = a->samples;
    /* Loop record: version 3 at 0x14, version 4 at 0x24 (after four
     * history words).  Layout: u16 alignment samples, u16 -, u32 enabled,
     * u32 start sample, u32 start byte, u32 end sample, u32 end byte. */
    uint32_t lp = ver == 3 ? 0x14 : ver == 4 ? 0x24 : 0;
    if (lp && lp + 0x18 <= n && lp + 0x18 <= a->data_off && be32(h + lp + 4) == 1) {
        a->loop_start = be32(h + lp + 8);
        a->loop_end = be32(h + lp + 16);
        if (a->loop_end > a->samples) a->loop_end = a->samples;
        if (a->loop_start < a->loop_end) a->loop = 1;
    }
    /* Coefficients of the second-order predictor (the public derivation):
     * a = sqrt2 - cos(2 pi f / rate), b = sqrt2 - 1,
     * c = (a - sqrt((a + b)(a - b))) / b; coef1 = 2c, coef2 = -c^2, in Q12. */
    double x = sqrt(2.0) - cos(2.0 * 3.14159265358979323846 * cutoff / (double)a->rate);
    double y = sqrt(2.0) - 1.0;
    double z = (x - sqrt((x + y) * (x - y))) / y;
    a->coef1 = (int32_t)floor(z * 8192.0);
    a->coef2 = (int32_t)floor(z * z * -4096.0);
    return 0;
}

void adx_decode_block(const uint8_t *blk, int32_t c1, int32_t c2, adx_hist *h, int16_t *out) {
    int32_t scale = (int32_t)be16(blk);
    int32_t h1 = h->h1, h2 = h->h2;
    for (int i = 0; i < ADX_BLOCK_SAMPLES; i++) {
        int32_t b = blk[2 + i / 2];
        int32_t nib = (i & 1) ? (b & 15) : (b >> 4);
        if (nib & 8) nib -= 16;
        int32_t s = nib * scale + ((c1 * h1 + c2 * h2) >> 12);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out[i] = (int16_t)s;
        h2 = h1; h1 = s;
    }
    h->h1 = h1; h->h2 = h2;
}
