/* vmu_store.h - where the HLE VMU (cfg.c, bu library) keeps its files: one
 * host file per VMU file, behind a small interface each backend fills in.
 *
 * hle_vmu_store NULL (the default) means no VMU is inserted: the bu stubs
 * then answer exactly as before the VMU existed, so replay tapes and oracle
 * runs see no difference.  The desktop backend sets it to a folder store
 * (backend/desktop/vmu_store_desktop.c) unless the run is --strict or
 * --no-vmu.
 *
 * PSP: TODO (sa1-psp-g) - implement these five on ms0: and set
 * hle_vmu_store before hle_init; until then the PSP build has no VMU.
 *
 * Names are VMU file names: up to 12 characters [A-Za-z0-9_.-], NUL
 * terminated.  Sizes are bytes; the bu layer rounds to 512-byte blocks.
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include <stdint.h>

typedef struct vmu_store_ops {
    /* Reads up to max bytes of the file into buf: bytes read, -1 if absent. */
    int32_t (*read)(const char *name, void *buf, uint32_t max);
    /* Creates or replaces the file with n bytes: 0, or -1 on a host error. */
    int (*write)(const char *name, const void *buf, uint32_t n);
    /* Deletes the file: 0, or -1 if absent. */
    int (*remove)(const char *name);
    /* The index-th file in a stable order (sorted by name): writes its name
     * (13 bytes with the NUL) and size and returns 0; -1 past the last. */
    int (*list)(int index, char name[13], uint32_t *size);
} vmu_store_ops;

extern const vmu_store_ops *hle_vmu_store;
