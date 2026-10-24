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
 *   --no-ta-direct  the recompiled TA strip writers instead of the native ones (docs/ta-direct.md)
 *   --gu-split Z    the depth passes' split (draw_gu.c, default 16; 0: one pass)
 *   --gu-debug N    renderer bisection switches (draw_gu.c)
 *   --no-ge-pipeline  wait for the GE at the end of each frame (default: it
 *                   draws frame N while the game runs N+1; draw_gu.c)
 *   --ge-stall-every N  move the GE's stall address every N draws (default
 *                   8; 0: after each, through sceGuDrawArray)
 *   --no-crash-screen  do not load sa1exc.prx: under psplink, whose own
 *                   exception handler keeps the console on and the USB link
 *                   up (ours would replace it and stop at a red screen)
 *   --data DIR      the data root (default data)
 *   --stage NAME    straight into a level at frame 12: ss (Station Square),
 *                   ec (Emerald Coast 1), chaos0, or LEVEL[:ACT[:MODE]]
 *   --poke FRAME:ADDR:VALUE[:SIZE]  write a guest word (as the desktop)
 *   --save FRAME:FILE, --load FILE  save states (savestate_psp.c): ~30 MB,
 *                   only for the build and memory layout that saved them
 *   --prof FROM:TO[:FILE]  sample the CPU's PC every ~1 ms from guest frame
 *                   FROM to TO (prof/sa1prof.c, a kernel module through
 *                   kubridge) into FILE (default prof_pc.bin);
 *                   tools/psp_prof.py --pcs names the samples
 *   --prof-us N     the sampling period (default 1000 us)
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
#include "../desktop/savestate.h"
#include "state.h"
#include "platform_psp.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspsysmem.h>
#include <psppower.h>
#include <kubridge.h>

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
void draw_gu_set_debug(int flags);
void draw_gu_set_split(float z);
void draw_gu_set_lod_bias(float b);
void draw_gu_set_max_mip(int n);
void draw_gu_set_tex_pool(int on, int reserve_kb);
void vmu_store_psp_init(const char *dir);   /* vmu_store_psp.c */
const char *vmu_store_psp_dir(void);
static int vmu_mode;                        /* -1 --no-vmu, 1 --vmu-dir, 0 default */
static const char *vmu_dir_arg;
void draw_gu_set_ge_pipeline(int on);
void draw_gu_set_stall_every(int n);
uint32_t hle_task_running(void);

static char base_dir[256] = "ms0:/PSP/GAME/SA1PSP";
static const char *data_dir = "data", *log_path = "sa1psp.log";
static int headless, no_pad, no_pace, to_stdout, sync_log, stats_every, coro_heap, no_crash_screen;
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
static int aspect_given;                               /* --aspect given: it overrides the tape's aspect line */
static uint32_t mem_frames[16];
static int nmem;
static struct { uint32_t frame, addr, value, size; } pokes[PRESS_MAX];   /* --poke, --stage */
static int npokes;
static struct { uint32_t frame; const char *path; } saves[16];   /* --save FRAME:FILE */
static int nsaves;
static const char *load_path;
/* --prof: the buffer prof/sa1prof.c's alarm handler fills (same layout). */
struct prof_buf { volatile uint32_t magic, run, count, cap, period_us, missed, alarm, reserved; uint32_t pc[]; };
static struct prof_buf *prof;
static uint32_t prof_from, prof_to, prof_us = 1000;
static const char *prof_path;
#define PROF_CAP (96u * 1024u)

/* --stage: straight into a level, past the title and menus (a test
 * checkpoint: seconds instead of the tape's minutes on the console).  At
 * guest frame 12, just after the Sega logo, the current and next level and
 * act are set and the game mode switched; the game's own mode change then
 * loads the level as from the menus, with Sonic.  The variables are the M2
 * report's (section 2): level 0x8C7493BA, act 0x8C7493BC, next level/act
 * 0x8C7493BE/C0, game mode 0x8C760970 (5 adventure field, 4 action stage). */
