/* Host simulation of sa1autolink.c: fake clock, fake ms0, fake loadexec. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mock/pspkernel.h"
#include "mock/pspctrl.h"
#include "mock/psploadexec_kernel.h"

int module_start(SceSize args, void *argp);

static jmp_buf done;
static long long us;                 /* fake time since base */
static const int BASE = 1792000000;  /* 2026-10 */
static SceKernelThreadEntry entry;
static char launched[64];
int low_until_s = -1, fs_fail_write, usb_until_s = -1, battery = 80, lr_held, clock_dead;

/* ---- fake filesystem ---- */
#define NF 16
static struct { char path[64]; char data[40000]; int len, used; } fs[NF];
static struct { int f, flags, pos, open; } fds[8];

static int ffind(const char *p) { for (int i = 0; i < NF; i++) if (fs[i].used && !strcmp(fs[i].path, p)) return i; return -1; }
void fput(const char *p, const void *d, int n) { int i = ffind(p); if (i < 0) for (i = 0; fs[i].used; i++); fs[i].used = 1; strcpy(fs[i].path, p); memcpy(fs[i].data, d, n); fs[i].len = n; }
void fdel(const char *p) { int i = ffind(p); if (i >= 0) fs[i].used = 0; }
int flen(const char *p) { int i = ffind(p); return i < 0 ? -1 : fs[i].len; }
const char *fdata(const char *p) { int i = ffind(p); return i < 0 ? "" : fs[i].data; }
static int usb_now(void) { return usb_until_s >= 0 && us / 1000000 < usb_until_s; }

int sceIoGetstat(const char *p, SceIoStat *s) { if (usb_now()) return -1; int i = ffind(p); if (i < 0) return 0x80010002; s->st_size = !strcmp(p + strlen(p) - 4, ".PBP") && fs[i].len == 7 ? 131031 : fs[i].len; return 0; }
SceUID sceIoOpen(const char *p, int f, int m) {
    (void)m; if (usb_now()) return -1;
    int i = ffind(p);
    if (f & PSP_O_WRONLY) { if (fs_fail_write) return -5; if (i < 0) { fput(p, "", 0); i = ffind(p); } if (f & PSP_O_TRUNC) fs[i].len = 0; }
    else if (i < 0) return -2;
    for (int k = 0; k < 8; k++) if (!fds[k].open) { fds[k].open = 1; fds[k].f = i; fds[k].flags = f; fds[k].pos = (f & PSP_O_APPEND) ? fs[i].len : 0; return k + 3; }
    return -3;
}
int sceIoRead(SceUID fd, void *b, unsigned n) { fd -= 3; int i = fds[fd].f; int left = fs[i].len - fds[fd].pos; if ((int)n > left) n = left; memcpy(b, fs[i].data + fds[fd].pos, n); fds[fd].pos += n; return n; }
int sceIoWrite(SceUID fd, const void *b, unsigned n) { fd -= 3; int i = fds[fd].f; if (fds[fd].pos + (int)n > 40000) abort(); memcpy(fs[i].data + fds[fd].pos, b, n); fds[fd].pos += n; if (fds[fd].pos > fs[i].len) fs[i].len = fds[fd].pos; return n; }
int sceIoClose(SceUID fd) { fds[fd - 3].open = 0; return 0; }

/* ---- the rest ---- */
SceUID sceKernelCreateThread(const char *n, SceKernelThreadEntry e, int p, int s, unsigned a, void *o) { (void)n; (void)p; (void)s; (void)a; (void)o; entry = e; return 1; }
int sceKernelStartThread(SceUID t, SceSize a, void *p) { (void)t; (void)a; (void)p; return 0; }
int sceKernelDeleteThread(SceUID t) { (void)t; return 0; }
int sceKernelTerminateDeleteThread(SceUID t) { (void)t; return 0; }
int sceKernelExitDeleteThread(int s) { (void)s; longjmp(done, 1); }
int sceKernelDelayThread(unsigned d) { us += d; return 0; }
int sceKernelLibcTime(int *t) { (void)t; return clock_dead ? -1 : BASE + (int)(us / 1000000); }
int sceCtrlPeekBufferPositive(SceCtrlData *d, int n) { (void)n; d->Buttons = lr_held ? PSP_CTRL_RTRIGGER : 0; return 1; }
int sceUsbGetState(void) { return usb_now() ? 0x220 : 0x10; }
int scePowerIsBatteryExist(void) { return 1; }
int scePowerGetBatteryLifePercent(void) { return us / 1000000 < low_until_s ? 3 : battery; }
void sctrlSESetBootConfFileIndex(int i) { (void)i; }
void sctrlSESetUmdFile(const char *f) { (void)f; }
int sctrlKernelLoadExecVSHMs2(const char *f, struct SceKernelLoadExecVSHParam *p) {
    if (strcmp(p->argp, f) || p->args != strlen(f) + 1 || strcmp(p->key, "game")) { printf("BAD PARAM\n"); exit(1); }
    strcpy(launched, f); longjmp(done, 2);
}

/* One XMB visit; returns seconds-since-base of the launch, or -1. */
static long long xmb_visit(void)
{
    launched[0] = 0;
    module_start(0, NULL);
    long long start = us;
    if (!setjmp(done))
        entry(0, NULL);
    (void)start;
    return launched[0] ? us / 1000000 : -1;
}

