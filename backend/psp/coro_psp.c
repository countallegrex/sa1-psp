/* coro_psp.c - the coroutines under hle/game/tasks.c on the PSP: our own
 * stacks in one static region and a context switch in Allegrex assembly,
 * the shape of backend/desktop/coro_switch.c and platform_desktop.c (same
 * slot numbering, same "a suspended coroutine is its saved stack pointer
 * plus the bytes above it").
 *
 * The switch saves what the PSP's ABI (MIPS EABI, single-float) keeps
 * across calls: s0-s7, fp, ra, gp and $f20-$f31, on the running stack,
 * then stores sp in the descriptor and loads the other one.  The kernel
 * does not care where a user thread's sp points (it switches to its own
 * stack on a syscall), so no thread state has to follow the switch.
 *
 * Part of sa1-psp (MIT). */
#include "platform_psp.h"

#include <malloc.h>
#include <string.h>

void sa1_coro_swap(uintptr_t *from_sp, uintptr_t *to_sp);
void sa1_coro_trampoline(void);
void sa1_coro_entry_c(void *co);

#define SAVE_BYTES 96   /* 11 words of integer registers, 12 FPU registers, 8-byte aligned */

__asm__(
    ".text\n"
    ".set push\n"
    ".set noreorder\n"
    ".globl sa1_coro_swap\n"
    ".ent sa1_coro_swap\n"
    "sa1_coro_swap:\n"          /* a0 = &from->sp, a1 = &to->sp */
    "    addiu $sp, $sp, -96\n"
    "    sw $16, 0($sp)\n"
    "    sw $17, 4($sp)\n"
    "    sw $18, 8($sp)\n"
    "    sw $19, 12($sp)\n"
    "    sw $20, 16($sp)\n"
    "    sw $21, 20($sp)\n"
    "    sw $22, 24($sp)\n"
    "    sw $23, 28($sp)\n"
    "    sw $30, 32($sp)\n"
    "    sw $31, 36($sp)\n"
    "    sw $28, 40($sp)\n"
    "    swc1 $f20, 44($sp)\n"
    "    swc1 $f21, 48($sp)\n"
    "    swc1 $f22, 52($sp)\n"
    "    swc1 $f23, 56($sp)\n"
    "    swc1 $f24, 60($sp)\n"
    "    swc1 $f25, 64($sp)\n"
    "    swc1 $f26, 68($sp)\n"
    "    swc1 $f27, 72($sp)\n"
    "    swc1 $f28, 76($sp)\n"
    "    swc1 $f29, 80($sp)\n"
    "    swc1 $f30, 84($sp)\n"
    "    swc1 $f31, 88($sp)\n"
    "    sw $sp, 0($4)\n"
    "    lw $sp, 0($5)\n"
    "    lw $16, 0($sp)\n"
    "    lw $17, 4($sp)\n"
    "    lw $18, 8($sp)\n"
    "    lw $19, 12($sp)\n"
    "    lw $20, 16($sp)\n"
    "    lw $21, 20($sp)\n"
    "    lw $22, 24($sp)\n"
    "    lw $23, 28($sp)\n"
    "    lw $30, 32($sp)\n"
    "    lw $31, 36($sp)\n"
    "    lw $28, 40($sp)\n"
    "    lwc1 $f20, 44($sp)\n"
    "    lwc1 $f21, 48($sp)\n"
    "    lwc1 $f22, 52($sp)\n"
    "    lwc1 $f23, 56($sp)\n"
    "    lwc1 $f24, 60($sp)\n"
    "    lwc1 $f25, 64($sp)\n"
    "    lwc1 $f26, 68($sp)\n"
    "    lwc1 $f27, 72($sp)\n"
    "    lwc1 $f28, 76($sp)\n"
    "    lwc1 $f29, 80($sp)\n"
    "    lwc1 $f30, 84($sp)\n"
    "    lwc1 $f31, 88($sp)\n"
    "    jr $31\n"
    "    addiu $sp, $sp, 96\n"
    ".end sa1_coro_swap\n"
    /* First entry of a new coroutine: the first frame (below) "returns"
     * here with s0 = the descriptor.  sa1_coro_entry_c never returns. */
    ".globl sa1_coro_trampoline\n"
    ".ent sa1_coro_trampoline\n"
    "sa1_coro_trampoline:\n"
    "    jal sa1_coro_entry_c\n"
    "    move $4, $16\n"
    "    break\n"
    "    nop\n"
    ".end sa1_coro_trampoline\n"
    ".set pop\n");

struct sa1_coro {
    uintptr_t sp;             /* first: the assembly addresses it by offset */
    void (*fn)(void *);
    void *arg;
    uintptr_t lo, hi;         /* the stack */
    int used, index;          /* index -1 = the supervisor (the PSP main thread) */
};

/* Where the stacks live: main_psp.c hands over a block of the PSP main
 * thread's own stack (sa1_plat_coro_set_region), so the kernel always sees
 * the thread's sp inside the thread's stack.  The first hardware boot put
 * the region in .bss and the console powered off at the first switch into
 * it (PPSSPP does not mind); without a block from main the region comes
 * from the heap, which keeps that layout for comparison (--coro-heap). */
static uint8_t *region;
void sa1_plat_coro_set_region(void *mem) { region = mem; }
static sa1_coro slots[SA1_CORO_SLOTS];
static sa1_coro supervisor = { 0, NULL, NULL, 0, 0, 1, -1 };
static sa1_coro *current = &supervisor;
static int mapped;