static int stage_arg(const char *spec) {
    static const struct { const char *name; uint32_t level, act, mode; } named[] = {
        { "ss", 26, 3, 5 },        /* Station Square, by day (the tape's first visit) */
        { "ec", 1, 0, 4 },         /* Emerald Coast act 1 */
        { "chaos0", 15, 0, 4 },
    };
    uint32_t level = 0, act = 0, mode = 5;
    int found = 0;
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++)
        if (!strcmp(spec, named[i].name)) { level = named[i].level; act = named[i].act; mode = named[i].mode; found = 1; }
    if (!found) {
        char *end;
        level = (uint32_t)strtoul(spec, &end, 0);
        if (end == spec) return -1;
        if (*end == ':') act = (uint32_t)strtoul(end + 1, &end, 0);
        if (*end == ':') mode = (uint32_t)strtoul(end + 1, &end, 0);
    }
    static const uint32_t addr[5] = { 0x8C7493BAu, 0x8C7493BCu, 0x8C7493BEu, 0x8C7493C0u, 0x8C760970u };
    uint32_t val[5] = { level, act, level, act, mode };
    for (int i = 0; i < 5 && npokes < PRESS_MAX; i++) {
        pokes[npokes].frame = 12; pokes[npokes].addr = addr[i]; pokes[npokes].value = val[i]; pokes[npokes].size = 2;
        npokes++;
    }
    return 0;
}

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
/* Runs on every indirect call and seam hit (2 % of the console's frame with
 * the slot lookup), so the current coroutine's limit is kept between calls. */
static const sa1_coro *probe_co;
static uintptr_t probe_limit;
static void stack_probe(void) {
    const sa1_coro *co = sa1_plat_coro_current();
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    if (co == probe_co && sp >= probe_limit) return;
    int i = sa1_plat_coro_index(co);
    if (i < 0 || stack_tripped) return;
    uintptr_t lo, hi;
    sa1_plat_coro_stack(i, &lo, &hi);
    probe_co = co; probe_limit = lo + SA1_CORO_CANARY + 4096u;
    if (sp >= probe_limit) return;
    stack_tripped = 1;
    uint32_t task = hle_task_running();
    sa1_log("stack: %u of %u bytes in use on %s%08x (slot %d): trapping before the overflow",
            (unsigned)(hi - sp), (unsigned)(hi - lo), task ? "task 0x" : "the root context 0x", task, i);
    plat_backtrace();
    sa1_trap(&ctx, 0, "host stack nearly exhausted");
}

/* --prof: its alarm ends at its next tick once run == 2 (the exit reset
 * reboots the kernel anyway). */
static void prof_stop(void) {
    if (!prof || prof->run == 2) return;
    prof->run = 2;
    sceKernelDelayThread(3 * prof_us);
}

/* The exit line last: psp_run.py takes it as the end of the run, so the
 * reports go before it, then the log is closed and the GE stopped. */
static void report_and_exit(int code) {
    code = input_tape_finish(code, NULL);
    prof_stop();
    uint64_t t_end = sa1_plat_time_us();
    kamui_report();
    draw_gu_report();
    mem_report("exit");
    sa1_seam_report(20);
    sa1_log("exit: %u frames, %.3f s, code %d", hle_frame_count(), (t_end - t0) / 1e6, code);
    plat_log_close();
    plat_files_close_all();
    draw_shutdown();
    sceKernelExitGame();
    exit(code);
}

