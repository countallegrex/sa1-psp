/* sa1autolink.prx: an ARK-4 XMB plugin that relaunches PSPLink.
 *
 * Every time the XMB comes up, wait about 8 s and then launch
 * ms0:/PSP/GAME/PSPLINK2/EBOOT.PBP (the patched PSPLink), or the stock
 * ms0:/PSP/GAME/PSPLINK/EBOOT.PBP if PSPLINK2 is missing, so the PC side
 * (usbhostfs_pc) never needs someone to start PSPLink by hand.
 *
 * It does nothing when, at any point during the wait:
 *   - L or R is held,
 *   - ms0:/SEPLUGINS/sa1autolink.off exists,
 *   - USB mass storage mode is active (launching would yank the card
 *     from under the PC).
 * It launches at most once per XMB session: the thread exits after one
 * attempt and the XMB keeps running untouched if the launch fails.
 *
 * The launch goes through ARK-4's SystemControl, the same call ARK's own
 * VshCtrl makes when the XMB starts a ms0 homebrew (core/vshctrl/xmbiso.c,
 * homebrewloadexec): reset the boot config to plain UMD mode with no ISO,
 * then sctrlKernelLoadExecVSHMs2 (apitype 0x141) with key "game" and argp
 * the EBOOT path.  The imports are declared in stubs.S; see README.md. */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <pspusb.h>
#include <psploadexec_kernel.h>

PSP_MODULE_INFO("sa1autolink", 0, 1, 0);

/* ARK-4 SystemCtrlForUser, NIDs in stubs.S. */
int sctrlKernelLoadExecVSHMs2(const char *file, struct SceKernelLoadExecVSHParam *param);
int sctrlSESetBootConfFileIndex(int index);
void sctrlSESetUmdFile(const char *file);

#define MODE_UMD 0             /* ARK's boot config index for "no ISO driver" */
#define WAIT_US (8 * 1000 * 1000)
#define POLL_US (100 * 1000)

static const char *const targets[] = {
    "ms0:/PSP/GAME/PSPLINK2/EBOOT.PBP",
    "ms0:/PSP/GAME/PSPLINK/EBOOT.PBP",
};
static const char off_file[] = "ms0:/SEPLUGINS/sa1autolink.off";

static SceUID thid = -1;

static int exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

/* Nonzero if the user (or the setup) asked us to stay put right now. */
static int vetoed(void)
{
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0 &&
        (pad.Buttons & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)))
        return 1;
    int usb = sceUsbGetState();
    if (usb >= 0 && (usb & PSP_USB_ACTIVATED))
        return 1;
    return 0;
}

static int autolink_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    if (exists(off_file))
        goto out;

    /* Poll through the wait so L/R held at any moment cancels. */
    for (int t = 0; t < WAIT_US; t += POLL_US) {
        if (vetoed())
            goto out;
        sceKernelDelayThread(POLL_US);
    }
    if (vetoed() || exists(off_file))
        goto out;

    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        const char *path = targets[i];
        if (!exists(path))
            continue;

        struct SceKernelLoadExecVSHParam param;
        char *p = (char *)&param;
        for (unsigned k = 0; k < sizeof(param); k++)
            p[k] = 0;
        param.size = sizeof(param);
        param.args = slen(path) + 1;
        param.argp = (void *)path;
        param.key = "game";

        sctrlSESetBootConfFileIndex(MODE_UMD);
        sctrlSESetUmdFile("");
        /* Does not return on success.  On failure, try the next one. */
        sctrlKernelLoadExecVSHMs2(path, &param);
    }

out:
    thid = -1;
    sceKernelExitDeleteThread(0);
    return 0;
}

int module_start(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    /* Low priority (0x30) and a small stack: the XMB comes first. */
    thid = sceKernelCreateThread("sa1autolink", autolink_thread, 0x30, 0x1000, 0, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);
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
