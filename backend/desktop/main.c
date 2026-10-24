/* main.c - desktop boot of the recompiled game (M2: graphics, no audio).
 *
 *   sa1psp <1ST_READ.BIN> <data root> [--frames N] [--seconds S] [--log FILE]
 *          [--watch ADDR[:OFF...]]... [--press FRAME:BUTTONS[:HOLD]]... [--trace-funcs FILE]
 *          [--headless] [--scale N] [--screenshot FRAME:FILE.ppm]... [--stats N]
 *          [--stick FRAME:X:Y[:HOLD]]... [--poke FRAME:ADDR:VALUE[:SIZE]]... [--dump FRAME:FILE]...
 *          [--texdump DIR]  (every decoded texture as PPM; F9 in the window, or --mark FRAME, saves a
 *                            screenshot there and logs the textures bound in that frame)
 *          [--aspect wide|4:3] [--no-pad] [--goto FRAME:X:Z[:RADIUS]]...
 *          [--save FRAME:FILE]... [--load FILE] [--quick FILE]
 *          [--wav FILE] [--voice en|ja|game] [--no-audio] [--event-skip on|off|force]
 *
 * Sound (M3, hle/sound/): the audio core renders 735 samples per guest
 * frame whether or not a device is open, so a run behaves the same headless
 * or windowed; the window opens an SDL audio device unless --no-audio.
 * --wav records everything rendered (headless runs too); --voice picks the
 * voice language over sa1psp.ini in the data root (default: the game's
 * choice, English on this disc).
 *
 * --save writes a save state (guest RAM, the HLE's state, the task
 * coroutines' stacks: docs/savestate.md) at guest frame FRAME, where --dump
 * writes; --load restores one before the first frame and continues from it;
 * F5 / F8 in the window save / load --quick (default quick.state).  The
 * game runs on coroutines at fixed addresses under a supervisor
 * (backend/desktop/savestate.c), which is what makes the stacks portable
 * between runs of one build.
 *
 * Without --headless an SDL2 window opens at 480x272 times --scale (2 by
 * default) with an OpenGL 1.x fixed-function context (backend/draw.h), and
 * the controller or keyboard drives the pad whenever no --press is active.
 * --hidden creates that window hidden, so a scripted screenshot run never
 * shows a window or takes the focus.
 * --headless keeps the deterministic M1 behaviour for CI.  --screenshot
 * writes the frame buffer after guest frame FRAME; --stats N logs the
 * renderer counters every N frames.
 *
 * --trace-funcs writes, at exit, every host function that ran with the frame
 * it first ran in (needs a build with -DSA1_TRACE_FUNCS=ON).
 *
 * --no-vmu: no VMU in port A1 (strict runs never have one); --vmu-dir DIR:
 * keep the VMU's files in DIR (default "vmu" beside the executable).
 * --strict, --record FILE, --replay FILE, --check FILE, --expect-frame N
 * and --epoch S: deterministic runs, input tapes and state checks
 * (input_tape.c, tests/replay/README.md).
 *
 * --press scripts the controller: at guest frame FRAME the buttons (hex
 * mask, hle.h: Start 8, A 4, B 2, Up 0x10 ...) are held for HOLD frames
 * (default 2).  This is how the boot test walks the title and menus.
 * --stick holds the analog stick at X,Y (-128..127, y positive = down)
 * for HOLD frames, so a scripted run can walk a stage.  --no-pad ignores
 * the controller and keyboard, so a scripted windowed run (screenshots)
 * is not steered by an idle controller.
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
extern int hle_renderer_cli, hle_renderer_force, hle_land_cull_cli;   /* hle/ninja/native_land.c */
#include "../draw.h"
#include "input_tape.h"
#include "platform_desktop.h"
#include "savestate.h"
#include "../../hle/sound/snd_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fenv.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

