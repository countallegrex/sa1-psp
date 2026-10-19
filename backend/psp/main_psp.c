/* main_psp.c - the PSP boot of the recompiled game (M4).
 *
 * The EBOOT lives in ms0:/PSP/GAME/SA1PSP/ (or wherever argv[0] says), the
 * game data in data/ next to it (tools/convert_assets.py writes both), and
 * the command line in args.txt there, one or more options per line, '#'
 * starting a comment.  Paths in it are relative to that folder.  Options
 * are the desktop's (backend/desktop/main.c) where they mean the same:
 *
 *   --frames N  --seconds S  --log FILE (default sa1psp.log)  --headless
 *   --press FRAME:BUTTONS[:HOLD]  --stick FRAME:X:Y[:HOLD]  --no-pad
 *   --screenshot FRAME:FILE.ppm  --stats N  --aspect wide|4:3
 *   --strict --replay FILE --check FILE --expect-frame N --record FILE ...
 *     (input_tape.c, tests/replay/README.md: the same tapes as the desktop)
 * and the PSP's own:
 *   --mem FRAME     log the memory report at guest frame FRAME (repeatable;
 *                   it is also logged at exit)
 *   --no-pace       run guest frames as fast as they come (no vblank wait)
 *   --stdout        copy the log to stdout (psplink, PPSSPP's log)
 *   --sync-log      write every log line through to the card for the whole
 *                   run (default: through frame 300, then buffered)
 *   --cull-flip     the other winding convention for PowerVR culling (draw_gu.c)
 *   --data DIR      the data root (default data)
 *
 * Without args.txt the EBOOT plays: the pad drives the game, frames are
 * paced to the vblank.  HOME exits.
 *
 * Boot state as on the desktop (docs/m1-boot-report.md section 2): the
 * image at 0x8C010000, r15 = 0x8C00F400, FPSCR 0x00040001, then
 * fn_8c010000, on the root coroutine (coro_psp.c) with the PSP's main thread
 * as the supervisor.
 *
 * Part of sa1-psp (MIT). */
#include "hle.h"
#include "../draw.h"
#include "../desktop/input_tape.h"
#include "platform_psp.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspsysmem.h>
#include <psppower.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

PSP_MODULE_INFO("SA1PSP", PSP_MODULE_USER, 0, 4);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
/* 256 KB for main itself plus the coroutine stacks (coro_psp.c), which are
 * carved from this stack so the kernel sees sp inside the thread's stack. */
PSP_MAIN_THREAD_STACK_SIZE_KB(2048);
/* Heap = all free user memory (backend/psp/hello.c, README section 7). */
#ifdef PSP_HEAP_SIZE_MAX
PSP_HEAP_SIZE_MAX();
#else
PSP_HEAP_SIZE_KB(-1);
#endif

void fn_8c010000(sh4_ctx *c);
void kamui_report(void);
void kamui_set_screenshot(const char *path);
void draw_gu_report(void);
void draw_gu_set_cull_flip(int on);
uint32_t hle_task_running(void);

static char base_dir[256] = "ms0:/PSP/GAME/SA1PSP";
static const char *data_dir = "data", *log_path = "sa1psp.log";
static int headless, no_pad, no_pace, to_stdout, sync_log, stats_every, coro_heap;
static uint32_t frame_limit;
static uint64_t time_limit_us, t0;
static sh4_ctx ctx;
#define PRESS_MAX 64
static struct { uint32_t frame, buttons, hold; } presses[PRESS_MAX];
static int npresses;
static struct { uint32_t frame, hold; int x, y; } sticks[PRESS_MAX];
static int nsticks;
static struct { uint32_t frame; const char *path; } shots[PRESS_MAX];
static int nshots;
static uint32_t mem_frames[16];
static int nmem;

/* ---- memory report (README section 7) ------------------------------------- */
extern char __executable_start[] __attribute__((weak)), _etext[] __attribute__((weak)),
            __bss_start[] __attribute__((weak)), _end[] __attribute__((weak));

/* The heap holds all user memory from the start (PSP_HEAP_SIZE_KB(-1)), so
 * what is left is the largest block malloc still gives, found by halving. */
static uint32_t heap_largest_free(void) {
    uint32_t lo = 0, hi = 64u << 20;
    while (hi - lo > (64u << 10)) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint8_t *p = malloc(mid);
        /* PPSSPP's sbrk never refuses; a block past user RAM (0x0C000000 with MEMSIZE=1) is a refusal too */
        if (p && (uintptr_t)p + mid <= 0x0C000000u) { free(p); lo = mid; }
        else { free(p); hi = mid; }
    }
    return lo;
}

