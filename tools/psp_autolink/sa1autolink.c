/* sa1autolink.prx: an ARK-4 XMB plugin that relaunches PSPLink.
 *
 * Every time the XMB comes up, wait about 8 s and then launch
 * ms0:/PSP/GAME/PSPLINK2/EBOOT.PBP (the patched PSPLink), or the stock
 * ms0:/PSP/GAME/PSPLINK/EBOOT.PBP if PSPLINK2 looks broken, so the PC side
 * never needs someone to start PSPLink by hand.  It has to keep working with
 * nobody there, so it is careful about everything that can go wrong:
 *
 * Cancel for this XMB visit:  L or R held, ms0:/SEPLUGINS/sa1autolink.off,
 *                             no EBOOT that passes validation.
 * Hold (wait, then settle 8 s again):  USB mass storage active, battery < 5%.
 * Crash-loop guard:  ms0:/SEPLUGINS/sa1autolink.st keeps the last three
 *   launch times and a back-off level.  Three launches within 3 minutes
 *   (with no healthy 10-minute gap) start a back-off of 10, then 30, then
 *   60 minutes (capped) before each further launch.  An XMB return 10 min
 *   or more after the last launch clears it.  Written once per launch.
 * Log:  ms0:/SEPLUGINS/sa1autolink.log, one buffered write per XMB visit,
 *   truncated when it would pass 16 KB.  Nothing is written while USB mass
 *   storage is active.
 *
 * module_start only creates one low-priority thread; any failed call means
 * giving up quietly.  Every import is a user-mode library that the VSH
 * itself uses (ThreadMan, IoFileMgr, sceCtrl, sceUsb, scePower, UtilsForUser)
 * or ARK's SystemCtrlForUser (stubs.S).  An unresolved import returns an
 * error code, which is treated as "unknown" and never as permission.
 *
 * The launch makes the same calls ARK-4's own VshCtrl makes when the XMB
 * starts a ms0 homebrew (core/vshctrl/xmbiso.c, homebrewloadexec). */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <pspusb.h>
#include <psppower.h>
#include <psploadexec_kernel.h>

PSP_MODULE_INFO("sa1autolink", 0, 1, 1);

/* ARK-4 SystemCtrlForUser, NIDs in stubs.S. */
int sctrlKernelLoadExecVSHMs2(const char *file, struct SceKernelLoadExecVSHParam *param);
void sctrlSESetBootConfFileIndex(int index);
void sctrlSESetUmdFile(const char *file);

/* UtilsForUser's sceKernelLibcTime returns a 32-bit time in v0, but
 * psputils.h declares it with newlib's 64-bit time_t; bind our own 32-bit
 * prototype to the same stub instead. */
extern int libc_time32(int *t) __asm__("sceKernelLibcTime");

#define MODE_UMD 0                 /* ARK boot config: no ISO driver */
#define SETTLE_S 8                 /* quiet seconds before a launch */
#define POLL_US (100 * 1000)
#define LOOP_S 180                 /* three launches within this = a loop */
#define HEALTHY_S 600              /* XMB away this long = PSPLink was fine */
#define MIN_EBOOT 100000           /* PSPLink's EBOOT is ~131 KB */
#define LOW_BATTERY 5
#define LOG_CAP 16384
#define MAX_LEVEL 3

static const unsigned backoff_s[MAX_LEVEL] = {600, 1800, 3600};

static const char *const targets[] = {
    "ms0:/PSP/GAME/PSPLINK2/EBOOT.PBP",
    "ms0:/PSP/GAME/PSPLINK/EBOOT.PBP",
};
static const char off_file[] = "ms0:/SEPLUGINS/sa1autolink.off";
static const char st_file[] = "ms0:/SEPLUGINS/sa1autolink.st";
static const char log_file[] = "ms0:/SEPLUGINS/sa1autolink.log";

#define ST_MAGIC 0x4c413153u       /* "S1AL" */
struct state {
    unsigned magic;
    unsigned t[3];                 /* last three launch times, oldest first */
    unsigned level;                /* back-off level, 0 = none */
    unsigned check;
};

static SceUID thid = -1;
static struct state st;
static char logbuf[1536];
static unsigned loglen;
static char pathbuf[64];

/* ---- tiny libc replacements (the PRX links no libc) ---- */

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static void zero(void *p, unsigned n)
{
    volatile char *c = p;
    while (n--)
        *c++ = 0;
}

static void put(const char *s)
{
    while (*s && loglen < sizeof(logbuf) - 1)
        logbuf[loglen++] = *s++;
}

static void putu(unsigned v, int width)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = '0' + v % 10;
        v /= 10;
    } while (v && n < 10);
    while (n < width && n < 10)
        tmp[n++] = '0';
    char out[12];
    int k = 0;
    while (n)
        out[k++] = tmp[--n];
    out[k] = 0;
    put(out);
}

static void puthex(unsigned v)
{
    char out[9];
    for (int k = 0; k < 8; k++)
        out[k] = "0123456789abcdef"[(v >> (28 - 4 * k)) & 15];
    out[8] = 0;
    put(out);
}