void fn_8c010000(sh4_ctx *c);
void plat_set_data_root(const char *root);
void plat_backtrace(void);
void plat_backtrace_ctx(void *context);
void plat_trace_dump(const char *path);
void draw_gl_pace(uint32_t frame);    /* draw_gl.c: fixed 60 Hz pacing of guest frames */
uint32_t hle_task_running(void);      /* hle/game/tasks.c */
void plat_watchdog_start(uint32_t (*frame)(void), unsigned seconds);
void plat_hw_watch(uint32_t guest_addr, int len);
static const char *trace_path;
static int no_audio;                  /* --no-audio: the audio core runs, nothing is played */
static const char *wav_path;          /* --wav FILE: everything the core renders, as a WAV */
extern int audio_sdl_enabled;         /* audio_sdl.c */
extern int draw_gl_hidden;            /* draw_gl.c: --hidden */
int hle_parse_voice(const char *v);   /* hle/settings.c */

static FILE *log_file;
#define PRESS_MAX 64
static struct { uint32_t frame, buttons, hold; } presses[PRESS_MAX];
static int npresses;
static struct { uint32_t frame, hold; int x, y; } sticks[PRESS_MAX];   /* --stick FRAME:X:Y[:HOLD] */
static int nsticks;
static uint32_t frame_limit;
static uint64_t time_limit_us, t0;
static sh4_ctx ctx;
static int headless, scale = 2, stats_every, no_pad;   /* --no-pad: ignore the controller and keyboard (scripted runs) */
int sh4_fmac_fused = 1;                                /* sh4_rt.h FMAC_F; set from the tape at start-up */
static int aspect_given;                               /* --aspect given: it overrides the tape's aspect line */
#define SHOT_MAX 64
static struct { uint32_t frame; const char *path; } shots[SHOT_MAX], dumps[SHOT_MAX];
static int nshots, ndumps;
static struct { uint32_t frame, addr; int len; } hwwatch;   /* --hwwatch FRAME:ADDR[:LEN]: hardware write watch */
static struct { uint32_t frame, addr, value, size; } pokes[SHOT_MAX];
static int npokes;
static struct { uint32_t frame; const char *path; } saves[SHOT_MAX];   /* --save FRAME:FILE */
static int nsaves;
static const char *load_path, *quick_path = "quick.state";
void kamui_report(void);            /* hle/ninja/kamui.c: renderer counters */
void kamui_mark_frame(const char *dir);
void texture_set_dump_dir(const char *dir, unsigned min_w);
int draw_gl_mark_pressed(void);     /* draw_gl.c: F9 */
int draw_gl_quick_key(void);        /* draw_gl.c: F5 = 5, F8 = 8 */
void draw_gl_texture_state(hle_state_io *io);
static const char *texdump_dir;     /* --texdump DIR: every decoded texture, and the F9 marks */
static uint32_t mark_frames[SHOT_MAX]; static int nmarks;   /* --mark FRAME: the F9 mark from a script */
void kamui_set_screenshot(const char *path);

static void log_sink(const char *line) {
    uint64_t t = sa1_plat_time_us() - t0;
    fprintf(log_file ? log_file : stdout, "[%6u.%03u f%-6u] %s\n", (unsigned)(t / 1000000u),
            (unsigned)(t / 1000u % 1000u), hle_frame_count(), line);
    if (log_file) fflush(log_file);
}

/* ---- host stack probe (sa1_depth_hook) ------------------------------------
 * Every indirect call and HLE entry samples how much of the running stack
 * (the main thread's, or a task coroutine's fiber: hle/game/tasks.c) is in
 * use.  The high-water marks are reported at exit, and a sample within
 * STACK_MARGIN of the end traps with the guest call stack before the
 * overflow: the maintainer's second exit-test run died of exception 0xC00000FD at
 * frame 23,274 with no trace, in 0x8C040880's recursion over an object
 * tree. */
