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
 *   MODE 3: boot the ME into a function of the game (fn, arg, sp, gp; see
 *           boot below) and stay resident; module_stop holds the ME in
 *           reset again.  What the ME then runs is the game's own code and
 *           data (backend/psp/me_psp.c), started and stopped by the game.
 * The MODE 1-2 program and its mailbox live only in the reset vector area
 * (0xBFC00000, uncached), never in this module's memory, so nothing the ME
 * touches goes away when the module is unloaded; and the ME is left held
 * in reset.  Taking the ME stops the firmware's own codecs (Atrac, MP3,
 * video) until the next boot; the game uses none of them.
 *
 * Part of sa1-psp (MIT). */
#include <pspkernel.h>
#include <pspsysreg.h>
#include <psputilsforkernel.h>

PSP_MODULE_INFO("sa1me", PSP_MODULE_KERNEL, 1, 0);

struct me_test {                /* the game's (main_psp.c) */
    volatile u32 magic, mode, done, step;
    volatile u32 stub[3][2];    /* first two words of the ResetEnable / BusClockEnable / ResetDisable imports */
    volatile s32 r_reset, r_clock, r_release, r_hold;
    volatile u32 readback_ok, count1, count2, ack, us1, us2;
    volatile u32 kc[3][16];     /* the kernel's DcacheWritebackInvalidateAll / DcacheInvalidateAll / IcacheInvalidateAll code */
    volatile u32 fn, arg, sp, gp;   /* MODE 3: the game's function the ME runs, its argument, stack top and $gp */
    volatile u32 expect;            /* MODE 3: fn's first instruction word, as the game reads it */
    volatile u32 regs[19];          /* MODE 3: 0xBC000000-0xBC000044 and 0xBC100050 before the boot */
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

/* MODE 3: the ME's boot code at its reset vector.  It clears both caches
 * the way the kernel's sceKernelDcacheInvalidateAll / IcacheInvalidateAll
 * do (TagLo/TagHi 0, index store tag over twice the size Config gives),
 * enables the FPU with the boot exception vectors, and calls the game's
 * function with the stack, $gp and argument from ME_ARGS.  If the function
 * returns, the ME parks with ME_RET set.  An exception parks it with
 * EXC_MAGIC, Cause, EPC, BadVAddr and ErrorEPC at ME_EXC (both boot
 * vectors, 0x200 and 0x380, hold the handler).  The game reads both
 * through kubridge (kuKernelPeekw). */
#define ME_ARGS 0xBFC00600u     /* fn, arg, sp, gp; +0x10 the return flag */
#define ME_EXC  0xBFC00700u
extern const u32 me_boot[], me_boot_end[], me_exc[], me_exc_end[];
__asm__(
    ".section .rodata\n .align 4\n .globl me_boot\n me_boot:\n"
    ".set push\n .set noreorder\n .set noat\n"
    " mtc0 $0, $28\n"
    " mtc0 $0, $29\n"
    " mfc0 $8, $16\n"              /* Config */
    " ext $9, $8, 9, 3\n"          /* I-cache size field */
    " li $10, 0x1000\n"
    " sllv $10, $10, $9\n"
    " move $11, $0\n"
    "1: addiu $11, $11, 64\n"
    " bne $11, $10, 1b\n"
    " cache 0x01, -64($11)\n"
    " ext $9, $8, 6, 3\n"          /* D-cache size field */
    " li $10, 0x1000\n"
    " sllv $10, $10, $9\n"
    " move $11, $0\n"
    "2: addiu $11, $11, 64\n"
    " bne $11, $10, 2b\n"
    " cache 0x11, -64($11)\n"
    " sync\n"
    " mtc0 $0, $13\n"              /* Cause */
    " mfc0 $8, $12\n"              /* Status: FPU on, boot vectors, kernel, interrupts off */
    " li $9, 0x20400000\n"
    " or $8, $8, $9\n"
    " li $9, ~0x1F\n"
    " and $8, $8, $9\n"
    " mtc0 $8, $12\n"
    " sync\n"
    /* The ME's own view of the system registers (each processor has its
     * own): bus clocks 0x0F, Tachyon config |= 2, NMI flags cleared, and
     * the memory protection 0xBC000000-0xBC000044 opened (all ones), as
     * the handler of mcidclan/psp-media-engine-custom-core (MIT) does on
     * the ME before it touches main RAM. */
    " lui $8, 0xbc10\n"
    " li $9, 0x0f\n"
    " sw $9, 0x50($8)\n"
    " lw $9, 0x40($8)\n"
    " ori $9, $9, 2\n"
    " sw $9, 0x40($8)\n"
    " li $9, -1\n"
    " sw $9, 0x04($8)\n"
    " sync\n"
    " lui $8, 0xbc00\n"
    " li $10, 0x48\n"
    " move $11, $0\n"
    "7: addu $12, $8, $11\n"
    " sw $9, 0($12)\n"
    " addiu $11, $11, 4\n"
    " bne $11, $10, 7b\n"
    " nop\n"
    " sync\n"
    " lui $26, 0xbfc0\n"
    " lw $25, 0x600($26)\n"
    " lw $4, 0x604($26)\n"
    " lw $29, 0x608($26)\n"
    " lw $28, 0x60c($26)\n"
    /* first read the function's first word three ways (as the game passed
     * it, kseg1, kseg0) into +0x20..+0x28, and call it only if the first
     * read matches the word the game read there (+0x14); else park, +0x18 = 3 */
    " li $9, 2\n"
    " sw $9, 0x618($26)\n"
    " lui $10, 0x1fff\n"
    " ori $10, $10, 0xffff\n"
    " and $11, $25, $10\n"
    " lui $12, 0xa000\n"
    " or $12, $12, $11\n"
    " lw $9, 0($12)\n"
    " sw $9, 0x624($26)\n"
    " lw $9, 0($25)\n"
    " sw $9, 0x620($26)\n"
    /* the call goes through kseg0 (cached, kernel), as the ME library of
     * mcidclan/psp-media-engine-custom-core (MIT) calls the game's code */
    " lui $12, 0x8000\n"
    " or $12, $12, $11\n"
    " lw $9, 0($12)\n"
    " sw $9, 0x628($26)\n"
    " lw $8, 0x614($26)\n"
    " bne $9, $8, 5f\n"
    " nop\n"
    " li $9, 4\n"
    " sw $9, 0x618($26)\n"
    " jalr $12\n"
    " nop\n"
    " lui $26, 0xbfc0\n"
    " li $27, 1\n"
    " sw $27, 0x610($26)\n"
    /* park with `halt` (the ME library of mcidclan/psp-media-engine-custom-core,
     * MIT, ends its ME code so), never held in reset by us: an ME we held in
     * reset hung the next PSPLink reboot (b_p3-b_p6) */
    "3: .word 0x70000000\n"
    " b 3b\n"
    " nop\n"
    "5: li $9, 3\n"
    " sw $9, 0x618($26)\n"
    "6: .word 0x70000000\n"     /* halt */
    " b 6b\n"
    " nop\n"
    " .globl me_boot_end\n me_boot_end:\n"
    " .globl me_exc\n me_exc:\n"
    " lui $26, 0xbfc0\n"
    " mfc0 $27, $13\n"
    " sw $27, 0x704($26)\n"
    " mfc0 $27, $14\n"
    " sw $27, 0x708($26)\n"
    " mfc0 $27, $8\n"
    " sw $27, 0x70c($26)\n"
    " mfc0 $27, $30\n"
    " sw $27, 0x710($26)\n"
    " sw $31, 0x714($26)\n"
    " lui $27, 0x4558\n"           /* "EXC!" */
    " ori $27, $27, 0x4321\n"
    " sw $27, 0x700($26)\n"
    "4: b 4b\n"
    " nop\n"
    " .globl me_exc_end\n me_exc_end:\n"
    ".set pop\n .previous\n");

static int booted;
/* What the vector area held before MODE 3 (the 2026-10-27 probe, which
 * zeroed it, left a PSPLink reset hanging): put back at module_stop. */
static u32 saved[0x800 / 4];
/* The memory protection registers (0xBC000000-0xBC000044) and the bus
 * clocks (0xBC100050) before the boot, put back at module_stop.  Main RAM
 * is closed to the ME until they are opened: the b_p4 probe read zeros
 * through every alias.  The values (all ones; clocks 0x0F) are those the
 * ME library of mcidclan/psp-media-engine-custom-core (MIT) writes. */
#define HW(a) (*(volatile u32 *)(a))
static u32 prot[18], busclk;

/* MODE 3: hold the ME, place the boot code and the handler, release it.
 * The module stays resident; module_stop holds the ME in reset again. */
static void boot(struct me_test *t) {
    volatile u32 *vec = (volatile u32 *)ME_VEC, *args = (volatile u32 *)ME_ARGS, *exc = (volatile u32 *)ME_EXC;
    u32 n = (u32)(me_boot_end - me_boot), ne = (u32)(me_exc_end - me_exc), ok = 1;
    t->r_reset = sceSysregMeResetEnable();
    for (u32 i = 0; i < 0x800 / 4; i++) saved[i] = vec[i];
    for (u32 i = 0; i < n; i++) vec[i] = me_boot[i];
    for (u32 i = 0; i < ne; i++) { vec[0x200 / 4 + i] = me_exc[i]; vec[0x380 / 4 + i] = me_exc[i]; }
    for (u32 i = 0; i < 16; i++) args[i] = 0;
    args[0] = t->fn; args[1] = t->arg; args[2] = t->sp; args[3] = t->gp; args[5] = t->expect;
    for (u32 i = 0; i < 8; i++) exc[i] = 0;
    for (u32 i = 0; i < n; i++) if (vec[i] != me_boot[i]) ok = 0;
    for (u32 i = 0; i < ne; i++) if (vec[0x380 / 4 + i] != me_exc[i]) ok = 0;
    t->readback_ok = ok ? n + ne : 0;
    t->step = 2;
    if (!ok) return;
    for (u32 i = 0; i < 18; i++) { prot[i] = HW(0xBC000000u + 4 * i); t->regs[i] = prot[i]; }
    busclk = HW(0xBC100050u); t->regs[18] = busclk;
    t->r_clock = sceSysregMeBusClockEnable();
    t->r_release = sceSysregMeResetDisable();
    booted = 1;
    t->step = 3;
}

static void stub_words(volatile u32 *out, const void *fn) {
    const u32 *p = (const u32 *)fn;
    out[0] = p[0]; out[1] = p[1];
}

/* The first 16 words of the kernel function an import stub jumps to (the
 * cache operations the ME's own boot code must repeat). */
static void kcode(volatile u32 *out, const void *fn) {
    const u32 *p = (const u32 *)fn;
    for (int i = 0; i < 16; i++) out[i] = 0;
    if ((p[0] >> 26) != 2) { out[0] = p[0]; out[1] = p[1]; return; }     /* not a j: the stub itself */
    const u32 *q = (const u32 *)(((u32)p & 0xF0000000u) | (p[0] & 0x03FFFFFFu) << 2);
    for (int i = 0; i < 16; i++) out[i] = q[i];
}

int module_start(SceSize args, void *argp) {
    if (args < sizeof(u32) || !argp) return 0;
    struct me_test *t = *(struct me_test **)argp;
    if (t->magic != 0x31454D53u) return 0;      /* "SME1" */
    stub_words(t->stub[0], (const void *)sceSysregMeResetEnable);
    stub_words(t->stub[1], (const void *)sceSysregMeBusClockEnable);
    stub_words(t->stub[2], (const void *)sceSysregMeResetDisable);
    kcode(t->kc[0], (const void *)sceKernelDcacheWritebackInvalidateAll);
    kcode(t->kc[1], (const void *)sceKernelDcacheInvalidateAll);
    kcode(t->kc[2], (const void *)sceKernelIcacheInvalidateAll);
    t->step = 1;
    if (t->mode == 3) {
        boot(t);
        t->done = 1;
        return booted ? 0 : 1;      /* resident while the ME runs */
    }
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
    /* The ME has parked itself with halt (the game waits for that).  It is
     * not held in reset and its boot code is left in place: the firmware's
     * reboot resets and reloads the ME itself, and the four probes that
     * held it here (b_p3-b_p6) each hung the next PSPLink reboot. */
    booted = 0;
    return 0;
}