static void mem_report(const char *when) {
    uint32_t largest = heap_largest_free();
    struct mallinfo mi = mallinfo();
    const draw_stats *ds = draw_get_stats();
    uint32_t image = (_end && __executable_start) ? (uint32_t)(_end - __executable_start) : 0;
    uint32_t text = (_etext && __executable_start) ? (uint32_t)(_etext - __executable_start) : 0;
    uint32_t bss = (_end && __bss_start) ? (uint32_t)(_end - __bss_start) : 0;
    sa1_log("mem (%s): EBOOT image %u KB (text %u KB, bss %u KB: coroutine stacks, display list)",
            when, image / 1024u, text / 1024u, bss / 1024u);
    sa1_log("mem (%s): heap in use %u KB (guest RAM %u KB, textures %u KB in %u), sbrk %u KB, largest free block %u KB; outside the heap %u KB",
            when, (unsigned)mi.uordblks / 1024u, SA1_RAM_ALLOC / 1024u, ds->texture_bytes / 1024u, ds->textures_alive,
            (unsigned)mi.arena / 1024u, largest / 1024u, (unsigned)sceKernelTotalFreeMemSize() / 1024u);
    uint32_t root = sa1_plat_coro_high_water(0), task = 0;
    int task_i = -1;
    for (int i = 1; i < SA1_CORO_SLOTS; i++) { uint32_t h = sa1_plat_coro_high_water(i); if (h > task) { task = h; task_i = i; } }
    sa1_log("mem (%s): stack high water root %u KB of %u KB, tasks %u KB of %u KB (slot %d)", when,
            root / 1024u, SA1_CORO_ROOT_STACK / 1024u, task / 1024u, SA1_CORO_STACK / 1024u, task_i);
}

/* ---- host stack probe (sa1_depth_hook): the desktop's, against our slots ---- */
static int stack_tripped;
static void stack_probe(void) {
    int i = sa1_plat_coro_index(sa1_plat_coro_current());
    if (i < 0 || stack_tripped) return;
    uintptr_t lo, hi, sp = (uintptr_t)__builtin_frame_address(0);
    sa1_plat_coro_stack(i, &lo, &hi);
    if (sp >= lo + SA1_CORO_CANARY + 4096u) return;
    stack_tripped = 1;
    uint32_t task = hle_task_running();
    sa1_log("stack: %u of %u bytes in use on %s%08x (slot %d): trapping before the overflow",
            (unsigned)(hi - sp), (unsigned)(hi - lo), task ? "task 0x" : "the root context 0x", task, i);
    plat_backtrace();
    sa1_trap(&ctx, 0, "host stack nearly exhausted");
}

static void report_and_exit(int code) {
    code = input_tape_finish(code, NULL);
    sa1_log("exit: %u frames, %.3f s, code %d", hle_frame_count(), (sa1_plat_time_us() - t0) / 1e6, code);
    kamui_report();
    draw_gu_report();
    mem_report("exit");
    sa1_seam_report(20);
    plat_log_flush();
    plat_files_close_all();
    draw_shutdown();
    sceKernelExitGame();
    exit(code);
}

static void on_trap(sh4_ctx *c, uint32_t pc, const char *what) {
    (void)c; (void)pc; (void)what;
    input_tape_finish(3, "trap");
    plat_backtrace();
    sa1_log("exit: trap after %u frames, %.3f s", hle_frame_count(), (sa1_plat_time_us() - t0) / 1e6);
    mem_report("trap");
    sa1_seam_report(20);
    plat_log_flush();
}
static void on_badaddr(uint32_t addr) { (void)addr; plat_backtrace(); }

/* Guest frame N waits for vblank N (from the first frame on): 60 Hz, as
 * the console, whatever the game draws.  More than 15 vblanks behind (a
 * load) re-bases instead of running fast to catch up. */
static unsigned pace_v0;
static uint32_t pace_f0;
static int pace_set;
static void pace(uint32_t frame) {
    unsigned now = sceDisplayGetVcount();
    if (!pace_set) { pace_set = 1; pace_v0 = now; pace_f0 = frame; return; }
    unsigned due = pace_v0 + (frame - pace_f0);
    if ((int)(now - due) > 15) { pace_v0 = now; pace_f0 = frame; return; }
    while ((int)(due - sceDisplayGetVcount()) > 0) sceDisplayWaitVblankStart();
}

/* Milestones for the hardware log: the game mode and the level as the
 * check files name them (tests/replay/NAME.check: 0x8C760970 mode, 0x8C7493BA
 * level, 0x8C7493BC act), with the time since main(). */