/* "YYYY-MM-DD hh:mm:ss " from Unix seconds (civil-from-days, H. Hinnant). */
static void putstamp(unsigned t)
{
    unsigned s = t % 86400, z = t / 86400 + 719468;
    unsigned era = z / 146097, doe = z - era * 146097;
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    unsigned y = yoe + era * 400 + (m <= 2);
    putu(y, 4); put("-"); putu(m, 2); put("-"); putu(d, 2); put(" ");
    putu(s / 3600, 2); put(":"); putu(s / 60 % 60, 2); put(":"); putu(s % 60, 2);
    put(" ");
}

static unsigned now(void)
{
    int t = libc_time32(NULL);
    return t > 0 ? (unsigned)t : 0;
}

static void logline(const char *msg)
{
    putstamp(now());
    put(msg);
    put("\n");
}

static void logline_u(const char *msg, unsigned v, const char *tail)
{
    putstamp(now());
    put(msg);
    putu(v, 1);
    put(tail);
    put("\n");
}

/* ---- checks ---- */

static int usb_active(void)
{
    int s = sceUsbGetState();
    return s >= 0 && (s & PSP_USB_ACTIVATED);
}

static int battery_low(void)
{
    if (scePowerIsBatteryExist() <= 0)
        return 0;                  /* no battery: running on AC */
    int p = scePowerGetBatteryLifePercent();
    return p >= 0 && p < LOW_BATTERY;
}

static int triggers_held(void)
{
    SceCtrlData pad;
    zero(&pad, sizeof(pad));
    return sceCtrlPeekBufferPositive(&pad, 1) > 0 &&
           (pad.Buttons & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER));
}

static int exists(const char *path)
{
    SceIoStat s;
    zero(&s, sizeof(s));
    return sceIoGetstat(path, &s) >= 0;
}

/* The EBOOT must be a PBP of plausible size, and the psplink.prx that its
 * bootstrap loads from the same folder must be there too. */
static int eboot_ok(const char *path)
{
    SceIoStat s;
    zero(&s, sizeof(s));
    if (sceIoGetstat(path, &s) < 0 || s.st_size < MIN_EBOOT)
        return 0;

    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    unsigned char magic[4] = {0, 0, 0, 0};
    int n = sceIoRead(fd, magic, 4);
    sceIoClose(fd);
    if (n != 4 || magic[0] != 0 || magic[1] != 'P' || magic[2] != 'B' || magic[3] != 'P')
        return 0;

    unsigned len = slen(path);
    if (len + 12 > sizeof(pathbuf))
        return 0;
    unsigned cut = len;
    while (cut && path[cut - 1] != '/')
        cut--;
    for (unsigned i = 0; i < cut; i++)
        pathbuf[i] = path[i];
    const char *prx = "psplink.prx";
    unsigned j = 0;
    while (prx[j]) {
        pathbuf[cut + j] = prx[j];
        j++;
    }
    pathbuf[cut + j] = 0;
    zero(&s, sizeof(s));
    return sceIoGetstat(pathbuf, &s) >= 0 && s.st_size > 0;
}

/* ---- files ---- */

static unsigned st_sum(const struct state *s)
{
    return s->magic ^ s->t[0] ^ s->t[1] ^ s->t[2] ^ s->level ^ 0x5a5a5a5au;
}

static void st_load(void)
{
    zero(&st, sizeof(st));
    SceUID fd = sceIoOpen(st_file, PSP_O_RDONLY, 0);
    if (fd < 0)
        return;
    struct state s;
    int n = sceIoRead(fd, &s, sizeof(s));
    sceIoClose(fd);
    if (n == (int)sizeof(s) && s.magic == ST_MAGIC && s.check == st_sum(&s) &&
        s.level <= MAX_LEVEL)
        for (unsigned k = 0; k < sizeof(st); k++)
            ((char *)&st)[k] = ((const char *)&s)[k];
    else
        logline("state file unreadable, starting fresh");
}

