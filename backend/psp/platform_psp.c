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
 * The pack: SONICADV/ is 2,068 files in one FAT directory, and each open
 * scans its long-name entries (tens of milliseconds on the console; about
 * 40 opens when Station Square loads).  When data/SONICADV.PAK is there
 * (tools/convert_assets.py --pack), every SONICADV/ path is served from it
 * through one descriptor: its index (name, offset, size, sorted by name) is
 * read once at the first file request.  Without it, the loose files.
 *
 * Part of sa1-psp (MIT). */
#include "platform_psp.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psprtc.h>

#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char data_root[256] = "ms0:/PSP/GAME/SA1PSP/data";

void plat_set_data_root(const char *root) { snprintf(data_root, sizeof data_root, "%s", root); }

uint64_t sa1_plat_time_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }

static void full_path(char *out, unsigned n, const char *rel) {
    snprintf(out, n, "%s/%s", data_root, rel);
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
}

static struct { uint64_t open_us, read_us, read_bytes, ra_wait_us; uint32_t opens, reads, ra_hits, ra_misses, ra_fetches; } io;

/* ---- the pack (tools/convert_assets.py writes it) ---------------------------
 * "SA1PAK" 0 1, u32 count, u32 index bytes, then count entries of
 * { char name[32]; u32 offset; u32 size; } sorted by name (upper case, the
 * name inside SONICADV/), then the files, each at a 2048-byte boundary. */
#define PAK_DIR "SONICADV/"
typedef struct { char name[32]; uint32_t off, size; } pak_ent;
static pak_ent *pak;
static uint32_t npak;
static SceUID pak_fd = -1;
static int pak_tried;

