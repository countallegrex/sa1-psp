/* main.c - desktop boot of the recompiled game (M2: graphics, no audio).
 *
 *   sa1psp <1ST_READ.BIN> <data root> [--frames N] [--seconds S] [--log FILE]
 *          [--watch ADDR[:OFF...]]... [--press FRAME:BUTTONS[:HOLD]]... [--trace-funcs FILE]
 *          [--headless] [--scale N] [--screenshot FRAME:FILE.ppm]... [--stats N]
 *          [--stick FRAME:X:Y[:HOLD]]... [--poke FRAME:ADDR:VALUE[:SIZE]]... [--dump FRAME:FILE]...
 *          [--aspect wide|4:3]
 *
 * Without --headless an SDL2 window opens at 480x272 times --scale (2 by
 * default) with an OpenGL 1.x fixed-function context (backend/draw.h), and
 * the controller or keyboard drives the pad whenever no --press is active.
 * --headless keeps the deterministic M1 behaviour for CI.  --screenshot
 * writes the frame buffer after guest frame FRAME; --stats N logs the
 * renderer counters every N frames.
 *
 * --trace-funcs writes, at exit, every host function that ran with the frame
 * it first ran in (needs a build with -DSA1_TRACE_FUNCS=ON).
 *
 * --strict, --record FILE, --replay FILE, --check FILE, --expect-frame N
 * and --epoch S: deterministic runs, input tapes and state checks
 * (input_tape.c, tests/replay/README.md).
 *
 * --press scripts the controller: at guest frame FRAME the buttons (hex
 * mask, hle.h: Start 8, A 4, B 2, Up 0x10 ...) are held for HOLD frames
 * (default 2).  This is how the boot test walks the title and menus.
 * --stick holds the analog stick at X,Y (-128..127, y positive = down)
 * for HOLD frames, so a scripted run can walk a stage.
 *
 * The data root is the directory that holds SONICADV/ (the disc's file
 * tree, which is also what the Memory Stick carries).  Nothing is built in:
 * both paths come from the command line.
 *
 * Boot state (Katana reset conditions, docs/m1-boot-report.md section 2):
 * the image at 0x8C010000, the stack at 0x8C00F400 growing down (the loader
 * stub fills 0x8C00C000-0x8C00F400 itself), FPSCR 0x00040001 (single
 * precision, round to nearest, all exceptions masked), GBR 0, the BIOS
 * vectors at 0x8C0000B0-0xBC installed by hle/bios.  Then fn_8c010000.
 *
 * Part of sa1-psp (MIT). */
#include "hle.h"
#include "../draw.h"
#include "input_tape.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

void fn_8c010000(sh4_ctx *c);
void plat_set_data_root(const char *root);
void plat_backtrace(void);
void plat_trace_dump(const char *path);
void plat_watchdog_start(uint32_t (*frame)(void), unsigned seconds);
void plat_hw_watch(uint32_t guest_addr, int len);
static const char *trace_path;

static FILE *log_file;
#define PRESS_MAX 64
static struct { uint32_t frame, buttons, hold; } presses[PRESS_MAX];
static int npresses;
static struct { uint32_t frame, hold; int x, y; } sticks[PRESS_MAX];   /* --stick FRAME:X:Y[:HOLD] */
static int nsticks;
static uint32_t frame_limit;
static uint64_t time_limit_us, t0;
static sh4_ctx ctx;
static int headless, scale = 2, stats_every;
#define SHOT_MAX 16
static struct { uint32_t frame; const char *path; } shots[SHOT_MAX], dumps[SHOT_MAX];
static int nshots, ndumps;
static struct { uint32_t frame, addr; int len; } hwwatch;   /* --hwwatch FRAME:ADDR[:LEN]: hardware write watch */
static struct { uint32_t frame, addr, value, size; } pokes[SHOT_MAX];
static int npokes;
void kamui_report(void);            /* hle/ninja/kamui.c: renderer counters */
void kamui_set_screenshot(const char *path);

static void log_sink(const char *line) {
    uint64_t t = sa1_plat_time_us() - t0;
    fprintf(log_file ? log_file : stdout, "[%6u.%03u f%-6u] %s\n", (unsigned)(t / 1000000u),
            (unsigned)(t / 1000u % 1000u), hle_frame_count(), line);
    if (log_file) fflush(log_file);
}

#ifdef _WIN32
/* A host crash (an out-of-range guest access, usually) prints where the
 * recompiled code was before the process dies. */
static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    sa1_log("crash: exception 0x%08lx at %p (guest frame %u)", ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress, hle_frame_count());
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        sa1_log("crash: %s of host address %p (sa1_ram is %p, so guest offset 0x%llx)",
                ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                (void *)ep->ExceptionRecord->ExceptionInformation[1], (void *)sa1_ram,
                (unsigned long long)((uint8_t *)ep->ExceptionRecord->ExceptionInformation[1] - sa1_ram));
    plat_backtrace();
    if (log_file) fflush(log_file);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static void report_and_exit(int code) {
    code = input_tape_finish(code, NULL);
    sa1_log("exit: %u frames, %.3f s", hle_frame_count(), (sa1_plat_time_us() - t0) / 1e6);
    kamui_report();
    sa1_seam_report(getenv("SA1_REPORT_TOP") ? atoi(getenv("SA1_REPORT_TOP")) : 30);
    draw_shutdown();
    if (trace_path) plat_trace_dump(trace_path);
    if (log_file) fclose(log_file);
    exit(code);
}

