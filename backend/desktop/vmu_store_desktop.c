/* vmu_store_desktop.c - the HLE VMU's files in a host folder (by default
 * "vmu" beside the executable, or --vmu-dir DIR): VMU file NAME is the host
 * file NAME.VMS, holding the exact image the game saved (VMS header, icons,
 * data; a multiple of 512 bytes).  hle/shinobi/vmu_store.h.
 *
 * Part of sa1-psp (MIT). */
#include "../../hle/shinobi/vmu_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static char vmu_dir[1024];

static int path_of(char *out, size_t n, const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len > 12) return -1;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-' && name[i] != '.') return -1;
    snprintf(out, n, "%s/%s.VMS", vmu_dir, name);
    return 0;
}

static int32_t st_read(const char *name, void *buf, uint32_t max) {
    char p[1100];
    if (path_of(p, sizeof p, name)) return -1;
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t r = fread(buf, 1, max, f);
    fclose(f);
    return (int32_t)r;
}

static int st_write(const char *name, const void *buf, uint32_t n) {
    char p[1100], t[1110];
    if (path_of(p, sizeof p, name)) return -1;
    snprintf(t, sizeof t, "%s.tmp", p);   /* a crash mid-write keeps the old save */
    FILE *f = fopen(t, "wb");
    if (!f) return -1;
    int ok = fwrite(buf, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (!ok) { remove(t); return -1; }
#ifdef _WIN32
    return MoveFileExA(t, p, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(t, p) == 0 ? 0 : -1;
#endif
}

static int st_remove(const char *name) {
    char p[1100];
    if (path_of(p, sizeof p, name)) return -1;
    return remove(p) == 0 ? 0 : -1;
}

typedef struct { char name[13]; uint32_t size; } entry;
static int cmp_entry(const void *a, const void *b) { return strcmp(((const entry *)a)->name, ((const entry *)b)->name); }

/* Fills e[] (up to max) with the folder's NAME.VMS files; returns the count. */
static int scan(entry *e, int max) {
    int n = 0;
    char fname[260];
#ifdef _WIN32
    char pat[1100];
    snprintf(pat, sizeof pat, "%s/*.VMS", vmu_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(fname, sizeof fname, "%s", fd.cFileName);
#else
    DIR *d = opendir(vmu_dir);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        snprintf(fname, sizeof fname, "%s", de->d_name);
#endif
        size_t len = strlen(fname);
        if (len < 5 || len > 16 || strcmp(fname + len - 4, ".VMS") != 0 || n >= max) continue;
        fname[len - 4] = 0;
        char p[1100];
        if (path_of(p, sizeof p, fname)) continue;
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        if (sz <= 0) continue;
        snprintf(e[n].name, sizeof e[n].name, "%s", fname);
        e[n].size = (uint32_t)sz;
        n++;
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif
    qsort(e, (size_t)n, sizeof *e, cmp_entry);
    return n;
}

static int st_list(int index, char name[13], uint32_t *size) {
    entry e[200];   /* a VMU holds at most 200 one-block files */
    int n = scan(e, 200);
    if (index < 0 || index >= n) return -1;
    memcpy(name, e[index].name, 13);
    *size = e[index].size;
    return 0;
}

static const vmu_store_ops desktop_ops = { st_read, st_write, st_remove, st_list };

/* dir NULL: "vmu" beside the executable.  Creates the folder. */
void vmu_store_desktop_init(const char *dir) {
    if (dir) snprintf(vmu_dir, sizeof vmu_dir, "%s", dir);
    else {
        char exe[1024] = "";
#ifdef _WIN32
        GetModuleFileNameA(NULL, exe, sizeof exe);
#else
        ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1);
        exe[r > 0 ? r : 0] = 0;
#endif
        char *slash = strrchr(exe, '/'), *bs = strrchr(exe, '\\');
        if (bs > slash) slash = bs;
        if (slash) { *slash = 0; snprintf(vmu_dir, sizeof vmu_dir, "%s/vmu", exe); }
        else snprintf(vmu_dir, sizeof vmu_dir, "vmu");
    }
#ifdef _WIN32
    _mkdir(vmu_dir);
#else
    mkdir(vmu_dir, 0777);
#endif
    hle_vmu_store = &desktop_ops;
}

const char *vmu_store_desktop_dir(void) { return vmu_dir; }