static void reset_world(void)
{
    memset(fs, 0, sizeof(fs)); memset(fds, 0, sizeof(fds));
    us = 0; fs_fail_write = 0; usb_until_s = -1; battery = 80; low_until_s = -1; lr_held = 0; clock_dead = 0;
    fput("ms0:/PSP/GAME/PSPLINK2/EBOOT.PBP", "\0PBPxyz", 7);
    fput("ms0:/PSP/GAME/PSPLINK2/psplink.prx", "x", 1);
    fput("ms0:/PSP/GAME/PSPLINK/EBOOT.PBP", "\0PBPxyz", 7);
    fput("ms0:/PSP/GAME/PSPLINK/psplink.prx", "x", 1);
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* A: PSPLink dies 5 s after every launch, for 6 hours. */
    reset_world();
    long long last = -1, gaps[40]; int n = 0;
    while (us < 6LL * 3600 * 1000000 && n < 40) {
        long long t = xmb_visit();
        if (t < 0) { printf("A: no launch?\n"); fails++; break; }
        if (last >= 0) gaps[n++] = t - last;
        last = t;
        us += 5 * 1000000;  /* PSPLink runs 5 s then back to XMB */
    }
    printf("A: launch gaps (s):");
    for (int i = 0; i < n; i++) printf(" %lld", gaps[i]);
    printf("\n");
    CHECK(n >= 4 && gaps[0] < 30 && gaps[1] < 30, "first two quick");
    CHECK(gaps[2] >= 600 && gaps[2] < 630, "10 min back-off, got %lld", gaps[2]);
    CHECK(gaps[3] >= 1800 && gaps[3] < 1830, "30 min, got %lld", gaps[3]);
    CHECK(gaps[4] >= 3600 && gaps[4] < 3630, "60 min, got %lld", gaps[4]);
    CHECK(gaps[5] >= 3600 && gaps[5] < 3630, "60 min cap, got %lld", gaps[5]);
    printf("A log tail:\n%s", fdata("ms0:/SEPLUGINS/sa1autolink.log") + (flen("ms0:/SEPLUGINS/sa1autolink.log") > 700 ? flen("ms0:/SEPLUGINS/sa1autolink.log") - 700 : 0));
    /* then PSPLink is healthy for 15 min: back to quick launches */
    us += 15 * 60 * 1000000LL;
    long long t1 = us / 1000000, t = xmb_visit();
    CHECK(t >= 0 && t - t1 < 30, "healthy gap resets, got %lld", t - t1);

    /* B: healthy PSPLink (exits after 11 min each time): never backs off. */
    reset_world();
    for (int i = 0; i < 6; i++) {
        long long a = us / 1000000, b = xmb_visit();
        CHECK(b >= 0 && b - a < 30, "B visit %d delay %lld", i, b - a);
        us += 11 * 60 * 1000000LL;
    }

    /* C: L/R held. D: off file. */
    reset_world(); lr_held = 1; CHECK(xmb_visit() < 0, "C L/R");
    reset_world(); fput("ms0:/SEPLUGINS/sa1autolink.off", "", 0); CHECK(xmb_visit() < 0, "D off");

    /* E: PSPLINK2 bad (small, wrong magic, missing prx) -> PSPLINK. */
    reset_world(); fput("ms0:/PSP/GAME/PSPLINK2/EBOOT.PBP", "\0PBP", 4);
    xmb_visit(); CHECK(!strcmp(launched, "ms0:/PSP/GAME/PSPLINK/EBOOT.PBP"), "E small: %s", launched);
    reset_world(); fput("ms0:/PSP/GAME/PSPLINK2/EBOOT.PBP", "XPBPxyz", 7);
    xmb_visit(); CHECK(!strcmp(launched, "ms0:/PSP/GAME/PSPLINK/EBOOT.PBP"), "E magic: %s", launched);
    reset_world(); fdel("ms0:/PSP/GAME/PSPLINK2/psplink.prx");
    xmb_visit(); CHECK(!strcmp(launched, "ms0:/PSP/GAME/PSPLINK/EBOOT.PBP"), "E prx: %s", launched);
    /* F: both bad */
    reset_world(); fdel("ms0:/PSP/GAME/PSPLINK2/EBOOT.PBP"); fdel("ms0:/PSP/GAME/PSPLINK/psplink.prx");
    CHECK(xmb_visit() < 0, "F both bad");

    /* G: USB mass storage for the first 60 s: no file access, launch after. */
    reset_world(); usb_until_s = 60;
    { long long t = xmb_visit(); CHECK(t >= 67 && t < 80, "G usb hold, launched at %lld", t); }
    /* H: card not writable: no launch. */
    reset_world(); fs_fail_write = 1; CHECK(xmb_visit() < 0, "H unwritable");
    /* I: battery 3% for 2 min then 6%. */
    reset_world(); low_until_s = 120;
    { long long t = xmb_visit(); CHECK(t >= 127 && t < 140, "I battery hold, launched at %lld", t); }
    /* J: clock dead: no launch. */
    reset_world(); clock_dead = 1; CHECK(xmb_visit() < 0, "J clock");
    /* K: log cap. */
    reset_world(); lr_held = 1;
    for (int i = 0; i < 2000; i++) xmb_visit();
    CHECK(flen("ms0:/SEPLUGINS/sa1autolink.log") <= 16384 + 64, "K log size %d", flen("ms0:/SEPLUGINS/sa1autolink.log"));
    /* L: corrupt state file -> fresh. */
    reset_world(); fput("ms0:/SEPLUGINS/sa1autolink.st", "garbage-garbage-garbage", 23);
    { long long t = xmb_visit(); CHECK(t >= 0 && t < 30, "L corrupt state, %lld", t); }

    printf("%s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
