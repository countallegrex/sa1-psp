/* sa1prof.c - a PC-sampling profiler for sa1-psp on hardware: a kernel-mode
 * PRX the game loads through kubridge with --prof (main_psp.c).
 *
 * PSPLink's thctx cannot sample the game on the 3001's firmware 6.61: the
 * thread context it prints is not the thread's (strings and 0xDEADBEEF
 * fill; the kernel's thread structure moved since PSPLink was written), so
 * tools/psp_run.py --sample-after gave no usable PCs (2026-10-19).  This
 * module sets a kernel alarm instead: its handler runs in interrupt
 * context, where cop0 EPC still holds the PC the interrupt stopped, and
 * stores it in a buffer the game owns.  Every ~1 ms (jittered so periodic
 * work does not alias), whichever thread was running: PCs outside the
 * game's PRX are the kernel, other modules or the idle thread (the game
 * waiting).  tools/psp_prof.py --pcs names them.
 *
 * The game passes the buffer's address as the start argument; the handler
 * records while run == 1, stops rescheduling when run == 2.
 *
 * Part of sa1-psp (MIT). */
#include <pspkernel.h>

PSP_MODULE_INFO("sa1prof", PSP_MODULE_KERNEL, 1, 0);

struct prof_buf {               /* the game's (main_psp.c) */
    volatile u32 magic, run, count, cap, period_us, missed, alarm, reserved;
    u32 pc[];
};

static struct prof_buf *buf;
static u32 lfsr = 0xACE1u;

static SceUInt on_alarm(void *common) {
    (void)common;
    struct prof_buf *b = buf;
    u32 epc;
    __asm__ volatile("mfc0 %0, $14" : "=r"(epc));
    if (b->run == 2) return 0;
    if (b->run == 1) {
        if (b->count < b->cap) b->pc[b->count++] = epc;
        else b->missed++;
    }
    lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
    return b->period_us - b->period_us / 8 + (lfsr % (b->period_us / 4 + 1));
}

int module_start(SceSize args, void *argp) {
    if (args < sizeof(u32) || !argp) return 0;
    buf = *(struct prof_buf **)argp;
    if (buf->magic != 0x464F5250u || buf->period_us < 50) { buf->alarm = 0x80000000u; return 0; }
    buf->alarm = (u32)sceKernelSetAlarm(buf->period_us, on_alarm, 0);
    return 0;
}

int module_stop(SceSize args, void *argp) {
    (void)args; (void)argp;
    if (buf && (int)buf->alarm > 0) sceKernelCancelAlarm((SceUID)buf->alarm);
    return 0;
}
