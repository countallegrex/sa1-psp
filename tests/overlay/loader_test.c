/* loader_test.c - the overlay loader hook's slot rules, without game data.
 *
 *   cc -std=c11 -Ihle -Ihle/overlay -Irecomp/emitter/runtime tests/overlay/loader_test.c \
 *      hle/overlay/overlay_loader.c recomp/emitter/runtime/sh4_rt.c -o loader_test
 *
 * A fake registry with one file per slot shape (an area file, the Egg
 * Carrier's ADV01OBJ at the area base plus ADV0100 at 0x8C920000, an event
 * file, and a configured file whose code was not emitted) and fake function
 * tables.  The test drives hle_overlay_load_begin/end as the wrapped PRS
 * loader does and checks which table sa1_fn_lookup reaches afterwards.
 * docs/m1-overlays.md section 7, docs/overlays-sonic.md.
 *
 * Part of sa1-psp (MIT). */
#include "overlay_loader.h"

#include <stdio.h>
#include <stdlib.h>

/* What the main build normally provides. */
const sa1_fn_entry sa1_fn_table[] = {{0x0C010000u, NULL}};
const uint32_t sa1_fn_count = 0;
const sa1_fn_entry sa1_seam_table[] = {{0x0C5FA000u, NULL}};
const uint32_t sa1_seam_count = 0;
void fn_8c09da44_orig(sh4_ctx *c) { c->r[0] = 0; }
void fn_8c099872_orig(sh4_ctx *c) { (void)c; }

static void f_stg(sh4_ctx *c) { (void)c; }
static void f_obj(sh4_ctx *c) { (void)c; }
static void f_ec(sh4_ctx *c) { (void)c; }
static void f_ev(sh4_ctx *c) { (void)c; }

static const sa1_fn_entry t_stg[] = {{0x0C900000u, f_stg}};
static const sa1_fn_entry t_obj[] = {{0x0C900000u, f_obj}};
static const sa1_fn_entry t_ec[] = {{0x0C920000u, f_ec}};
static const sa1_fn_entry t_ev[] = {{0x0CB80020u, f_ev}};
static const sa1_overlay o_stg = {"STG01", 0x0C900000u, 0x0C90F3E0u, t_stg, 1};
static const sa1_overlay o_obj = {"ADV01OBJ", 0x0C900000u, 0x0C901460u, t_obj, 1};
static const sa1_overlay o_ec = {"ADV0100", 0x0C920000u, 0x0C92F160u, t_ec, 1};
static const sa1_overlay o_ev = {"EV0001", 0x0CB80020u, 0x0CB83220u, t_ev, 1};

const hle_overlay_file hle_overlay_files[] = {
    {"SONICADV/STG01.PRS", SA1_SLOT_AREA, 1954056, "-", &o_stg},
    {"SONICADV/ADV01OBJ.PRS", SA1_SLOT_AREA, 6764, "-", &o_obj},
    {"SONICADV/ADV0100.PRS", SA1_SLOT_AREA2, 2160414, "-", &o_ec},
    {"SONICADV/EV0001.PRS", SA1_SLOT_EVENT, 81596, "-", &o_ev},
    {"SONICADV/STG02.PRS", SA1_SLOT_AREA, 1138093, "-", NULL},   /* configured, not emitted */
};
const unsigned hle_overlay_file_count = sizeof hle_overlay_files / sizeof hle_overlay_files[0];

static int failures;

static void load(const char *name, uint32_t dest) {
    hle_overlay_load_begin(name, dest);
    hle_overlay_load_end(name, dest, 1);
}

static void expect(const char *what, uint32_t addr, sa1_fn want) {
    sa1_fn got = sa1_fn_lookup(addr);
    if (got != want) {
        printf("FAIL %s: 0x%08x resolves to %p, expected %p\n", what, (unsigned)addr, (void *)got, (void *)want);
        failures++;
    }
}

int main(void) {
    static uint8_t ram[64];
    sa1_ram_set(ram);

    load("stg01.PRS", 0x8C900000u);
    expect("stage installed", 0x8C900000u, f_stg);

    /* Egg Carrier: ADV01OBJ at the area base, then ADV0100 above it. */
    load("adv01obj.PRS", 0x8C900000u);
    expect("ADV01OBJ replaces the stage", 0x8C900000u, f_obj);
    load("adv0100.PRS", 0x8C920000u);
    expect("ADV0100 in the second area slot", 0x8C920000u, f_ec);
    expect("ADV01OBJ kept beside it", 0x8C900000u, f_obj);

    /* ADV01OBJ reloaded: 6,764 bytes do not reach 0x8C920000. */
    load("ADV01OBJ.PRS", 0x8C900000u);
    expect("small area file keeps ADV0100", 0x8C920000u, f_ec);

    /* An event file goes to its own slot and touches neither area slot. */
    load("ev0001.prs", 0x8CB80000u);
    expect("event installed", 0x8CB80020u, f_ev);
    expect("event load keeps ADV0100", 0x8C920000u, f_ec);

    /* A big area file covers 0x8C920000: both area slots are emptied. */
    load("stg01.PRS", 0x8C900000u);
    expect("stage back", 0x8C900000u, f_stg);
    expect("big area file empties ADV0100", 0x8C920000u, NULL);

    /* A file the registry does not know counts as filling the area. */
    load("adv0100.PRS", 0x8C920000u);
    load("unknown.prs", 0x8C900000u);
    expect("unknown file empties the area", 0x8C900000u, NULL);
    expect("unknown file empties ADV0100", 0x8C920000u, NULL);

    /* A configured file without emitted code leaves its slot empty. */
    load("stg01.PRS", 0x8C900000u);
    load("stg02.PRS", 0x8C900000u);
    expect("not-emitted file leaves the slot empty", 0x8C900000u, NULL);
    expect("event still there", 0x8CB80020u, f_ev);

    /* The texture loader's scratch use of the area empties both slots. */
    load("adv01obj.PRS", 0x8C900000u);
    load("adv0100.PRS", 0x8C920000u);
    hle_overlay_load_begin("scratch.PRS", 0x8C900000u);
    expect("scratch empties the area", 0x8C900000u, NULL);
    expect("scratch empties ADV0100", 0x8C920000u, NULL);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
