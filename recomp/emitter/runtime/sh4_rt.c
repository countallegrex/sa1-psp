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

#define SA1_REG_MAX 64
static sa1_fn_entry sa1_reg[SA1_REG_MAX];
static uint32_t sa1_reg_count;

void sa1_register_fn(uint32_t addr, sa1_fn fn) {
    uint32_t key = addr & 0x1FFFFFFFu;
    for (uint32_t i = 0; i < sa1_reg_count; i++)
        if (sa1_reg[i].addr == key) { sa1_reg[i].fn = fn; return; }
    if (sa1_reg_count < SA1_REG_MAX) {
        sa1_reg[sa1_reg_count].addr = key;
        sa1_reg[sa1_reg_count].fn = fn;
        sa1_reg_count++;
    } else {
        sa1_log("sa1_register_fn: table full, 0x%08x dropped", addr);
    }
}

sa1_fn sa1_fn_lookup(uint32_t addr) {
    uint32_t key = addr & 0x1FFFFFFFu;
    for (uint32_t i = 0; i < sa1_reg_count; i++)
        if (sa1_reg[i].addr == key) return sa1_reg[i].fn;
    for (int s = 0; s < SA1_SLOT_COUNT; s++) {
        const sa1_overlay *ov = sa1_slots[s];
        if (ov && key >= ov->code_lo && key < ov->code_hi)
            return sa1_table_find(ov->fns, ov->count, key);
    }
    sa1_fn fn = sa1_table_find(sa1_fn_table, sa1_fn_count, key);
    return fn ? fn : sa1_table_find(sa1_seam_table, sa1_seam_count, key);
}

uint32_t sa1_watch_addr;
static uint32_t watch_last, watch_init;

void sa1_watch_check(const char *where) {
    if (!sa1_watch_addr) return;
    uint32_t v = RD32(sa1_watch_addr, 0);
    if (!watch_init) { watch_init = 1; watch_last = v; return; }
    if (v != watch_last) {
        sa1_log("watch: 0x%08x changed 0x%08x -> 0x%08x (%s)", sa1_watch_addr, watch_last, v, where);
        watch_last = v;
    }
}

void sa1_call(sh4_ctx *c, uint32_t addr) {
    if (sa1_watch_addr) sa1_watch_check("indirect call");
    sa1_fn fn = sa1_fn_lookup(addr);
    if (fn == NULL) {
        sa1_trap(c, addr, "indirect call to an address with no function");
        return;
    }
    fn(c);
}

void (*sa1_trap_hook)(sh4_ctx *c, uint32_t pc, const char *what);

void sa1_trap(sh4_ctx *c, uint32_t pc, const char *what) {
    sa1_log("trap at 0x%08x: %s", pc, what);
    sa1_log("  r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x",
            c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
    sa1_log("  r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x r13=%08x r14=%08x r15=%08x pr=%08x",
            c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13], c->r[14], c->r[15], c->pr);
    if (sa1_trap_hook) sa1_trap_hook(c, pc, what);
    abort();
}

/* A poll on a hardware register that never changes would spin forever: the
 * backend implements the registers the game touches directly
 * (sa1_mmio_hook); anything else reads as 0, logged once, and a run of
 * SA1_SPIN_LIMIT consecutive reads of one address traps with a backtrace. */
uint32_t (*sa1_mmio_hook)(uint32_t addr, int size, int *handled);
static uint32_t mmio_last_addr, mmio_repeat;
static uint32_t mmio_logged[32], mmio_logged_n;
#define SA1_SPIN_LIMIT 2000000u

static int mmio_log_once(uint32_t addr) {
    for (uint32_t i = 0; i < mmio_logged_n; i++) if (mmio_logged[i] == addr) return 0;
    if (mmio_logged_n < 32) mmio_logged[mmio_logged_n++] = addr;
    return 1;
}

