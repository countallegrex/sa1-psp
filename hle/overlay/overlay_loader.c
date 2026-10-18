/* overlay_loader.c - the loader hook that installs recompiled overlay tables.
 * See overlay_loader.h and docs/m1-overlays.md section 7.
 *
 * Wrapped block-A functions (recomp/sa1_us_1005/hle_wraps.tsv): the emitter
 * defines their recompiled bodies as fn_<addr>_orig, and every call site and
 * the function table reach the wrappers below instead.  The wrappers read
 * their arguments from the context before the body runs; the bodies' call
 * summaries keep r4/r5 in sync at entry, since both are live-in.
 *
 * Part of sa1-psp (MIT). */
#include "overlay_loader.h"
#include "../state.h"

#include <string.h>

void fn_8c09da44_orig(sh4_ctx *c);
void fn_8c099872_orig(sh4_ctx *c);

#define PHYS(a) ((uint32_t)(a) & 0x1FFFFFFFu)

static const uint32_t slot_base[SA1_SLOT_COUNT] = {0x0C900000u, 0x0CB80000u, 0x0C920000u};
#define AREA_SPAN 0x00280000u   /* 0x8C900000 up to the event slot */

uint32_t hle_overlay_slot_base(int slot) {
    return slot >= 0 && slot < SA1_SLOT_COUNT ? slot_base[slot] : 0;
}

static const char *last_component(const char *path) {
    const char *p = path;
    for (const char *s = path; *s; s++)
        if (*s == '/' || *s == '\\') p = s + 1;
    return p;
}

static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; }

static int same_name(const char *a, const char *b) {
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) a++, b++;
    return *a == 0 && *b == 0;
}

const hle_overlay_file *hle_overlay_find(const char *name, int slot) {
    const char *n = last_component(name);
    for (unsigned i = 0; i < hle_overlay_file_count; i++) {
        const hle_overlay_file *f = &hle_overlay_files[i];
        if (f->slot == slot && same_name(n, last_component(f->file))) return f;
    }
    return NULL;
}

/* Decompressed size of a file the registry knows, under any slot; an
 * unknown file (data, or code not recompiled) counts as filling the area. */
static uint32_t load_span(const char *name) {
    const char *n = last_component(name);
    for (unsigned i = 0; i < hle_overlay_file_count; i++)
        if (same_name(n, last_component(hle_overlay_files[i].file))) return hle_overlay_files[i].size;
    return AREA_SPAN;
}

void hle_overlay_load_begin(const char *name, uint32_t dest) {
    uint32_t d = PHYS(dest), span = load_span(name);
    for (int s = 0; s < SA1_SLOT_COUNT; s++) {
        const sa1_overlay *ov = sa1_overlay_get(s);
        if (ov && (d == slot_base[s] || (d >= ov->code_lo && d < ov->code_hi) ||
                   (d < ov->code_lo && ov->code_lo - d < span))) {
            sa1_log("overlay: %s -> 0x%08x overwrites %s", name, (unsigned)(d | 0x80000000u), ov->name);
            sa1_overlay_set(s, NULL);
        }
    }
}

void hle_overlay_load_end(const char *name, uint32_t dest, int ok) {
    uint32_t d = PHYS(dest);
    for (int s = 0; s < SA1_SLOT_COUNT; s++) {
        if (d != slot_base[s]) continue;
        if (!ok) {
            sa1_log("overlay: loading %s to slot %d failed", name, s);
            return;
        }
        const hle_overlay_file *f = hle_overlay_find(name, s);
        if (f && !f->ov) {
            sa1_log("overlay: loaded %s to 0x%08x; its code was not emitted into this build", f->file,
                    (unsigned)(d | 0x80000000u));
        } else if (f) {
            sa1_log("overlay: loaded %s to 0x%08x", f->file, (unsigned)(d | 0x80000000u));
            sa1_overlay_set(s, f->ov);
        } else {
            sa1_log("overlay: loaded %s to 0x%08x, no recompiled code for it", name, (unsigned)(d | 0x80000000u));
        }
        return;
    }
}

/* Copies a NUL-terminated guest string; truncates to n - 1 characters. */
static void guest_string(uint32_t addr, char *buf, unsigned n) {
    unsigned i = 0;
    for (; i + 1 < n; i++) {
        char ch = (char)RD8(addr, i);
        if (ch == 0) break;
        buf[i] = ch;
    }
    buf[i] = 0;
}

/* 0x8C09DA44: r4 = file name (guest C string with no directory, spelled as
 * in the game: "advertise.prs", "adv00.PRS", "ev002B.prs"), r5 = destination.
 * Reads the whole file to 0x8CD00000, PRS-decompresses it (0x8C04C5E0) to
 * the destination, then flushes the cache.  r0 = 0, or -1 if the file did
 * not open (nothing is decompressed then). */
void fn_8c09da44(sh4_ctx *c) {
    char name[64];
    uint32_t dest = c->r[5];
    guest_string(c->r[4], name, sizeof name);
    hle_overlay_load_begin(name, dest);
    fn_8c09da44_orig(c);
    hle_overlay_load_end(name, dest, c->r[0] == 0);
}

/* 0x8C099872: r4 = file name of a PRS texture archive, r5 = texture list.
 * Decompresses the archive to 0x8C900000 as scratch space and loads the
 * textures from there, so whatever area code was in the slot is gone.  No
 * call, tail call or stored pointer in block A or in the seven emitted
 * overlays reaches it, and the M0 trace never ran it; wrapped so that a
 * caller found later cannot leave a stale area table installed. */
void fn_8c099872(sh4_ctx *c) {
    char name[64];
    guest_string(c->r[4], name, sizeof name);
    hle_overlay_load_begin(name, slot_base[SA1_SLOT_AREA]);
    fn_8c099872_orig(c);
}

/* Save states: the installed tables, by overlay name; a load looks each name
 * up in this build's registry (an overlay that was not emitted into this
 * build is logged and left empty). */
void hle_overlay_state(hle_state_io *io) {
    char names[SA1_SLOT_COUNT][16];
    memset(names, 0, sizeof names);
    if (!hle_state_loading(io))
        for (int s = 0; s < SA1_SLOT_COUNT; s++) {
            const sa1_overlay *ov = sa1_overlay_get(s);
            if (ov) { strncpy(names[s], ov->name, 15); names[s][15] = 0; }
        }
    hle_state_blob(io, "overlays", names, sizeof names);
    if (!hle_state_loading(io) || hle_state_error(io)) return;
    for (int s = 0; s < SA1_SLOT_COUNT; s++) {
        const sa1_overlay *ov = NULL;
        if (names[s][0]) {
            for (unsigned i = 0; i < hle_overlay_file_count && !ov; i++) {
                const hle_overlay_file *f = &hle_overlay_files[i];
                if (f->ov && f->ov->name && same_name(f->ov->name, names[s])) ov = f->ov;
            }
            if (!ov) sa1_log("overlay: state has %s in slot %d, which this build did not emit", names[s], s);
        }
        sa1_overlay_set(s, ov);
    }
}
