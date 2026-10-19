/* platform_desktop.c - the desktop half of the platform interface in
 * hle/hle.h: a monotonic clock and game-data files from a directory on the
 * host disk, laid out like the disc (and like the Memory Stick will be).
 *
 * Part of sa1-psp (MIT). */
#include "platform_desktop.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#endif

static char data_root[1024] = ".";

void plat_set_data_root(const char *root) {
    snprintf(data_root, sizeof data_root, "%s", root);
}

uint64_t sa1_plat_time_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void full_path(char *out, unsigned n, const char *rel) {
    snprintf(out, n, "%s/%s", data_root, rel);
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
}

int sa1_plat_file_size(const char *path, uint32_t *size) {
    char full[1200];
    full_path(full, sizeof full, path);
    FILE *f = fopen(full, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    fclose(f);
    if (n < 0) return -1;
    *size = (uint32_t)n;
    return 0;
}

int sa1_plat_file_read(const char *path, uint32_t off, void *dst, uint32_t n) {
    char full[1200];
    full_path(full, sizeof full, path);
    FILE *f = fopen(full, "rb");
    if (!f) return -1;
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return -1; }
    size_t got = fread(dst, 1, n, f);
    fclose(f);
    return (int)got;
}

int sa1_plat_dir_list(const char *dir, void (*cb)(const char *name, uint32_t size, void *arg), void *arg) {
    char full[1200];
    full_path(full, sizeof full, dir);
#ifdef _WIN32
    char pattern[1300];
    snprintf(pattern, sizeof pattern, "%s/*", full);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        cb(fd.cFileName, fd.nFileSizeLow, arg);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
#else
    DIR *d = opendir(full);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char p[1500];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", full, e->d_name);
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        cb(e->d_name, (uint32_t)st.st_size, arg);
    }
    closedir(d);
    return 0;
#endif
}

/* ---- memory at fixed addresses (docs/savestate.md section 1) ---------------- */
void *sa1_plat_fixed_alloc(uint64_t at, size_t n) {
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)at, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) sa1_log("fixed alloc: cannot map %zu bytes at 0x%llx (error %lu)", n, (unsigned long long)at, (unsigned long)GetLastError());
    return p;
#else
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
    void *p = mmap((void *)(uintptr_t)at, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) { sa1_log("fixed alloc: cannot map %zu bytes at 0x%llx (%s)", n, (unsigned long long)at, strerror(errno)); return NULL; }
    if ((uintptr_t)p != (uintptr_t)at) {       /* an old kernel took the address as a hint */
        munmap(p, n);
        sa1_log("fixed alloc: 0x%llx is taken (got %p)", (unsigned long long)at, p);
        return NULL;
    }
    return p;
#endif
}

int sa1_plat_ram_alloc(void) {
    sa1_ram = sa1_plat_fixed_alloc(SA1_FIXED_BASE, SA1_RAM_ALLOC);
    return sa1_ram ? 0 : -1;
}

/* ---- coroutines: our own stacks at fixed addresses, our own switch ---------
 * (coro_switch.c).  Slot i's stack is [lo, hi) inside one reservation above
 * guest RAM; a 64 KB gap below each stack stays unmapped, so an overflow
 * past main.c's stack probe faults instead of running into the next stack.
 * The OS thread's own stack is the supervisor's (savestate.c): the game
 * never runs on it. */
typedef struct sa1_coro_regs { uintptr_t sp; uintptr_t teb[3]; } sa1_coro_regs;
void sa1_coro_swap(sa1_coro_regs *from, sa1_coro_regs *to);
uintptr_t sa1_coro_first_frame(uintptr_t hi, void *co);

struct sa1_coro {
    sa1_coro_regs regs;       /* first: the assembly addresses it by offset */
    void (*fn)(void *);
    void *arg;
    uintptr_t lo, hi;         /* the stack */
    int used, index;          /* index -1 = the supervisor */
};

static sa1_coro slots[SA1_CORO_SLOTS];
static sa1_coro supervisor = { {0, {0, 0, 0}}, NULL, NULL, 0, 0, 1, -1 };
static sa1_coro *current = &supervisor;
static uint8_t *stack_region;

