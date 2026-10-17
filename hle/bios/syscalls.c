/* syscalls.c - the Dreamcast BIOS entry points the game reaches through its
 * A-tail shims (0x8C10C0D0-0x8C10DA78): the vector words at 0x8C0000B0
 * (system information), 0x8C0000B4 (ROM font), 0x8C0000B8 (flash ROM) and
 * 0x8C0000BC (GD-ROM and misc).  Katana's own libraries used the same
 * vectors; those libraries are replaced by hle/shinobi, so what remains
 * here is what game code calls directly: the cable/broadcast detection at
 * boot, the console ID, and the flash settings.
 *
 * The interface is the public one (r4-r7 arguments, r0 result, r6/r7
 * selecting the function); the facts about it come from Marcus Comstedt's
 * Dreamcast programming notes and the KallistiOS headers.  Nothing is
 * emulated: each call writes the guest-visible result and returns.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"

#define VEC_SYSINFO  0x8C0000B0u
#define VEC_FONT     0x8C0000B4u
#define VEC_FLASHROM 0x8C0000B8u
#define VEC_GDROM    0x8C0000BCu
#define VEC_MISC     0x8C0000E0u
#define SYSINFO_AREA 0x8C000068u   /* 8-byte ID, 5 bytes of factory settings */

/* The factory settings and ID the oracle run (Flycast, docs/m0-trace.md)
 * reported, so a desktop boot takes the same branches: country '0',
 * language '0', broadcast '1'(?), the fifth byte is what the boot code at
 * 0x8C10C044 maps to a video mode ('0'-'3'). */
static const uint8_t sys_props[5] = { '0', '0', '1', '1', '0' };
static const uint8_t sys_id[8] = { 0xc7, 0x38, 0x90, 0xf0, 0xc4, 0xdd, 0x89, 0x8e };

static void fill_sysinfo_area(void) {
    for (int i = 0; i < 8; i++) hle_wr8(SYSINFO_AREA + i, sys_id[i]);
    for (int i = 0; i < 5; i++) hle_wr8(SYSINFO_AREA + 8 + i, sys_props[i]);
    for (int i = 13; i < 24; i++) hle_wr8(SYSINFO_AREA + i, 0);
}

static void sys_sysinfo(sh4_ctx *c) {
    HLE_ENTER(VEC_SYSINFO, "bios sysinfo");
    switch (ARG3) {
    case 0:  fill_sysinfo_area(); RET(0); break;         /* SYSINFO_INIT */
    case 2:  RET(ARG0 > 9 ? -1 : 704); break;            /* SYSINFO_ICON: nothing drawn */
    case 3:  RET(SYSINFO_AREA); break;                   /* SYSINFO_ID */
    default: sa1_log("bios sysinfo: unknown function %u", ARG3); RET(-1); break;
    }
}

static void sys_font(sh4_ctx *c) {
    HLE_ENTER(VEC_FONT, "bios romfont");
    switch (ARG1) {
    case 0:  RET(0); break;      /* FONTROM_ADDRESS: no ROM font; nothing draws yet */
    case 1:  RET(0); break;      /* FONTROM_LOCK */
    case 2:  RET(0); break;      /* FONTROM_UNLOCK */
    default: RET(-1); break;
    }
}

/* Flash partitions: (offset, size) as the console reports them. */
static const uint32_t flash_part[5][2] = {
    {0x1a000, 8 * 1024}, {0x18000, 8 * 1024}, {0x1c000, 16 * 1024}, {0x10000, 32 * 1024}, {0x00000, 64 * 1024},
};

/* What a read of the flash returns.  The game reads its settings block
 * (language, audio mode) from the user partition; an erased partition is
 * all 0xFF, which the Katana syCfg code treats as "defaults", and the
 * factory partition carries the five settings bytes at 0x1a000. */
static uint8_t flash_byte(uint32_t off) {
    if (off >= 0x1a000 && off < 0x1a005) return sys_props[off - 0x1a000];
    if (off >= 0x1a056 && off < 0x1a05e) return sys_id[off - 0x1a056];
    return 0xFF;
}

static void sys_flashrom(sh4_ctx *c) {
    HLE_ENTER(VEC_FLASHROM, "bios flashrom");
    switch (ARG3) {
    case 0:                                              /* FLASHROM_INFO(part, u32[2]) */
        if (ARG0 < 5) { hle_wr32(ARG1, flash_part[ARG0][0]); hle_wr32(ARG1 + 4, flash_part[ARG0][1]); RET(0); }
        else RET(-1);
        break;
    case 1:                                              /* FLASHROM_READ(off, dst, n) */
        for (uint32_t i = 0; i < ARG2; i++) hle_wr8(ARG1 + i, flash_byte(ARG0 + i));
        RET(0);
        break;
    case 2:                                              /* FLASHROM_WRITE(off, src, n): accepted, not kept */
        sa1_log("bios flashrom: write 0x%x bytes at 0x%x ignored", ARG2, ARG0);
        RET(ARG2);
        break;
    case 3:  RET(0); break;                              /* FLASHROM_DELETE */
    default: RET(-1); break;
    }
}

/* GD-ROM: the game's own file system calls go to hle/shinobi/gdfs.c, so the
 * drive syscalls only see the drive-status probe the boot code makes. */
static uint32_t gd_req_id = 1;
static int gd_pending;

static void sys_gdrom(sh4_ctx *c) {
    HLE_ENTER(VEC_GDROM, "bios gdrom");
    if ((int32_t)ARG2 == -1) {                           /* misc: 0 init, 1 set vector */
        RET(0);
        return;
    }
    switch (ARG3) {
    case 0:  gd_pending = 1; RET(gd_req_id++); break;    /* REQ_CMD(cmd, params): accepted */
    case 1:                                              /* GET_CMD_STAT(id, u32[4]) */
        if (ARG1) for (int i = 0; i < 4; i++) hle_wr32(ARG1 + 4 * i, 0);
        if (gd_pending) { gd_pending = 0; RET(2); }      /* COMPLETE once */
        else RET(0);                                     /* idle */
        break;
    case 2:  RET(0); break;                              /* EXEC_SERVER */
    case 3:  RET(0); break;                              /* INIT_SYSTEM */
    case 4:                                              /* GET_DRV_STAT(u32[2]): PAUSE, GD-ROM */
        hle_wr32(ARG0, 1);
        hle_wr32(ARG0 + 4, 0x80);
        RET(0);
        break;
    default: RET(0); break;                              /* DMA/abort/reset/data type: nothing to do */
    }
}

static void sys_misc(sh4_ctx *c) {
    HLE_ENTER(VEC_MISC, "bios misc");
    sa1_log("bios misc: function %u (exit to BIOS?) ignored", ARG3);
    RET(0);
}

void hle_bios_init(void) {
    /* Each vector word points at itself: the shim loads the word and jumps
     * to it, sa1_call finds the native handler registered for that address. */
    static const struct { uint32_t vec; sa1_fn fn; } v[] = {
        {VEC_SYSINFO, sys_sysinfo}, {VEC_FONT, sys_font}, {VEC_FLASHROM, sys_flashrom},
        {VEC_GDROM, sys_gdrom}, {VEC_MISC, sys_misc},
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        hle_wr32(v[i].vec, v[i].vec);
        sa1_register_fn(v[i].vec, v[i].fn);
    }
    fill_sysinfo_area();
}
