/* main.c - desktop boot of the recompiled game (M1: no graphics, no audio).
 *
 *   sa1psp <1ST_READ.BIN> <data root> [--frames N] [--seconds S] [--log FILE]
 *          [--watch ADDR]
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fn_8c010000(sh4_ctx *c);
void plat_set_data_root(const char *root);
void plat_backtrace(void);

static FILE *log_file;
static uint32_t frame_limit;
static uint64_t time_limit_us, t0;
static sh4_ctx ctx;

static void log_sink(const char *line) {
    uint64_t t = sa1_plat_time_us() - t0;
    fprintf(log_file ? log_file : stdout, "[%6u.%03u f%-6u] %s\n", (unsigned)(t / 1000000u),
            (unsigned)(t / 1000u % 1000u), hle_frame_count(), line);
    if (log_file) fflush(log_file);
}

static void report_and_exit(int code) {
    sa1_log("exit: %u frames, %.3f s", hle_frame_count(), (sa1_plat_time_us() - t0) / 1e6);
    sa1_seam_report(30);
    if (log_file) fclose(log_file);
    exit(code);
}

static void on_trap(sh4_ctx *c, uint32_t pc, const char *what) {
    (void)c; (void)pc; (void)what;
    plat_backtrace();
    sa1_log("exit: trap after %u frames, %.3f s", hle_frame_count(), (sa1_plat_time_us() - t0) / 1e6);
    sa1_seam_report(30);
    if (log_file) fclose(log_file);
}

static void on_frame(uint32_t frame) {
    sa1_watch_check("frame");
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
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) sa1_watch_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
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
    sa1_trap_hook = on_trap;
    hle_frame_hook = on_frame;

    sa1_ram = calloc(SA1_RAM_SIZE + SA1_RAM_SLACK, 1);
    if (!sa1_ram) { fprintf(stderr, "cannot allocate guest RAM\n"); return 1; }
    if (load_image(bin) != 0) return 1;
    plat_set_data_root(root);
    hle_init();

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