static int stacks_map(void) {
    if (stack_region) return 0;
    uint64_t at = SA1_FIXED_BASE + SA1_FIXED_STACKS_OFF;
    stack_region = sa1_plat_fixed_alloc(at, (size_t)SA1_CORO_STRIDE * SA1_CORO_SLOTS);
    if (!stack_region) return -1;
    for (int i = 0; i < SA1_CORO_SLOTS; i++) {
        uint8_t *guard = stack_region + (size_t)i * SA1_CORO_STRIDE;
        slots[i].lo = (uintptr_t)guard + SA1_CORO_GUARD;
        slots[i].hi = slots[i].lo + SA1_CORO_STACK;
        slots[i].index = i;
#ifdef _WIN32
        DWORD old;
        VirtualProtect(guard, SA1_CORO_GUARD, PAGE_NOACCESS, &old);
        /* The lowest pages of the stack itself are guard pages: a stack that
         * runs into them raises an exception Windows can still dispatch (the
         * dispatcher needs stack below the faulting frame; in the no-access
         * gap it has none, and the process dies with no report: the maintainer's
         * fourth exit-test run).  main.c reports it as a host stack overflow. */
        VirtualProtect((void *)slots[i].lo, SA1_CORO_GUARD_BAND, PAGE_READWRITE | PAGE_GUARD, &old);
#else
        mprotect(guard, SA1_CORO_GUARD, PROT_NONE);
#endif
    }
    return 0;
}

/* The TEB stack bounds a slot runs with (Windows); unused elsewhere. */
static void slot_teb(sa1_coro *co) {
    co->regs.teb[0] = co->hi;        /* StackBase */
    co->regs.teb[1] = co->lo;        /* StackLimit: the whole slot is committed */
    co->regs.teb[2] = co->lo - SA1_CORO_GUARD;   /* DeallocationStack */
}

/* First code of a new coroutine (sa1_coro_trampoline calls it). */
void sa1_coro_entry_c(void *p) {
    sa1_coro *co = (sa1_coro *)p;
    co->fn(co->arg);
    sa1_log("coroutine: entry function returned; back to the supervisor");
    for (;;) sa1_plat_coro_switch(&supervisor);
}

sa1_coro *sa1_plat_coro_supervisor(void) { return &supervisor; }
sa1_coro *sa1_plat_coro_current(void) { return current; }
/* The root game context: slot 0, created by the supervisor before the boot. */
sa1_coro *sa1_plat_coro_main(void) { return &slots[0]; }

sa1_coro *sa1_plat_coro_create(void (*fn)(void *arg), void *arg, unsigned stack_bytes) {
    if (stacks_map() != 0) return NULL;
    if (stack_bytes > SA1_CORO_STACK) sa1_log("coroutine: %u bytes of stack asked, slots are %u", stack_bytes, SA1_CORO_STACK);
    for (int i = 0; i < SA1_CORO_SLOTS; i++) {
        sa1_coro *co = &slots[i];
        if (co->used) continue;
        co->used = 1; co->fn = fn; co->arg = arg;
        slot_teb(co);
        co->regs.sp = sa1_coro_first_frame(co->hi, co);
        return co;
    }
    sa1_log("coroutine: all %d slots in use", SA1_CORO_SLOTS);
    return NULL;
}

void sa1_plat_coro_switch(sa1_coro *to) {
    sa1_coro *from = current;
    if (to == from) return;
    current = to;
    sa1_coro_swap(&from->regs, &to->regs);
}

void sa1_plat_coro_destroy(sa1_coro *co) {
    if (!co || co == &supervisor || co == current) return;
    co->used = 0; co->fn = NULL; co->arg = NULL;
}

int sa1_plat_coro_index(const sa1_coro *co) { return co ? co->index : -1; }
sa1_coro *sa1_plat_coro_by_index(int i) { return i >= 0 && i < SA1_CORO_SLOTS ? &slots[i] : NULL; }
void sa1_plat_coro_stack(int i, uintptr_t *lo, uintptr_t *hi) {
    if (stacks_map() != 0 || i < 0 || i >= SA1_CORO_SLOTS) { *lo = *hi = 0; return; }
    *lo = slots[i].lo; *hi = slots[i].hi;
}
int sa1_plat_coro_used(int i) { return i >= 0 && i < SA1_CORO_SLOTS && slots[i].used; }
uintptr_t sa1_plat_coro_sp(int i) { return i >= 0 && i < SA1_CORO_SLOTS ? slots[i].regs.sp : 0; }
void sa1_plat_coro_reset_all(void) {
    if (stacks_map() != 0) return;
    for (int i = 0; i < SA1_CORO_SLOTS; i++) { slots[i].used = 0; slots[i].fn = NULL; slots[i].arg = NULL; slots[i].regs.sp = 0; }
}
void sa1_plat_coro_restore(int i, uintptr_t sp) {
    if (stacks_map() != 0 || i < 0 || i >= SA1_CORO_SLOTS) return;
    slots[i].used = 1;
    slots[i].regs.sp = sp;
    slot_teb(&slots[i]);
}
