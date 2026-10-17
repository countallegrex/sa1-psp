/* gdfs.c - the GD-ROM file system (gdFs) over the data root.
 *
 * The game opens, sizes, reads and closes files through eleven entries, and
 * polls two of them (gdFsGetStat, gdFsExecServer) while a request is in
 * flight.  Reads complete synchronously here, so a request is "complete"
 * (status 1) as soon as it is issued; the game's own loaders then take the
 * done branch on the next poll.
 *
 * Handles are guest-RAM records, 84 bytes each, inside the work area the
 * game passes to gdFsInit (at work + 0x1080, as the SDK lays them out): the
 * loaders keep the handle pointer in guest memory and the game reads the
 * size (+12), sectors (+16), position (+20), in-use (+76) and status (+80)
 * fields only through these entries, but keeping the real offsets costs
 * nothing and lets a memory dump be compared with the console.
 *
 * Names are matched as the disc spells them: the current directory
 * ("SONICADV" after init, the root after "..") plus the file name in upper
 * case.  The data root holds the same tree the Memory Stick will.
 *
 * Part of sa1-psp (MIT). */
#include "shinobi.h"

static char last_opened[200];           /* the last file opened (debug aids name their source by it) */
const char *shinobi_last_opened_file(void) { return last_opened; }

#include <stdio.h>
#include <string.h>

#define GD_MAX_OPEN 8
#define H_SIZE 84
#define H_WORK 0
#define H_FAD 8
#define H_BYTES 12
#define H_SECTORS 16
#define H_POS 20          /* current sector offset */
#define H_DONE 24         /* bytes transferred */
#define H_REQ 28
#define H_INUSE 76
#define H_STATUS 80       /* 0 idle, 1 complete, 2 reading, 5 error */
#define H_ERR 82

enum { GDFS_OK = 0, GDFS_ERR_NOTFOUND = -8, GDFS_ERR_NOHANDLE = -9, GDFS_ERR_BADHANDLE = -10,
       GDFS_ERR_NOTDIR = -11, GDFS_ERR_ALIGN = -14 };

typedef struct { char path[128]; uint32_t size; int open; } gd_file;
static gd_file files[GD_MAX_OPEN];
static uint32_t work_area, handles_base;
static char cur_dir[64];      /* "" = root, else "SONICADV" */
static int initialised;
static uint32_t err_func, err_arg;

void shinobi_gdfs_init(void) { initialised = 0; cur_dir[0] = 0; }