/* The whole region is painted once; nothing repaints, so high water is the
 * run's maximum per slot. */
static void stacks_map(void) {
    if (mapped) return;
    mapped = 1;
    const char *where = region ? "a block of the main thread's stack" : "the heap";
    if (!region) region = memalign(64, SA1_CORO_REGION_BYTES);
    sa1_log("coroutine: stacks at %p-%p (%s)", (void *)region, (void *)(region + SA1_CORO_REGION_BYTES), where);
    uint32_t *w = (uint32_t *)region;
    for (uint32_t i = 0; i < SA1_CORO_REGION_BYTES / 4; i++) w[i] = SA1_CORO_PAINT;
    uintptr_t at = (uintptr_t)region;
    for (int i = 0; i < SA1_CORO_SLOTS; i++) {
        uint32_t size = i == 0 ? SA1_CORO_ROOT_STACK : SA1_CORO_STACK;
        slots[i].lo = at;
        slots[i].hi = at + size;
        slots[i].index = i;
        at += size;
    }
}

/* The first frame: the swap's restore area with ra = the trampoline, s0 =
 * the descriptor, fp = 0 (ends a stack walk) and the caller's gp, under 16
 * zero bytes at the top. */
static uintptr_t first_frame(sa1_coro *co) {
    uintptr_t sp = co->hi - 16 - SAVE_BYTES;
    uint32_t *regs = (uint32_t *)sp;
    memset((void *)sp, 0, 16 + SAVE_BYTES);
    uint32_t gp;
    __asm__ volatile("move %0, $gp" : "=r"(gp));
    regs[0] = (uint32_t)(uintptr_t)co;                    /* s0 */
    regs[9] = (uint32_t)(uintptr_t)sa1_coro_trampoline;   /* ra */
    regs[10] = gp;
    return sp;
}

void sa1_coro_entry_c(void *p) {
    sa1_coro *co = (sa1_coro *)p;
    co->fn(co->arg);
    sa1_log("coroutine: entry function returned; back to the supervisor");
    for (;;) sa1_plat_coro_switch(&supervisor);
}

sa1_coro *sa1_plat_coro_current(void) { return current; }
sa1_coro *sa1_plat_coro_supervisor(void) { return &supervisor; }
sa1_coro *sa1_plat_coro_main(void) { return &slots[0]; }

static sa1_coro *create_at(int i, void (*fn)(void *), void *arg) {
    sa1_coro *co = &slots[i];
    co->used = 1; co->fn = fn; co->arg = arg;
    co->sp = first_frame(co);
    return co;
}

sa1_coro *sa1_plat_coro_create(void (*fn)(void *arg), void *arg, unsigned stack_bytes) {
    stacks_map();
    (void)stack_bytes;        /* tasks.c asks for the desktop's 1 MB; the slots are what they are */
    for (int i = 0; i < SA1_CORO_SLOTS; i++)      /* the first one created is the root: slot 0, the big stack */
        if (!slots[i].used) return create_at(i, fn, arg);
    sa1_log("coroutine: all %d slots in use", SA1_CORO_SLOTS);
    return NULL;
}

int sa1_plat_coro_run_root(void (*fn)(void *), void *arg) {
    stacks_map();
    if (slots[0].used) return -1;
    create_at(0, fn, arg);
    sa1_plat_coro_switch(&slots[0]);
    return 0;
}

void sa1_plat_coro_switch(sa1_coro *to) {
    sa1_coro *from = current;
    if (to == from) return;
    current = to;
    sa1_coro_swap(&from->sp, &to->sp);
}

void sa1_plat_coro_destroy(sa1_coro *co) {
    if (!co || co == &supervisor || co == current) return;
    co->used = 0; co->fn = NULL; co->arg = NULL;
}

int sa1_plat_coro_index(const sa1_coro *co) { return co ? co->index : -1; }
sa1_coro *sa1_plat_coro_by_index(int i) { return i >= 0 && i < SA1_CORO_SLOTS ? &slots[i] : NULL; }
void sa1_plat_coro_stack(int i, uintptr_t *lo, uintptr_t *hi) {
    stacks_map();
    if (i < 0 || i >= SA1_CORO_SLOTS) { *lo = *hi = 0; return; }
    *lo = slots[i].lo; *hi = slots[i].hi;
}
int sa1_plat_coro_used(int i) { return i >= 0 && i < SA1_CORO_SLOTS && slots[i].used; }
uintptr_t sa1_plat_coro_sp(int i) { return i >= 0 && i < SA1_CORO_SLOTS ? slots[i].sp : 0; }
void sa1_plat_coro_reset_all(void) {
    stacks_map();
    for (int i = 0; i < SA1_CORO_SLOTS; i++) { slots[i].used = 0; slots[i].fn = NULL; slots[i].arg = NULL; slots[i].sp = 0; }
}
void sa1_plat_coro_restore(int i, uintptr_t sp) {
    stacks_map();
    if (i < 0 || i >= SA1_CORO_SLOTS) return;
    slots[i].used = 1;
    slots[i].sp = sp;
}

uint32_t sa1_plat_coro_high_water(int i) {
    stacks_map();
    if (i < 0 || i >= SA1_CORO_SLOTS) return 0;
    const uint32_t *w = (const uint32_t *)slots[i].lo, *end = (const uint32_t *)slots[i].hi;
    while (w < end && *w == SA1_CORO_PAINT) w++;
    return (uint32_t)((uintptr_t)end - (uintptr_t)w);
}
