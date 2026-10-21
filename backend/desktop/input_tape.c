/* input_tape.c - TAS-style input tapes and state checks for the desktop
 * build (tests/replay/README.md has the formats and the reasoning).
 *
 *   --strict            guest time is exactly 1/60 s per frame (hle.h,
 *                       hle_strict) and the clock starts at a fixed epoch
 *   --epoch SECONDS     the real-time clock's start (Unix time) in strict mode
 *   --record FILE       write the pad of every guest frame to FILE (implies
 *                       --strict); the file is complete on disk after every
 *                       frame, so a crash keeps the run up to that frame
 *   --replay FILE       feed FILE's pad frame by frame, overriding the real
 *                       pad (implies --strict; --press buttons still add on)
 *   --check FILE        compare guest memory with FILE's rows; any primary
 *                       mismatch, a trap or a missed --expect-frame fails the run
 *   --expect-frame N    the run must reach guest frame N
 *   --watch-list FILE   the addresses to trace (tests/replay/watch.txt)
 *   --watch-trace FILE  write their values every frame, tab-separated, for
 *                       tools/tape_expect.py and tools/tape_diff.py
 *   --sound-timing real|none
 *                       what the guest sees of sound playback: "real" (the
 *                       ADX players take as long as their streams, M3) or
 *                       "none" (every player reports stopped, as before M3,
 *                       when no ADX player existed).  A recording writes
 *                       its mode into the tape; a replay takes the tape's,
 *                       and a tape without the line (recorded before M3)
 *                       replays as "none"
 *   --fpu-round zero|nearest
 *                       the host FPU's rounding: "zero" is the console's (the
 *                       Katana reset FPSCR 0x00040001 has RM = 1, round
 *                       toward zero), "nearest" what every run used before
 *                       2026-10-19.  A recording writes its mode into the
 *                       tape; a tape without the line replays as "nearest"
 *   (--aspect, main.c)  a recording writes the run's aspect into the tape; a
 *                       replay without --aspect takes the tape's (absent:
 *                       wide, the default every tape before 2026-10-21 had)
 *
 * main.c calls in at five points: argument parsing, start-up (before
 * hle_init), the pad of each frame, the end of each frame hook (where
 * --dump writes, so checks see what tools/tape_expect.py saw), and exit.
 *
 * Part of sa1-psp (MIT). */
#include "hle.h"
#include "input_tape.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TAPE_FORMAT 1
#define TAPE_TOOL "sa1psp input_tape 1"

static int strict;
static int64_t epoch = 946684800;           /* 2000-01-01 00:00 UTC */
static int epoch_set;
static int sound_timing = -1, sound_timing_set;   /* 1 real, 0 none, -1 not given */
static int fpu_zero = -1, fpu_zero_set;           /* 1 toward zero, 0 nearest, -1 not given */
static int tape_aspect = -1;                      /* the tape's line: 1 wide, 0 4:3, -1 none */
static const char *record_path, *replay_path, *check_path, *watch_list_path, *watch_trace_path;
static uint32_t expect_frame;
static char title_sha[65];

/* ---- the tape --------------------------------------------------------------
 * Text, one record per run of identical frames:
 *   FRAME BUTTONS(hex) STICK_X STICK_Y LTRIG RTRIG [COUNT]
 * COUNT (default 1) is how many consecutive frames, from FRAME, hold that pad. */
typedef struct { uint32_t buttons; int8_t x, y; uint8_t lt, rt; } tape_pad;
static tape_pad *tape;                 /* replay: index = guest frame */
static uint32_t tape_frames;           /* replay: frames 1..tape_frames-1 are defined */
static FILE *rec;
static tape_pad rec_last;
static uint32_t rec_first, rec_count, rec_end;
static long rec_run_pos = -1;          /* file offset of the open run's line */

static int pad_eq(const tape_pad *a, const tape_pad *b) {
    return a->buttons == b->buttons && a->x == b->x && a->y == b->y && a->lt == b->lt && a->rt == b->rt;
}

/* The open run is (re)written in place and flushed to disk every frame, so
 * a crash or a kill loses nothing but the "end" line (the tape of the
 * second exit-test run was 4 KB of a 23,000-frame run: stdio's buffer). */