static void upper(char *s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A'); }

static void make_path(char *out, unsigned n, const char *name) {
    char nm[64];
    snprintf(nm, sizeof nm, "%s", name);
    upper(nm);
    /* a leading separator means the root */
    const char *p = nm;
    while (*p == '/' || *p == '\\') p++;
    if (p != nm || cur_dir[0] == 0) snprintf(out, n, "%s", p);
    else snprintf(out, n, "%s/%s", cur_dir, p);
}

/* For other HLE modules (texture files by name): the data-root-relative
 * path of a name in the current directory. */
void shinobi_gdfs_path(char *out, unsigned n, const char *name) { make_path(out, n, name); }

static int handle_index(uint32_t h) {
    if (!initialised || h < handles_base || h >= handles_base + GD_MAX_OPEN * H_SIZE) return -1;
    if ((h - handles_base) % H_SIZE) return -1;
    int i = (int)((h - handles_base) / H_SIZE);
    return files[i].open ? i : -1;
}

static uint32_t handle_addr(int i) { return handles_base + (uint32_t)i * H_SIZE; }

/* 0x8C600032: gdFsInit(max_open, work, max_dirent, dirbuf).  The game
 * retries on -23/-33 (drive errors) and exits to the BIOS; 0 is the only
 * answer that boots. */
void sdk_8c600032(sh4_ctx *c) {
    HLE_ENTER(0x8c600032u, "gdFsInit");
    work_area = ARG1;
    handles_base = work_area + 0x1080;
    for (int i = 0; i < GD_MAX_OPEN; i++) files[i].open = 0;
    hle_wr32(work_area + 0, ARG0);
    hle_wr16(work_area + 56, 1);
    cur_dir[0] = 0;
    initialised = 1;
    sa1_log("gdFs: init, %u handles at 0x%08x", (unsigned)GD_MAX_OPEN, handles_base);
    RET(GDFS_OK);
}

/* 0x8C600DDC: gdFsChangeDir(name): "SONICADV", "..", or a rooted path. */
void sdk_8c600ddc(sh4_ctx *c) {
    HLE_ENTER(0x8c600ddcu, "gdFsChangeDir");
    char name[64];
    hle_guest_string(ARG0, name, sizeof name);
    if (!strcmp(name, "..") || name[0] == '/' || name[0] == '\\' || name[0] == 0) {
        cur_dir[0] = 0;
        if (name[0] == '/' || name[0] == '\\') { snprintf(cur_dir, sizeof cur_dir, "%s", name + 1); upper(cur_dir); }
    } else {
        snprintf(cur_dir, sizeof cur_dir, "%s", name);
        upper(cur_dir);
    }
    sa1_log("gdFs: cd \"%s\" -> \"%s\"", name, cur_dir[0] ? cur_dir : "/");
    RET(GDFS_OK);
}

/* 0x8C6016D6: gdFsOpen(fname, dirrec) -> handle or 0 (-8 not found, -9 no
 * free handle are recorded as the last error). */
void sdk_8c6016d6(sh4_ctx *c) {
    HLE_ENTER(0x8c6016d6u, "gdFsOpen");
    char name[64], path[128];
    hle_guest_string(ARG0, name, sizeof name);
    make_path(path, sizeof path, name);
    int i;
    for (i = 0; i < GD_MAX_OPEN && files[i].open; i++) {}
    if (i == GD_MAX_OPEN) {
        sa1_log("gdFs: open \"%s\": no free handle", path);
        hle_wr32(work_area + 48, (uint32_t)GDFS_ERR_NOHANDLE);
        RET(0);
        return;
    }
    uint32_t size;
    if (sa1_plat_file_size(path, &size) != 0) {
        sa1_log("gdFs: open \"%s\": not found", path);
        hle_wr32(work_area + 48, (uint32_t)GDFS_ERR_NOTFOUND);
        RET(0);
        return;
    }
    snprintf(files[i].path, sizeof files[i].path, "%s", path);
    files[i].size = size;
    files[i].open = 1;
    uint32_t h = handle_addr(i);
    for (int k = 0; k < H_SIZE; k += 4) hle_wr32(h + k, 0);
    hle_wr32(h + H_WORK, work_area);
    hle_wr32(h + H_BYTES, size);
    hle_wr32(h + H_SECTORS, (size + 2047) >> 11);
    hle_wr16(h + H_INUSE, 1);
    hle_wr16(h + H_STATUS, 0);
    sa1_log("gdFs: open \"%s\" (%u bytes) -> handle %d", path, size, i);
    snprintf(last_opened, sizeof last_opened, "%s", path);
    RET(h);
}

/* 0x8C600E2C: gdFsGetFileSize(handle, &size) -> 1, or 0 for a bad handle. */
void sdk_8c600e2c(sh4_ctx *c) {
    HLE_ENTER(0x8c600e2cu, "gdFsGetFileSize");
    int i = handle_index(ARG0);
    if (i < 0) { RET(0); return; }
    hle_wr32(ARG1, files[i].size);
    RET(1);
}

/* Reads n sectors at the handle's position into guest RAM; returns sectors
 * actually read (clamped at EOF) or -1 on a host error. */
static int read_sectors(int i, uint32_t h, uint32_t nsect, uint32_t dst) {
    uint32_t pos = hle_rd32(h + H_POS);
    uint32_t total = (files[i].size + 2047) >> 11;
    if (pos >= total) return 0;
    if (pos + nsect > total) nsect = total - pos;
    uint32_t off = pos << 11, bytes = nsect << 11;
    if (off + bytes > files[i].size) bytes = files[i].size - off;
    if (!hle_in_ram(dst) || (dst & SA1_RAM_MASK) + (nsect << 11) > SA1_RAM_SIZE) {
        sa1_log("gdFs: read of %u sectors to 0x%08x is outside guest RAM", nsect, dst);
        return -1;
    }
    int got = sa1_plat_file_read(files[i].path, off, hle_ptr(dst), bytes);
    if (got < 0) return -1;
    /* the drive delivers whole sectors; the tail of the last one is zero */
    if ((uint32_t)got < (nsect << 11)) memset(hle_ptr(dst) + got, 0, (nsect << 11) - (uint32_t)got);
    hle_wr32(h + H_POS, pos + nsect);
    hle_wr32(h + H_DONE, hle_rd32(h + H_DONE) + (uint32_t)got);
    return (int)nsect;
}

/* 0x8C600F1E: gdFsRead(handle, nsectors, buf), synchronous -> 0 or error. */
void sdk_8c600f1e(sh4_ctx *c) {
    HLE_ENTER(0x8c600f1eu, "gdFsRead");
    int i = handle_index(ARG0);
    if (i < 0) { RET(GDFS_ERR_BADHANDLE); return; }
    int n = read_sectors(i, ARG0, ARG1, ARG2);
    hle_wr16(ARG0 + H_STATUS, n < 0 ? 5 : 1);
    RET(n < 0 ? -1 : GDFS_OK);
}

/* 0x8C601084: gdFsReqRd32(handle, nsectors, buf), asynchronous on the
 * console -> sectors queued (0 at EOF).  Done at once here; the handle is
 * complete (status 1) when the game polls it. */
void sdk_8c601084(sh4_ctx *c) {
    HLE_ENTER(0x8c601084u, "gdFsReqRd32");
    int i = handle_index(ARG0);
    if (i < 0) { RET(GDFS_ERR_BADHANDLE); return; }
    if (ARG2 & 31) { RET(GDFS_ERR_ALIGN); return; }
    int n = read_sectors(i, ARG0, ARG1, ARG2);
    hle_wr32(ARG0 + H_REQ, n > 0 ? (uint32_t)n : 0);
    hle_wr16(ARG0 + H_STATUS, n < 0 ? 5 : 1);
    RET(n < 0 ? 0 : (uint32_t)n);
}

/* 0x8C6013A4: gdFsGetStat(handle) -> status (0 idle, 1 complete, 2 busy,
 * 5 error), -10 bad handle.  Polled by the streaming loader. */
void sdk_8c6013a4(sh4_ctx *c) {
    HLE_ENTER(0x8c6013a4u, "gdFsGetStat");
    int i = handle_index(ARG0);
    RET(i < 0 ? (uint32_t)GDFS_ERR_BADHANDLE : hle_rd16(ARG0 + H_STATUS));
}

/* 0x8C600DF4: gdFsClose(handle). */
void sdk_8c600df4(sh4_ctx *c) {
    HLE_ENTER(0x8c600df4u, "gdFsClose");
    int i = handle_index(ARG0);
    if (i < 0) return;
    files[i].open = 0;
    hle_wr16(ARG0 + H_INUSE, 0);
    hle_wr16(ARG0 + H_STATUS, 0);
}

/* 0x8C601748: gdFsExecServer: drives asynchronous requests; polled by the
 * CRI read loop.  Everything is already complete. */
void sdk_8c601748(sh4_ctx *c) { HLE_ENTER(0x8c601748u, "gdFsExecServer"); RET(0); }

/* 0x8C60178C: gdFsGetDrvStat -> 2 (standby: disc present, idle).  The game
 * treats 6 as "tray open". */
void sdk_8c60178c(sh4_ctx *c) { HLE_ENTER(0x8c60178cu, "gdFsGetDrvStat"); RET(2); }

/* 0x8C60175E: gdFsEntryErrFuncAll(func, arg): kept, never invoked. */
void sdk_8c60175e(sh4_ctx *c) {
    HLE_ENTER(0x8c60175eu, "gdFsEntryErrFuncAll");
    err_func = ARG0; err_arg = ARG1;
}

/* Byte position / size queries the CRI ADX layer makes about files by name
 * (0x8C65C25C start sector, 0x8C65C27A size in sectors, 0x8C65C2A8 size in
 * bytes).  -1 makes the ADX partition loader give up cleanly (no audio
 * this milestone). */
void sdk_8c65c25c(sh4_ctx *c) { HLE_ENTER(0x8c65c25cu, "ADXF file start (off)"); RET(-1); }
void sdk_8c65c27a(sh4_ctx *c) { HLE_ENTER(0x8c65c27au, "ADXF file sectors (off)"); RET(-1); }
void sdk_8c65c2a8(sh4_ctx *c) { HLE_ENTER(0x8c65c2a8u, "ADXF file bytes (off)"); RET(-1); }