static void milestones(uint32_t frame) {
    static uint32_t mode = ~0u, level = ~0u, act = ~0u;
    uint32_t m = hle_rd16(0x8C760970u), l = hle_rd16(0x8C7493BAu), a = hle_rd16(0x8C7493BCu);
    if (m != mode || l != level || a != act)
        sa1_log("milestone: frame %u, %.2f s: game mode %u, level %u, act %u", frame, (sa1_plat_time_us() - t0) / 1e6, m, l, a);
    mode = m; level = l; act = a;
}

static void on_frame(uint32_t frame) {
    if (!headless && !no_pace) pace(frame);
    milestones(frame);
    uint32_t b = 0;
    int scripted = 0;
    for (int i = 0; i < npresses; i++)
        if (frame >= presses[i].frame && frame < presses[i].frame + presses[i].hold) { b |= presses[i].buttons; scripted = 1; }
    int sx = 0, sy = 0, stick_scripted = 0;
    for (int i = 0; i < nsticks; i++)
        if (frame >= sticks[i].frame && frame < sticks[i].frame + sticks[i].hold) { sx = sticks[i].x; sy = sticks[i].y; stick_scripted = 1; }
    hle_pad live = {0};
    int quit = 0;
    if (!headless && draw_poll_input(&live, &quit) == 0) {
        if (no_pad) live = (hle_pad){0};
        if (!scripted) { hle_pad_state = live; b = live.buttons; }
        else { hle_pad_state.x = live.x; hle_pad_state.y = live.y; hle_pad_state.ltrig = live.ltrig; hle_pad_state.rtrig = live.rtrig; }
    }
    if (stick_scripted || headless) { hle_pad_state.x = (int8_t)sx; hle_pad_state.y = (int8_t)sy; }
    b = input_tape_pad(frame, b, scripted);
    if (b != hle_pad_state.buttons) sa1_log("pad: buttons 0x%x", b);
    hle_pad_state.buttons = b;
    for (int i = 0; i < nshots; i++) if (shots[i].frame == frame) kamui_set_screenshot(shots[i].path);
    for (int i = 0; i < nmem; i++) if (mem_frames[i] == frame) { char w[32]; snprintf(w, sizeof w, "frame %u", frame); mem_report(w); }
    input_tape_frame_end(frame);
    if (stats_every && frame % (uint32_t)stats_every == 0) { char w[32]; snprintf(w, sizeof w, "frame %u", frame); kamui_report(); draw_gu_report(); mem_report(w); }
    if (frame == 300 && !sync_log) { sa1_log("log: buffered from here (--sync-log keeps writing through)"); plat_log_sync(0); }
    if (frame % 120 == 0) plat_log_flush();
    if (frame_limit && frame >= frame_limit) { sa1_log("frame limit %u reached", frame_limit); report_and_exit(0); }
    if (time_limit_us && sa1_plat_time_us() - t0 >= time_limit_us) { sa1_log("time limit reached"); report_and_exit(0); }
}

static void boot(void *arg) {
    (void)arg;
    memset(&ctx, 0, sizeof ctx);
    ctx.r[15] = 0x8C00F400u;
    ctx.fpscr = 0x00040001u;
    ctx.vbr = 0x8C000000u;
    sa1_log("boot: entering fn_8c010000");
    fn_8c010000(&ctx);
    sa1_log("boot: fn_8c010000 returned (r0=%08x)", ctx.r[0]);
    report_and_exit(0);
}

static int load_image(const char *path) {
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) { sa1_log("cannot open %s (0x%08x)", path, (unsigned)fd); return -1; }
    int n = sceIoRead(fd, sa1_ram + 0x010000, SA1_RAM_SIZE - 0x010000);
    sceIoClose(fd);
    if (n < 0x100000) { sa1_log("%s: too small (%d bytes)", path, n); return -1; }
    sa1_log("loaded %s: %d bytes at 0x8c010000", path, n);
    return 0;
}

/* sceIo takes device paths; anything relative is under the EBOOT's folder. */
static const char *abs_path(char *out, unsigned n, const char *p) {
    if (strchr(p, ':')) snprintf(out, n, "%s", p);
    else snprintf(out, n, "%s/%s", base_dir, p);
    return out;
}

/* ---- arguments: argv, then args.txt -------------------------------------- */
#define ARGS_MAX 128
static char args_text[8192];
static char *args_v[ARGS_MAX];
static int args_c;