static void rec_write_run(void) {
    if (!rec || !rec_count) return;
    if (rec_run_pos < 0) rec_run_pos = ftell(rec);
    fseek(rec, rec_run_pos, SEEK_SET);
    fprintf(rec, "%u %x %d %d %u %u", rec_first, rec_last.buttons, rec_last.x, rec_last.y, rec_last.lt, rec_last.rt);
    if (rec_count > 1) fprintf(rec, " %u", rec_count);
    fputc('\n', rec);
    fflush(rec);
}
static void rec_flush(void) {
    rec_write_run();
    rec_count = 0;
    rec_run_pos = -1;
}

static int load_tape(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); return -1; }
    char line[512];
    int format = 0, lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        char key[64], val[128];
        if (*p >= '0' && *p <= '9') {
            unsigned fr, b, lt, rt, count = 1;
            int x, y;
            int n = sscanf(p, "%u %x %d %d %u %u %u", &fr, &b, &x, &y, &lt, &rt, &count);
            if (n < 6 || x < -128 || x > 127 || y < -128 || y > 127 || lt > 255 || rt > 255 || count == 0) {
                fprintf(stderr, "replay: %s:%d: bad record\n", path, lineno); fclose(f); return -1;
            }
            if (fr + count > tape_frames) {
                tape_pad *t = realloc(tape, (size_t)(fr + count) * sizeof *t);
                if (!t) { fclose(f); return -1; }
                memset(t + tape_frames, 0, (size_t)(fr + count - tape_frames) * sizeof *t);
                tape = t; tape_frames = fr + count;
            }
            tape_pad pad = {b, (int8_t)x, (int8_t)y, (uint8_t)lt, (uint8_t)rt};
            for (unsigned i = 0; i < count; i++) tape[fr + i] = pad;
        } else if (sscanf(p, "%63s %127s", key, val) == 2) {
            if (!strcmp(key, "sa1psp-tape")) format = atoi(val);
            else if (!strcmp(key, "title-sha256")) {
                if (title_sha[0] && strcmp(val, title_sha)) {
                    fprintf(stderr, "replay: %s was recorded on another 1ST_READ.BIN (sha256 %s, this one %s)\n", path, val, title_sha);
                    fclose(f); return -1;
                }
            } else if (!strcmp(key, "epoch")) { if (!epoch_set) epoch = strtoll(val, NULL, 0); }
            else if (!strcmp(key, "sound-timing")) { if (!sound_timing_set) sound_timing = strcmp(val, "none") != 0; }
            else if (!strcmp(key, "fpu-round")) { if (!fpu_zero_set) fpu_zero = !strcmp(val, "zero"); }
            else if (!strcmp(key, "aspect")) tape_aspect = strcmp(val, "4:3") != 0 && strcmp(val, "4x3") != 0;
            else if (!strcmp(key, "end")) {
                uint32_t e = (uint32_t)strtoul(val, NULL, 0) + 1;
                if (e > tape_frames) {
                    tape_pad *t = realloc(tape, (size_t)e * sizeof *t);
                    if (!t) { fclose(f); return -1; }
                    memset(t + tape_frames, 0, (size_t)(e - tape_frames) * sizeof *t);
                    tape = t; tape_frames = e;
                }
            }
            /* other keys (tool, comment-like metadata) are informational */
        }
    }
    fclose(f);
    if (format != TAPE_FORMAT) { fprintf(stderr, "replay: %s: not a format-%d tape\n", path, TAPE_FORMAT); return -1; }
    sa1_log("replay: %s, %u frames, epoch %lld", path, tape_frames ? tape_frames - 1 : 0, (long long)epoch);
    return 0;
}

/* ---- checks ----------------------------------------------------------------
 *   FRAME ADDR SIZE EXPECTED [TOLERANCE] [NAME]
 * FRAME is N (at frame N exactly), <=N (at some frame up to N) or M..N (at
 * some frame from M to N): the last two are milestones.
 * SIZE is 1, 2, 4 (unsigned), s1, s2, s4 (signed) or f (float32).
 * TOLERANCE is an absolute difference ("-" for exact).  Rows after a
 * "[secondary]" line are reported but do not fail the run. */
typedef struct {
    uint32_t from, frame, addr;
    int by, size, is_signed, is_float, secondary, done, ok;
    double expected, tol, last;
    char name[48];
} check_row;
static check_row *checks;
static int nchecks, check_fail_primary, check_fail_secondary;

static double read_value(const check_row *r) {
    if (r->is_float) return hle_rdf(r->addr);
    uint32_t v = r->size == 1 ? hle_rd8(r->addr) : r->size == 2 ? hle_rd16(r->addr) : hle_rd32(r->addr);
    if (r->is_signed) return r->size == 1 ? (int8_t)v : r->size == 2 ? (int16_t)v : (double)(int32_t)v;
    return v;
}

