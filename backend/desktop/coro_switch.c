/* coro_switch.c - the context switch under the desktop coroutines
 * (platform_desktop.c): x86-64, Windows and System V.
 *
 * A coroutine's state while it is not running is its stack: the switch
 * pushes the callee-saved registers on the current stack, stores the stack
 * pointer in the descriptor, loads the other descriptor's, pops and
 * returns.  Nothing is kept anywhere else, which is what lets a save state
 * carry a coroutine as a stack pointer plus the bytes above it
 * (docs/savestate.md section 1).  On Windows the thread's stack bounds in
 * the TEB (StackBase, StackLimit, DeallocationStack) are swapped as well,
 * as fibers do: __chkstk, GetCurrentThreadStackLimits (the stack probe in
 * main.c) and the exception dispatcher read them.
 *
 * The descriptor layout the assembly relies on is sa1_coro_regs
 * (platform_desktop.c keeps it at the start of sa1_coro).
 *
 * Part of sa1-psp (MIT). */
#include <stdint.h>
#include <stddef.h>

typedef struct sa1_coro_regs {
    uintptr_t sp;        /* +0  saved stack pointer */
    uintptr_t teb[3];    /* +8  Windows: StackBase, StackLimit, DeallocationStack */
} sa1_coro_regs;

void sa1_coro_swap(sa1_coro_regs *from, sa1_coro_regs *to);
void sa1_coro_trampoline(void);
void sa1_call_on_stack(void (*fn)(void *), void *arg, void *stack_top);
void sa1_coro_entry_c(void *co);   /* platform_desktop.c: runs the coroutine's function */

#if defined(__x86_64__) || defined(_M_X64)
#ifdef _WIN32
/* Windows x64: callee-saved rbx rbp rdi rsi r12-r15 and xmm6-xmm15.  rcx =
 * from, rdx = to.  The TEB is at gs:0; NT_TIB.StackBase 0x08, StackLimit
 * 0x10, TEB.DeallocationStack 0x1478. */
__asm__(
    ".text\n"
    ".globl sa1_coro_swap\n"
    "sa1_coro_swap:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %rdi\n"
    "    pushq %rsi\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    subq $160, %rsp\n"
    "    movups %xmm6,  0(%rsp)\n"
    "    movups %xmm7,  16(%rsp)\n"
    "    movups %xmm8,  32(%rsp)\n"
    "    movups %xmm9,  48(%rsp)\n"
    "    movups %xmm10, 64(%rsp)\n"
    "    movups %xmm11, 80(%rsp)\n"
    "    movups %xmm12, 96(%rsp)\n"
    "    movups %xmm13, 112(%rsp)\n"
    "    movups %xmm14, 128(%rsp)\n"
    "    movups %xmm15, 144(%rsp)\n"
    "    movq %gs:8, %rax\n"
    "    movq %rax, 8(%rcx)\n"
    "    movq %gs:16, %rax\n"
    "    movq %rax, 16(%rcx)\n"
    "    movq %gs:0x1478, %rax\n"
    "    movq %rax, 24(%rcx)\n"
    "    movq %rsp, 0(%rcx)\n"
    "    movq 0(%rdx), %rsp\n"
    "    movq 8(%rdx), %rax\n"
    "    movq %rax, %gs:8\n"
    "    movq 16(%rdx), %rax\n"
    "    movq %rax, %gs:16\n"
    "    movq 24(%rdx), %rax\n"
    "    movq %rax, %gs:0x1478\n"
    "    movups 0(%rsp),   %xmm6\n"
    "    movups 16(%rsp),  %xmm7\n"
    "    movups 32(%rsp),  %xmm8\n"
    "    movups 48(%rsp),  %xmm9\n"
    "    movups 64(%rsp),  %xmm10\n"
    "    movups 80(%rsp),  %xmm11\n"
    "    movups 96(%rsp),  %xmm12\n"
    "    movups 112(%rsp), %xmm13\n"
    "    movups 128(%rsp), %xmm14\n"
    "    movups 144(%rsp), %xmm15\n"
    "    addq $160, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rsi\n"
    "    popq %rdi\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    retq\n"
    /* First entry of a new coroutine: the initial frame (platform_desktop.c)
     * returns here with r12 = the descriptor and rsp 8 mod 16, as at a
     * function entry.  32 bytes of shadow space, then call; never returns. */
    ".globl sa1_coro_trampoline\n"
    "sa1_coro_trampoline:\n"
    "    movq %r12, %rcx\n"
    "    subq $40, %rsp\n"
    "    call sa1_coro_entry_c\n"
    "    ud2\n"
    /* sa1_call_on_stack(fn, arg, stack_top): run fn(arg) on another stack
     * (the crash handler, when the faulting stack has no room left).
     * rcx = fn, rdx = arg, r8 = stack top.  32 bytes of shadow space. */
    ".globl sa1_call_on_stack\n"
    "sa1_call_on_stack:\n"
    "    pushq %rbp\n"
    "    movq %rsp, %rbp\n"
    "    movq %r8, %rsp\n"
    "    andq $-16, %rsp\n"
    "    subq $32, %rsp\n"
    "    movq %rcx, %rax\n"
    "    movq %rdx, %rcx\n"
    "    call *%rax\n"
    "    movq %rbp, %rsp\n"
    "    popq %rbp\n"
    "    retq\n");
