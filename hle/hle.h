/* hle.h - what every HLE body shares: guest-memory helpers, the platform
 * interface the backends implement, and the boot/frame hooks the backend
 * calls.  Platform-neutral C11, 32-bit clean (guest addresses are uint32_t,
 * never host pointers held across calls).
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include "sh4_rt.h"
#include <stdint.h>

/* ---- guest memory ------------------------------------------------------ */
/* Host pointer to a guest address; valid until the next call that may move
 * sa1_ram (none do, but keep uses short). */
static inline uint8_t *hle_ptr(uint32_t addr) { return sa1_ram + (addr & SA1_RAM_MASK); }
static inline uint32_t hle_rd32(uint32_t a) { return RD32(a, 0); }
static inline uint32_t hle_rd16(uint32_t a) { return RD16(a, 0); }
static inline uint32_t hle_rd8(uint32_t a) { return RD8(a, 0); }
static inline void hle_wr32(uint32_t a, uint32_t v) { WR32(a, 0, v); }
static inline void hle_wr16(uint32_t a, uint32_t v) { WR16(a, 0, v); }
static inline void hle_wr8(uint32_t a, uint32_t v) { WR8(a, 0, v); }
static inline float hle_rdf(uint32_t a) { return RDF(a, 0); }
static inline void hle_wrf(uint32_t a, float v) { WRF(a, 0, v); }
/* Copies a NUL-terminated guest string into buf (at most n - 1 chars). */
void hle_guest_string(uint32_t addr, char *buf, unsigned n);
/* Is a guest address inside RAM (any of the P0/P1/P2 spellings)? */
static inline int hle_in_ram(uint32_t a) { return ((a & 0x1F000000u) == 0x0C000000u); }

/* ---- call-site conventions ---------------------------------------------- */
/* Arguments r4..r7 (SH ABI) and the return register. */
#define ARG0 (c->r[4])
#define ARG1 (c->r[5])
#define ARG2 (c->r[6])
#define ARG3 (c->r[7])
#define RET(v) (c->r[0] = (uint32_t)(v))
/* Stack argument i (0-based) for calls with more than four word arguments. */
#define STACK_ARG(i) RD32(c->r[15], 4 * (i))
/* Every HLE body counts itself so the exit report lists real bodies too. */
#define HLE_ENTER(addr, name) sa1_seam_hit((addr), (name))

/* ---- platform interface (backend/<target>/platform_*.c) ---------------- */
uint64_t sa1_plat_time_us(void);              /* monotonic microseconds */
/* Game data files.  Paths are relative to the data root and spelled as the
 * disc spells them ("SONICADV/ADV00.PRS"); the backend maps them to its own
 * file system.  Returns 0 on success. */
int sa1_plat_file_size(const char *path, uint32_t *size);
/* Reads n bytes at byte offset off into dst; returns bytes read or -1. */
int sa1_plat_file_read(const char *path, uint32_t off, void *dst, uint32_t n);
/* Lists the names of the files in a directory of the data root (upper case
 * as on the disc), one callback per entry.  Returns 0 on success. */
int sa1_plat_dir_list(const char *dir, void (*cb)(const char *name, uint32_t size, void *arg), void *arg);

/* ---- input ---------------------------------------------------------------- */
/* Dreamcast controller state, active-high: C 1, B 2, A 4, Start 8, Up 0x10,
 * Down 0x20, Left 0x40, Right 0x80, Z 0x100, Y 0x200, X 0x400, D 0x800.
 * The backend writes it before each frame; the pad server copies it into
 * the guest records. */
typedef struct hle_pad { uint32_t buttons; uint8_t ltrig, rtrig; int8_t x, y; } hle_pad;
extern hle_pad hle_pad_state;

/* ---- what the backend drives -------------------------------------------- */
/* One-time setup of the HLE state: BIOS area, syscall vectors, heap, file
 * system.  Call after sa1_ram is allocated and 1ST_READ.BIN is loaded. */
void hle_init(void);
/* Called by the vsync HLE once per guest frame, before the frame callbacks
 * run.  The backend uses it to pump input, check the frame budget and stop
 * the run (it may exit the process). */
extern void (*hle_frame_hook)(uint32_t frame);
uint32_t hle_frame_count(void);
/* Runs a guest function from native code (callbacks): r4..r7 = a0..a3. */
void hle_call_guest(sh4_ctx *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
