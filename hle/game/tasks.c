/* tasks.c - the game's cooperative task system, on native coroutines.
 *
 * The game (not the SDK) has a small coroutine library at 0x8C099A00-
 * 0x8C099CDA: 36-byte task records in a doubly linked list (root record at
 * 0x8C7A0A0C, current task pointer at 0x8C7A0A08; fields +0 next, +4 prev,
 * +8 parent, +12 first child, +16 saved stack pointer, +20 stack top,
 * +24 destructor), each with its own guest stack right after the record.
 * Yield saves r4, r8-r14, MACH/MACL, GBR, FR12-FR15 and PR on the current
 * stack, picks the next task and pops that task's frame; start builds such a
 * frame so the first switch lands in the task function with its argument.
 * The title screen (ADVERTISE), the menus and the event scripts run as
 * tasks and yield once per frame.
 *
 * Recompiled code cannot switch stacks (registers are C locals and PR
 * carries no value: recomp/emitter/README.md), so the four stack-switching
 * entries are wrapped (recomp/sa1_us_1005/hle_wraps.tsv): every task is a
 * host coroutine (sa1_plat_coro_*), the guest register file is saved and
 * restored per task in a side table, and the list bookkeeping stays in the
 * recompiled bodies.  Each task keeps its own guest stack region, as on the
 * console, so stack addresses stay distinct.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"

#include <string.h>

#define CURRENT_TASK 0x8C7A0A08u
#define ROOT_TASK    0x8C7A0A0Cu
#define T_SP   16
#define T_TOP  20
#define HOST_STACK (1u << 20)   /* 1 MB of host stack per task */
#define TASK_MAX 32

void fn_8c099a00_orig(sh4_ctx *c);
void fn_8c099a1c(sh4_ctx *c);          /* pick the next task (recompiled) */
void fn_8c099ab2_orig(sh4_ctx *c);     /* kill (recompiled body) */
void fn_8c099b2c(sh4_ctx *c);

typedef struct hle_task {
    uint32_t task;          /* guest record address, 0 = free slot */
    sa1_coro *coro;
    sh4_ctx saved;          /* guest registers while suspended */
    uint32_t fn, arg;       /* entry for a task not started yet */
    int started, dead;
} hle_task;

static hle_task tasks[TASK_MAX];
static hle_task *running;               /* the task whose coroutine runs now */
static sh4_ctx *gctx;                   /* the one guest context */
static uint32_t switches;

static hle_task *find(uint32_t task) {
    for (int i = 0; i < TASK_MAX; i++) if (tasks[i].task == task) return &tasks[i];
    return NULL;
}

static hle_task *alloc(uint32_t task) {
    hle_task *t = find(task);
    if (t) return t;
    for (int i = 0; i < TASK_MAX; i++) if (!tasks[i].task) { t = &tasks[i]; break; }
    if (!t) { sa1_log("tasks: table full"); return NULL; }
    memset(t, 0, sizeof *t);
    t->task = task;
    return t;
}

static void release(hle_task *t) {
    if (t->coro && t->coro != sa1_plat_coro_main()) sa1_plat_coro_destroy(t->coro);
    memset(t, 0, sizeof *t);
}

/* Destroys coroutines of tasks killed while they were running (they could
 * not free themselves). */
static void reap(void) {
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].task && tasks[i].dead && &tasks[i] != running) release(&tasks[i]);
}

static void task_entry(void *arg);

/* Switch from the running task to `to`: save our guest registers, run the
 * other coroutine, and when control comes back restore ours. */
static void switch_to(sh4_ctx *c, hle_task *to) {
    hle_task *from = running;
    if (to == from) return;
    if (!to->coro) {
        to->coro = sa1_plat_coro_create(task_entry, to, HOST_STACK);
        if (!to->coro) { sa1_trap(c, to->task, "tasks: cannot create a coroutine"); return; }
    }
    from->saved = *c;
    running = to;
    switches++;
    sa1_plat_coro_switch(to->coro);
    /* resumed */
    running = from;
    *c = from->saved;
    reap();
}

/* First entry of a task's coroutine: run its function on its own guest
 * stack, then kill the task (what the saved PR of the initial frame does). */
static void task_entry(void *arg) {
    hle_task *t = (hle_task *)arg;
    sh4_ctx *c = gctx;
    *c = t->saved;
    reap();
    sa1_log("tasks: task 0x%08x starts fn 0x%08x(0x%08x) on guest stack 0x%08x", t->task, t->fn, t->arg, c->r[15]);
    hle_call_guest(c, t->fn, t->arg, 0, 0, 0);
    sa1_log("tasks: task 0x%08x function returned; killing it", t->task);
    /* 0x8C099AAC: kill the current task and switch (never returns here). */
    c->r[4] = hle_rd32(CURRENT_TASK);
    fn_8c099b2c(c);
    sa1_trap(c, t->task, "tasks: a killed task resumed");
}

