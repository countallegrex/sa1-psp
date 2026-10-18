/* texture.c - Ninja texture management: texlists, the texture memory
 * list the game gave njInitTexture, PVR/PVM decoding from guest RAM or
 * from loose .PVR files, and the backend textures behind them.
 *
 * Formats (docs/formats/pvr.md): a PVR is an optional GINDEX chunk (global
 * index) then "PVR-T", size, pixel format byte (0 ARGB1555, 1 RGB565,
 * 2 ARGB4444, 3 YUV422, 4 bump, 5 4-bit palette, 6 8-bit palette), data
 * format byte (1 twiddled, 2 twiddled+mips, 3 VQ, 4 VQ+mips, 5/6 pal4
 * twiddled (+mips), 7/8 pal8 twiddled (+mips), 9 rectangle, 0xB stride,
 * 0xD twiddled rectangle, 0x10/0x11 small VQ (+mips)), width, height,
 * texels.  A PVM is archive header, size, flags, count, entries {id, name[28] if
 * flags&8, format if flags&4, dims if flags&2, gbix if flags&1}, then the
 * PVRs back to back, each 32-byte aligned... as laid out by the tools.
 *
 * Every texture is converted once to a 16-bit PSP-order image (the same
 * bit depth as the source, so no quality loss) and handed to
 * draw_texture_create; the handle goes into the TCW's address field.
 *
 * The NJS_TEXMEMLIST entry (68 bytes, what game code reads):
 *   +0 globalIndex  +4 bank  +8 TSP size bits  +12 TCW  +16 texture address
 *   +20 texinfo.texaddr  +24 surface.Type  +28 BitDepth  +32 PixelFormat
 *   +36 nWidth  +40 nHeight  +44 TextureSize  +48 flags  +52 pSurface
 *   +56 pVirtual  +60 pPhysical  +64 count (16) | pad (16)
 *
 * Part of sa1-psp (MIT). */
#include "ninja.h"
#include "../state.h"
#include "pvr_decode.h"
#include "../shinobi/shinobi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G_MEMLIST      0x8C890030u   /* our own bookkeeping lives natively; these mirror the SDK globals */
#define G_TEXCACHE     0x8C89003Cu
#define G_TEX_FAILIDX  0x8C88F7A8u
#define G_TEX_ERR      0x8C88F7B0u
#define ENTRY_SIZE 68
#define MAX_ENTRIES 0x800

static uint32_t memlist_base, memlist_count;
typedef struct { uint32_t handle; uint32_t bytes; int used; uint32_t gbix; uint32_t index; uint16_t w, h; uint8_t pixfmt, datafmt; } mem_slot;
static mem_slot slots[MAX_ENTRIES];
static uint32_t total_bytes;
static int ntex;
static uint32_t palette[1024];        /* KAMUI palette RAM, ARGB8888 */

uint32_t texture_memory_bytes(void) { return total_bytes; }
int texture_count(void) { return ntex; }

/* ---- PVR decoding (pvr_decode.c does the texels) ----------------------------- */
static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
/* chunk magics as little-endian words (the four-letter tags themselves are
 * on tools/check_clean.py's signature list, so they are not spelled here) */
#define MAGIC_GINDEX  0x58494247u   /* G B I X */
#define MAGIC_TEXTURE 0x54525650u   /* P V R T */
#define MAGIC_ARCHIVE 0x484D5650u   /* P V M H */
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int log2u(uint32_t v) { int n = 0; while ((1u << n) < v && n < 12) n++; return n; }

/* Decodes the PVR-T at p (length n) into a 16-bit PSP-order image.
 * Returns the backend handle, 0 on failure; *w, *h, *fmt describe it. */
static const char *decode_name = "";      /* the texture being decoded, for the debug log and dumps */
static uint32_t decode_index;              /* running number of decodes (the review index) */
static int decode_datafmt;
/* Review aid (--texdump DIR / SA1_TEX_DUMP_DIR): every decoded texture as
 * <index>_<gbix>_<fmt>_<flags>_<w>x<h>_<mips>.pam (RGB + alpha);
 * tools/texsheet.py tiles a directory of them into labelled contact sheets. */