static int load_checks(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "check: cannot open %s\n", path); return -1; }
    char line[512];
    int lineno = 0, secondary = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        if (!strncmp(p, "[secondary]", 11)) { secondary = 1; continue; }
        if (!strncmp(p, "[primary]", 9)) { secondary = 0; continue; }
        check_row r;
        memset(&r, 0, sizeof r);
        char fr[32], addr[32], size[8], exp[48], tol[48] = "-";
        int consumed = 0;
        int n = sscanf(p, "%31s %31s %7s %47s %n", fr, addr, size, exp, &consumed);
        if (n < 4) { fprintf(stderr, "check: %s:%d: need FRAME ADDR SIZE EXPECTED\n", path, lineno); fclose(f); return -1; }
        char *rest = p + consumed;
        int c2 = 0;
        if (sscanf(rest, "%47s %n", tol, &c2) >= 1) {
            char *e;
            strtod(tol, &e);
            if (!strcmp(tol, "-") || (e != tol && !*e)) rest += c2;   /* a number or "-": the tolerance */
            else strcpy(tol, "-");                                     /* no tolerance, just a name */
        }
        char *dots = strstr(fr, "..");
        r.by = (fr[0] == '<' && fr[1] == '=') || dots;
        r.from = dots ? (uint32_t)strtoul(fr, NULL, 0) : 0;
        r.frame = (uint32_t)strtoul(dots ? dots + 2 : fr + (r.by ? 2 : 0), NULL, 0);
        r.addr = (uint32_t)strtoul(addr, NULL, 16);
        r.is_signed = size[0] == 's';
        r.is_float = size[0] == 'f';
        r.size = r.is_float ? 4 : atoi(size + r.is_signed);
        if (!hle_in_ram(r.addr) || (r.size != 1 && r.size != 2 && r.size != 4)) {
            fprintf(stderr, "check: %s:%d: bad address or size\n", path, lineno); fclose(f); return -1;
        }
        r.expected = r.is_float ? strtod(exp, NULL) : (double)(r.is_signed ? strtoll(exp, NULL, 0) : (long long)strtoull(exp, NULL, 0));
        r.tol = strcmp(tol, "-") ? strtod(tol, NULL) : 0;
        r.secondary = secondary;
        size_t len = strcspn(rest, "\r\n");
        if (len >= sizeof r.name) len = sizeof r.name - 1;
        memcpy(r.name, rest, len);
        if (!len) snprintf(r.name, sizeof r.name, "%08x", r.addr);
        check_row *t = realloc(checks, (size_t)(nchecks + 1) * sizeof *t);
        if (!t) { fclose(f); return -1; }
        checks = t;
        checks[nchecks++] = r;
    }
    fclose(f);
    sa1_log("check: %s, %d rows", path, nchecks);
    return 0;
}

static void check_result(check_row *r, int ok, uint32_t frame) {
    r->done = 1; r->ok = ok;
    if (ok) return;
    if (r->secondary) check_fail_secondary++; else check_fail_primary++;
    if (r->by)
        sa1_log("check: %s %s: never %.6g in frames %u..%u [0x%08x] (last %.6g, tolerance %g)", r->secondary ? "DRIFT" : "FAIL",
                r->name, r->expected, r->from, r->frame, r->addr, r->last, r->tol);
    else
        sa1_log("check: %s %s: frame %u [0x%08x] = %.6g, expected %.6g (tolerance %g)", r->secondary ? "DRIFT" : "FAIL",
                r->name, frame, r->addr, r->last, r->expected, r->tol);
}

/* ---- the watch trace ---------------------------------------------------------
 * watch.txt rows: ADDR SIZE NAME [ROLE [TOLERANCE]]; only the first three
 * matter here.  One line per frame: frame, then each value. */
typedef struct { uint32_t addr; int size, is_signed, is_float; char name[32]; } watch_row;
static watch_row *watches;
static int nwatches;
static FILE *wtrace;

