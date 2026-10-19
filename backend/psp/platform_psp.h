/* platform_psp.h - the PSP backend's own interface beyond hle.h: where the
 * game data lives, the coroutine stacks (same slot layout as the desktop's,
 * backend/desktop/platform_desktop.h) and the debug aids main_psp.c and
 * crash_psp.c share.
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include "hle.h"
#include <stddef.h>

/* Coroutine stacks: one static region (a fixed address in every run of a
 * build, as on the desktop), slot i at [lo, hi).  The desktop gives every
 * slot 1 MB; measured high water on the whole M2 route was 12 KB on the root
 * context and 4 KB on the tasks (docs/m2-render-report.md section 10), so
 * the PSP gives the root 256 KB and each task 32 KB.  The bottom
 * SA1_CORO_CANARY bytes of each slot hold a pattern: main_psp.c's stack probe
 * traps before a stack reaches them and the report measures high water by
 * how much of the painted stack was overwritten. */
#define SA1_CORO_SLOTS        40                       /* hle/game/tasks.c has 32 tasks + the root */
#define SA1_CORO_ROOT_STACK   (256u << 10)
#define SA1_CORO_STACK        (32u << 10)
#define SA1_CORO_CANARY       (1u << 10)
#define SA1_CORO_PAINT        0x5A1C0DE5u

sa1_coro *sa1_plat_coro_current(void);
void sa1_plat_coro_stack(int i, uintptr_t *lo, uintptr_t *hi);
int sa1_plat_coro_used(int i);
/* Bytes of slot i's stack ever used (painted pattern overwritten). */
uint32_t sa1_plat_coro_high_water(int i);
/* Runs fn(arg) on the root slot, from the PSP main thread (the supervisor). */
int sa1_plat_coro_run_root(void (*fn)(void *), void *arg);

/* Game data root, "ms0:/PSP/GAME/SA1PSP/data" or wherever the EBOOT is. */
void plat_set_data_root(const char *root);
/* Files kept open between reads (gdFs reads a file in many pieces). */
void plat_files_close_all(void);
/* Log sink: buffered to a file on the Memory Stick, copied to stdout (psplink,
 * PPSSPP's log).  plat_log_flush writes the buffer out. */
int plat_log_open(const char *path, int to_stdout);
void plat_log_flush(void);
/* 1 = write every line through to the card (the default until main_psp.c
 * turns it off after the first frames), 0 = buffered. */
void plat_log_sync(int on);

/* crash_psp.c: the guest frame, the last HLE entry and a scan of the running
 * stack for return addresses into recompiled code. */
void plat_backtrace(void);
void plat_crash_init(void);
void plat_watchdog_start(uint32_t (*frame)(void), unsigned seconds);
