/* adx.h - CRI ADX: header parsing and the 4-bit ADPCM block decoder.
 *
 * Written from the public description of the format (docs/formats/sound.md
 * section 2): a big-endian header (0x8000 magic, offset of the "(c)" "CRI"
 * string, encoding 3, 18-byte blocks, 4 bits per sample, channels, rate,
 * sample count, high-pass cutoff, version 3 or 4 with a loop record), then
 * per channel interleaved 18-byte blocks: a 16-bit big-endian scale and 32
 * signed nibbles, high nibble first, each predicted from the two previous
 * output samples with fixed-point coefficients derived from the cutoff.
 *
 * Pure C, no I/O, no allocation, integer only after adx_parse: runs on the
 * main CPU or the PSP's Media Engine alike.
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include <stdint.h>

#define ADX_MAX_CH 2
#define ADX_BLOCK 18            /* bytes per channel block */
#define ADX_BLOCK_SAMPLES 32

typedef struct adx_info {
    uint32_t data_off;          /* first block, from the start of the file */
    uint32_t channels;
    uint32_t rate;
    uint32_t samples;           /* per channel */
    uint32_t loop;              /* 1 when the header asks for a loop */
    uint32_t loop_start;        /* sample index, inclusive */
    uint32_t loop_end;          /* sample index, exclusive */
    int32_t coef1, coef2;       /* prediction coefficients, Q12 */
} adx_info;

/* Parses the first n bytes of a file (0x30 are enough for every version the
 * game uses).  Returns 0, or -1 when this is not a type-3 ADX this decoder
 * handles (encrypted, other block sizes). */
int adx_parse(const uint8_t *hdr, uint32_t n, adx_info *out);

typedef struct adx_hist { int32_t h1, h2; } adx_hist;

/* Decodes one 18-byte block of one channel into 32 samples. */
void adx_decode_block(const uint8_t *blk, int32_t coef1, int32_t coef2, adx_hist *h, int16_t *out);

/* Bytes of one block group (every channel's block for 32 samples). */
static inline uint32_t adx_group_bytes(const adx_info *a) { return ADX_BLOCK * a->channels; }
/* Block-group index holding sample s. */
static inline uint32_t adx_group_of(uint32_t s) { return s / ADX_BLOCK_SAMPLES; }
/* Number of block groups a stream plays before it loops or ends. */
static inline uint32_t adx_play_groups(const adx_info *a) {
    uint32_t end = a->loop ? a->loop_end : a->samples;
    return (end + ADX_BLOCK_SAMPLES - 1) / ADX_BLOCK_SAMPLES;
}