/* Nonzero only if the state really reached the card. */
static int st_save(void)
{
    st.magic = ST_MAGIC;
    st.check = st_sum(&st);
    SceUID fd = sceIoOpen(st_file, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        return 0;
    int n = sceIoWrite(fd, &st, sizeof(st));
    int c = sceIoClose(fd);
    return n == (int)sizeof(st) && c >= 0;
}

static void log_flush(void)
{
    if (!loglen || usb_active())
        return;
    SceIoStat s;
    zero(&s, sizeof(s));
    int flags = PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND;
    if (sceIoGetstat(log_file, &s) >= 0 && s.st_size + loglen > LOG_CAP)
        flags = PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC;
    SceUID fd = sceIoOpen(log_file, flags, 0777);
    if (fd >= 0) {
        if (flags & PSP_O_TRUNC)
            sceIoWrite(fd, "--- log truncated at 16 KB ---\n", 31);
        sceIoWrite(fd, logbuf, loglen);
        sceIoClose(fd);
    }
    loglen = 0;
}

/* ---- the thread ---- */

enum { GO, CANCEL };

/* Wait until `target` (Unix seconds, 0 = none) has passed and the last
 * SETTLE_S seconds were quiet: no USB mass storage, battery not critical.
 * L/R or the off file cancel.  Returns GO or CANCEL. */
static int wait_until(unsigned target)
{
    unsigned quiet = 0, ticks = 0;
    int held = 0, low = 0;
    for (;;) {
        if (triggers_held()) {
            logline("L/R held: not launching this time");
            return CANCEL;
        }
        if (++ticks % 50 == 0 && !usb_active() && exists(off_file)) {
            logline("sa1autolink.off appeared: not launching");
            return CANCEL;
        }
        int u = usb_active();
        if (ticks % 10 == 1)
            low = battery_low();
        int b = !u && low;
        if (u || b) {
            if (!held)
                logline(u ? "holding: USB mass storage active"
                          : "holding: battery below 5%");
            held = 1;
            quiet = 0;
        } else {
            if (held)
                logline("hold cleared, settling");
            held = 0;
            quiet++;
        }
        unsigned t = now();
        if (t == 0) {
            logline("clock unreadable: giving up");
            return CANCEL;
        }
        if (quiet >= SETTLE_S * (1000000 / POLL_US) && (target == 0 || t >= target))
            return GO;
        if (target && t + 4000 < target) {
            logline("back-off target out of range: giving up");
            return CANCEL;
        }
        sceKernelDelayThread(POLL_US);
    }
}

static int autolink_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    /* First the plain settle: about 8 s of XMB with no USB mass storage and
     * a usable battery.  No file access happens before it. */
    if (wait_until(0) != GO)
        goto out;
    if (exists(off_file)) {
        logline("sa1autolink.off present: off");
        goto out;
    }

    unsigned t = now();
    if (t == 0) {
        logline("clock unreadable: off this time");
        goto out;
    }
    st_load();

    /* XMB came back long after the last launch, or the clock moved back:
     * PSPLink was fine (or we can't tell), so forget the history. */
    if (st.t[2] && (t < st.t[2] || t - st.t[2] >= HEALTHY_S)) {
        if (st.level)
            logline("healthy gap since last launch: back-off cleared");
        zero(&st, sizeof(st));
    }
    if (st.level == 0 && st.t[0] && st.t[2] - st.t[0] <= LOOP_S) {
        st.level = 1;
        logline("3 launches within 3 min: crash loop, backing off");
    }
    unsigned target = 0;
    if (st.level) {
        unsigned d = backoff_s[st.level - 1];
        target = st.t[2] + d;
        logline_u("back-off level ", st.level, "");
        logline_u("waiting until ", target > t ? (target - t) / 60 : 0,
                  " min from now before launching");
    }
    log_flush();

    if (target > now()) {
        if (wait_until(target) != GO)
            goto out;
        if (exists(off_file)) {
            logline("sa1autolink.off present: off");
            goto out;
        }
    }

    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        const char *path = targets[i];
        if (!eboot_ok(path)) {
            putstamp(now()); put("skipping "); put(path); put(": missing or invalid\n");
            continue;
        }

        st.t[0] = st.t[1];
        st.t[1] = st.t[2];
        st.t[2] = now();
        if (st.level && st.level < MAX_LEVEL)
            st.level++;
        /* No record of this launch means no crash-loop guard: don't. */
        if (!st_save()) {
            logline("cannot write sa1autolink.st: not launching");
            goto out;
        }
        putstamp(st.t[2]); put("launching "); put(path); put("\n");
        log_flush();

        struct SceKernelLoadExecVSHParam param;
        zero(&param, sizeof(param));
        param.size = sizeof(param);
        param.args = slen(path) + 1;
        param.argp = (void *)path;
        param.key = "game";

        sctrlSESetBootConfFileIndex(MODE_UMD);
        sctrlSESetUmdFile("");
        int r = sctrlKernelLoadExecVSHMs2(path, &param);
        /* Only reached on failure.  Don't try the next EBOOT: a failed
         * loadexec says nothing about the file, and once is the rule. */
        putstamp(now()); put("launch failed, error 0x"); puthex((unsigned)r); put("\n");
        goto out;
    }
    logline("no valid PSPLink EBOOT: not launching");

out:
    log_flush();
    thid = -1;
    sceKernelExitDeleteThread(0);
    return 0;
}

int module_start(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    /* Priority 0x30 (below the XMB's threads) and an 8 KB stack; the large
     * buffers are static.  If the thread can't be made, do nothing. */
    thid = sceKernelCreateThread("sa1autolink", autolink_thread, 0x30, 0x2000, 0, NULL);
    if (thid >= 0 && sceKernelStartThread(thid, 0, NULL) < 0) {
        sceKernelDeleteThread(thid);
        thid = -1;
    }
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    if (thid >= 0) {
        sceKernelTerminateDeleteThread(thid);
        thid = -1;
    }
    return 0;
}
