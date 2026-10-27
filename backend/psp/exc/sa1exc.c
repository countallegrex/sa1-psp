/* sa1exc.c - a crash screen for sa1-psp on hardware: a kernel-mode PRX the
 * EBOOT loads through kubridge (ARK and PRO CFW provide it) at start-up.
 *
 * A user-mode EBOOT cannot install an exception handler, and an unhandled
 * exception on the console powers it off with nothing on screen (the first
 * hardware boots, docs/m4-psp-report.md).  This module registers the
 * default exception handler (pspDebugInstallErrorHandler) and, on any
 * exception, paints the registers on the debug screen and stops there, so
 * the crash can be read off the screen and named with
 * tools/psp_addr2line.sh (EPC and RA, and the return addresses found on
 * the stack).
 *
 * Kernel modules link without libc (pspsdk build.mak, USE_KERNEL_LIBS), so
 * the formatting is done here by hand.
 *
 * Part of sa1-psp (MIT). */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>

PSP_MODULE_INFO("sa1exc", PSP_MODULE_KERNEL, 1, 0);

static const char *const causes[32] = {
    "interrupt", "TLB modification", "TLB load/fetch", "TLB store", "address error (load/fetch)",
    "address error (store)", "bus error (fetch)", "bus error (data)", "syscall", "breakpoint",
    "reserved instruction", "coprocessor unusable", "arithmetic overflow", "trap", "?14", "FPU",
    "?16", "?17", "?18", "?19", "?20", "?21", "?22", "watch", "?24", "?25", "?26", "?27", "?28",
    "?29", "?30", "?31" };
static const char *const regnames[32] = {
    "zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra" };

/* libpspdebug's exception code links pspDebugScreenPrintf, which wants
 * vsnprintf; nothing here calls it, so it copies the format and stops. */
#include <stdarg.h>
#include <stddef.h>
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    (void)ap;
    size_t i = 0;
    for (; n && i + 1 < n && fmt[i]; i++) buf[i] = fmt[i];
    if (n) buf[i] = 0;
    return (int)i;
}

static void puts_(const char *s) { int n = 0; while (s[n]) n++; pspDebugScreenPrintData(s, n); }
static void hex(u32 v) {
    char b[9];
    for (int i = 7; i >= 0; i--) { b[i] = "0123456789ABCDEF"[v & 15]; v >>= 4; }
    b[8] = 0;
    puts_(b);
}

static void on_exception(PspDebugRegBlock *r) {
    /* Paint into the frame buffer on screen now.  pspDebugScreenInit
     * points the display at VRAM 0 from the next vblank, which needs the
     * display interrupt, and this handler runs with interrupts off: on the
     * card the screen froze on the game's last picture and the text went
     * to a buffer nobody showed (2026-10-27, the save-select crash). */
    void *top = 0;
    int width = 512, fmt = PSP_DISPLAY_PIXEL_FORMAT_565;
    if (sceDisplayGetFrameBuf(&top, &width, &fmt, PSP_DISPLAY_SETBUF_IMMEDIATE) >= 0 && top)
        pspDebugScreenInitEx((void *)(((u32)top & 0x0FFFFFFFu) | 0x40000000u), fmt, 0);
    else
        pspDebugScreenInit();
    pspDebugScreenSetBackColor(0x00400000);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenClear();
    puts_("sa1-psp crashed: ");
    puts_(causes[(r->cause >> 2) & 31]);
    puts_("\nEPC "); hex(r->epc); puts_("  BadVAddr "); hex(r->badvaddr);
    puts_("  Cause "); hex(r->cause); puts_("  Status "); hex(r->status);
    puts_("\nFSR "); hex(r->fsr); puts_("\n\n");
    for (int i = 0; i < 32; i++) {
        puts_(regnames[i]); puts_(" "); hex(r->r[i]);
        puts_((i & 3) == 3 ? "\n" : "   ");
    }
    /* Return addresses on the stack: words that point into the EBOOT's code
     * (0x08804000 up), the first 24 within 4 KB above sp. */
    puts_("\nstack (code addresses above sp):\n");
    u32 sp = r->r[29], shown = 0;
    if (sp >= 0x08800000u && sp < 0x0C000000u - 4096u) {
        for (u32 a = sp; a < sp + 4096u && shown < 24; a += 4) {
            u32 w = *(volatile u32 *)a;
            if (w >= 0x08804000u && w < 0x09800000u && !(w & 3)) {
                hex(w); puts_((shown % 6) == 5 ? "\n" : " ");
                shown++;
            }
        }
    }
    puts_("\n\nPhotograph this screen; hold POWER to turn the PSP off.");
    for (;;) { }        /* no waits: they need the interrupts this context has off */
}

int module_start(SceSize args, void *argp) {
    (void)args; (void)argp;
    pspDebugInstallErrorHandler(on_exception);
    return 0;
}

int module_stop(SceSize args, void *argp) {
    (void)args; (void)argp;
    return 0;
}
