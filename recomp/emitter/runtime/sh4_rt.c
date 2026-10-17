/* sh4_rt.c - the small runtime behind sh4_rt.h: guest RAM, function table
 * dispatch, traps, MMIO logging and the seam stub logger.  Platform-neutral
 * C11; the backends only provide sa1_ram allocation and a log sink. */
#include "sh4_rt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t *sa1_ram;

static void (*sa1_log_sink)(const char *line);

void sa1_set_log_sink(void (*sink)(const char *line)) { sa1_log_sink = sink; }

void sa1_log(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (sa1_log_sink) sa1_log_sink(buf);
    else fprintf(stderr, "%s\n", buf);
}

static const sa1_overlay *sa1_slots[SA1_SLOT_COUNT];

void sa1_overlay_set(int slot, const sa1_overlay *ov) {
    if (slot < 0 || slot >= SA1_SLOT_COUNT) return;
    sa1_slots[slot] = ov;
    sa1_log("overlay slot %d: %s", slot, ov ? ov->name : "(empty)");
}

const sa1_overlay *sa1_overlay_get(int slot) {
    return slot >= 0 && slot < SA1_SLOT_COUNT ? sa1_slots[slot] : NULL;
}

static sa1_fn sa1_table_find(const sa1_fn_entry *t, uint32_t n, uint32_t key) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        uint32_t a = t[mid].addr;
        if (a == key) return t[mid].fn;
        if (a < key) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

sa1_fn sa1_fn_lookup(uint32_t addr) {
    uint32_t key = addr & 0x1FFFFFFFu;
    for (int s = 0; s < SA1_SLOT_COUNT; s++) {
        const sa1_overlay *ov = sa1_slots[s];
        if (ov && key >= ov->code_lo && key < ov->code_hi)
            return sa1_table_find(ov->fns, ov->count, key);
    }
    sa1_fn fn = sa1_table_find(sa1_fn_table, sa1_fn_count, key);
    return fn ? fn : sa1_table_find(sa1_seam_table, sa1_seam_count, key);
}

void sa1_call(sh4_ctx *c, uint32_t addr) {
    sa1_fn fn = sa1_fn_lookup(addr);
    if (fn == NULL) {
        sa1_trap(c, addr, "indirect call to an address with no function");
        return;
    }
    fn(c);
}

void sa1_trap(sh4_ctx *c, uint32_t pc, const char *what) {
    sa1_log("trap at 0x%08x: %s", pc, what);
    sa1_log("  r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x",
            c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
    sa1_log("  r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x r13=%08x r14=%08x r15=%08x pr=%08x",
            c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13], c->r[14], c->r[15], c->pr);
    abort();
}

uint32_t sa1_mmio_rd(uint32_t addr, int size) {
    sa1_log("mmio read%d 0x%08x -> 0", size * 8, addr);
    return 0;
}

void sa1_mmio_wr(uint32_t addr, int size, uint32_t value) {
    sa1_log("mmio write%d 0x%08x = 0x%08x", size * 8, addr, value);
}

void sa1_seam_stub(sh4_ctx *c, uint32_t addr, const char *name) {
    static uint32_t last; static int repeats;
    if (addr == last) { if (++repeats > 2) return; } else { last = addr; repeats = 0; }
    sa1_log("seam stub %s (0x%08x) r4=%08x r5=%08x r6=%08x r7=%08x", name, addr,
            c->r[4], c->r[5], c->r[6], c->r[7]);
}
