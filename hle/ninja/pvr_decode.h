/* pvr_decode.h - PowerVR texture (PVR-T chunk) decoding into 16-bit images,
 * with no dependency on the rest of the HLE so it can be unit tested
 * (tests/ninja/pvr_decode_test.c).  Formats: docs/formats/pvr.md.
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include <stdint.h>

typedef struct pvr_info {
    uint32_t w, h;
    int pixfmt;            /* 0 ARGB1555, 1 RGB565, 2 ARGB4444, 3 YUV422, 4 bump, 5 pal4, 6 pal8 */
    int datafmt;           /* 1 twiddled, 2 twiddled+mips, 3 VQ, 4 VQ+mips, 5-8 palettised, 9/0xB rectangle, 0xD twiddled rectangle, 0x10/0x11 small VQ */
    uint32_t gbix;         /* global index from the global-index chunk, PVR_NO_GINDEX when absent */
    const uint8_t *data;   /* texel data (after the 16-byte PVR-T header) */
    uint32_t data_len;     /* bytes available at data (the chunk's own size, clipped to the buffer) */
} pvr_info;
#define PVR_NO_GINDEX 0xFFFFFFFFu

/* Output pixel formats, in PSP bit order (red in the low bits). */
enum { PVR_OUT_5650 = 0, PVR_OUT_5551 = 1, PVR_OUT_4444 = 2 };   /* same values as DRAW_FMT_* */

enum { PVR_OK = 0, PVR_ERR_HEADER = -1, PVR_ERR_SIZE = -2, PVR_ERR_FORMAT = -3 };

/* Parses the chunk at p (n bytes: optional global-index chunk, then PVR-T).  Fails
 * on a bad header or a size that is not a power of two up to 1024. */
int pvr_parse(const uint8_t *p, uint32_t n, pvr_info *info);

/* Decodes info's texels into img (w*h 16-bit texels, row-major, PSP bit
 * order) and sets *out_fmt to the PVR_OUT_* format.  palette: KAMUI palette
 * RAM as 1024 ARGB8888 words, used by the palettised formats (may be NULL:
 * a grey ramp is used).  Bytes beyond data_len read as zero. */
int pvr_decode(const pvr_info *info, const uint32_t *palette, uint16_t *img, int *out_fmt);

/* Byte offset of the largest level of a twiddled 16-bit texture with
 * mipmaps, by log2(width) (the layout PVR files use, see pvr_decode.c). */
uint32_t pvr_mip_offset16(int log2w);
