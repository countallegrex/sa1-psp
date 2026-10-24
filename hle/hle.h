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
static inline uint8_t *hle_ptr(uint32_t addr) { return sa1_ram + sa1_off(addr); }
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
/* Is a guest address inside RAM (any of the P0/P1/P2 spellings), including
 * the HLE's extra area above the 16 MB (0x?D000000..)? */
static inline int hle_in_ram(uint32_t a) { return ((a & 0x1F000000u) == 0x0C000000u) || ((a & 0x1FE00000u) == 0x0D000000u); }
/* Guest address of the extra area (SA1_RAM_EXTRA bytes). */
#define HLE_EXTRA_BASE 0x8D000000u

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
#define HLE_ENTER(addr, name) do { static struct sa1_hit *hle_site_; sa1_seam_enter(&hle_site_, (addr), (name)); } while (0)

/* ---- platform interface (backend/<target>/platform_*.c) ---------------- */
uint64_t sa1_plat_time_us(void);              /* monotonic microseconds */
/* Game data files.  Paths are relative to the data root and spelled as the
 * disc spells them ("SONICADV/ADV00.PRS"); the backend maps them to its own
 * file system.  Returns 0 on success. */
int sa1_plat_file_size(const char *path, uint32_t *size);
/* Reads n bytes at byte offset off into dst; returns bytes read or -1. */
int sa1_plat_file_read(const char *path, uint32_t off, void *dst, uint32_t n);
/* The same read for a reader that comes back for what follows (the sound
 * streams' feeder): the bytes are the file's, as sa1_plat_file_read gives
 * them, but the backend may fetch ahead in the background (the PSP does). */
int sa1_plat_file_read_stream(const char *path, uint32_t off, void *dst, uint32_t n);
/* Lists the names of the files in a directory of the data root (upper case
 * as on the disc), one callback per entry.  Returns 0 on success. */
int sa1_plat_dir_list(const char *dir, void (*cb)(const char *name, uint32_t size, void *arg), void *arg);

/* ---- coroutines ------------------------------------------------------------ */
/* The game's own task system switches guest stacks (hle/game/tasks.c); each
 * guest task runs on a host execution context the backend provides: fibers
 * or ucontext on desktop, threads handed a baton on the PSP.  Only one runs
 * at a time; a switch returns when another context switches back. */
typedef struct sa1_coro sa1_coro;
sa1_coro *sa1_plat_coro_main(void);                 /* the context the game boots on (the root task) */
sa1_coro *sa1_plat_coro_create(void (*fn)(void *arg), void *arg, unsigned stack_bytes);
void sa1_plat_coro_switch(sa1_coro *to);            /* from the running context */
void sa1_plat_coro_destroy(sa1_coro *co);           /* never the running one */
/* A small stable number per context and back (save states store contexts
 * by number: docs/savestate.md). */
int sa1_plat_coro_index(const sa1_coro *co);
sa1_coro *sa1_plat_coro_by_index(int index);

/* ---- input ---------------------------------------------------------------- */
/* Dreamcast controller state, active-high: C 1, B 2, A 4, Start 8, Up 0x10,
 * Down 0x20, Left 0x40, Right 0x80, Z 0x100, Y 0x200, X 0x400, D 0x800.
 * The backend writes it before each frame; the pad server copies it into
 * the guest records. */
typedef struct hle_pad { uint32_t buttons; uint8_t ltrig, rtrig; int8_t x, y; } hle_pad;
extern hle_pad hle_pad_state;
/* 1 = the controller's round gate: the pad server scales a stick vector
 * longer than 128 back onto that circle, as a real controller (and the
 * Flycast oracle's maple controller) reports it, so (-127,-127) reads
 * (-91,-91).  0 = the square range every tape before 2026-10-22 replays
 * with (desktop: input_tape.c "stick-gate").  Set before the first frame. */
extern int hle_pad_round_gate;
/* Frames (hle_frame_count) in which TMU1 reads (hle/shinobi/hw.c) run two
 * vblanks late.  The game's catch-up loop (0x8C04EA4A) adds a third logic
 * sub-step to a game frame when TMU1 shows >= 1.85 vblanks after the
 * second; on the console that happens after a long GD-ROM load, which our
 * reads (complete at once) never cause.  Oracle tapes name those frames
 * (desktop: input_tape.c "slow-substep"); NULL / 0 = none, as live play.
 * Set before the first frame. */
extern const uint32_t *hle_tmu_slow_frames;
extern int hle_tmu_slow_count;
/* 1 = hor+ widescreen (the clip rectangle lets the game draw 853x480 of its
 * screen space), 0 = 4:3.  Set before hle_init; the backend gets the same
 * value through draw_set_aspect. */
extern int hle_aspect_wide;
/* 1 (default; --no-ta-direct 0): the native TA strip writers (hle/ninja/ta_direct.c, docs/ta-direct.md). */
extern int hle_ta_direct;

/* ---- what the backend drives -------------------------------------------- */
/* One-time setup of the HLE state: BIOS area, syscall vectors, heap, file
 * system.  Call after sa1_ram is allocated and 1ST_READ.BIN is loaded. */
void hle_init(void);
/* Called by the vsync HLE once per guest frame, before the frame callbacks
 * run.  The backend uses it to pump input, check the frame budget and stop
 * the run (it may exit the process). */
extern void (*hle_frame_hook)(uint32_t frame);
uint32_t hle_frame_count(void);
/* Strict determinism (backend --strict, tests/replay/README.md): guest time
 * is exactly 1/60 s per frame plus a fixed step per timer read, with no host
 * time in it, and the real-time clock reads hle_rtc_epoch (Unix seconds) plus
 * the guest time.  Off by default; set before hle_init. */
extern int hle_strict;
extern int64_t hle_rtc_epoch;
/* START skips a story event (hle/game/event_skip.c; never in strict runs). */
extern int hle_event_skip, hle_event_skip_cli;
void hle_event_skip_poll(sh4_ctx *c);
/* Frame skip (hle/game/frame_skip.c): logic every frame, the picture when
 * there is time; never in strict runs unless forced. */
extern int hle_frame_skip, hle_frame_skip_cli, hle_frame_skip_force;
int hle_render_skip(void);
void hle_frame_skip_report(void);
int hle_parse_frame_skip(const char *v, int *force);   /* settings.c: auto|off|N[, force:...]; -3 = bad */
/* Runs a guest function from native code (callbacks): r4..r7 = a0..a3. */
void hle_call_guest(sh4_ctx *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