static int load_watches(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "watch: cannot open %s\n", path); return -1; }
    char line[512], addr[32], size[8], name[32];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        if (sscanf(p, "%31s %7s %31s", addr, size, name) != 3) {
            fprintf(stderr, "watch: %s:%d: need ADDR SIZE NAME\n", path, lineno); fclose(f); return -1;
        }
        watch_row w = {(uint32_t)strtoul(addr, NULL, 16), 0, size[0] == 's', size[0] == 'f', ""};
        w.size = w.is_float ? 4 : atoi(size + w.is_signed);
        snprintf(w.name, sizeof w.name, "%s", name);
        if (!hle_in_ram(w.addr) || (w.size != 1 && w.size != 2 && w.size != 4)) {
            fprintf(stderr, "watch: %s:%d: bad address or size\n", path, lineno); fclose(f); return -1;
        }
        watch_row *t = realloc(watches, (size_t)(nwatches + 1) * sizeof *t);
        if (!t) { fclose(f); return -1; }
        watches = t;
        watches[nwatches++] = w;
    }
    fclose(f);
    return 0;
}

static void trace_watches(uint32_t frame) {
    fprintf(wtrace, "%u", frame);
    for (int i = 0; i < nwatches; i++) {
        const watch_row *w = &watches[i];
        if (w->is_float) { fprintf(wtrace, "\t%.9g", hle_rdf(w->addr)); continue; }
        uint32_t v = w->size == 1 ? hle_rd8(w->addr) : w->size == 2 ? hle_rd16(w->addr) : hle_rd32(w->addr);
        if (w->is_signed) fprintf(wtrace, "\t%d", w->size == 1 ? (int8_t)v : w->size == 2 ? (int16_t)v : (int32_t)v);
        else fprintf(wtrace, "\t%u", v);
    }
    fputc('\n', wtrace);
}

/* ---- the hooks main.c calls -------------------------------------------------- */
int input_tape_arg(int argc, char **argv, int *i) {
    const char *a = argv[*i];
    if (!strcmp(a, "--strict")) strict = 1;
    else if (*i + 1 >= argc) return 0;
    else if (!strcmp(a, "--epoch")) { epoch = strtoll(argv[++*i], NULL, 0); epoch_set = 1; }
    else if (!strcmp(a, "--sound-timing")) { sound_timing = strcmp(argv[++*i], "none") != 0; sound_timing_set = 1; }
    else if (!strcmp(a, "--fpu-round")) { fpu_zero = !strcmp(argv[++*i], "zero"); fpu_zero_set = 1; }
    else if (!strcmp(a, "--record")) { record_path = argv[++*i]; strict = 1; }
    else if (!strcmp(a, "--replay")) { replay_path = argv[++*i]; strict = 1; }
    else if (!strcmp(a, "--check")) check_path = argv[++*i];
    else if (!strcmp(a, "--expect-frame")) expect_frame = (uint32_t)strtoul(argv[++*i], NULL, 0);
    else if (!strcmp(a, "--watch-list")) watch_list_path = argv[++*i];
    else if (!strcmp(a, "--watch-trace")) watch_trace_path = argv[++*i];
    else return 0;
    return 1;
}

int input_tape_start(const char *title_path) {
    if (watch_trace_path) {
        if (!watch_list_path) { fprintf(stderr, "--watch-trace needs --watch-list\n"); return -1; }
        if (load_watches(watch_list_path) != 0) return -1;
        wtrace = fopen(watch_trace_path, "wb");
        if (!wtrace) { fprintf(stderr, "watch: cannot write %s\n", watch_trace_path); return -1; }
        fprintf(wtrace, "frame");
        for (int i = 0; i < nwatches; i++) fprintf(wtrace, "\t%s", watches[i].name);
        fputc('\n', wtrace);
    }
    if (!strict && !check_path && !expect_frame) return 0;
    uint8_t digest[32];
    if ((record_path || replay_path) && sha256_file(title_path, digest) != 0) {
        fprintf(stderr, "tape: cannot read %s\n", title_path); return -1;
    }
    if (record_path || replay_path) sha256_hex(digest, title_sha);
    if (replay_path && !sound_timing_set) sound_timing = 0;      /* the tape's line, if any, sets it back */
    if (replay_path && !fpu_zero_set) fpu_zero = 0;
    if (replay_path && load_tape(replay_path) != 0) return -1;
    if (check_path && load_checks(check_path) != 0) return -1;
    if (record_path) {
        rec = fopen(record_path, "wb");
        if (!rec) { fprintf(stderr, "record: cannot write %s\n", record_path); return -1; }
        fprintf(rec, "sa1psp-tape %d\ntitle-sha256 %s\nepoch %lld\ntool %s\n", TAPE_FORMAT, title_sha, (long long)epoch, TAPE_TOOL);
        fprintf(rec, "sound-timing %s\n", sound_timing == 0 ? "none" : "real");
        fprintf(rec, "fpu-round %s\n", fpu_zero == 0 ? "nearest" : "zero");
        fprintf(rec, "aspect %s\n", hle_aspect_wide ? "wide" : "4:3");
        fprintf(rec, "# FRAME BUTTONS STICK_X STICK_Y LTRIG RTRIG [COUNT]\n");
        sa1_log("record: %s", record_path);
    }
    hle_strict = strict;
    hle_rtc_epoch = epoch;
    if (replay_path) sa1_log("replay: sound timing %s", sound_timing ? "real (players take their streams' length)" : "none (the tape predates M3: players report stopped)");
    if (strict) sa1_log("strict: guest time 1/60 s per frame, clock epoch %lld", (long long)epoch);
    return 0;
}

