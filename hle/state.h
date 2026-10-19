/* state.h - serialising the HLE's state outside guest RAM (save states,
 * docs/savestate.md section 2).  Each module has one function that names
 * its variables to an hle_state_io; the same function saves and loads, and
 * a load checks every name and size against what this build expects.
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct hle_state_io hle_state_io;

/* 1 while a load runs the module functions, 0 while a save does. */
int hle_state_loading(const hle_state_io *io);
/* Saves, or loads into, the n bytes at p under name (at most 15 chars).  On
 * a load whose next chunk has another name or size nothing is copied and
 * the io carries an error (hle_state_error). */
void hle_state_blob(hle_state_io *io, const char *name, void *p, uint32_t n);
/* A chunk whose size the loader does not know in advance.  Save: writes n
 * bytes from p.  Load: returns a pointer to the chunk's bytes (valid until
 * the io is freed) and its size in *n; NULL on a name mismatch. */
const void *hle_state_var(hle_state_io *io, const char *name, const void *p, uint32_t *n);

/* The io itself (hle/state.c).  A writer collects chunks in memory; a reader
 * walks a buffer.  Chunk: char name[16], uint32_t len, bytes. */
hle_state_io *hle_state_writer(void);
hle_state_io *hle_state_reader(const void *buf, size_t n);
const void *hle_state_bytes(const hle_state_io *io, size_t *n);   /* writer: what was written */
const char *hle_state_error(const hle_state_io *io);              /* NULL when all is well */
int hle_state_at_end(const hle_state_io *io);                     /* reader: every chunk consumed */
void hle_state_free(hle_state_io *io);
/* Stream mode (the PSP, which has no room for a whole state in memory): the
 * chunks go straight to f, or come straight from it.  A stream reader keeps
 * only the last two hle_state_var payloads (a texture's pixels and its
 * CLUT): each is valid until the second var call after it. */
#include <stdio.h>
hle_state_io *hle_state_stream_writer(FILE *f);
hle_state_io *hle_state_stream_reader(FILE *f);
size_t hle_state_stream_written(const hle_state_io *io);   /* bytes so far */

/* The modules' state functions, and the one that runs them all in a fixed
 * order (hle_init.c).  The list is the inventory in docs/savestate.md. */
void hle_state_all(hle_state_io *io);
void hle_task_state(hle_state_io *io);          /* hle/game/tasks.c */
void shinobi_sys_state(hle_state_io *io);       /* hle/shinobi/sys.c: frame counter */
void shinobi_timer_state(hle_state_io *io);
void shinobi_hw_state(hle_state_io *io);
void shinobi_vsync_state(hle_state_io *io);
void shinobi_pad_state(hle_state_io *io);
void shinobi_gdfs_state(hle_state_io *io);
void shinobi_memory_state(hle_state_io *io);
void shinobi_chain_state(hle_state_io *io);
void shinobi_cfg_state(hle_state_io *io);
void hle_bios_state(hle_state_io *io);          /* hle/bios/syscalls.c */
void hle_overlay_state(hle_state_io *io);       /* hle/overlay/overlay_loader.c: installed tables */
void kamui_state(hle_state_io *io);             /* hle/ninja/kamui.c */
void texture_state(hle_state_io *io);           /* hle/ninja/texture.c */
