/* savestate.h - save states for the desktop build (docs/savestate.md):
 * guest RAM, the HLE's state outside it (hle/state.h) and the coroutine
 * stacks, written by a supervisor context that owns the OS thread's stack
 * while every game context is suspended.
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include "state.h"
#include <stdint.h>

/* What the header records about a run, for the loader to compare. */
typedef struct savestate_meta {
    uint32_t frame;
    uint32_t flags;        /* SAVESTATE_STRICT, SAVESTATE_WIDE */
    int64_t rtc_epoch;
} savestate_meta;
#define SAVESTATE_STRICT 1u
#define SAVESTATE_WIDE   2u

/* Registration, before savestate_supervise.  Modules are saved and loaded
 * in registration order; `fill` describes the run for the header; `loaded`
 * (optional) runs after a successful load with the header's meta. */
void savestate_add(const char *name, void (*fn)(hle_state_io *io));
void savestate_set_meta(void (*fill)(savestate_meta *m), void (*loaded)(const savestate_meta *m));

/* Runs the game: boot(arg) on the root context, or, with load_path, the
 * file's contexts resumed where they were.  Returns when the game context
 * returns from boot (0), or -1 when the load failed (logged). */
int savestate_supervise(void (*boot)(void *arg), void *arg, const char *load_path);

/* From a game context (the frame hook).  save returns 0 or -1 (logged);
 * load does not return on success and returns -1 on failure. */
int savestate_save(const char *path);
int savestate_load(const char *path);

/* The byte-run packer the file uses (tests/savestate).  pack returns a
 * malloc'd buffer; unpack fills out (out_n bytes) and returns 0 or -1. */
uint8_t *savestate_pack(const uint8_t *in, size_t n, size_t *out_n);
int savestate_unpack(const uint8_t *in, size_t n, uint8_t *out, size_t out_n);