static const char *dump_dir;
static unsigned dump_min_w = 128;
void texture_set_dump_dir(const char *dir, unsigned min_w) { dump_dir = dir; dump_min_w = min_w; }
static const char *pixfmt_name(int f) {
    static const char *n[] = {"1555", "565", "4444", "yuv", "bump", "pal4", "pal8"};
    return f >= 0 && f < 7 ? n[f] : "fmt?";
}
static const char *datafmt_name(int f) {
    switch (f) {
    case 1: return "tw"; case 2: return "twmip"; case 3: return "vq"; case 4: return "vqmip";
    case 5: return "pal4tw"; case 6: return "pal4mip"; case 7: return "pal8tw"; case 8: return "pal8mip";
    case 9: return "rect"; case 0xB: return "stride"; case 0xD: return "twrect"; case 0x10: return "svq"; case 0x11: return "svqmip";
    default: return "data?";
    }
}
static int datafmt_mips(int f) { return f == 2 || f == 4 || f == 6 || f == 8 || f == 0x11; }
static uint32_t decode_info(const pvr_info *pinfo, int *out_w, int *out_h, int *out_fmt, uint32_t *out_bytes);
static uint32_t decode_pvrt(const uint8_t *p, uint32_t n, int *out_w, int *out_h, int *out_fmt, uint32_t *out_bytes, uint32_t *gbix_out) {
    pvr_info info;
    int r = pvr_parse(p, n, &info);
    if (gbix_out) *gbix_out = r == PVR_ERR_HEADER ? PVR_NO_GINDEX : info.gbix;
    if (r == PVR_ERR_HEADER) return 0;
    if (r == PVR_ERR_SIZE) {
        sa1_log("texture: unsupported size %ux%u (fmt %d/%d)", info.w, info.h, info.pixfmt, info.datafmt);
        return 0;
    }
    return decode_info(&info, out_w, out_h, out_fmt, out_bytes);
}
/* Raw texels (no chunk headers) described by an NJS_TEXINFO: the message
 * renderer composes each subtitle line into such an image (type 0x900:
 * rectangle, ARGB1555) and loads it through the texinfo form. */