uint32_t input_tape_frames(void) { return replay_path && tape_frames ? tape_frames - 1 : 0; }

uint32_t input_tape_pad(uint32_t frame, uint32_t buttons, int scripted) {
    if (tape) {
        tape_pad t = frame < tape_frames ? tape[frame] : (tape_pad){0};
        hle_pad_state.x = t.x; hle_pad_state.y = t.y; hle_pad_state.ltrig = t.lt; hle_pad_state.rtrig = t.rt;
        buttons = t.buttons | (scripted ? buttons : 0);
    }
    if (rec) {
        tape_pad p = {buttons, hle_pad_state.x, hle_pad_state.y, hle_pad_state.ltrig, hle_pad_state.rtrig};
        if (rec_count && pad_eq(&p, &rec_last) && rec_first + rec_count == frame) rec_count++;
        else { rec_flush(); rec_last = p; rec_first = frame; rec_count = 1; }
        rec_end = frame;
        rec_write_run();
    }
    return buttons;
}

void input_tape_frame_end(uint32_t frame) {
    if (wtrace) trace_watches(frame);
    for (int i = 0; i < nchecks; i++) {
        check_row *r = &checks[i];
        if (r->done) continue;
        if (r->by ? frame < r->from || frame > r->frame : frame != r->frame) continue;
        r->last = read_value(r);
        int ok = fabs(r->last - r->expected) <= r->tol;
        if (ok || !r->by || frame == r->frame) check_result(r, ok, frame);
    }
}

/* A save state was loaded at `frame`: check rows whose frame or window
 * ended before it cannot be evaluated by this run and count as done. */
void input_tape_loaded(uint32_t frame) {
    int skipped = 0;
    for (int i = 0; i < nchecks; i++)
        if (!checks[i].done && checks[i].frame < frame) { checks[i].done = 1; skipped++; }
    if (skipped) sa1_log("check: %d row(s) due before the loaded frame %u skipped", skipped, frame);
}

int input_tape_recording(void) { return record_path != NULL; }

int input_tape_finish(int code, const char *why) {
    static int finished;
    if (finished) return code;
    finished = 1;
    if (wtrace) { fclose(wtrace); wtrace = NULL; }
    if (rec) {
        rec_flush();
        fprintf(rec, "end %u\n", rec_end);
        fclose(rec);
        rec = NULL;
        sa1_log("record: %u frames written to %s", rec_end, record_path);
    }
    if (!check_path && !expect_frame) return code;
    uint32_t reached = hle_frame_count();
    for (int i = 0; i < nchecks; i++)
        if (!checks[i].done) {                     /* the run ended before the row's frame */
            checks[i].last = NAN;
            check_result(&checks[i], 0, reached);
        }
    int fail = check_fail_primary > 0 || code != 0 || why;
    if (expect_frame && reached < expect_frame) {
        sa1_log("check: FAIL run ended at frame %u, expected to reach %u", reached, expect_frame);
        fail = 1;
    }
    int primary = 0;
    for (int i = 0; i < nchecks; i++) primary += !checks[i].secondary;
    char line[256];
    snprintf(line, sizeof line, "check: %s at frame %u: %d/%d primary failed, %d/%d secondary drifted%s%s",
             fail ? "FAIL" : "PASS", reached, check_fail_primary, primary, check_fail_secondary, nchecks - primary,
             why ? ", " : "", why ? why : "");
    sa1_log("%s", line);
    fprintf(stderr, "%s\n", line);
    return fail && code == 0 ? 1 : code;
}

int input_tape_sound_timing(void) { return sound_timing; }
int input_tape_fpu_zero(void) { return fpu_zero; }
int input_tape_aspect_wide(void) { return tape_aspect; }