#define STACK_MARGIN (160u * 1024u)
static struct { uintptr_t hi; uint32_t size, high, task; } stack_recs[48];
static int stack_nrecs, stack_tripped;
static uint32_t stack_main_high, stack_task_high, stack_task_high_task;
#ifdef _WIN32
static void stack_probe(void) {
    ULONG_PTR lo, hi;
    GetCurrentThreadStackLimits(&lo, &hi);
    char here;
    uint32_t used = (uint32_t)(hi - (uintptr_t)&here), size = (uint32_t)(hi - lo);
    uint32_t task = hle_task_running();
    if (task) { if (used > stack_task_high) { stack_task_high = used; stack_task_high_task = task; } }
    else if (used > stack_main_high) stack_main_high = used;
    if (used + STACK_MARGIN > size && !stack_tripped) {
        stack_tripped = 1;
        sa1_log("stack: %u KB of the %u KB host stack in use on %s%08x: unbounded guest recursion; trapping before the overflow",
                used / 1024, size / 1024, task ? "task 0x" : "the main context 0x", task);
        plat_backtrace();
        sa1_trap(&ctx, 0, "host stack nearly exhausted (guest recursion)");
    }
}
#else
static void stack_probe(void) { }
#endif
static void stack_report(void) {
    sa1_log("stack: high-water main %u KB; tasks %u KB (task 0x%08x) of %u KB each", stack_main_high / 1024,
            stack_task_high / 1024, stack_task_high_task, 1024u);
}

#ifdef _WIN32
/* A host crash (an out-of-range guest access, usually) prints where the
 * recompiled code was before the process dies.  The report runs on its
 * own 256 KB stack (sa1_call_on_stack), so it survives a stack overflow of
 * the faulting context; the backtrace is walked from the exception
 * context. */
static int crash_reported;
void sa1_call_on_stack(void (*fn)(void *), void *arg, void *stack_top);   /* coro_switch.c */
static uint8_t crash_stack[256 * 1024] __attribute__((aligned(16)));
/* The report itself.  It runs on crash_stack (on_crash below), because the
 * faulting stack may have none left: the maintainer's fourth run overflowed the root
 * coroutine's 1 MB into its guard gap, and the handler, running there,
 * faulted again before its first line (docs/m2-render-report.md section 10). */
static void on_crash_body(void *arg) {
    EXCEPTION_POINTERS *ep = arg;
    sa1_log("crash: exception 0x%08lx at %p (guest frame %u)", ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress, hle_frame_count());
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || ep->ExceptionRecord->ExceptionCode == EXCEPTION_GUARD_PAGE
        || ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        for (int i = 0; i < SA1_CORO_SLOTS; i++) {
            uintptr_t lo, hi; sa1_plat_coro_stack(i, &lo, &hi);
            if (a >= lo - SA1_CORO_GUARD && a < lo + SA1_CORO_GUARD_BAND)
                sa1_log("crash: the address is in the guard band of coroutine slot %d's stack [%p, %p): a host stack overflow (%u KB used)%s",
                        i, (void *)lo, (void *)hi, (unsigned)((hi - a) >> 10), i == 0 ? " on the root context" : "");
        }
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        sa1_log("crash: %s of host address %p (sa1_ram is %p, so guest offset 0x%llx)",
                ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                (void *)ep->ExceptionRecord->ExceptionInformation[1], (void *)sa1_ram,
                (unsigned long long)((uint8_t *)ep->ExceptionRecord->ExceptionInformation[1] - sa1_ram));
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        uint32_t task = hle_task_running();
        sa1_log("crash: host stack overflow on %s%08x", task ? "task 0x" : "the main context 0x", task);
    }
    plat_backtrace_ctx(ep->ContextRecord);
    input_tape_finish(4, "crash");
    stack_report();
    if (log_file) fflush(log_file);
}
static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    if (crash_reported) return EXCEPTION_EXECUTE_HANDLER;
    crash_reported = 1;
    sa1_call_on_stack(on_crash_body, ep, crash_stack + sizeof crash_stack);
    return EXCEPTION_EXECUTE_HANDLER;
}
/* The game runs on coroutine stacks of our own (platform_desktop.c), whose
 * bottom frame has no unwind information, so the frame-based dispatch that
 * would reach the unhandled-exception filter stops short on them.  A
 * vectored handler sees every exception first; for the fatal codes it runs
 * the same report and lets the process die.  Single-step (--hwwatch), guard
 * pages and breakpoints pass through untouched. */