static uint32_t decode_raw(const uint8_t *p, uint32_t n, uint32_t w, uint32_t h, int pixfmt, int datafmt, int *out_w, int *out_h, int *out_fmt, uint32_t *out_bytes) {
    pvr_info info;
    memset(&info, 0, sizeof info);
    if (w < 8 || h < 8 || w > 1024 || h > 1024 || (w & (w - 1)) || (h & (h - 1))) {
        sa1_log("texture: raw texels %ux%u (fmt %d/%d): unsupported size", w, h, pixfmt, datafmt);
        return 0;
    }
    info.w = w; info.h = h; info.pixfmt = pixfmt; info.datafmt = datafmt; info.gbix = PVR_NO_GINDEX;
    info.data = p; info.data_len = n < w * h * 2 ? n : w * h * 2;
    return decode_info(&info, out_w, out_h, out_fmt, out_bytes);
}
static uint32_t decode_info(const pvr_info *pinfo, int *out_w, int *out_h, int *out_fmt, uint32_t *out_bytes) {
    const pvr_info info = *pinfo;
    uint32_t w = info.w, h = info.h;
    int pixfmt = info.pixfmt, datafmt = info.datafmt;
    uint16_t *img = calloc((size_t)w * h, 2);
    if (!img) return 0;
    static int tex_log = -1;
    if (tex_log < 0) tex_log = getenv("SA1_TEX_LOG") != NULL;       /* debug aid: every decode */
    if (tex_log) sa1_log("texture: decode %s %ux%u pixel %d data 0x%x size %u", decode_name, w, h, pixfmt, datafmt, info.data_len + 8);
    int fmt = DRAW_FMT_5650;
    if (pvr_decode(&info, palette, img, &fmt) != PVR_OK) {
        sa1_log("texture: data format 0x%x not supported (pixel format %d, %ux%u)", datafmt, pixfmt, w, h);
        free(img);
        return 0;
    }
    uint32_t handle = draw_texture_create((int)w, (int)h, fmt, img, 0, NULL);
    decode_index++; decode_datafmt = datafmt;
    if (!dump_dir && getenv("SA1_TEX_DUMP_DIR")) {
        dump_dir = getenv("SA1_TEX_DUMP_DIR");
        if (getenv("SA1_TEX_DUMP_MIN")) dump_min_w = (unsigned)atoi(getenv("SA1_TEX_DUMP_MIN"));
    }
    if (dump_dir && w >= dump_min_w) {
        char path[512]; FILE *o;
        snprintf(path, sizeof path, "%s/%04u_%u_%s_%s_%ux%u_%d.pam", dump_dir, decode_index, info.gbix == PVR_NO_GINDEX ? 0 : info.gbix,
                 pixfmt_name(pixfmt), datafmt_name(datafmt), w, h, datafmt_mips(datafmt));
        sa1_log("texture: dump %04u = %s (%s %s %ux%u, gbix %u)", decode_index, decode_name, pixfmt_name(pixfmt), datafmt_name(datafmt), w, h, info.gbix);
        if ((o = fopen(path, "wb")) != NULL) {
            /* PAM, RGB + alpha (alpha 255 where the format has none), so the
             * contact sheets can show transparency */
            fprintf(o, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
            for (uint32_t i = 0; i < w * h; i++) {
                uint16_t v = img[i]; uint8_t rgba[4];
                if (fmt == DRAW_FMT_5650) { rgba[0] = (v & 31) << 3; rgba[1] = ((v >> 5) & 63) << 2; rgba[2] = ((v >> 11) & 31) << 3; rgba[3] = 255; }
                else if (fmt == DRAW_FMT_5551) { rgba[0] = (v & 31) << 3; rgba[1] = ((v >> 5) & 31) << 3; rgba[2] = ((v >> 10) & 31) << 3; rgba[3] = (v & 0x8000) ? 255 : 0; }
                else { rgba[0] = (v & 15) * 17; rgba[1] = ((v >> 4) & 15) * 17; rgba[2] = ((v >> 8) & 15) * 17; rgba[3] = ((v >> 12) & 15) * 17; }
                fwrite(rgba, 1, 4, o);
            }
            fclose(o);
        }
    }
    free(img);
    if (!handle) return 0;
    *out_w = (int)w; *out_h = (int)h; *out_fmt = pixfmt; *out_bytes = w * h * 2;
    return handle;
}

/* ---- the texture memory list ----------------------------------------------------- */
static uint32_t entry_addr(uint32_t i) { return memlist_base + i * ENTRY_SIZE; }

static int find_free(void) {
    for (uint32_t i = 0; i < memlist_count; i++) if (!slots[i].used) return (int)i;
    return -1;
}
static int find_gbix(uint32_t gbix) {
    if (gbix == 0xFFFFFFFFu) return -1;
    for (uint32_t i = 0; i < memlist_count; i++) if (slots[i].used && slots[i].gbix == gbix) return (int)i;
    return -1;
}

/* TSP size codes: log2(size) - 3 in 3 bits each, U high. */
static uint32_t size_code(int w, int h) {
    uint32_t cu = (uint32_t)(log2u((uint32_t)w) - 3), cv = (uint32_t)(log2u((uint32_t)h) - 3);
    return (cu & 7) << 3 | (cv & 7);
}

/* Loads the PVR-T at host memory p into a memlist entry; returns the entry
 * index or -1.  raw_w/raw_h non-zero: p holds bare texels of that size in
 * format raw_fmt (pixel format low byte, data format high byte). */
static int load_entry_ex(const uint8_t *p, uint32_t n, uint32_t gbix_hint, const char *what, uint32_t raw_w, uint32_t raw_h, uint32_t raw_fmt) {
    int w, h, pixfmt; uint32_t bytes, gbix = 0xFFFFFFFFu;
    decode_name = what;
    uint32_t handle = raw_w ? decode_raw(p, n, raw_w, raw_h, (int)(raw_fmt & 0xFF), (int)(raw_fmt >> 8), &w, &h, &pixfmt, &bytes)
                            : decode_pvrt(p, n, &w, &h, &pixfmt, &bytes, &gbix);
    if (gbix == 0xFFFFFFFFu) gbix = gbix_hint;
    if (!handle) { sa1_log("texture: %s: decode failed", what); return -1; }
    int shared = find_gbix(gbix);
    if (shared >= 0) {              /* already in memory: share it, like the SDK's global index cache */
        draw_texture_destroy(handle);
        uint32_t e = entry_addr((uint32_t)shared);
        hle_wr32(e + 64, hle_rd32(e + 64) + 1);
        return shared;
    }
    int i = find_free();
    if (i < 0) { draw_texture_destroy(handle); sa1_log("texture: memory list full (%u entries)", memlist_count); return -1; }
    slots[i].used = 1; slots[i].handle = handle; slots[i].bytes = bytes; slots[i].gbix = gbix;
    slots[i].index = decode_index; slots[i].w = (uint16_t)w; slots[i].h = (uint16_t)h; slots[i].pixfmt = (uint8_t)pixfmt; slots[i].datafmt = (uint8_t)decode_datafmt;
    total_bytes += bytes; ntex++;
    uint32_t e = entry_addr((uint32_t)i);
    hle_wr32(e + 0, gbix); hle_wr32(e + 4, 0);
    hle_wr32(e + 8, size_code(w, h));
    hle_wr32(e + 12, (uint32_t)pixfmt << 27 | 1u << 26 | handle);   /* TCW: format, non-twiddled flag, our handle as the address */
    hle_wr32(e + 16, handle * 8u);
    hle_wr32(e + 20, 0);
    hle_wr32(e + 24, 1); hle_wr32(e + 28, 16); hle_wr32(e + 32, (uint32_t)pixfmt);
    hle_wr32(e + 36, (uint32_t)w); hle_wr32(e + 40, (uint32_t)h); hle_wr32(e + 44, bytes);
    hle_wr32(e + 48, 0); hle_wr32(e + 52, 0); hle_wr32(e + 56, 0); hle_wr32(e + 60, 0);
    hle_wr32(e + 64, 1);
    return i;
}
static int load_entry(const uint8_t *p, uint32_t n, uint32_t gbix_hint, const char *what) { return load_entry_ex(p, n, gbix_hint, what, 0, 0, 0); }

static void release_entry(uint32_t e) {
    if (!memlist_base || e < memlist_base || e >= memlist_base + memlist_count * ENTRY_SIZE) return;
    uint32_t i = (e - memlist_base) / ENTRY_SIZE;
    if (!slots[i].used) return;
    uint32_t cnt = hle_rd32(e + 64) & 0xFFFF;
    if (cnt > 1) { hle_wr32(e + 64, cnt - 1); return; }
    draw_texture_destroy(slots[i].handle);
    total_bytes -= slots[i].bytes; ntex--;
    memset(&slots[i], 0, sizeof slots[i]);
    hle_wr32(e + 0, 0xFFFFFFFFu); hle_wr32(e + 12, 0); hle_wr32(e + 64, 0);
}

/* Selects the texture with this global index into the context copy (the
 * njDrawTexture family names textures by global index, 0x8C606B66). */
int texture_select_gindex(uint32_t gbix) {
    int i = find_gbix(gbix);
    if (i < 0) return -1;
    uint32_t e = entry_addr((uint32_t)i);
    hle_wr32(NJ_CUR_GINDEX, gbix); hle_wr32(NJ_CUR_BANK, hle_rd32(e + 4));
    hle_wr32(KM_CTX_TSP, (hle_rd32(KM_CTX_TSP) & ~63u) | (hle_rd32(e + 8) & 63));
    hle_wr32(KM_CTX_TCW, hle_rd32(e + 12));
    return 1;
}

/* "<index> <fmt> <flags> <w>x<h>" of a live texture handle (the mark key). */
int texture_describe(uint32_t handle, char *buf, unsigned n) {
    for (uint32_t i = 0; i < memlist_count; i++) if (slots[i].used && slots[i].handle == handle) {
        snprintf(buf, n, "%04u %s %s %ux%u", slots[i].index, pixfmt_name(slots[i].pixfmt), datafmt_name(slots[i].datafmt), slots[i].w, slots[i].h);
        return 1;
    }
    snprintf(buf, n, "handle %u?", handle);
    return 0;
}

uint32_t texture_from_tcw(uint32_t tcw) {
    uint32_t h = tcw & 0x1FFFFFu;
    for (uint32_t i = 0; i < memlist_count; i++) if (slots[i].used && slots[i].handle == h) return h;
    return 0;
}

void texture_init(void) { memset(slots, 0, sizeof slots); total_bytes = 0; ntex = 0; }

/* ---- texlist helpers --------------------------------------------------------------- */
#define ATTR_GLOBALINDEX 0x80000000u
#define ATTR_MEMORY      0x40000000u
#define ATTR_FRAMEBUFFER 0x20000000u
#define ATTR_LOADED      0x01000000u
#define ATTR_TEXINFO     0x00800000u   /* with MEMORY: filename -> NJS_TEXINFO, whose texaddr is the PVR image (SDK loader 0x8C64D1A6) */

static void texname_string(uint32_t texname, char *out, unsigned n) {
    uint32_t fn = hle_rd32(texname);
    if (!fn || !hle_in_ram(fn)) { out[0] = 0; return; }
    unsigned i = 0;
    for (; i + 1 < n && i < 28; i++) { char ch = (char)hle_rd8(fn + i); if (!ch) break; out[i] = ch; }
    out[i] = 0;
}

/* Loads one NJS_TEXNAME; returns the entry index or -1. */
static int load_texname(uint32_t texname) {
    uint32_t attr = hle_rd32(texname + 4), fn = hle_rd32(texname);
    if (attr & ATTR_FRAMEBUFFER) return -1;
    if (attr & ATTR_MEMORY) {
        uint32_t hint = (attr & ATTR_GLOBALINDEX) ? (attr & 0x00FFFFFFu) : 0xFFFFFFFFu;
        if (attr & ATTR_TEXINFO) {               /* the texinfo form: its texaddr is the image, +8 of the name the global index */
            uint32_t info = fn;
            if (!info || !hle_in_ram(info)) return -1;
            hint = hle_rd32(texname + 8);
            fn = hle_rd32(info);
            if (!fn || !hle_in_ram(fn)) return -1;
            uint32_t avail = SA1_RAM_SIZE + SA1_RAM_EXTRA - (fn & SA1_RAM_MASK), magic = hle_rd32(fn);
            if (magic != MAGIC_GINDEX && magic != MAGIC_TEXTURE)   /* bare texels: size and format from the texinfo (SDK loader 0x8C64D324) */
                return load_entry_ex(hle_ptr(fn), avail, hint, "texinfo texels", hle_rd32(info + 16), hle_rd32(info + 20), hle_rd32(info + 4) >> 16);
            return load_entry(hle_ptr(fn), avail, hint, "texinfo texture");
        }
        if (!fn || !hle_in_ram(fn)) return -1;
        uint32_t avail = SA1_RAM_SIZE + SA1_RAM_EXTRA - (fn & SA1_RAM_MASK);
        return load_entry(hle_ptr(fn), avail, hint, "memory texture");
    }
    char name[40], rel[160], path[200];
    texname_string(texname, name, sizeof name);
    if (!name[0]) return -1;
    size_t len = strlen(name);
    int has_ext = len > 4 && (name[len - 4] == '.') && (name[len - 3] | 0x20) == 'p' && (name[len - 2] | 0x20) == 'v' && (name[len - 1] | 0x20) == 'r';
    if (has_ext) snprintf(rel, sizeof rel, "%s", name);   /* the boss-name font B32ASCII.PVR names itself with the extension */
    else snprintf(rel, sizeof rel, "%s.PVR", name);
    shinobi_gdfs_path(path, sizeof path, rel);
    uint32_t size = 0;
    if (sa1_plat_file_size(path, &size) != 0 || size == 0) { sa1_log("texture: %s not found", path); return -1; }
    uint8_t *buf = malloc(size);
    if (!buf) return -1;
    int got = sa1_plat_file_read(path, 0, buf, size);
    int r = got > 0 ? load_entry(buf, (uint32_t)got, 0xFFFFFFFFu, name) : -1;
    free(buf);
    return r;
}

/* ---- entries ------------------------------------------------------------------------ */
void sdk_8c608c2a(sh4_ctx *c) {         /* njInitTexture(memlist, n) */
    HLE_ENTER(0x8c608c2au, "njInitTexture");
    memlist_base = ARG0; memlist_count = ARG1 < MAX_ENTRIES ? ARG1 : MAX_ENTRIES;
    for (uint32_t i = 0; i < memlist_count; i++) { hle_wr32(entry_addr(i), 0xFFFFFFFFu); hle_wr32(entry_addr(i) + 64, 0); }
    hle_wr32(G_MEMLIST, ARG0); hle_wr32(G_TEXCACHE, 0);
    texture_init();
    sa1_log("njInitTexture: %u entries at 0x%08x", memlist_count, memlist_base);
}
/* 0x8C608DD4 (memlist, n): after a display re-init, re-upload every live
 * entry to texture memory (KAMUI 0x8C64DF66 per entry).  Our textures
 * never leave the backend, so there is nothing to do. */
void sdk_8c608dd4(sh4_ctx *c) { HLE_ENTER(0x8c608dd4u, "njReloadTextures"); (void)c; }

void sdk_8c608c82(sh4_ctx *c) {         /* njLoadTexture(texlist) */
    HLE_ENTER(0x8c608c82u, "njLoadTexture");
    uint32_t tl = ARG0;
    if (!tl) { RET(-1); return; }
    uint32_t names = hle_rd32(tl), count = hle_rd32(tl + 4);
    int fail = 0, loaded = 0;
    for (uint32_t i = 0; i < count && i < 2048; i++) {
        uint32_t tn = names + i * 12;
        uint32_t attr = hle_rd32(tn + 4);
        if ((attr & ATTR_LOADED) && hle_rd32(tn + 8)) continue;
        int e = load_texname(tn);
        if (e < 0) {
            if (!fail) hle_wr32(G_TEX_FAILIDX, i);
            fail = 1; hle_wr32(tn + 8, 0);
            uint32_t p = hle_rd32(tn); char raw[33] = "";
            for (int k = 0; k < 16 && p && hle_in_ram(p); k++) snprintf(raw + 2 * k, 3, "%02x", hle_rd8(p + (uint32_t)k));
            sa1_log("texture: texlist 0x%08x entry %u not loaded: ptr 0x%08x attr 0x%08x [%s]", tl, i, p, attr, raw);
            continue;
        }
        hle_wr32(tn + 8, entry_addr((uint32_t)e));
        loaded++;
    }
    sa1_log("njLoadTexture(0x%08x): %d of %u textures%s", tl, loaded, count, fail ? " (some failed)" : "");
    RET(fail ? -1 : 1);
}

/* 0x8C608134: load a PVM archive in memory into a texlist.  The texlist's
 * name array is matched by name when the archive has names and the list
 * has them, else by position; nbTexture is set to the archive's count. */
void sdk_8c608134(sh4_ctx *c) {
    HLE_ENTER(0x8c608134u, "njLoadTexturePvmMemory");
    uint32_t data = ARG0, tl = ARG1;
    if (!data || !tl || !hle_in_ram(data)) { hle_wr32(G_TEX_ERR, 4); RET(-1); return; }
    const uint8_t *p = hle_ptr(data);
    uint32_t avail = SA1_RAM_SIZE + SA1_RAM_EXTRA - (data & SA1_RAM_MASK);
    if (avail < 16 || rd32le(p) != MAGIC_ARCHIVE) { hle_wr32(G_TEX_ERR, 4); RET(-1); return; }
    uint32_t hsize = rd32le(p + 4);
    uint32_t flags = rd16le(p + 8), count = rd16le(p + 10);
    uint32_t q = 12;
    struct { uint32_t id; char name[29]; uint32_t gbix; } *ents = calloc(count ? count : 1, sizeof *ents);
    if (!ents) { RET(-1); return; }
    for (uint32_t i = 0; i < count; i++) {
        ents[i].id = rd16le(p + q); q += 2;
        ents[i].gbix = 0xFFFFFFFFu;
        if (flags & 8) { memcpy(ents[i].name, p + q, 28); ents[i].name[28] = 0; q += 28; }
        if (flags & 4) q += 2;
        if (flags & 2) q += 2;
        if (flags & 1) { ents[i].gbix = rd32le(p + q); q += 4; }
    }
    uint32_t names = hle_rd32(tl), listn = hle_rd32(tl + 4);
    if (!hle_in_ram(names)) names = 0;
    if (listn > 2048) listn = 0;
    hle_wr32(tl + 4, count);
    /* the PVR-T chunks follow the header, each prefixed by its own GINDEX */
    uint32_t off = 8 + hsize;
    int fail = 0;
    for (uint32_t i = 0; i < count; i++) {
        /* find the next chunk start */
        while (off + 8 <= avail && rd32le(p + off) != MAGIC_GINDEX && rd32le(p + off) != MAGIC_TEXTURE) off += 4;
        if (off + 16 > avail) { fail = 1; break; }
        uint32_t start = off;
        if (rd32le(p + off) == MAGIC_GINDEX) off += 8 + rd32le(p + off + 4);
        if (off + 8 > avail || rd32le(p + off) != MAGIC_TEXTURE) { fail = 1; break; }
        uint32_t chunk_end = off + 8 + rd32le(p + off + 4);
        if (chunk_end > avail) chunk_end = avail;
        /* which texlist slot gets it: by name when both sides have names, else position */
        uint32_t slot = i;
        if ((flags & 8) && names && listn) {
            for (uint32_t j = 0; j < listn; j++) {
                char nm[40]; texname_string(names + j * 12, nm, sizeof nm);
                if (nm[0] && strncmp(nm, ents[i].name, 28) == 0) { slot = j; break; }
            }
        }
        int e = load_entry(p + start, chunk_end - start, ents[i].gbix, ents[i].name[0] ? ents[i].name : "pvm entry");
        if (names && slot < 2048) {
            uint32_t tn = names + slot * 12;
            hle_wr32(tn + 8, e >= 0 ? entry_addr((uint32_t)e) : 0);
            if (e >= 0 && ents[i].gbix != 0xFFFFFFFFu) hle_wr32(tn + 4, (hle_rd32(tn + 4) & 0xFF000000u) | ATTR_GLOBALINDEX | (ents[i].gbix & 0x00FFFFFFu));
        }
        if (e < 0) fail = 1;
        off = chunk_end;
    }
    sa1_log("njLoadTexturePvm(0x%08x -> 0x%08x names 0x%08x): %u textures (flags 0x%x, first gbix %u)%s, %d live, %u bytes", data, tl, names, count, flags, count ? ents[0].gbix : 0, fail ? " (some failed)" : "", ntex, total_bytes);
    const char *save_dir = getenv("SA1_PVM_SAVE_DIR");    /* tools/pvm_ram_check.py: the archive as found in guest RAM */
    if (save_dir && off <= avail) {
        static unsigned seq; char path[512], fname[64]; FILE *o;
        snprintf(fname, sizeof fname, "pvm_%03u_%08x.bin", seq++, data);
        snprintf(path, sizeof path, "%s/%s", save_dir, fname);
        if ((o = fopen(path, "wb")) != NULL) { fwrite(p, 1, off, o); fclose(o); }
        sa1_log("pvm: saved %s (%u bytes at 0x%08x) from \"%s\"", fname, off, data, shinobi_last_opened_file());
    }
    free(ents);
    RET(fail ? -1 : 1);
}

void sdk_8c6088bc(sh4_ctx *c) {         /* njReleaseTexture(texlist) */
    HLE_ENTER(0x8c6088bcu, "njReleaseTexture");
    uint32_t tl = ARG0;
    if (!tl) { RET(-1); return; }
    uint32_t names = hle_rd32(tl), count = hle_rd32(tl + 4);
    for (uint32_t i = 0; i < count && i < 2048; i++) {
        uint32_t tn = names + i * 12, e = hle_rd32(tn + 8);
        if (e) release_entry(e);
        hle_wr32(tn + 8, 0);
    }
    RET(1);
}
void sdk_8c608c5c(sh4_ctx *c) {         /* njReleaseTextureAll */
    HLE_ENTER(0x8c608c5cu, "njReleaseTextureAll");
    for (uint32_t i = 0; i < memlist_count; i++) if (slots[i].used) { hle_wr32(entry_addr(i) + 64, 1); release_entry(entry_addr(i)); }
    hle_wr32(G_TEXCACHE, 0);
    (void)c;
}
void sdk_8c6087f4(sh4_ctx *c) { HLE_ENTER(0x8c6087f4u, "njTextureGarbage"); (void)c; }

/* pure-memory helpers */
void sdk_8c607264(sh4_ctx *c) {         /* build a texlist: list->textures = names; names[i].filename = base + 28 i */
    HLE_ENTER(0x8c607264u, "njSetTextureList");
    for (uint32_t i = 0; i < ARG3 && i < 2048; i++) { hle_wr32(ARG1 + i * 12, ARG2 + 28 * i); }
    hle_wr32(ARG0, ARG1);
}
void sdk_8c608914(sh4_ctx *c) {         /* njSetTextureInfo(info, tex, type, w, h) */
    HLE_ENTER(0x8c608914u, "njSetTextureInfo");
    hle_wr32(ARG0, ARG1); hle_wr32(ARG0 + 4, ARG2 << 16); hle_wr32(ARG0 + 16, ARG3); hle_wr32(ARG0 + 20, STACK_ARG(0));
}
/* njSetTextureName(name, addr, globalIndex, attr): filename = addr, attr,
 * and the global index in the texaddr slot until the texture is loaded
 * (the SDK body 0x8C608922).  The message renderer (0x8C055C4C) builds the
 * subtitle texture this way every frame: an NJS_TEXINFO whose texaddr is a
 * PVR image the game composed in RAM, attr 0x40800000. */
void sdk_8c608922(sh4_ctx *c) {
    HLE_ENTER(0x8c608922u, "njSetTextureName");
    hle_wr32(ARG0, ARG1); hle_wr32(ARG0 + 4, ARG3); hle_wr32(ARG0 + 8, ARG2);
}

/* ---- save states (hle/state.h, docs/savestate.md) ----------------------------
 * The memory list mirror and the palette.  The backend textures behind the
 * handles are the backend's to save (the handles also sit in guest RAM). */
void texture_state(hle_state_io *io) {
    hle_state_blob(io, "tex.base", &memlist_base, sizeof memlist_base);
    hle_state_blob(io, "tex.count", &memlist_count, sizeof memlist_count);
    hle_state_blob(io, "tex.slots", slots, sizeof slots);
    hle_state_blob(io, "tex.bytes", &total_bytes, sizeof total_bytes);
    hle_state_blob(io, "tex.n", &ntex, sizeof ntex);
    hle_state_blob(io, "tex.palette", palette, sizeof palette);
    hle_state_blob(io, "tex.index", &decode_index, sizeof decode_index);
}
