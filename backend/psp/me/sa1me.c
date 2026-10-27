/* sa1me.c - Media Engine loader, step 1 (qd): a kernel-mode PRX the game
 * loads through kubridge with --me-test MODE (main_psp.c).  It proves the
 * two things the ME mixer needs before any of it is written: that the
 * firmware's own sceSysreg ME calls (pspsysreg.h, the SDK's stubs) resolve
 * on the console's firmware, and that the ME runs code we place at its
 * reset vector.
 *
 * The game passes a struct me_test (below) as the start argument; the whole
 * test runs in module_start and leaves its results there.
 *   MODE 0: report the sysreg import stubs only (are they linked?).
 *   MODE 1: also write the ME program at the reset vector and read it back.
 *   MODE 2: also start the ME: reset held, bus clock on, reset released; the
 *           program counts in a mailbox; after ~1 s the stop flag is set,
 *           the ME acknowledges and parks, and the reset is held again.
 * The ME program and its mailbox live only in the reset vector area
 * (0xBFC00000, uncached), never in this module's memory, so nothing the ME
 * touches goes away when the module is unloaded; and the ME is left held
 * in reset.  Taking the ME stops the firmware's own codecs (Atrac, MP3,
 * video) until the next boot; the game uses none of them.
 *
 * Part of sa1-psp (MIT). */
#include <pspkernel.h>
#include <pspsysreg.h>

PSP_MODULE_INFO("sa1me", PSP_MODULE_KERNEL, 1, 0);

struct me_test {                /* the game's (main_psp.c) */
    volatile u32 magic, mode, done, step;
    volatile u32 stub[3][2];    /* first two words of the ResetEnable / BusClockEnable / ResetDisable imports */
    volatile s32 r_reset, r_clock, r_release, r_hold;
    volatile u32 readback_ok, count1, count2, ack, us1, us2;
};

#define ME_VEC  0xBFC00000u     /* the ME's reset vector (uncached) */
#define ME_MBOX 0xBFC00600u     /* +0 counter, +4 stop, +8 ack */

/* The ME program: count in the mailbox until stop is set, acknowledge, park.
 * Uncached loads and stores only: no cache, no stack, no setup needed. */
extern const u32 me_prog[], me_prog_end[];
__asm__(
    ".section .rodata\n .align 4\n .globl me_prog\n me_prog:\n"
    ".set push\n .set noreorder\n .set noat\n"
    " lui $8, 0xbfc0\n"             /* t0 = mailbox */
    " ori $8, $8, 0x0600\n"
    "1: lw $9, 0($8)\n"
    " addiu $9, $9, 1\n"
    " sw $9, 0($8)\n"
    " lw $10, 4($8)\n"
    " beq $10, $0, 1b\n"
    " nop\n"
    " li $9, 1\n"
    " sw $9, 8($8)\n"
    "2: b 2b\n"
    " nop\n"
    ".set pop\n"
    " .globl me_prog_end\n me_prog_end:\n .previous\n");

static void stub_words(volatile u32 *out, const void *fn) {
    const u32 *p = (const u32 *)fn;
    out[0] = p[0]; out[1] = p[1];
}

int module_start(SceSize args, void *argp) {
    if (args < sizeof(u32) || !argp) return 0;
    struct me_test *t = *(struct me_test **)argp;
    if (t->magic != 0x31454D53u) return 0;      /* "SME1" */
    stub_words(t->stub[0], (const void *)sceSysregMeResetEnable);
    stub_words(t->stub[1], (const void *)sceSysregMeBusClockEnable);
    stub_words(t->stub[2], (const void *)sceSysregMeResetDisable);
    t->step = 1;
    if (t->mode >= 1) {
        volatile u32 *vec = (volatile u32 *)ME_VEC, *mb = (volatile u32 *)ME_MBOX;
        u32 n = (u32)(me_prog_end - me_prog), ok = 1;
        if (t->mode >= 2) t->r_reset = sceSysregMeResetEnable();   /* hold the ME before its code changes */
        for (u32 i = 0; i < n; i++) vec[i] = me_prog[i];
        mb[0] = 0; mb[1] = 0; mb[2] = 0;
        for (u32 i = 0; i < n; i++) if (vec[i] != me_prog[i]) ok = 0;
        t->readback_ok = ok ? n : 0;
        t->step = 2;
        if (t->mode >= 2 && ok) {
            t->r_clock = sceSysregMeBusClockEnable();
            t->r_release = sceSysregMeResetDisable();
            t->step = 3;
            u32 a = sceKernelGetSystemTimeLow();
            sceKernelDelayThread(100000);
            t->count1 = mb[0]; t->us1 = sceKernelGetSystemTimeLow() - a;
            sceKernelDelayThread(1000000);
            t->count2 = mb[0]; t->us2 = sceKernelGetSystemTimeLow() - a;
            mb[1] = 1;
            for (int i = 0; i < 100 && !mb[2]; i++) sceKernelDelayThread(100);
            t->ack = mb[2];
            t->r_hold = sceSysregMeResetEnable();
            t->step = 4;
        }
    }
    t->done = 1;
    return 0;
}

int module_stop(SceSize args, void *argp) {
    (void)args; (void)argp;
    return 0;
}
