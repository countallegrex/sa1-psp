/* savestate_test.c - the save-state machinery without the game
 * (backend/desktop/savestate.c, platform_desktop.c, coro_switch.c,
 * hle/state.c): a synthetic run whose state lives where the game's does,
 * in guest RAM, in a module's variables and in the locals of coroutines
 * that yield every step.  tests/savestate/run_synthetic.py drives it (CI).
 *
 *   savestate_test run SAVE_AT STOP_AT FILE   run; save at step SAVE_AT, print the checksum at STOP_AT
 *   savestate_test load FILE STOP_AT          resume FILE, print the checksum at STOP_AT
 *   savestate_test pack                        byte-run packer round trip
 *
 * A loaded run must print the same checksum as the straight run: every
 * worker's pseudo-random generator and ring buffer are stack locals, so
 * only a correctly restored stack continues the sequence.
 *
 * Part of sa1-psp (MIT). */
#include "platform_desktop.h"
#include "savestate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the recompiled game would provide. */
const sa1_fn_entry sa1_fn_table[] = {{0x0C010000u, NULL}};
const uint32_t sa1_fn_count = 0;
const sa1_fn_entry sa1_seam_table[] = {{0x0C5FA000u, NULL}};
const uint32_t sa1_seam_count = 0;

#define WORKERS 4
static sa1_coro *root, *workers[WORKERS];
static uint32_t step, save_at, stop_at, module_counter;
static const char *save_path;

static void worker(void *arg) {
    int id = (int)(intptr_t)arg;
    uint32_t x = 1000u + (uint32_t)id, ring[64];
    memset(ring, 0, sizeof ring);
    for (;;) {
        x = x * 1103515245u + 12345u;
        ring[step & 63] ^= x;
        hle_wr32(0x8C100000u + (uint32_t)id * 4u, x);
        hle_wr32(0x8C200000u + (uint32_t)id * 256u + (step & 63) * 4u, ring[step & 63]);
        hle_wr32(0x8C300000u + ((x >> 8) & 0xFFFFCu), step);          /* scattered writes */
        sa1_plat_coro_switch(root);
    }
}

/* The module's state: the step, a counter, and the coroutines by number
 * (host pointers to the descriptors are rebuilt from them, as tasks.c does). */
static void module_state(hle_state_io *io) {
    int32_t idx[WORKERS + 1];
    if (!hle_state_loading(io)) {
        idx[0] = sa1_plat_coro_index(root);
        for (int i = 0; i < WORKERS; i++) idx[i + 1] = sa1_plat_coro_index(workers[i]);
    }
    hle_state_blob(io, "test.step", &step, sizeof step);
    hle_state_blob(io, "test.counter", &module_counter, sizeof module_counter);
    hle_state_blob(io, "test.coros", idx, sizeof idx);
    if (hle_state_loading(io) && !hle_state_error(io)) {
        root = sa1_plat_coro_by_index(idx[0]);
        for (int i = 0; i < WORKERS; i++) workers[i] = sa1_plat_coro_by_index(idx[i + 1]);
    }
}

static uint32_t checksum(void) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < SA1_RAM_SIZE; i++) h = (h ^ sa1_ram[i]) * 16777619u;
    h = (h ^ step) * 16777619u;
    h = (h ^ module_counter) * 16777619u;
    return h;
}

static void run(void) {
    for (;;) {
        step++;
        module_counter += step * 7u;
        for (int i = 0; i < WORKERS; i++) sa1_plat_coro_switch(workers[i]);
        if (step == save_at && save_path && savestate_save(save_path) != 0) { fprintf(stderr, "save failed\n"); exit(3); }
        if (step == stop_at) { printf("step %u checksum %08x\n", step, checksum()); exit(0); }
    }
}

static void boot(void *arg) {
    (void)arg;
    root = sa1_plat_coro_current();
    for (int i = 0; i < WORKERS; i++) {
        workers[i] = sa1_plat_coro_create(worker, (void *)(intptr_t)i, SA1_CORO_STACK);
        if (!workers[i]) { fprintf(stderr, "cannot create worker %d\n", i); exit(2); }
    }
    run();
}

static void meta_fill(savestate_meta *m) { m->frame = step; m->flags = SAVESTATE_STRICT; m->rtc_epoch = 12345; }
static void meta_loaded(const savestate_meta *m) {
    if (m->frame != step || m->rtc_epoch != 12345) { fprintf(stderr, "header disagrees with the blob (frame %u vs step %u)\n", m->frame, step); exit(4); }
}

static int pack_test(void) {
    enum { N = 300000 };
    uint8_t *buf = malloc(N), *back = malloc(N);
    uint32_t x = 7;
    for (size_t i = 0; i < N; ) {                /* runs of zeros, runs of a byte, noise */
        x = x * 1103515245u + 12345u;
        size_t len = 1 + (x >> 20) % 700;
        if (i + len > N) len = N - i;
        int kind = (x >> 8) % 3;
        for (size_t k = 0; k < len; k++) {
            if (kind == 0) buf[i + k] = 0;
            else if (kind == 1) buf[i + k] = (uint8_t)(x >> 16);
            else { x = x * 1103515245u + 12345u; buf[i + k] = (uint8_t)(x >> 24); }
        }
        i += len;
    }
    size_t pn;
    uint8_t *packed = savestate_pack(buf, N, &pn);
    if (!packed) { printf("pack: out of memory\n"); return 1; }
    int r = savestate_unpack(packed, pn, back, N);
    int same = r == 0 && memcmp(buf, back, N) == 0;
    /* a corrupt stream must be refused, not read past its end */
    int rejected = savestate_unpack(packed, pn / 2, back, N) != 0;
    printf("pack: %u -> %zu bytes, round trip %s, truncated stream %s\n", (unsigned)N, pn, same ? "ok" : "FAIL", rejected ? "rejected" : "ACCEPTED");
    free(buf); free(back); free(packed);
    return same && rejected ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "pack")) return pack_test();
    if (sa1_plat_ram_alloc() != 0) { fprintf(stderr, "cannot map guest RAM at the fixed base\n"); return 2; }
    savestate_add("test", module_state);
    savestate_set_meta(meta_fill, meta_loaded);
    if (argc == 5 && !strcmp(argv[1], "run")) {
        save_at = (uint32_t)strtoul(argv[2], NULL, 0);
        stop_at = (uint32_t)strtoul(argv[3], NULL, 0);
        save_path = argv[4];
        if (savestate_supervise(boot, NULL, NULL) != 0) return 3;
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "load")) {
        stop_at = (uint32_t)strtoul(argv[3], NULL, 0);
        if (savestate_supervise(boot, NULL, argv[2]) != 0) { fprintf(stderr, "load refused\n"); return 5; }
        return 0;
    }
    fprintf(stderr, "usage: savestate_test run SAVE_AT STOP_AT FILE | load FILE STOP_AT | pack\n");
    return 2;
}
