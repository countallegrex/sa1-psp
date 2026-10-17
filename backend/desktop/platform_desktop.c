/* platform_desktop.c - the desktop half of the platform interface in
 * hle/hle.h: a monotonic clock and game-data files from a directory on the
 * host disk, laid out like the disc (and like the Memory Stick will be).
 *
 * Part of sa1-psp (MIT). */
#include "hle.h"

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
#include <ucontext.h>
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

/* ---- coroutines: Windows fibers, or ucontext elsewhere ---------------------- */
struct sa1_coro {
    void (*fn)(void *);
    void *arg;
#ifdef _WIN32
    LPVOID fiber;
#else
    ucontext_t uc;
    void *stack;
#endif
};

static sa1_coro main_coro;

#ifdef _WIN32
static VOID CALLBACK coro_start(LPVOID p) {
    sa1_coro *co = (sa1_coro *)p;
    co->fn(co->arg);
    sa1_log("coroutine: entry function returned; back to main");
    for (;;) SwitchToFiber(main_coro.fiber);
}

sa1_coro *sa1_plat_coro_main(void) {
    if (!main_coro.fiber) main_coro.fiber = ConvertThreadToFiber(NULL);
    return &main_coro;
}

sa1_coro *sa1_plat_coro_create(void (*fn)(void *arg), void *arg, unsigned stack_bytes) {
    sa1_plat_coro_main();
    sa1_coro *co = calloc(1, sizeof *co);
    if (!co) return NULL;
    co->fn = fn; co->arg = arg;
    co->fiber = CreateFiber(stack_bytes, coro_start, co);
    if (!co->fiber) { free(co); return NULL; }
    return co;
}

void sa1_plat_coro_switch(sa1_coro *to) { SwitchToFiber(to->fiber); }

void sa1_plat_coro_destroy(sa1_coro *co) {
    if (co == &main_coro || !co) return;
    DeleteFiber(co->fiber);
    free(co);
}
#else
static sa1_coro *running = &main_coro;

static void coro_start(void) {
    sa1_coro *co = running;
    co->fn(co->arg);
    sa1_log("coroutine: entry function returned; back to main");
    for (;;) sa1_plat_coro_switch(&main_coro);
}

sa1_coro *sa1_plat_coro_main(void) { return &main_coro; }

sa1_coro *sa1_plat_coro_create(void (*fn)(void *arg), void *arg, unsigned stack_bytes) {
    sa1_coro *co = calloc(1, sizeof *co);
    if (!co) return NULL;
    co->fn = fn; co->arg = arg;
    co->stack = malloc(stack_bytes);
    getcontext(&co->uc);
    co->uc.uc_stack.ss_sp = co->stack;
    co->uc.uc_stack.ss_size = stack_bytes;
    co->uc.uc_link = NULL;
    makecontext(&co->uc, coro_start, 0);
    return co;
}

void sa1_plat_coro_switch(sa1_coro *to) {
    sa1_coro *from = running;
    running = to;
    swapcontext(&from->uc, &to->uc);
}

void sa1_plat_coro_destroy(sa1_coro *co) {
    if (co == &main_coro || !co) return;
    free(co->stack);
    free(co);
}
#endif