static void on_trap(sh4_ctx *c, uint32_t pc, const char *what) {
    (void)c; (void)pc; (void)what;
    input_tape_finish(3, "trap");
    plat_backtrace();
    sa1_log("exit: trap after %u frames, %.3f s", hle_frame_count(), (sa1_plat_time_us() - t0) / 1e6);
    sa1_seam_report(30);
    if (trace_path) plat_trace_dump(trace_path);
    if (log_file) fclose(log_file);
}

/* Debug aid (SA1_TASK_CHECK=1): walk the game's task lists (0x8C79F04C[8],
 * 48-byte tasks: +0 next, +4 prev, +8 parent, +12 child, +16 main) every
 * frame and log the first broken link, with the frame and the task's main
 * (which names the object).  A stale child pointer makes the game's
 * delete-children loop spin forever at the next level change. */
static int task_check_on = -1, task_check_bad;
static void task_check_list(uint32_t t, uint32_t parent, int depth, uint32_t frame) {
    uint32_t prev = 0;
    for (int n = 0; t && n < 8192 && task_check_bad < 20; n++) {
        uint32_t nxt = hle_rd32(t), prv = hle_rd32(t + 4), par = hle_rd32(t + 8), chd = hle_rd32(t + 12), main_ = hle_rd32(t + 16);
        if (par != parent || prv != prev) {
            sa1_log("task-check: frame %u: task 0x%08x (main 0x%08x, depth %d) parent 0x%08x expected 0x%08x (main 0x%08x), prev 0x%08x expected 0x%08x",
                    frame, t, main_, depth, par, parent, parent ? hle_rd32(parent + 16) : 0, prv, prev);
            task_check_bad++;
            return;
        }
        if (chd && depth < 8) task_check_list(chd, t, depth + 1, frame);
        prev = t; t = nxt;
    }
}
static void task_check(uint32_t frame) {
    if (task_check_on < 0) task_check_on = getenv("SA1_TASK_CHECK") != NULL;
    if (!task_check_on || task_check_bad >= 20) return;
    for (int i = 0; i < 8; i++) task_check_list(hle_rd32(0x8C79F04Cu + 4u * (uint32_t)i), 0, 0, frame);
}

static void on_frame(uint32_t frame) {
    sa1_watch_check("frame");
    task_check(frame);
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
        if (quit) { sa1_log("window closed"); report_and_exit(0); }
        if (!scripted) { hle_pad_state = live; b = live.buttons; }
        else { hle_pad_state.x = live.x; hle_pad_state.y = live.y; hle_pad_state.ltrig = live.ltrig; hle_pad_state.rtrig = live.rtrig; }
    }
    if (stick_scripted || headless) { hle_pad_state.x = (int8_t)sx; hle_pad_state.y = (int8_t)sy; }
    b = input_tape_pad(frame, b, scripted);
    if (b != hle_pad_state.buttons) sa1_log("pad: buttons 0x%x", b);
    hle_pad_state.buttons = b;
    for (int i = 0; i < nshots; i++) if (shots[i].frame == frame) kamui_set_screenshot(shots[i].path);
    if (hwwatch.len && hwwatch.frame == frame) plat_hw_watch(hwwatch.addr, hwwatch.len);
    for (int i = 0; i < npokes; i++) if (pokes[i].frame == frame) {       /* debug: write a guest word */
        if (pokes[i].size == 1) hle_wr8(pokes[i].addr, pokes[i].value);
        else if (pokes[i].size == 2) hle_wr16(pokes[i].addr, pokes[i].value);
        else hle_wr32(pokes[i].addr, pokes[i].value);
        sa1_log("poke: [0x%08x] = 0x%x (%u bytes)", pokes[i].addr, pokes[i].value, pokes[i].size);
    }
    for (int i = 0; i < ndumps; i++) if (dumps[i].frame == frame) {      /* debug: guest RAM to a file */
        FILE *o = fopen(dumps[i].path, "wb");
        if (o) { fwrite(sa1_ram, 1, SA1_RAM_SIZE, o); fclose(o); sa1_log("dump: %s", dumps[i].path); }
    }
    input_tape_frame_end(frame);
    if (stats_every && frame % (uint32_t)stats_every == 0) kamui_report();
    if (frame_limit && frame >= frame_limit) {
        sa1_log("frame limit %u reached", frame_limit);
        report_and_exit(0);
    }
    if (time_limit_us && sa1_plat_time_us() - t0 >= time_limit_us) {
        sa1_log("time limit reached");
        report_and_exit(0);
    }
}

