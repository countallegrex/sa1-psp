/* platform_psp.c - the PSP half of the platform interface in hle/hle.h:
 * the clock, game-data files on the Memory Stick through sceIo, and the log.
 *
 * Files: the data root holds the disc's tree ("SONICADV/ADV00.PRS", and
 * 1ST_READ.BIN next to it), as tools/convert_assets.py writes it.  gdFs
 * reads a file in many pieces (hle/shinobi/gdfs.c), and an open on the
 * Memory Stick costs milliseconds, so a few descriptors stay open, least
 * recently used first out.  Reads go through sceIoReadAsync and wait for it
 * at once: gdFs completes every request synchronously (the game polls a
 * status that is already "done"), so nothing overlaps yet; the async call
 * is where a read could later run behind the frame (README section 8, M5).
 *
 * Part of sa1-psp (MIT). */
#include "platform_psp.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psprtc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char data_root[256] = "ms0:/PSP/GAME/SA1PSP/data";

void plat_set_data_root(const char *root) { snprintf(data_root, sizeof data_root, "%s", root); }

uint64_t sa1_plat_time_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }

static void full_path(char *out, unsigned n, const char *rel) {
    snprintf(out, n, "%s/%s", data_root, rel);
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
}

/* ---- open-file cache ------------------------------------------------------- */
#define FD_CACHE 6
static struct { char path[160]; SceUID fd; uint32_t size, used; } fds[FD_CACHE];
static uint32_t fd_tick;

void plat_files_close_all(void) {
    for (int i = 0; i < FD_CACHE; i++) if (fds[i].path[0]) { sceIoClose(fds[i].fd); fds[i].path[0] = 0; }
}

static int fd_get(const char *rel, uint32_t *size) {
    for (int i = 0; i < FD_CACHE; i++)
        if (fds[i].path[0] && strcmp(fds[i].path, rel) == 0) { fds[i].used = ++fd_tick; if (size) *size = fds[i].size; return i; }
    char full[300];
    full_path(full, sizeof full, rel);
    SceUID fd = sceIoOpen(full, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    SceOff end = sceIoLseek(fd, 0, PSP_SEEK_END);
    if (end < 0) { sceIoClose(fd); return -1; }
    int victim = 0;
    for (int i = 0; i < FD_CACHE; i++) {
        if (!fds[i].path[0]) { victim = i; break; }
        if (fds[i].used < fds[victim].used) victim = i;
    }
    if (fds[victim].path[0]) sceIoClose(fds[victim].fd);
    snprintf(fds[victim].path, sizeof fds[victim].path, "%s", rel);
    fds[victim].fd = fd; fds[victim].size = (uint32_t)end; fds[victim].used = ++fd_tick;
    if (size) *size = (uint32_t)end;
    return victim;
}

int sa1_plat_file_size(const char *path, uint32_t *size) {
    return fd_get(path, size) < 0 ? -1 : 0;
}

int sa1_plat_file_read(const char *path, uint32_t off, void *dst, uint32_t n) {
    int i = fd_get(path, NULL);
    if (i < 0) return -1;
    if (sceIoLseek32(fds[i].fd, (int)off, PSP_SEEK_SET) < 0) return -1;
    uint32_t done = 0;
    while (done < n) {
        if (sceIoReadAsync(fds[i].fd, (uint8_t *)dst + done, n - done) < 0) return done ? (int)done : -1;
        SceInt64 got = 0;
        if (sceIoWaitAsync(fds[i].fd, &got) < 0 || got < 0) return done ? (int)done : -1;
        if (got == 0) break;
        done += (uint32_t)got;
    }
    return (int)done;
}

int sa1_plat_dir_list(const char *dir, void (*cb)(const char *name, uint32_t size, void *arg), void *arg) {
    char full[300];
    full_path(full, sizeof full, dir);
    SceUID d = sceIoDopen(full);
    if (d < 0) return -1;
    SceIoDirent e;
    memset(&e, 0, sizeof e);
    while (sceIoDread(d, &e) > 0) {
        if (!FIO_S_ISDIR(e.d_stat.st_mode)) {
            char name[260];
            snprintf(name, sizeof name, "%s", e.d_name);
            for (char *p = name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
            cb(name, (uint32_t)e.d_stat.st_size, arg);
        }
        memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);
    return 0;
}

/* ---- log ---------------------------------------------------------------------
 * Lines collect in a buffer that goes to the file when it fills, every few
 * seconds (main_psp.c flushes per 120 frames) and at exit or trap: a write
 * per line would cost a Memory Stick write each. */
static SceUID log_fd = -1;
static int log_stdout;
static char log_path_s[300];
/* Write-through mode (plat_log_sync): every line is appended and the file
 * closed at once, so a crash that takes the console down still leaves the
 * lines before it on the card (a crash on the first hardware boot left an
 * empty log: the buffer never reached the stick). */
static int log_sync = 1;
static char log_buf[32 * 1024];
static unsigned log_len;
static uint64_t log_t0;

void plat_log_flush(void) {
    if (log_fd >= 0 && log_len) sceIoWrite(log_fd, log_buf, log_len);
    log_len = 0;
}

void plat_log_sync(int on) {
    if (on == log_sync) return;
    if (on) { plat_log_flush(); if (log_fd >= 0) { sceIoClose(log_fd); log_fd = -1; } }
    else if (log_path_s[0]) log_fd = sceIoOpen(log_path_s, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    log_sync = on;
}

static void log_sink(const char *line) {
    uint64_t t = sa1_plat_time_us() - log_t0;
    char head[48];
    int h = snprintf(head, sizeof head, "[%6u.%03u f%-6u] ", (unsigned)(t / 1000000u), (unsigned)(t / 1000u % 1000u), hle_frame_count());
    unsigned l = (unsigned)strlen(line);
    if (log_stdout) { sceIoWrite(1, head, (SceSize)h); sceIoWrite(1, line, l); sceIoWrite(1, "\n", 1); }
    if (log_sync) {
        if (!log_path_s[0]) return;
        SceUID fd = sceIoOpen(log_path_s, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd < 0) return;
        sceIoWrite(fd, head, (SceSize)h); sceIoWrite(fd, line, l); sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
        return;
    }
    if (log_fd < 0) return;
    if (log_len + (unsigned)h + l + 1 > sizeof log_buf) plat_log_flush();
    if ((unsigned)h + l + 1 > sizeof log_buf) return;
    memcpy(log_buf + log_len, head, (size_t)h); log_len += (unsigned)h;
    memcpy(log_buf + log_len, line, l); log_len += l;
    log_buf[log_len++] = '\n';
}

int plat_log_open(const char *path, int to_stdout) {
    log_t0 = sa1_plat_time_us();
    log_stdout = to_stdout;
    if (path) {
        snprintf(log_path_s, sizeof log_path_s, "%s", path);
        log_fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (log_fd < 0) return -1;
        if (log_sync) { sceIoClose(log_fd); log_fd = -1; }   /* truncated; lines append from here */
    }
    sa1_set_log_sink(log_sink);
    return 0;
}
