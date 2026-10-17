/* M-1 exit test, PSP half: an EBOOT that boots and prints on screen.
 * It also allocates the 16 MB the guest RAM array will need from M4 on,
 * so a too-small heap shows up now rather than later. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspsysmem.h>
#include <stdlib.h>

PSP_MODULE_INFO("SA1PSP", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
/* Heap = all free user memory. Current pspsdk dropped PSP_HEAP_SIZE_MAX();
 * a negative size means the same thing to libcglue's sbrk. */
#ifdef PSP_HEAP_SIZE_MAX
PSP_HEAP_SIZE_MAX();
#else
PSP_HEAP_SIZE_KB(-1);
#endif

#define GUEST_RAM_BYTES (16u * 1024u * 1024u)

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1; (void)arg2; (void)common;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int thid = sceKernelCreateThread("callbacks", callback_thread, 0x11, 0xFA0, 0, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);
}

int main(void)
{
    setup_callbacks();
    pspDebugScreenInit();
    pspDebugScreenPrintf("sa1-psp: hello from the PSP backend\n\n");

    /* The numbers README section 7 budgets against: what this firmware
     * actually leaves a user-mode EBOOT with MEMSIZE=1 in PARAM.SFO. */
    pspDebugScreenPrintf("kernel: total free %u KB, largest block %u KB\n\n",
                         (unsigned)sceKernelTotalFreeMemSize() / 1024u,
                         (unsigned)sceKernelMaxFreeMemSize() / 1024u);

    void *guest_ram = malloc(GUEST_RAM_BYTES);
    pspDebugScreenPrintf("16 MB guest RAM allocation: %s\n", guest_ram ? "ok" : "FAILED");
    free(guest_ram);

    /* Largest single malloc, by halving search, in 256 KB steps. */
    unsigned lo = 0, hi = 64u * 1024u * 1024u;
    while (hi - lo > 256u * 1024u) {
        unsigned mid = lo + (hi - lo) / 2u;
        void *p = malloc(mid);
        if (p) { free(p); lo = mid; } else hi = mid;
    }
    pspDebugScreenPrintf("largest single malloc: %u KB\n", lo / 1024u);
    pspDebugScreenPrintf("\nPress HOME to exit.\n");

    sceKernelSleepThread();
    return 0;
}