static void pak_open(void) {
    pak_tried = 1;
    char full[300];
    full_path(full, sizeof full, "SONICADV.PAK");
    SceUID fd = sceIoOpen(full, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    uint8_t hdr[16];
    uint32_t count = 0, bytes = 0;
    if (sceIoRead(fd, hdr, 16) == 16 && memcmp(hdr, "SA1PAK\0\1", 8) == 0) {
        memcpy(&count, hdr + 8, 4); memcpy(&bytes, hdr + 12, 4);
    }
    if (count && count < 65536 && bytes == count * sizeof(pak_ent) && (pak = malloc(bytes)) != NULL &&
        sceIoRead(fd, pak, bytes) == (int)bytes) {
        npak = count; pak_fd = fd;
        sa1_log("io: %s: %u files", full, npak);
        return;
    }
    sa1_log("io: %s is not a pack this build reads; using the loose files", full);
    free(pak); pak = NULL;
    sceIoClose(fd);
}

/* The pack entry of a data-root path, or NULL (no pack, outside SONICADV/,
 * not in it: a file the pack lacks is not looked for loose). */
static const pak_ent *pak_find(const char *rel) {
    if (!pak_tried) pak_open();
    if (!pak || strncasecmp(rel, PAK_DIR, sizeof PAK_DIR - 1) != 0) return NULL;
    char name[33];                 /* upper case, as the index (sorted by byte value) */
    snprintf(name, sizeof name, "%s", rel + sizeof PAK_DIR - 1);
    for (char *q = name; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 'a' + 'A');
    uint32_t lo = 0, hi = npak;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = strncmp(name, pak[mid].name, sizeof pak[mid].name);
        if (c == 0) return &pak[mid];
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
static int pak_covers(const char *rel) { if (!pak_tried) pak_open(); return pak && strncasecmp(rel, PAK_DIR, sizeof PAK_DIR - 1) == 0; }

/* n bytes at pos of fd, through the async read (see the top). */
static int read_at(SceUID fd, uint32_t pos, void *dst, uint32_t n) {
    uint64_t t0 = sa1_plat_time_us();
    if (sceIoLseek(fd, (SceOff)pos, PSP_SEEK_SET) < 0) return -1;
    uint32_t done = 0;
    while (done < n) {
        if (sceIoReadAsync(fd, (uint8_t *)dst + done, n - done) < 0) return done ? (int)done : -1;
        SceInt64 got = 0;
        if (sceIoWaitAsync(fd, &got) < 0 || got < 0) return done ? (int)done : -1;
        if (got == 0) break;
        done += (uint32_t)got;
    }
    io.read_us += sa1_plat_time_us() - t0; io.reads++; io.read_bytes += done;
    return (int)done;
}

/* ---- open-file cache ------------------------------------------------------- */
#define FD_CACHE 6
static struct { char path[160]; SceUID fd; uint32_t size, used; } fds[FD_CACHE];
static uint32_t fd_tick;

static void ra_close_all(void);

void plat_files_close_all(void) {
    ra_close_all();
    for (int i = 0; i < FD_CACHE; i++) if (fds[i].path[0]) { sceIoClose(fds[i].fd); fds[i].path[0] = 0; }
    if (pak_fd >= 0) { sceIoClose(pak_fd); pak_fd = -1; }
    free(pak); pak = NULL; npak = 0; pak_tried = 0;
}

static int fd_get(const char *rel, uint32_t *size) {
    for (int i = 0; i < FD_CACHE; i++)
        if (fds[i].path[0] && strcmp(fds[i].path, rel) == 0) { fds[i].used = ++fd_tick; if (size) *size = fds[i].size; return i; }
    char full[300];
    full_path(full, sizeof full, rel);
    uint64_t t0 = sa1_plat_time_us();
    SceUID fd = sceIoOpen(full, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    SceOff end = sceIoLseek(fd, 0, PSP_SEEK_END);
    if (end < 0) { sceIoClose(fd); return -1; }
    uint64_t dt = sa1_plat_time_us() - t0;
    io.open_us += dt; io.opens++;
    if (dt >= 20000) sa1_log("io: open %s took %u ms", rel, (unsigned)(dt / 1000u));
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
    if (pak_covers(path)) {
        const pak_ent *e = pak_find(path);
        if (!e) return -1;
        if (size) *size = e->size;
        return 0;
    }
    return fd_get(path, size) < 0 ? -1 : 0;
}

/* ---- read-ahead for the sound streams ---------------------------------------
 * The feeder (hle/sound/snd_host.c, feed_stream) tops each ADX ring up every
 * frame, a couple of KB at a time, and every read waited ~3 ms on the Memory
 * Stick on the main thread: 54 reads, 170 ms over 75 frames in Station
 * Square, 2.3 ms a frame.  Each stream path keeps a window of its file here,
 * topped up RA_STEP bytes at a time by sceIoReadAsync on a descriptor of its
 * own while the frame runs; a request inside the window is a memcpy.  The
 * bytes handed out are the file's, on the frame they are asked for, as with
 * a plain read: only when the Memory Stick does the work changes.  A request
 * outside the window (a loop back, a new stream) is read at once, and the
 * window restarts after it.  Window offsets and fetch sizes stay multiples
 * of 64, so a fetch never shares a cache line with bytes the CPU wrote. */
#define RA_SLOTS 2
#define RA_BYTES (32u << 10)
#define RA_STEP  (16u << 10)
typedef struct {
    uint8_t buf[RA_BYTES];          /* first: 64-byte aligned with the struct */
    char path[160];
    SceUID fd;
    uint32_t base, size;            /* where the path's bytes start in fd, and how many */
    uint32_t off, len;              /* the window: path bytes [off, off + len) at buf */
    uint32_t pend;                  /* a fetch in flight into buf + len, this many bytes */
    uint32_t used;
} ra_slot;
static ra_slot *ra[RA_SLOTS];
static uint32_t ra_tick;

/* Ends the fetch in flight; wait 0 only polls (1 when it is still running). */
static int ra_finish(ra_slot *s, int wait) {
    if (!s->pend) return 0;
    SceInt64 got = 0;
    if (!wait) {
        int r = sceIoPollAsync(s->fd, &got);
        if (r == 1) return 1;
        if (r < 0) got = 0;
    } else {
        uint64_t t0 = sa1_plat_time_us();
        if (sceIoWaitAsync(s->fd, &got) < 0) got = 0;
        io.ra_wait_us += sa1_plat_time_us() - t0;
    }
    if (got > 0) s->len += (uint32_t)got > s->pend ? s->pend : (uint32_t)got;
    s->pend = 0;
    return 0;
}

/* Waits for every read-ahead fetch in flight (the save files' synchronous
 * Memory Stick calls go after them: vmu_store_psp.c). */
void plat_io_quiesce(void) {
    for (int i = 0; i < RA_SLOTS; i++) if (ra[i] && ra[i]->path[0]) ra_finish(ra[i], 1);
}

static void ra_close(ra_slot *s) {
    ra_finish(s, 1);
    sceIoClose(s->fd);
    s->path[0] = 0;
}

static void ra_close_all(void) {
    for (int i = 0; i < RA_SLOTS; i++) if (ra[i] && ra[i]->path[0]) ra_close(ra[i]);
}

/* Starts the next fetch, first dropping what the reader is past (whole
 * lines below pos) when the window has no room for a step. */
static void ra_advance(ra_slot *s, uint32_t pos) {
    if (s->pend) return;
    uint32_t at = s->off + s->len;
    if (at >= s->size || (s->len & 63u)) return;
    if (RA_BYTES - s->len < RA_STEP) {
        uint32_t drop = pos > s->off ? (pos - s->off) & ~63u : 0;
        if (drop > s->len) drop = s->len;
        if (RA_BYTES - s->len + drop < RA_STEP) return;
        memmove(s->buf, s->buf + drop, s->len - drop);
        s->off += drop; s->len -= drop;
    }
    uint32_t n = s->size - at < RA_STEP ? s->size - at : RA_STEP;
    sceKernelDcacheWritebackInvalidateRange(s->buf + s->len, RA_STEP);
    if (sceIoLseek(s->fd, (SceOff)(s->base + at), PSP_SEEK_SET) < 0) return;
    if (sceIoReadAsync(s->fd, s->buf + s->len, n) < 0) return;
    s->pend = n;
    io.ra_fetches++;
}

/* The slot of path, opened on a descriptor of its own; NULL when it cannot be. */
static ra_slot *ra_get(const char *path) {
    int victim = -1;
    for (int i = 0; i < RA_SLOTS; i++) {
        if (ra[i] && ra[i]->path[0] && strcmp(ra[i]->path, path) == 0) { ra[i]->used = ++ra_tick; return ra[i]; }
        if (victim < 0 || !ra[i] || !ra[i]->path[0] || (ra[victim] && ra[victim]->path[0] && ra[i]->used < ra[victim]->used)) victim = i;
    }
    if (strlen(path) >= sizeof ra[0]->path) return NULL;
    uint32_t base = 0, size = 0;
    char full[300];
    if (pak_covers(path)) {
        const pak_ent *e = pak_find(path);
        if (!e) return NULL;
        base = e->off; size = e->size;
        full_path(full, sizeof full, "SONICADV.PAK");
    } else full_path(full, sizeof full, path);
    if (!ra[victim] && !(ra[victim] = memalign(64, sizeof *ra[victim]))) return NULL;
    ra_slot *s = ra[victim];
    if (s->path[0]) ra_close(s);
    SceUID fd = sceIoOpen(full, PSP_O_RDONLY, 0);
    if (fd < 0) return NULL;
    if (!base) {
        SceOff end = sceIoLseek(fd, 0, PSP_SEEK_END);
        if (end < 0) { sceIoClose(fd); return NULL; }
        size = (uint32_t)end;
    }
    snprintf(s->path, sizeof s->path, "%s", path);
    s->fd = fd; s->base = base; s->size = size;
    s->off = s->len = s->pend = 0;
    s->used = ++ra_tick;
    return s;
}

int sa1_plat_file_read_stream(const char *path, uint32_t off, void *dst, uint32_t n) {
    ra_slot *s = ra_get(path);
    if (!s) return sa1_plat_file_read(path, off, dst, n);
    if (off > s->size) return -1;
    if (n > s->size - off) n = s->size - off;
    ra_finish(s, 0);
    if (off >= s->off && off + n > s->off + s->len && off + n <= s->off + s->len + s->pend) ra_finish(s, 1);
    if (off >= s->off && off + n <= s->off + s->len) {
        memcpy(dst, s->buf + (off - s->off), n);
        io.ra_hits++;
    } else {
        ra_finish(s, 1);
        int got = read_at(s->fd, s->base + off, dst, n);
        io.ra_misses++;
        if (got < 0) return got;
        n = (uint32_t)got;
        s->off = (off + n) & ~63u;           /* the window restarts at the reader's line (re-fetching its head) */
        s->len = 0;
    }
    ra_advance(s, off + n);
    return (int)n;
}

int sa1_plat_file_read(const char *path, uint32_t off, void *dst, uint32_t n) {
    if (pak_covers(path)) {
        const pak_ent *e = pak_find(path);
        if (!e || off > e->size) return -1;
        if (n > e->size - off) n = e->size - off;
        return read_at(pak_fd, e->off + off, dst, n);
    }
    int i = fd_get(path, NULL);
    if (i < 0) return -1;
    return read_at(fds[i].fd, off, dst, n);
}

void plat_io_report(void) {
    if (!io.opens && !io.reads && !io.ra_hits && !io.ra_misses) return;
    if (io.opens || io.reads)
        sa1_log("io: %u opens %.1f ms (%.1f ms each), %u reads %.1f ms, %u KB (%.0f KB/s)", io.opens, io.open_us / 1000.0,
            io.opens ? io.open_us / 1000.0 / io.opens : 0.0, io.reads, io.read_us / 1000.0, (unsigned)(io.read_bytes / 1024u),
            io.read_us ? io.read_bytes / 1024.0 / (io.read_us / 1e6) : 0.0);
    if (io.ra_hits || io.ra_misses)
        sa1_log("io: stream read-ahead %u hits, %u misses, %u fetches, %.1f ms waited", io.ra_hits, io.ra_misses,
                io.ra_fetches, io.ra_wait_us / 1000.0);
    memset(&io, 0, sizeof io);
}

int sa1_plat_dir_list(const char *dir, void (*cb)(const char *name, uint32_t size, void *arg), void *arg) {
    char sub[300];
    snprintf(sub, sizeof sub, "%s/", dir);
    if (pak_covers(sub) && strlen(sub) == sizeof PAK_DIR - 1) {
        for (uint32_t i = 0; i < npak; i++) {
            char name[33];
            memcpy(name, pak[i].name, 32); name[32] = 0;
            cb(name, pak[i].size, arg);
        }
        return 0;
    }
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

/* At exit: the buffer out and nothing left open on host0:. */
void plat_log_close(void) {
    plat_log_flush();
    if (log_fd >= 0) { sceIoClose(log_fd); log_fd = -1; }
    log_path_s[0] = 0;
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