#define SA1_CORO_SAVE_BYTES (8 * 8 + 160)   /* what the swap pushes below the return address */
#else
/* System V x86-64: callee-saved rbx rbp r12-r15.  rdi = from, rsi = to. */
__asm__(
    ".text\n"
    ".globl sa1_coro_swap\n"
    ".type sa1_coro_swap, @function\n"
    "sa1_coro_swap:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rsp, 0(%rdi)\n"
    "    movq 0(%rsi), %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    retq\n"
    ".globl sa1_coro_trampoline\n"
    ".type sa1_coro_trampoline, @function\n"
    "sa1_coro_trampoline:\n"
    "    movq %r12, %rdi\n"
    "    subq $8, %rsp\n"
    "    call sa1_coro_entry_c\n"
    "    ud2\n"
    ".section .note.GNU-stack,\"\",@progbits\n"
    ".text\n
    /* sa1_call_on_stack(fn, arg, stack_top): rdi = fn, rsi = arg, rdx = stack top */
    ".globl sa1_call_on_stack\n"
    ".type sa1_call_on_stack, @function\n"
    "sa1_call_on_stack:\n"
    "    pushq %rbp\n"
    "    movq %rsp, %rbp\n"
    "    movq %rdx, %rsp\n"
    "    andq $-16, %rsp\n"
    "    movq %rdi, %rax\n"
    "    movq %rsi, %rdi\n"
    "    call *%rax\n"
    "    movq %rbp, %rsp\n"
    "    popq %rbp\n"
    "    retq\n"");       /* back to .text: the C below must not land in the note section */
#define SA1_CORO_SAVE_BYTES (6 * 8)
#endif

/* Lays out the first frame of a coroutine on its stack (hi = the 16-byte
 * aligned top) so that the first switch into it lands in the trampoline
 * with r12 = co.  Returns the stack pointer to store in the descriptor.
 * From the top: a zero return address (ends every stack walk), the
 * trampoline's address (what the swap's ret pops), the callee-saved slots. */
uintptr_t sa1_coro_first_frame(uintptr_t hi, void *co) {
    uintptr_t *p = (uintptr_t *)hi;
    *--p = 0;                                   /* hi - 8: fake return address */
    *--p = (uintptr_t)sa1_coro_trampoline;      /* hi - 16: popped by retq */
    uintptr_t sp = (uintptr_t)p - SA1_CORO_SAVE_BYTES;
    uintptr_t *regs = (uintptr_t *)sp;
    for (size_t i = 0; i < SA1_CORO_SAVE_BYTES / sizeof(uintptr_t); i++) regs[i] = 0;
#ifdef _WIN32
    /* popped in order: r15 r14 r13 r12 rsi rdi rbx rbp, after the 160 xmm bytes */
    regs[160 / 8 + 3] = (uintptr_t)co;         /* r12 */
#else
    /* popped in order: r15 r14 r13 r12 rbx rbp */
    regs[3] = (uintptr_t)co;                   /* r12 */
#endif
    return sp;
}
#else
#error "sa1-psp desktop coroutines: only x86-64 (Windows, System V) is implemented; see backend/desktop/coro_switch.c"
#endif