static void read_args_file(const char *path) {
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    int n = sceIoRead(fd, args_text, sizeof args_text - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    args_text[n] = 0;
    char *p = args_text;
    while (*p && args_c < ARGS_MAX) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        if (!*p) break;
        args_v[args_c++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if (*p) *p++ = 0;
    }
}

static int exit_callback(int a, int b, void *c) {
    (void)a; (void)b; (void)c;
    sa1_log("exit: HOME at frame %u", hle_frame_count());
    plat_log_flush();
    sceKernelExitGame();
    return 0;
}
static int callback_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int cb = sceKernelCreateCallback("exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

/* Start-up self-test of the coroutine switch, each stage logged from the
 * main stack before the next: a bare switch in and out, a syscall on the
 * coroutine's stack, then a log line (file I/O) from it. */
static volatile int coro_crumb;
static void coro_test_fn(void *arg) {
    (void)arg;
    coro_crumb = 1;
    sa1_plat_coro_switch(sa1_plat_coro_supervisor());
    coro_crumb = 2;
    (void)sceKernelGetSystemTimeWide();
    coro_crumb = 3;
    sa1_plat_coro_switch(sa1_plat_coro_supervisor());
    sa1_log("coro test: a log line written from the coroutine's stack");
    coro_crumb = 4;
    for (;;) sa1_plat_coro_switch(sa1_plat_coro_supervisor());
}
static void coro_self_test(void) {
    sa1_coro *co = sa1_plat_coro_create(coro_test_fn, NULL, 0);
    if (!co) { sa1_log("coro test: cannot create"); return; }
    sa1_log("coro test: first switch into slot %d", sa1_plat_coro_index(co));
    sa1_plat_coro_switch(co);
    sa1_log("coro test: switch in and out ok (crumb %d); now a syscall on its stack", coro_crumb);
    sa1_plat_coro_switch(co);
    sa1_log("coro test: syscall ok (crumb %d); now a log line from it", coro_crumb);
    sa1_plat_coro_switch(co);
    sa1_log("coro test: back (crumb %d); all three stages passed", coro_crumb);
    sa1_plat_coro_destroy(co);
}

int main(int argc, char **argv) {
    /* The coroutine stacks, on this thread's stack (main never returns
     * while the game runs). */
    static uint8_t *coro_block;
    uint8_t coro_area[SA1_CORO_REGION_BYTES] __attribute__((aligned(64)));
    coro_block = coro_area;
    { SceUID th = sceKernelCreateThread("callbacks", callback_thread, 0x11, 0xFA0, 0, NULL); if (th >= 0) sceKernelStartThread(th, 0, NULL); }
    if (argc > 0 && argv[0] && strchr(argv[0], '/')) {
        snprintf(base_dir, sizeof base_dir, "%s", argv[0]);
        *strrchr(base_dir, '/') = 0;
    }
    chdir(base_dir);
    char path[300];
    read_args_file(abs_path(path, sizeof path, "args.txt"));
    for (int i = 0; i < args_c; i++) {
        char **av = args_v;
        if (!strcmp(av[i], "--frames") && i + 1 < args_c) frame_limit = (uint32_t)strtoul(av[++i], NULL, 0);
        else if (!strcmp(av[i], "--seconds") && i + 1 < args_c) time_limit_us = (uint64_t)(atof(av[++i]) * 1e6);
        else if (!strcmp(av[i], "--log") && i + 1 < args_c) log_path = av[++i];
        else if (!strcmp(av[i], "--data") && i + 1 < args_c) data_dir = av[++i];
        else if (!strcmp(av[i], "--headless")) headless = 1;
        else if (!strcmp(av[i], "--no-pad")) no_pad = 1;
        else if (!strcmp(av[i], "--no-pace")) no_pace = 1;
        else if (!strcmp(av[i], "--stdout")) to_stdout = 1;
        else if (!strcmp(av[i], "--sync-log")) sync_log = 1;
        else if (!strcmp(av[i], "--coro-heap")) coro_heap = 1;
        else if (!strcmp(av[i], "--cull-flip")) draw_gu_set_cull_flip(1);
        else if (!strcmp(av[i], "--stats") && i + 1 < args_c) stats_every = atoi(av[++i]);
        else if (!strcmp(av[i], "--mem") && i + 1 < args_c && nmem < 16) mem_frames[nmem++] = (uint32_t)strtoul(av[++i], NULL, 0);
        else if (!strcmp(av[i], "--aspect") && i + 1 < args_c) { const char *a = av[++i]; hle_aspect_wide = strcmp(a, "4:3") != 0 && strcmp(a, "4x3") != 0; }
        else if (!strcmp(av[i], "--screenshot") && i + 1 < args_c && nshots < PRESS_MAX) {
            char *spec = av[++i], *end;
            shots[nshots].frame = (uint32_t)strtoul(spec, &end, 0);
            shots[nshots].path = *end == ':' ? end + 1 : "frame.ppm";
            nshots++;
        } else if (!strcmp(av[i], "--stick") && i + 1 < args_c && nsticks < PRESS_MAX) {
            char *spec = av[++i], *end;
            sticks[nsticks].frame = (uint32_t)strtoul(spec, &end, 0);
            sticks[nsticks].x = *end == ':' ? (int)strtol(end + 1, &end, 0) : 0;
            sticks[nsticks].y = *end == ':' ? (int)strtol(end + 1, &end, 0) : 0;
            sticks[nsticks].hold = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 0) : 2;
            nsticks++;
        } else if (!strcmp(av[i], "--press") && i + 1 < args_c && npresses < PRESS_MAX) {
            char *spec = av[++i], *end;
            presses[npresses].frame = (uint32_t)strtoul(spec, &end, 0);
            presses[npresses].buttons = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 16) : 0;
            presses[npresses].hold = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 0) : 2;
            npresses++;
        } else if (input_tape_arg(args_c, av, &i)) continue;
        else { /* logged once the log is open */ }
    }
    freopen(abs_path(path, sizeof path, "stderr.txt"), "w", stderr);   /* input_tape.c reports start-up errors there */
    plat_log_open(abs_path(path, sizeof path, log_path), to_stdout);
    t0 = sa1_plat_time_us();
    sa1_log("sa1-psp (PSP): base %s, data %s, %d args from args.txt%s", base_dir, data_dir, args_c, headless ? ", headless" : "");
    for (int i = 0; i < args_c; i++) sa1_log("  arg %s", args_v[i]);
    if (!coro_heap) sa1_plat_coro_set_region(coro_block);
    sa1_log("step: coroutine self-test (%s)", coro_heap ? "stacks on the heap" : "stacks on the main thread's stack");
    coro_self_test();
    sa1_log("step: memory report");
    mem_report("start");

    /* The PSP boots at 222 MHz; the game wants all of it (README section 5). */
    sa1_log("step: clock 333 MHz");
    int clk = scePowerSetClockFrequency(333, 333, 166);
    sa1_log("clock: cpu %d MHz, bus %d MHz (set: %d)", scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt(), clk);

    sa1_depth_hook = stack_probe;
    sa1_trap_hook = on_trap;
    sa1_badaddr_hook = on_badaddr;
    hle_frame_hook = on_frame;
    sa1_trace_frame = hle_frame_count;

    sa1_log("step: guest RAM (%u KB)", SA1_RAM_ALLOC / 1024u);
    sa1_ram = memalign(64, SA1_RAM_ALLOC);
    if (!sa1_ram) { sa1_log("cannot allocate %u KB of guest RAM", SA1_RAM_ALLOC / 1024u); plat_log_flush(); sceKernelExitGame(); return 1; }
    memset(sa1_ram, 0, SA1_RAM_ALLOC);
    sa1_log("guest RAM: %u KB at %p (16 MB + the 2 MB list area + the 2 MB null page)", SA1_RAM_ALLOC / 1024u, (void *)sa1_ram);
    char root[300], bin[320];
    abs_path(root, sizeof root, data_dir);
    snprintf(bin, sizeof bin, "%s/1ST_READ.BIN", root);
    sa1_log("step: load %s", bin);
    if (load_image(bin) != 0) { plat_log_flush(); sceKernelExitGame(); return 1; }
    sa1_log("step: tapes");
    if (input_tape_start(bin) != 0) { sa1_log("tape: start-up failed (stderr.txt has why)"); plat_log_flush(); sceKernelExitGame(); return 2; }
    if (!frame_limit) frame_limit = input_tape_frames();
    plat_set_data_root(root);
    sa1_log("step: hle_init");
    hle_init();
    sa1_log("step: function map");
    plat_crash_init();
    sa1_log("step: draw_init");
    draw_init(1, headless);
    sa1_log("step: draw_set_aspect");
    draw_set_aspect(hle_aspect_wide);
    sa1_log("step: watchdog");
    plat_watchdog_start(hle_frame_count, 10);
    mem_report("boot");
    plat_log_flush();
    sa1_log("step: root coroutine");
    if (sa1_plat_coro_run_root(boot, NULL) != 0) sa1_log("boot: cannot start the root coroutine");
    report_and_exit(1);
    return 1;
}
