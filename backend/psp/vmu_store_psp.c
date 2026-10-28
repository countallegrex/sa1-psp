/* vmu_store_psp.c - the HLE VMU's files on the memory stick: VMU file NAME
 * is DIR/NAME.VMS (DIR = "vmu" beside the EBOOT, e.g.
 * ms0:/PSP/GAME/SA1PSP/vmu), the exact image the game saved, as the desktop
 * folder store (backend/desktop/vmu_store_desktop.c) keeps it, so the two
 * can trade saves.  hle/shinobi/vmu_store.h.
 *
 * Part of sa1-psp (MIT). */
#include "../../hle/shinobi/vmu_store.h"
#include "hle.h"
#include "platform_psp.h"
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char vmu_dir[300];
/* --vmu-quiesce: finish the sound streams' async reads before any call
 * here; --vmu-trace: a flushed log line around every Memory Stick call
 * (the 2026-10-27 play-test froze with the stick's light on at the first
 * save-select screen; main_psp.c). */
int vmu_psp_quiesce, vmu_psp_trace;
static unsigned trace_n;
#define VT(...) do { if (vmu_psp_trace && trace_n < 400) { trace_n++; sa1_log(__VA_ARGS__); plat_log_flush(); } } while (0)
static void enter(void) { if (vmu_psp_quiesce) plat_io_quiesce(); }

static int path_of(char *out, size_t n, const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len > 12) return -1;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-' && name[i] != '.') return -1;
    snprintf(out, n, "%s/%s.VMS", vmu_dir, name);
    return 0;
}

static int32_t st_read(const char *name, void *buf, uint32_t max) {
    char p[340];
    if (path_of(p, sizeof p, name)) return -1;
    enter();
    VT("vmu: read %s: open", p);
    SceUID fd = sceIoOpen(p, PSP_O_RDONLY, 0);
    if (fd < 0) {   /* a write that died between its two copies (st_write) */
        char t[350];
        snprintf(t, sizeof t, "%s.tmp", p);
        fd = sceIoOpen(t, PSP_O_RDONLY, 0);
    }
    VT("vmu: read: open -> 0x%08x", (unsigned)fd);
    if (fd < 0) return -1;
    int r = sceIoRead(fd, buf, max);
    sceIoClose(fd);
    VT("vmu: read: %d bytes", r);
    return r < 0 ? -1 : r;
}

static int write_file(const char *path, const void *buf, uint32_t n) {
    VT("vmu: write %s (%u bytes): open", path, (unsigned)n);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    VT("vmu: write: open -> 0x%08x", (unsigned)fd);
    if (fd < 0) return -1;
    int w = sceIoWrite(fd, buf, n);
    VT("vmu: write: write -> %d", w);
    int ok = w == (int)n;
    ok &= sceIoClose(fd) >= 0;
    VT("vmu: write: closed, %s", ok ? "ok" : "FAILED");
    return ok ? 0 : -1;
}

/* NAME.tmp first, then NAME.VMS, then the .tmp goes: a power-off mid-write
 * leaves one good copy (st_read falls back to the .tmp).  A rename would be
 * one step, but sceIoRename fails on host0: (PSPLink's usbhostfs). */
static int st_write(const char *name, const void *buf, uint32_t n) {
    char p[340], t[350];
    if (path_of(p, sizeof p, name)) return -1;
    enter();
    snprintf(t, sizeof t, "%s.tmp", p);
    if (write_file(t, buf, n)) { sceIoRemove(t); return -1; }
    if (write_file(p, buf, n)) return -1;
    sceIoRemove(t);
    return 0;
}

static int st_remove(const char *name) {
    char p[340];
    if (path_of(p, sizeof p, name)) return -1;
    enter();
    VT("vmu: remove %s", p);
    return sceIoRemove(p) >= 0 ? 0 : -1;
}

typedef struct { char name[13]; uint32_t size; } entry;
static int cmp_entry(const void *a, const void *b) { return strcmp(((const entry *)a)->name, ((const entry *)b)->name); }

static int st_list(int index, char name[13], uint32_t *size) {
    static entry e[200];   /* a VMU holds at most 200 one-block files */
    int n = 0;
    enter();
    VT("vmu: list %d: dopen %s", index, vmu_dir);
    SceUID d = sceIoDopen(vmu_dir);
    VT("vmu: list: dopen -> 0x%08x", (unsigned)d);
    if (d < 0) return -1;
    SceIoDirent de;
    memset(&de, 0, sizeof de);
    while (sceIoDread(d, &de) > 0) {
        size_t len = strlen(de.d_name);
        if (!FIO_S_ISDIR(de.d_stat.st_mode) && len >= 5 && len <= 16 && n < 200) {
            /* the stick may report names in either case */
            const char *ext = de.d_name + len - 4;
            if (ext[0] == '.' && toupper((unsigned char)ext[1]) == 'V' && toupper((unsigned char)ext[2]) == 'M' && toupper((unsigned char)ext[3]) == 'S'
                && de.d_stat.st_size > 0) {
                memcpy(e[n].name, de.d_name, len - 4);
                e[n].name[len - 4] = 0;
                char p[340];
                if (!path_of(p, sizeof p, e[n].name)) { e[n].size = (uint32_t)de.d_stat.st_size; n++; }
            }
        }
        memset(&de, 0, sizeof de);
    }
    sceIoDclose(d);
    VT("vmu: list: %d files, closed", n);
    qsort(e, (size_t)n, sizeof *e, cmp_entry);
    if (index < 0 || index >= n) return -1;
    memcpy(name, e[index].name, 13);
    *size = e[index].size;
    return 0;
}

static const vmu_store_ops psp_ops = { st_read, st_write, st_remove, st_list };

/* dir: the folder for the files (main_psp.c: "vmu" beside the EBOOT, or
 * --vmu-dir).  Creates it. */
void vmu_store_psp_init(const char *dir) {
    snprintf(vmu_dir, sizeof vmu_dir, "%s", dir);
    sceIoMkdir(vmu_dir, 0777);
    hle_vmu_store = &psp_ops;
}

const char *vmu_store_psp_dir(void) { return vmu_dir; }