static void on_trap(sh4_ctx *c, uint32_t pc, const char *what) {
    (void)c; (void)pc; (void)what;
    input_tape_finish(3, "trap");
    prof_stop();
    plat_backtrace();
    uint64_t t_end = sa1_plat_time_us();
    mem_report("trap");
    sa1_seam_report(20);
    sa1_log("exit: trap after %u frames, %.3f s", hle_frame_count(), (t_end - t0) / 1e6);
    plat_log_close();
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

/* --prof: load the sampler with the buffer's address as its argument; it
 * records while run == 1 and stops its alarm when run == 2. */
static const char *abs_path(char *out, unsigned n, const char *p);
static void prof_start(void) {
    char path[300];
    prof = malloc(sizeof *prof + PROF_CAP * 4u);
    if (!prof) { sa1_log("prof: no memory for %u samples", PROF_CAP); prof_path = NULL; return; }
    memset(prof, 0, sizeof *prof);
    prof->magic = 0x464F5250u;
    prof->cap = PROF_CAP;
    prof->period_us = prof_us;
    SceUID m = kuKernelLoadModule(abs_path(path, sizeof path, "sa1prof.prx"), 0, NULL);
    struct prof_buf *arg = prof;
    int st = 0, r = m >= 0 ? sceKernelStartModule(m, sizeof arg, &arg, &st, NULL) : m;
    sa1_log("prof: sa1prof.prx load 0x%08x, start 0x%08x, alarm 0x%08x; frames %u-%u every ~%u us -> %s",
            (unsigned)m, (unsigned)r, (unsigned)prof->alarm, prof_from, prof_to, prof_us, prof_path);
    if (m < 0 || r < 0 || (int)prof->alarm <= 0) prof_path = NULL;
}

static void prof_frame(uint32_t frame) {
    if (frame >= prof_from && !prof->run) { prof->run = 1; sa1_log("prof: sampling from frame %u", frame); }
    if (frame < prof_to) return;
    prof->run = 2;
    char path[300];
    uint32_t n = prof->count;
    FILE *f = fopen(abs_path(path, sizeof path, prof_path), "wb");
    if (f) {
        uint32_t hdr[4] = { 0x464F5250u, n, prof->period_us, prof->missed };
        fwrite(hdr, 4, 4, f);
        fwrite(prof->pc, 4, n, f);
        fclose(f);
    }
    sa1_log("prof: %u samples (%u missed) over frames %u-%u -> %s%s", n, (unsigned)prof->missed,
            prof_from, prof_to, prof_path, f ? "" : " (cannot write)");
    prof_path = NULL;
}

static void on_frame(uint32_t frame) {
    if (!headless && !no_pace) pace(frame);
    milestones(frame);
    if (prof_path) prof_frame(frame);
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
    for (int i = 0; i < npokes; i++) if (pokes[i].frame == frame) {       /* as the desktop's --poke */
        if (pokes[i].size == 1) hle_wr8(pokes[i].addr, pokes[i].value);
        else if (pokes[i].size == 2) hle_wr16(pokes[i].addr, pokes[i].value);
        else hle_wr32(pokes[i].addr, pokes[i].value);
        sa1_log("poke: [0x%08x] = 0x%x (%u bytes)", pokes[i].addr, pokes[i].value, pokes[i].size);
    }
    for (int i = 0; i < nmem; i++) if (mem_frames[i] == frame) { char w[32]; snprintf(w, sizeof w, "frame %u", frame); mem_report(w); }
    input_tape_frame_end(frame);
    for (int i = 0; i < nsaves; i++) if (saves[i].frame == frame) savestate_save(saves[i].path);
    if (stats_every && frame % (uint32_t)stats_every == 0) { char w[32]; snprintf(w, sizeof w, "frame %u", frame); kamui_report(); draw_gu_report(); plat_io_report(); mem_report(w); }
    if (frame == 300 && !sync_log) { sa1_log("log: buffered from here (--sync-log keeps writing through)"); plat_log_sync(0); }
    if (frame % 120 == 0) plat_log_flush();
    if (frame_limit && frame >= frame_limit) { sa1_log("frame limit %u reached", frame_limit); report_and_exit(0); }
    if (time_limit_us && sa1_plat_time_us() - t0 >= time_limit_us) { sa1_log("time limit reached"); report_and_exit(0); }
}

/* Save states (savestate_psp.c, --save/--load): this file's share is the
 * register file; the header carries the frame and the run's settings. */
void draw_gu_texture_state(hle_state_io *io);
static void main_state(hle_state_io *io) { hle_state_blob(io, "main.ctx", &ctx, sizeof ctx); }
static void state_meta_fill(savestate_meta *m) {
    m->frame = hle_frame_count();
    m->flags = (hle_strict ? SAVESTATE_STRICT : 0) | (hle_aspect_wide ? SAVESTATE_WIDE : 0);
    m->rtc_epoch = hle_rtc_epoch;
}
static void state_loaded(const savestate_meta *m) {
    if (!!(m->flags & SAVESTATE_STRICT) != !!hle_strict)
        sa1_log("savestate: the state was saved %s --strict and this run is %s; the runs will differ", (m->flags & SAVESTATE_STRICT) ? "with" : "without", hle_strict ? "strict" : "not");
    input_tape_loaded(m->frame);
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
    prof_stop();
    sa1_log("exit: HOME at frame %u", hle_frame_count());
    plat_log_close();
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
    if (argc > 0 && argv[0]) savestate_set_image(argv[0]);
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
        else if (!strcmp(av[i], "--no-crash-screen")) no_crash_screen = 1;
        else if (!strcmp(av[i], "--cull-flip")) draw_gu_set_cull_flip(1);
        else if (!strcmp(av[i], "--ta-direct")) hle_ta_direct = 1;
        else if (!strcmp(av[i], "--no-ta-direct")) hle_ta_direct = 0;
        else if (!strcmp(av[i], "--event-skip") && i + 1 < args_c) {   /* hle/game/event_skip.c */
            const char *v = av[++i];
            hle_event_skip_cli = !strcmp(v, "force") ? 2 : strcmp(v, "off") != 0;
        }
        else if (!strcmp(av[i], "--gu-debug") && i + 1 < args_c) draw_gu_set_debug(atoi(av[++i]));
        else if (!strcmp(av[i], "--gu-split") && i + 1 < args_c) draw_gu_set_split((float)atof(av[++i]));
        else if (!strcmp(av[i], "--gu-lod-bias") && i + 1 < args_c) draw_gu_set_lod_bias((float)atof(av[++i]));
        else if (!strcmp(av[i], "--gu-max-mip") && i + 1 < args_c) draw_gu_set_max_mip(atoi(av[++i]));
        else if (!strcmp(av[i], "--no-vmu")) vmu_mode = -1;
        else if (!strcmp(av[i], "--vmu-dir") && i + 1 < args_c) { vmu_dir_arg = av[++i]; vmu_mode = 1; }
        else if (!strcmp(av[i], "--tex-pool") && i + 1 < args_c) draw_gu_set_tex_pool(strcmp(av[++i], "off") != 0, -1);   /* draw_gu.c: texture chunks */
        else if (!strcmp(av[i], "--tex-reserve") && i + 1 < args_c) draw_gu_set_tex_pool(1, atoi(av[++i]));       /* KB of heap the pool leaves free */
        else if (!strcmp(av[i], "--env") && i + 1 < args_c) {   /* NAME=VALUE, e.g. SA1_COLL_REC=1: a same-binary A/B */
            char *eq = strchr(av[++i], '=');
            if (eq) { *eq = 0; setenv(av[i], eq + 1, 1); *eq = '='; }
        }
        else if (!strcmp(av[i], "--no-ge-pipeline")) draw_gu_set_ge_pipeline(0);
        else if (!strcmp(av[i], "--ge-stall-every") && i + 1 < args_c) draw_gu_set_stall_every(atoi(av[++i]));
        else if (!strcmp(av[i], "--stats") && i + 1 < args_c) stats_every = atoi(av[++i]);
        else if (!strcmp(av[i], "--mem") && i + 1 < args_c && nmem < 16) mem_frames[nmem++] = (uint32_t)strtoul(av[++i], NULL, 0);
        else if (!strcmp(av[i], "--aspect") && i + 1 < args_c) { const char *a = av[++i]; hle_aspect_wide = strcmp(a, "4:3") != 0 && strcmp(a, "4x3") != 0; aspect_given = 1; }
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
        } else if (!strcmp(av[i], "--poke") && i + 1 < args_c && npokes < PRESS_MAX) {   /* FRAME:ADDR:VALUE[:SIZE] */
            char *spec = av[++i], *end;
            pokes[npokes].frame = (uint32_t)strtoul(spec, &end, 0);
            pokes[npokes].addr = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 16) : 0;
            pokes[npokes].value = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 0) : 0;
            pokes[npokes].size = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 0) : 4;
            npokes++;
        } else if (!strcmp(av[i], "--save") && i + 1 < args_c && nsaves < 16) {   /* FRAME:FILE */
            char *spec = av[++i], *end;
            saves[nsaves].frame = (uint32_t)strtoul(spec, &end, 0);
            saves[nsaves].path = *end == ':' ? end + 1 : "frame.state";
            nsaves++;
        } else if (!strcmp(av[i], "--load") && i + 1 < args_c) load_path = av[++i];
        else if (!strcmp(av[i], "--prof") && i + 1 < args_c) {        /* FROM:TO[:FILE] */
            char *end;
            prof_from = (uint32_t)strtoul(av[++i], &end, 0);
            prof_to = *end == ':' ? (uint32_t)strtoul(end + 1, &end, 0) : prof_from + 300;
            prof_path = *end == ':' ? end + 1 : "prof_pc.bin";
        } else if (!strcmp(av[i], "--prof-us") && i + 1 < args_c) prof_us = (uint32_t)strtoul(av[++i], NULL, 0);
        else if (!strcmp(av[i], "--stage") && i + 1 < args_c) {
            if (stage_arg(av[++i]) != 0) { /* logged below */ }
        } else if (input_tape_arg(args_c, av, &i)) continue;
        else { /* logged once the log is open */ }
    }
    freopen(abs_path(path, sizeof path, "stderr.txt"), "w", stderr);   /* input_tape.c reports start-up errors there */
    plat_log_open(abs_path(path, sizeof path, log_path), to_stdout);
    t0 = sa1_plat_time_us();
    sa1_log("sa1-psp (PSP): base %s, data %s, %d args from args.txt%s", base_dir, data_dir, args_c, headless ? ", headless" : "");
    /* The PRX build (PSPLink) loads anywhere: tools/psp_run.py names crash
     * addresses from this line and the ELF's own address of main. */
    sa1_log("image: main at %p", (void *)main);
    for (int i = 0; i < args_c; i++) sa1_log("  arg %s", args_v[i]);
    if (!coro_heap) sa1_plat_coro_set_region(coro_block);
    sa1_log("step: coroutine self-test (%s)", coro_heap ? "stacks on the heap" : "stacks on the main thread's stack");
    coro_self_test();
    /* The crash screen (backend/psp/exc/sa1exc.c), a kernel module: only
     * kubridge (ARK, PRO) can load it from a user EBOOT. */
    if (no_crash_screen) sa1_log("crash screen: not loaded (--no-crash-screen; under psplink its own handler reports exceptions)");
    else {
        SceUID m = kuKernelLoadModule(abs_path(path, sizeof path, "sa1exc.prx"), 0, NULL);
        int st = 0, r = m >= 0 ? sceKernelStartModule(m, 0, NULL, &st, NULL) : m;
        sa1_log("crash screen: sa1exc.prx load 0x%08x, start 0x%08x%s", (unsigned)m, (unsigned)r,
                r >= 0 ? " (exceptions now show their registers)" : " (not installed: an exception powers the console off)");
    }
    if (prof_path) prof_start();
    /* The FPU as the SH-4 runs the game (FPSCR 0x00040001: every exception
     * masked, denormals are zero).  The PSP starts threads with the invalid,
     * divide-by-zero and overflow traps enabled (FCSR 0x00000E00): a float
     * 0/0 in the recompiled code then traps and the console powers off,
     * which PPSSPP never shows (it raises no FPU exceptions).  So: enables
     * (bits 7-11) and causes (12-17) cleared, FS (24, flush to zero) set.
     * The rounding (RM, bits 0-1) is set once the tapes are read, below. */
    {
        uint32_t fcsr, now;
        __asm__ volatile("cfc1 %0, $31" : "=r"(fcsr));
        uint32_t want = (fcsr & ~0x0003FF83u) | (1u << 24);
        __asm__ volatile("ctc1 %0, $31" : : "r"(want));
        __asm__ volatile("cfc1 %0, $31" : "=r"(now));
        sa1_log("fpu: FCSR 0x%08x -> 0x%08x (exceptions masked, flush to zero)", fcsr, now);
    }
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
    uint8_t *ram = memalign(64, SA1_RAM_ALLOC);
    if (!ram) { sa1_log("cannot allocate %u KB of guest RAM", SA1_RAM_ALLOC / 1024u); plat_log_flush(); sceKernelExitGame(); return 1; }
    memset(ram, 0, SA1_RAM_ALLOC);
    sa1_ram_set(ram);   /* once, before any guest code (sh4_rt.h) */
    sa1_log("guest RAM: %u KB at %p (16 MB + the 2 MB list area + the 2 MB null page)", SA1_RAM_ALLOC / 1024u, (void *)sa1_ram);
    char root[300], bin[320];
    abs_path(root, sizeof root, data_dir);
    snprintf(bin, sizeof bin, "%s/1ST_READ.BIN", root);
    sa1_log("step: load %s", bin);
    if (load_image(bin) != 0) { plat_log_flush(); sceKernelExitGame(); return 1; }
    sa1_log("step: tapes");
    if (input_tape_start(bin) != 0) { sa1_log("tape: start-up failed (stderr.txt has why)"); plat_log_flush(); sceKernelExitGame(); return 2; }
    if (!frame_limit) frame_limit = input_tape_frames();
    if (!aspect_given && input_tape_aspect_wide() >= 0) hle_aspect_wide = input_tape_aspect_wide();
    /* The rounding as the desktop's (backend/desktop/main.c): the console
     * runs with FPSCR.RM = 1, round toward zero (the Katana reset value
     * 0x00040001), and the recompiled code computes on this FPU, so FCSR.RM
     * = 1 (0x01000001) unless a tape recorded before 2026-10-19 (no
     * fpu-round line) or --fpu-round nearest asks for round-to-nearest.
     * FCSR is per thread: this is the game's thread, every coroutine runs on
     * it and coro_psp.c's switch does not touch FCSR, so this covers them
     * all.  (Float-to-int casts are trunc.w.s, which ignores RM, as the
     * SH-4's ftrc truncates.) */
    {
        int fpu_zero = input_tape_fpu_zero() != 0;
        uint32_t fcsr, now;
        __asm__ volatile("cfc1 %0, $31" : "=r"(fcsr));
        uint32_t want = (fcsr & ~3u) | (fpu_zero ? 1u : 0u);
        __asm__ volatile("ctc1 %0, $31" : : "r"(want));
        __asm__ volatile("cfc1 %0, $31" : "=r"(now));
        sa1_log("fpu: FCSR 0x%08x, rounds %s", now, fpu_zero ? "toward zero, as the console" : "to nearest (a tape recorded before round-toward-zero)");
    }
    plat_set_data_root(root);
    /* VMU in port A1 (hle/shinobi/vmu_store.h): its files in "vmu" beside the
     * EBOOT, unless --no-vmu or a strict run (tapes keep "no VMU"). */
    if (vmu_mode > 0 || (vmu_mode == 0 && !hle_strict)) {
        char vd[300];
        if (vmu_dir_arg) snprintf(vd, sizeof vd, "%s", vmu_dir_arg);
        else abs_path(vd, sizeof vd, "vmu");
        vmu_store_psp_init(vd);
        sa1_log("VMU A1: %s", vmu_store_psp_dir());
    }
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
    sa1_log("step: root coroutine%s%s", load_path ? ", from the state " : "", load_path ? load_path : "");
    savestate_add("hle", hle_state_all);
    savestate_add("main", main_state);
    savestate_add("textures", draw_gu_texture_state);
    savestate_set_meta(state_meta_fill, state_loaded);
    if (savestate_supervise(boot, NULL, load_path) != 0) sa1_log("boot: the run could not start or resume");
    report_and_exit(1);
    return 1;
}
