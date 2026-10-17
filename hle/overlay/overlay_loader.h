/* overlay_loader.h - switches the recompiled code overlays when the game
 * loads a file into an overlay slot.  docs/m1-overlays.md section 7.
 *
 * The game decompresses its area code (SONICADV/ADV00.PRS, STG01.PRS, ...)
 * to 0x8C900000 and its event code (EVnnnn.PRS) to 0x8CB80000.  Every load
 * on the route goes through one block-A routine, 0x8C09DA44 (r4 = file name,
 * r5 = destination).  overlay_loader.c wraps it: before the load it empties
 * any slot the load overwrites, after a successful load at a slot's base it
 * installs the matching recompiled table from the registry.
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include "sh4_rt.h"

/* One code overlay the build carries: generated into overlay_registry.c by
 * tools/gen_overlay_registry.py from recomp/sa1_us_1005/overlays/<name>.toml. */
typedef struct hle_overlay_file {
    const char *file;          /* disc path, e.g. "SONICADV/ADV00.PRS" */
    int slot;                  /* SA1_SLOT_AREA, SA1_SLOT_EVENT or SA1_SLOT_AREA2 */
    uint32_t size;             /* decompressed bytes */
    const char *sha256;        /* of the decompressed bytes (tools/extract_overlays.py) */
    const sa1_overlay *ov;     /* the recompiled function table; NULL if not emitted */
} hle_overlay_file;

/* The registry references the tables weakly (a config without emitted code
 * still links; its ov is NULL). */
#define HLE_OVERLAY_WEAK __attribute__((weak))

extern const hle_overlay_file hle_overlay_files[];
extern const unsigned hle_overlay_file_count;

/* Physical base address of a slot (0x0C900000, 0x0CB80000, 0x0C920000); 0 if none. */
uint32_t hle_overlay_slot_base(int slot);

/* The registry entry for a file name as the game passes it ("adv00.PRS",
 * "ev002B.prs", or with a directory): matched case-insensitively on the
 * last path component, and only among files of that slot.  NULL if the
 * build has no recompiled code for it. */
const hle_overlay_file *hle_overlay_find(const char *name, int slot);

/* Call before the game writes a loaded file to guest address dest: empties
 * every slot whose base is dest, whose installed code range contains it, or
 * whose code lies within the file's decompressed size above dest (a file the
 * registry does not know is assumed to fill the area up to the event slot,
 * so an area load also empties the 0x8C920000 slot). */
void hle_overlay_load_begin(const char *name, uint32_t dest);

/* Call after the load: if it succeeded (ok != 0) and dest is a slot base,
 * installs the file's table, or leaves the slot empty for a file with no
 * recompiled code (data, or an overlay not yet recompiled). */
void hle_overlay_load_end(const char *name, uint32_t dest, int ok);