uint32_t sa1_mmio_rd(uint32_t addr, int size) {
    if (addr == mmio_last_addr) {
        if (++mmio_repeat == SA1_SPIN_LIMIT) {
            sh4_ctx dummy = {0};
            sa1_trap(&dummy, addr, "spinning on an mmio read that never changes");
        }
    } else { mmio_last_addr = addr; mmio_repeat = 0; }
    if (sa1_mmio_hook) {
        int handled = 0;
        uint32_t v = sa1_mmio_hook(addr, size, &handled);
        if (handled) return v;
    }
    if (mmio_log_once(addr)) sa1_log("mmio read%d 0x%08x -> 0 (unhandled; logged once)", size * 8, addr);
    return 0;
}

void (*sa1_mmio_wr_hook)(uint32_t addr, int size, uint32_t value, int *handled);

void sa1_mmio_wr(uint32_t addr, int size, uint32_t value) {
    if (sa1_mmio_wr_hook) {
        int handled = 0;
        sa1_mmio_wr_hook(addr, size, value, &handled);
        if (handled) return;
    }
    if (mmio_log_once(addr | 1u)) sa1_log("mmio write%d 0x%08x = 0x%08x (unhandled; logged once)", size * 8, addr, value);
}

/* ---- seam hit counters ----------------------------------------------- */
#define SA1_HIT_MAX 512          /* 291 seam entries + room for HLE-side names */
typedef struct { uint32_t addr; uint32_t hits; const char *name; int logged; } sa1_hit;
static sa1_hit sa1_hits[SA1_HIT_MAX];
static uint32_t sa1_hit_count;

static sa1_hit *sa1_hit_slot(uint32_t addr, const char *name) {
    uint32_t key = addr & 0x1FFFFFFFu;
    /* open addressing keyed on the address; the table never fills (512 > entries) */
    uint32_t i = (key * 2654435761u) >> 23;   /* 9 bits */
    for (;;) {
        i &= SA1_HIT_MAX - 1;
        sa1_hit *h = &sa1_hits[i];
        if (h->name == NULL) {
            if (sa1_hit_count + 1 >= SA1_HIT_MAX) return NULL;
            h->addr = key; h->name = name; h->hits = 0; h->logged = 0;
            sa1_hit_count++;
            return h;
        }
        if (h->addr == key) return h;
        i++;
    }
}

void sa1_seam_hit(uint32_t addr, const char *name) {
    if (sa1_watch_addr) sa1_watch_check(name);
    sa1_hit *h = sa1_hit_slot(addr, name);
    if (h) h->hits++;
}

static uint32_t stub_last_addr, stub_repeat;

void sa1_seam_stub(sh4_ctx *c, uint32_t addr, const char *name) {
    if (sa1_watch_addr) sa1_watch_check(name);
    sa1_hit *h = sa1_hit_slot(addr, name);
    if (addr == stub_last_addr) {
        if (++stub_repeat == SA1_SPIN_LIMIT)
            sa1_trap(c, addr, "spinning on a seam stub (the game polls it; it needs a body)");
    } else { stub_last_addr = addr; stub_repeat = 0; }
    if (h) {
        h->hits++;
        if (h->logged >= 1) return;     /* every stub logs once, with its arguments */
        h->logged++;
    }
    sa1_log("seam stub %s (0x%08x) r4=%08x r5=%08x r6=%08x r7=%08x", name, addr | 0x80000000u,
            c->r[4], c->r[5], c->r[6], c->r[7]);
}

void sa1_seam_report(int top) {
    sa1_hit *sorted[SA1_HIT_MAX];
    uint32_t n = 0;
    for (uint32_t i = 0; i < SA1_HIT_MAX; i++)
        if (sa1_hits[i].name && sa1_hits[i].hits) sorted[n++] = &sa1_hits[i];
    /* insertion sort by hits, descending; n is small */
    for (uint32_t i = 1; i < n; i++) {
        sa1_hit *x = sorted[i]; uint32_t j = i;
        while (j > 0 && sorted[j - 1]->hits < x->hits) { sorted[j] = sorted[j - 1]; j--; }
        sorted[j] = x;
    }
    sa1_log("seam report: %u distinct entries called", n);
    for (uint32_t i = 0; i < n && (int)i < top; i++)
        sa1_log("  %10u  0x%08x  %s", sorted[i]->hits, sorted[i]->addr | 0x80000000u, sorted[i]->name);
}