static int load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    size_t n = fread(sa1_ram + 0x010000, 1, SA1_RAM_SIZE - 0x010000, f);
    fclose(f);
    if (n < 0x100000) { fprintf(stderr, "%s: too small (%zu bytes)\n", path, n); return -1; }
    sa1_log("loaded %s: %zu bytes at 0x8c010000", path, n);
    return 0;
}

int main(int argc, char **argv) {
    const char *bin = NULL, *root = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frame_limit = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) time_limit_us = (uint64_t)(atof(argv[++i]) * 1e6);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_file = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) { sa1_watch_add(argv[++i]); sa1_watch_addr = 1; }
        else if (!strcmp(argv[i], "--trace-funcs") && i + 1 < argc) trace_path = argv[++i];
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--aspect") && i + 1 < argc) { const char *a = argv[++i]; hle_aspect_wide = strcmp(a, "4:3") != 0 && strcmp(a, "4x3") != 0; }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stats") && i + 1 < argc) stats_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc && npokes < SHOT_MAX) {   /* FRAME:ADDR:VALUE[:SIZE] */
            char *spec = argv[++i], *end;
            pokes[npokes].frame = (uint32_t)strtoul(spec, &end, 0);
            pokes[npokes].addr = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 16) : 0;
            pokes[npokes].value = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 0) : 0;
            pokes[npokes].size = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 0) : 4;
            npokes++;
        }
        else if (!strcmp(argv[i], "--hwwatch") && i + 1 < argc) {   /* FRAME:ADDR[:LEN] */
            char *spec = argv[++i], *end;
            hwwatch.frame = (uint32_t)strtoul(spec, &end, 0);
            hwwatch.addr = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 16) : 0;
            hwwatch.len = *end == ':' ? atoi(end + 1) : 4;
        }
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc && ndumps < SHOT_MAX) {
            char *spec = argv[++i], *end;
            dumps[ndumps].frame = (uint32_t)strtoul(spec, &end, 0);
            dumps[ndumps].path = *end == ':' ? end + 1 : "ram.bin";
            ndumps++;
        }
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc && nshots < SHOT_MAX) {
            char *spec = argv[++i], *end;
            shots[nshots].frame = (uint32_t)strtoul(spec, &end, 0);
            shots[nshots].path = *end == ':' ? end + 1 : "frame.ppm";
            nshots++;
        }
        else if (!strcmp(argv[i], "--stick") && i + 1 < argc && nsticks < PRESS_MAX) {   /* FRAME:X:Y[:HOLD], -128..127 */
            char *spec = argv[++i], *end;
            sticks[nsticks].frame = (uint32_t)strtoul(spec, &end, 0);
            sticks[nsticks].x = *end == ':' ? (int)strtol(end + 1, &end, 0) : 0;
            sticks[nsticks].y = *end == ':' ? (int)strtol(end + 1, &end, 0) : 0;
            sticks[nsticks].hold = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 0) : 2;
            nsticks++;
        }
        else if (!strcmp(argv[i], "--press") && i + 1 < argc && npresses < PRESS_MAX) {
            char *spec = argv[++i], *end;
            presses[npresses].frame = (uint32_t)strtoul(spec, &end, 0);
            presses[npresses].buttons = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 16) : 0;
            presses[npresses].hold = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 0) : 2;
            npresses++;
        }
        else if (input_tape_arg(argc, argv, &i)) continue;
        else if (!bin) bin = argv[i];
        else if (!root) root = argv[i];
        else { fprintf(stderr, "unexpected argument %s\n", argv[i]); return 2; }
    }
    if (!bin || !root) {
        fprintf(stderr, "usage: sa1psp <1ST_READ.BIN> <data root> [--frames N] [--seconds S] [--log FILE]\n");
        return 2;
    }
    t0 = sa1_plat_time_us();
    sa1_set_log_sink(log_sink);
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_crash);
#endif
    sa1_trap_hook = on_trap;
    hle_frame_hook = on_frame;
    sa1_trace_frame = hle_frame_count;
    plat_watchdog_start(hle_frame_count, 10);   /* a guest loop with no frame progress is a bug */

    sa1_ram = calloc(SA1_RAM_ALLOC, 1);
    if (!sa1_ram) { fprintf(stderr, "cannot allocate guest RAM\n"); return 1; }
    if (load_image(bin) != 0) return 1;
    if (input_tape_start(bin) != 0) return 2;
    if (!frame_limit) frame_limit = input_tape_frames();
    plat_set_data_root(root);
    hle_init();
    if (draw_init(scale, headless) != 0 && !headless) { sa1_log("no window; running headless"); headless = 1; }
    draw_set_aspect(hle_aspect_wide);

    memset(&ctx, 0, sizeof ctx);
    ctx.r[15] = 0x8C00F400u;
    ctx.fpscr = 0x00040001u;
    ctx.vbr = 0x8C000000u;
    sa1_log("boot: entering fn_8c010000");
    fn_8c010000(&ctx);
    sa1_log("boot: fn_8c010000 returned (r0=%08x)", ctx.r[0]);
    report_and_exit(0);
    return 0;
}
