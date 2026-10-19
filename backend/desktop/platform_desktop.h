/* platform_desktop.h - the desktop backend's own interface beyond hle.h:
 * memory at fixed addresses and the coroutine internals that save states
 * need (docs/savestate.md section 1).
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include "hle.h"
#include <stddef.h>

/* One fixed reservation per process: guest RAM at the base, the coroutine
 * stacks above it.  The addresses are the same in every run of a build, so
 * a saved stack (return addresses, frame pointers, pointers into guest RAM)
 * can be put back where it was.  32 TB is inside every x86-64 user space
 * (Windows: 128 TB, Linux: 128 TB) and nothing else lands there. */
#define SA1_FIXED_BASE        0x0000200000000000ull
#define SA1_FIXED_STACKS_OFF  0x0000000010000000ull   /* 256 MB above the base; RAM takes 36 MB */
#define SA1_CORO_SLOTS        40                       /* hle/game/tasks.c has 32 tasks + the root */
#define SA1_CORO_STACK        (1u << 20)               /* 1 MB per coroutine */
#define SA1_CORO_GUARD        (64u << 10)              /* no-access gap below each stack */
#define SA1_CORO_GUARD_BAND   (64u << 10)              /* guard pages at the bottom of each stack (Windows): an overflow reports itself */
#define SA1_CORO_STRIDE       (SA1_CORO_STACK + SA1_CORO_GUARD)

/* Maps n bytes, read/write, at exactly `at`; NULL when the address is not
 * free (logged). */
void *sa1_plat_fixed_alloc(uint64_t at, size_t n);
/* Guest RAM (SA1_RAM_ALLOC bytes, zeroed) at the fixed base; sets sa1_ram.
 * 0 = ok, -1 = the address could not be mapped. */
int sa1_plat_ram_alloc(void);

/* Coroutine internals (platform_desktop.c, coro_switch.c).  Slot i's stack
 * is [lo, hi); a suspended coroutine's state is its saved stack pointer and
 * the bytes from there to hi. */
sa1_coro *sa1_plat_coro_supervisor(void);     /* the OS thread's own context (main) */
sa1_coro *sa1_plat_coro_current(void);
void sa1_plat_coro_stack(int i, uintptr_t *lo, uintptr_t *hi);
int sa1_plat_coro_used(int i);
uintptr_t sa1_plat_coro_sp(int i);
/* Restoring a file: free every slot, then mark the saved ones used with
 * their saved stack pointer (the caller has written the stack bytes). */
void sa1_plat_coro_reset_all(void);
void sa1_plat_coro_restore(int i, uintptr_t sp);