/* 0x8C099A00: init.  Empties the table; the root task runs on the main
 * context. */
void fn_8c099a00(sh4_ctx *c) {
    HLE_ENTER(0x8c099a00u, "TaskInit (wrapped)");
    gctx = c;
    for (int i = 0; i < TASK_MAX; i++) if (tasks[i].task && &tasks[i] != running) release(&tasks[i]);
    memset(tasks, 0, sizeof tasks);
    fn_8c099a00_orig(c);
    hle_task *root = alloc(ROOT_TASK);
    root->coro = sa1_plat_coro_main();
    root->started = 1;
    running = root;
}

/* 0x8C099A50: yield.  The recompiled selector picks the next task; if it is
 * another one, switch to it. */
void fn_8c099a50(sh4_ctx *c) {
    HLE_ENTER(0x8c099a50u, "TaskYield (wrapped)");
    gctx = c;
    uint32_t cur = hle_rd32(CURRENT_TASK);
    fn_8c099a1c(c);
    uint32_t nxt = hle_rd32(CURRENT_TASK);
    if (nxt == cur) return;
    hle_task *to = find(nxt);
    if (!to) { sa1_trap(c, nxt, "tasks: yield to a task with no record"); return; }
    if (!running) running = find(cur);
    switch_to(c, to);
}

/* 0x8C099BBA: start(task, fn, arg).  For another task, remember the entry
 * and give it a fresh guest register file with r15 at its stack top; the
 * first yield into it runs fn.  For the current task (or NULL), the console
 * restarts the current context on the task's stack; here fn runs at once on
 * this coroutine and the task is killed when it returns. */
void fn_8c099bba(sh4_ctx *c) {
    HLE_ENTER(0x8c099bbau, "TaskStart (wrapped)");
    gctx = c;
    uint32_t task = ARG0, fn = ARG1, arg = ARG2;
    uint32_t cur = hle_rd32(CURRENT_TASK);
    if (task == 0 || task == cur) {
        uint32_t top = hle_rd32(cur + T_TOP) & 0x9FFFFFFFu;
        sa1_log("tasks: restart current task 0x%08x with fn 0x%08x(0x%08x)", cur, fn, arg);
        c->r[15] = top;
        hle_call_guest(c, fn, arg, 0, 0, 0);
        c->r[4] = hle_rd32(CURRENT_TASK);
        fn_8c099b2c(c);
        return;
    }
    hle_task *t = alloc(task);
    if (!t) return;
    t->fn = fn; t->arg = arg;
    t->saved = *c;
    t->saved.r[15] = hle_rd32(task + T_TOP) & 0x9FFFFFFFu;
    t->saved.r[4] = arg;
    hle_wr32(task + T_SP, t->saved.r[15]);
    sa1_log("tasks: task 0x%08x created, fn 0x%08x(0x%08x), stack top 0x%08x", task, fn, arg, t->saved.r[15]);
}

/* 0x8C099AB2: kill(task), recursive over children.  The body does the list
 * work; the wrapper drops the task's coroutine (deferred when it is ours). */
void fn_8c099ab2(sh4_ctx *c) {
    HLE_ENTER(0x8c099ab2u, "TaskKill (wrapped)");
    uint32_t task = ARG0;
    fn_8c099ab2_orig(c);
    hle_task *t = find(task);
    if (!t) return;
    if (t == running) t->dead = 1;
    else release(t);
}

/* 0x8C099B2C: kill(task) and, if the current task changed (it was the one
 * killed), resume the new current task; the killed coroutine never runs
 * again. */
void fn_8c099b2c(sh4_ctx *c) {
    HLE_ENTER(0x8c099b2cu, "TaskKillSwitch (wrapped)");
    gctx = c;
    uint32_t before = hle_rd32(CURRENT_TASK);
    fn_8c099ab2(c);
    uint32_t after = hle_rd32(CURRENT_TASK);
    if (after == before) return;
    hle_task *to = find(after);
    if (!to) { sa1_trap(c, after, "tasks: switch to a task with no record"); return; }
    if (running && running->dead) {
        /* our own task died: hand over and never come back */
        hle_task *self = running;
        sa1_log("tasks: task 0x%08x exits, resuming 0x%08x", self->task, after);
        if (!to->coro) to->coro = sa1_plat_coro_create(task_entry, to, HOST_STACK);
        running = to;
        switches++;
        sa1_plat_coro_switch(to->coro);
        sa1_trap(c, self->task, "tasks: a dead task resumed");
        return;
    }
    switch_to(c, to);
}

uint32_t hle_task_switches(void) { return switches; }