static LONG CALLBACK on_crash_vectored(EXCEPTION_POINTERS *ep) {
    switch (ep->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_GUARD_PAGE: {     /* the band at the bottom of a coroutine stack (platform_desktop.c) */
        uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        int ours = 0;
        for (int i = 0; i < SA1_CORO_SLOTS && !ours; i++) { uintptr_t lo, hi; sa1_plat_coro_stack(i, &lo, &hi); ours = a >= lo && a < lo + SA1_CORO_GUARD_BAND; }
        if (!ours) return EXCEPTION_CONTINUE_SEARCH;
    }   /* fall through */
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        on_crash(ep);
        TerminateProcess(GetCurrentProcess(), (UINT)ep->ExceptionRecord->ExceptionCode);
        return EXCEPTION_CONTINUE_SEARCH;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}
#endif

static void report_and_exit(int code) {
    code = input_tape_finish(code, NULL);
    sa1_log("exit: %u frames, %.3f s", hle_frame_count(), (sa1_plat_time_us() - t0) / 1e6);
    kamui_report();
    snd_host_report();
    stack_report();
    sa1_seam_report(getenv("SA1_REPORT_TOP") ? atoi(getenv("SA1_REPORT_TOP")) : 30);
    draw_shutdown();
    if (trace_path) plat_trace_dump(trace_path);
    if (log_file) fclose(log_file);
    exit(code);
}

/* SA1_RAM_CHECK (sh4_rt.h): a guest access outside RAM; the backtrace names
 * the recompiled functions (fn_8c0xxxxx) that made it. */
static void on_badaddr(uint32_t addr) { (void)addr; plat_backtrace(); }

static void on_trap(sh4_ctx *c, uint32_t pc, const char *what) {
    (void)c; (void)pc; (void)what;
    input_tape_finish(3, "trap");
    plat_backtrace();
    sa1_log("exit: trap after %u frames, %.3f s", hle_frame_count(), (sa1_plat_time_us() - t0) / 1e6);
    stack_report();
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

/* --goto FRAME:X:Z[:RADIUS] (debug aid): a waypoint autopilot.  From FRAME
 * on, the stick steers the player toward (X, Z) in world space (position
 * 0x8C6BAD10/18, tests/replay/watch.txt); within RADIUS (default 40) the
 * next --goto takes over.  The stick is camera-relative, so the mapping
 * from stick to world is calibrated from the movement it produces (the
 * first window pushes straight up) and re-estimated every 15 frames; no
 * progress for 90 frames means a wall: the heading is swung 70 degrees to
 * alternate sides and A is tapped (a jump) every 30 frames.  This is how
 * the Emerald Coast pier (the second exit-test crash) is reached headlessly:
 * docs/m2-render-report.md section 9. */
#define GOTO_MAX 32
#define PLAYER_POS 0x8C6BAD10u
static struct { uint32_t frame; float x, z, r; } gotos[GOTO_MAX];
static int ngotos, goto_cur, goto_cal, goto_nudge;
static float goto_theta, goto_best = 1e30f, goto_px0, goto_pz0, goto_ux, goto_uz;
static uint32_t goto_progress_frame;
static int goto_step(uint32_t frame, int *sx, int *sy, uint32_t *buttons) {
    if (goto_cur >= ngotos || frame < gotos[goto_cur].frame) return 0;
    float px = hle_rdf(PLAYER_POS), py = hle_rdf(PLAYER_POS + 4), pz = hle_rdf(PLAYER_POS + 8);
    float dx = gotos[goto_cur].x - px, dz = gotos[goto_cur].z - pz, dist = sqrtf(dx * dx + dz * dz);
    if (dist <= gotos[goto_cur].r) {
        sa1_log("goto: waypoint %d (%.0f, %.0f) reached at frame %u; player (%.0f, %.0f, %.0f)", goto_cur, gotos[goto_cur].x, gotos[goto_cur].z, frame, px, py, pz);
        goto_cur++; goto_best = 1e30f; goto_progress_frame = frame;
        *sx = *sy = 0;
        return 1;
    }
    if (dist < goto_best - 1.0f) { goto_best = dist; goto_progress_frame = frame; }
    if (frame % 15 == 0) {                        /* calibrate stick -> world from the movement */
        float mx = px - goto_px0, mz = pz - goto_pz0, ml = sqrtf(mx * mx + mz * mz);
        if (ml > 6.0f && (goto_ux != 0.0f || goto_uz != 0.0f)) {
            float th = atan2f(mz, mx) - atan2f(goto_uz, goto_ux), d = th - goto_theta;
            while (d > 3.14159f) d -= 6.28318f;
            while (d < -3.14159f) d += 6.28318f;
            goto_theta = goto_cal ? goto_theta + 0.5f * d : th;
            goto_cal = 1;
        }
        goto_px0 = px; goto_pz0 = pz;
    }
    float wx = dx / dist, wz = dz / dist;         /* wanted world heading */
    uint32_t stuck = frame - goto_progress_frame;
    if (stuck > 90) {                             /* a wall: swing the heading, jump */
        float ang = (goto_nudge & 1) ? 1.22f : -1.22f, ca = cosf(ang), sa = sinf(ang);
        float tx = wx * ca - wz * sa, tz = wx * sa + wz * ca; wx = tx; wz = tz;
        if (stuck % 30 == 0) *buttons |= 4;
        if (stuck % 150 == 0) goto_nudge++;
    }
    float ux, uz;                                 /* stick as a vector: x right, "up" = -y */
    if (!goto_cal) { ux = 0.0f; uz = 1.0f; }
    else { float c = cosf(goto_theta), s = sinf(goto_theta); ux = wx * c + wz * s; uz = -wx * s + wz * c; }
    if (frame % 15 == 0) { goto_ux = ux; goto_uz = uz; }
    *sx = (int)(ux * 127.0f); *sy = (int)(-uz * 127.0f);
    if (frame % 300 == 0)
        sa1_log("goto: frame %u player (%.0f, %.0f, %.0f) -> waypoint %d (%.0f, %.0f) dist %.0f, theta %.2f, stick %d,%d%s",
                frame, px, py, pz, goto_cur, gotos[goto_cur].x, gotos[goto_cur].z, dist, goto_theta, *sx, *sy, stuck > 90 ? " (stuck)" : "");
    return 1;
}

static void on_frame(uint32_t frame) {
    sa1_watch_check("frame");
    task_check(frame);
    if (!headless) draw_gl_pace(frame);
    uint32_t b = 0;
    int scripted = 0;
    for (int i = 0; i < npresses; i++)
        if (frame >= presses[i].frame && frame < presses[i].frame + presses[i].hold) { b |= presses[i].buttons; scripted = 1; }
    int sx = 0, sy = 0, stick_scripted = 0;
    for (int i = 0; i < nsticks; i++)
        if (frame >= sticks[i].frame && frame < sticks[i].frame + sticks[i].hold) { sx = sticks[i].x; sy = sticks[i].y; stick_scripted = 1; }
    { int gx, gy; if (goto_step(frame, &gx, &gy, &b)) { sx = gx; sy = gy; stick_scripted = 1; scripted = 1; } }
    hle_pad live = {0};
    int quit = 0;
    if (!headless && draw_poll_input(&live, &quit) == 0) {
        if (quit) { sa1_log("window closed"); report_and_exit(0); }
        if (draw_gl_mark_pressed()) kamui_mark_frame(texdump_dir);
        if (no_pad) { live = (hle_pad){0}; }
        if (!scripted) { hle_pad_state = live; b = live.buttons; }
        else { hle_pad_state.x = live.x; hle_pad_state.y = live.y; hle_pad_state.ltrig = live.ltrig; hle_pad_state.rtrig = live.rtrig; }
    }
    for (int i = 0; i < nmarks; i++) if (mark_frames[i] == frame) { kamui_mark_frame(texdump_dir); break; }
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
        if (o) { fwrite(sa1_ram, 1, SA1_RAM_SIZE + SA1_RAM_EXTRA, o); fclose(o); sa1_log("dump: %s (%u bytes, lists included)", dumps[i].path, SA1_RAM_SIZE + SA1_RAM_EXTRA); }
    }
    for (int i = 0; i < nsaves; i++) if (saves[i].frame == frame) savestate_save(saves[i].path);   /* the state at the dump point */
    if (!headless) {
        int k = draw_gl_quick_key();
        if (k == 5) savestate_save(quick_path);
        else if (k == 8 && savestate_load(quick_path) != 0) sa1_log("quick load: %s not loaded", quick_path);
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

/* The game, on the root coroutine (savestate_supervise). */
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

/* Save states: this file's share (docs/savestate.md section 2): the live
 * register file and the --goto autopilot, which steers from past positions. */
static void main_state(hle_state_io *io) {
    hle_state_blob(io, "main.ctx", &ctx, sizeof ctx);
    hle_state_blob(io, "goto.cur", &goto_cur, sizeof goto_cur);
    hle_state_blob(io, "goto.cal", &goto_cal, sizeof goto_cal);
    hle_state_blob(io, "goto.nudge", &goto_nudge, sizeof goto_nudge);
    hle_state_blob(io, "goto.theta", &goto_theta, sizeof goto_theta);
    hle_state_blob(io, "goto.best", &goto_best, sizeof goto_best);
    hle_state_blob(io, "goto.px0", &goto_px0, sizeof goto_px0);
    hle_state_blob(io, "goto.pz0", &goto_pz0, sizeof goto_pz0);
    hle_state_blob(io, "goto.ux", &goto_ux, sizeof goto_ux);
    hle_state_blob(io, "goto.uz", &goto_uz, sizeof goto_uz);
    hle_state_blob(io, "goto.progress", &goto_progress_frame, sizeof goto_progress_frame);
}
static void state_meta_fill(savestate_meta *m) {
    m->frame = hle_frame_count();
    m->flags = (hle_strict ? SAVESTATE_STRICT : 0) | (hle_aspect_wide ? SAVESTATE_WIDE : 0);
    m->rtc_epoch = hle_rtc_epoch;
}
static void state_loaded(const savestate_meta *m) {
    if (!!(m->flags & SAVESTATE_STRICT) != !!hle_strict)
        sa1_log("savestate: the state was saved %s --strict and this run is %s; the runs will differ", (m->flags & SAVESTATE_STRICT) ? "with" : "without", hle_strict ? "strict" : "not");
    if (!!(m->flags & SAVESTATE_WIDE) != !!hle_aspect_wide)
        sa1_log("savestate: the state was saved with the %s aspect; this run draws %s (the game's clip rectangle came from the saving run)", (m->flags & SAVESTATE_WIDE) ? "wide" : "4:3", hle_aspect_wide ? "wide" : "4:3");
    if (m->rtc_epoch != hle_rtc_epoch)
        sa1_log("savestate: the state's clock epoch is %lld, this run's %lld; strict runs will differ", (long long)m->rtc_epoch, (long long)hle_rtc_epoch);
    input_tape_loaded(m->frame);
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

void vmu_store_desktop_init(const char *dir);   /* vmu_store_desktop.c */
const char *vmu_store_desktop_dir(void);
static int vmu_mode;               /* -1 --no-vmu, 1 --vmu-dir, 0 default */
static const char *vmu_dir;

int main(int argc, char **argv) {
    const char *bin = NULL, *root = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frame_limit = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) time_limit_us = (uint64_t)(atof(argv[++i]) * 1e6);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_file = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) { sa1_watch_add(argv[++i]); sa1_watch_addr = 1; }
        else if (!strcmp(argv[i], "--trace-funcs") && i + 1 < argc) trace_path = argv[++i];
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--hidden")) draw_gl_hidden = 1;
        else if (!strcmp(argv[i], "--no-pad")) no_pad = 1;
        else if (!strcmp(argv[i], "--no-vmu")) vmu_mode = -1;
        else if (!strcmp(argv[i], "--vmu-dir") && i + 1 < argc) { vmu_dir = argv[++i]; vmu_mode = 1; }
        else if (!strcmp(argv[i], "--no-audio")) no_audio = 1;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--voice") && i + 1 < argc) {
            int v = hle_parse_voice(argv[++i]);
            if (v < -1) { fprintf(stderr, "--voice: en, ja or game\n"); return 2; }
            snd_voice_lang_cli = v;
        }
        else if (!strcmp(argv[i], "--land-cull") && i + 1 < argc) hle_land_cull_cli = strcmp(argv[++i], "off") != 0;   /* hle/ninja/native_land.c */
        else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) {   /* hle/ninja/native_land.c */
            const char *v = argv[++i];
            if (!strncmp(v, "force:", 6)) { hle_renderer_force = 1; v += 6; }
            if (!strcmp(v, "native")) hle_renderer_cli = 1; else if (!strcmp(v, "ta")) hle_renderer_cli = 0;
            else if (!strcmp(v, "noland")) hle_renderer_cli = 2;   /* measurement: the land not drawn */
            else if (!strcmp(v, "native-sets")) hle_renderer_cli = 4;   /* native without persistent models (a check) */
            else { fprintf(stderr, "--renderer: ta, native or force:native\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--frame-skip") && i + 1 < argc) {   /* hle/game/frame_skip.c: auto|off|N|force:... */
            int m = hle_parse_frame_skip(argv[++i], &hle_frame_skip_force);
            if (m == -3) { fprintf(stderr, "--frame-skip: auto, off, 2-60, or force:...\n"); return 2; }
            hle_frame_skip_cli = m;
        }
        else if (!strcmp(argv[i], "--event-skip") && i + 1 < argc) {   /* hle/game/event_skip.c */
            const char *v = argv[++i];
            hle_event_skip_cli = !strcmp(v, "force") ? 2 : strcmp(v, "off") != 0;
        }
        else if (!strcmp(argv[i], "--texdump") && i + 1 < argc) { texdump_dir = argv[++i]; texture_set_dump_dir(texdump_dir, 0); }
        else if (!strcmp(argv[i], "--mark") && i + 1 < argc && nmarks < SHOT_MAX) mark_frames[nmarks++] = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ta-direct")) hle_ta_direct = 1;   /* docs/ta-direct.md */
        else if (!strcmp(argv[i], "--no-ta-direct")) hle_ta_direct = 0;
        else if (!strcmp(argv[i], "--aspect") && i + 1 < argc) { const char *a = argv[++i]; hle_aspect_wide = strcmp(a, "4:3") != 0 && strcmp(a, "4x3") != 0; aspect_given = 1; }
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
        else if (!strcmp(argv[i], "--save") && i + 1 < argc && nsaves < SHOT_MAX) {   /* FRAME:FILE */
            char *spec = argv[++i], *end;
            saves[nsaves].frame = (uint32_t)strtoul(spec, &end, 0);
            saves[nsaves].path = *end == ':' ? end + 1 : "frame.state";
            nsaves++;
        }
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) load_path = argv[++i];
        else if (!strcmp(argv[i], "--quick") && i + 1 < argc) quick_path = argv[++i];
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
        else if (!strcmp(argv[i], "--goto") && i + 1 < argc && ngotos < GOTO_MAX) {   /* FRAME:X:Z[:RADIUS] */
            char *spec = argv[++i], *end;
            gotos[ngotos].frame = (uint32_t)strtoul(spec, &end, 0);
            gotos[ngotos].x = *end == ':' ? strtof(end + 1, &end) : 0.0f;
            gotos[ngotos].z = *end == ':' ? strtof(end + 1, &end) : 0.0f;
            gotos[ngotos].r = *end == ':' ? strtof(end + 1, &end) : 40.0f;
            ngotos++;
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
    AddVectoredExceptionHandler(0, on_crash_vectored);
    { ULONG guarantee = 64 * 1024; SetThreadStackGuarantee(&guarantee); }
#endif
    sa1_depth_hook = stack_probe;
    sa1_trap_hook = on_trap;
    sa1_badaddr_hook = on_badaddr;
    hle_frame_hook = on_frame;
    sa1_trace_frame = hle_frame_count;
    plat_watchdog_start(hle_frame_count, 10);   /* a guest loop with no frame progress is a bug */

    if (sa1_plat_ram_alloc() != 0) { fprintf(stderr, "cannot map guest RAM at its fixed address (docs/savestate.md section 1)\n"); return 1; }
    if (load_image(bin) != 0) return 1;
    if (input_tape_start(bin) != 0) return 2;
    if (load_path && input_tape_recording()) { fprintf(stderr, "--record cannot start from a loaded state (a tape starts at frame 1)\n"); return 2; }
    if (!frame_limit) frame_limit = input_tape_frames();
    if (!aspect_given && input_tape_aspect_wide() >= 0) hle_aspect_wide = input_tape_aspect_wide();
    hle_pad_round_gate = input_tape_round_gate();
    if (hle_pad_round_gate) sa1_log("pad: round stick gate, as the controller");
    hle_tmu_slow_frames = input_tape_slow_frames(&hle_tmu_slow_count);
    for (int i = 0; i < hle_tmu_slow_count; i++) sa1_log("tmu: frame %u runs two vblanks late (slow-substep)", hle_tmu_slow_frames[i]);
    plat_set_data_root(root);
    if (headless || no_audio) audio_sdl_enabled = 0;
    if (input_tape_sound_timing() >= 0) snd_guest_timing = input_tape_sound_timing();
    if (wav_path) snd_host_set_wav(wav_path);
    /* VMU in port A1 (hle/shinobi/vmu_store.h): on unless --no-vmu or a
     * strict run (tapes, the oracle), which keep "no VMU"; --vmu-dir forces it. */
    if (vmu_mode > 0 || (vmu_mode == 0 && !hle_strict)) {
        vmu_store_desktop_init(vmu_dir);
        sa1_log("VMU A1: %s", vmu_store_desktop_dir());
    }
    hle_init();
    if (draw_init(scale, headless) != 0 && !headless) { sa1_log("no window; running headless"); headless = 1; }
    draw_set_aspect(hle_aspect_wide);

    savestate_add("hle", hle_state_all);
    savestate_add("main", main_state);
    savestate_add("textures", draw_gl_texture_state);
    savestate_set_meta(state_meta_fill, state_loaded);
    /* The console runs with FPSCR.RM = 1 (round toward zero, the Katana
     * reset value 0x00040001); the recompiled code does its float work on
     * the host FPU, so the host rounds the same way.  Every coroutine runs
     * on this OS thread and the switch does not touch MXCSR, so this one
     * call covers them all.  Tapes recorded before this replay with
     * round-to-nearest (input_tape.c, fpu-round). */
    int fpu_zero = input_tape_fpu_zero() != 0;
    fesetround(fpu_zero ? FE_TOWARDZERO : FE_TONEAREST);
    sa1_log("fpu: host rounds %s", fpu_zero ? "toward zero, as the console" : "to nearest (a tape recorded before round-toward-zero)");
    sh4_fmac_fused = input_tape_fmac_fused();
    sa1_log("fpu: fmac %s", sh4_fmac_fused ? "fused, as the console" : "rounded twice (a tape recorded before e8a141a)");
    if (savestate_supervise(boot, NULL, load_path) != 0) { report_and_exit(1); return 1; }
    report_and_exit(0);
    return 0;
}
