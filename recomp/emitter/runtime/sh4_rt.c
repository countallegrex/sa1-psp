/* sh4_rt.c - the small runtime behind sh4_rt.h: guest RAM, function table
 * dispatch, traps, MMIO logging and the seam stub logger.  Platform-neutral
 * C11; the backends only provide sa1_ram allocation and a log sink. */
#define SA1_RAM_WRITER 1   /* the one file that sees sa1_ram non-const (sh4_rt.h) */
#include "sh4_rt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- fsca (sh4_trig.h) ---------------------------------------------------- */
uint32_t sh4_fsca_sin[32768];
int sh4_fsca_ready, sh4_fsca_authentic;

int sh4_trig_init(const void *table, uint32_t size) {
    const uint8_t *t = (const uint8_t *)table;
    int ok = t && size == 65536u * 8u;
    uint32_t w[4];
    for (uint32_t a = 0; ok && a < 65536u; a++) {
        memcpy(w, t + 8u * a, 8);                                 /* sin, cos of a */
        memcpy(w + 2, t + 8u * ((a + 0x4000u) & 0xFFFFu), 4);      /* sin of a + 1/4 */
        memcpy(w + 3, t + 8u * ((a + 0x8000u) & 0xFFFFu), 4);      /* sin of a + 1/2 */
        float f; memcpy(&f, w, 4);
        /* the table's own structure: cos = sin a quarter on, sin flips sign a half on, |sin| <= 1 */
        if (w[1] != w[2] || w[3] != (w[0] ^ 0x80000000u) || !(f >= -1.0f && f <= 1.0f)) ok = 0;
    }
    if (ok && (((const uint32_t *)(const void *)t)[0] != 0 || ((const uint32_t *)(const void *)t)[1] != 0x3F800000u)) ok = 0;
    if (ok)
        for (uint32_t a = 0; a < 32768u; a++) memcpy(&sh4_fsca_sin[a], t + 8u * a, 4);
    else
        for (uint32_t a = 0; a < 32768u; a++) {      /* the platform's libm: not the console's values */
            float f = sinf((float)a * (float)(6.283185307179586 / 65536.0));
            if (a == 0) f = 0.0f;
            memcpy(&sh4_fsca_sin[a], &f, 4);
        }
    sh4_fsca_authentic = ok;
    sh4_fsca_ready = 1;
    return ok || !t ? 0 : -1;
}

uint8_t *sa1_ram;
void sa1_ram_set(uint8_t *ram) { sa1_ram = ram; }
extern uint32_t (*sa1_trace_frame)(void);

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

/* sa1_fn_lookup's answers, direct-mapped by address: an indirect call
 * otherwise scans the registered functions and the overlay slots and
 * binary-searches the main table every time.  Emptied whenever a slot or a
 * registered function changes. */
#define SA1_LOOKUP_CACHE 1024
static struct { uint32_t key; sa1_fn fn; } sa1_lookup_cache[SA1_LOOKUP_CACHE];
static void lookup_cache_clear(void) { memset(sa1_lookup_cache, 0, sizeof sa1_lookup_cache); }

void sa1_overlay_set(int slot, const sa1_overlay *ov) {
    if (slot < 0 || slot >= SA1_SLOT_COUNT) return;
    sa1_slots[slot] = ov;
    lookup_cache_clear();
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
    lookup_cache_clear();
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

static sa1_fn fn_lookup_slow(uint32_t key) {
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

sa1_fn sa1_fn_lookup(uint32_t addr) {
    uint32_t key = addr & 0x1FFFFFFFu;
    uint32_t i = (key >> 1) & (SA1_LOOKUP_CACHE - 1);   /* SH4 code is 2-byte aligned */
    if (sa1_lookup_cache[i].key == key && sa1_lookup_cache[i].fn) return sa1_lookup_cache[i].fn;
    sa1_fn fn = fn_lookup_slow(key);
    if (fn) { sa1_lookup_cache[i].key = key; sa1_lookup_cache[i].fn = fn; }
    return fn;
}

uint32_t sa1_watch_addr;
#define WATCH_MAX 8
typedef struct { uint32_t base; int32_t off[4]; int noff; uint32_t last; int init; char spec[48]; } sa1_watch;
static sa1_watch watches[WATCH_MAX];
static int nwatches;

int sa1_watch_add(const char *spec) {
    if (nwatches >= WATCH_MAX) return -1;
    sa1_watch *w = &watches[nwatches];
    memset(w, 0, sizeof *w);
    strncpy(w->spec, spec, sizeof w->spec - 1);
    char *end;
    w->base = (uint32_t)strtoul(spec, &end, 0);
    while (*end == ':' && w->noff < 4) w->off[w->noff++] = (int32_t)strtol(end + 1, &end, 0);
    nwatches++;
    return 0;
}

static int watch_value(const sa1_watch *w, uint32_t *out) {
    uint32_t a = w->base;
    for (int i = 0; i < w->noff; i++) {
        if ((a & 0x1F000000u) != 0x0C000000u) return 0;      /* chain broken: not in RAM */
        a = RD32(a, 0) + (uint32_t)w->off[i];
    }
    if ((a & 0x1F000000u) != 0x0C000000u) return 0;
    *out = RD32(a, 0);
    return 1;
}

void sa1_watch_check(const char *where) {
    if (sa1_watch_addr && nwatches == 0) { char b[16]; snprintf(b, sizeof b, "0x%x", sa1_watch_addr); sa1_watch_add(b); sa1_watch_addr = 0; nwatches = 1; sa1_watch_addr = 1; }
    for (int i = 0; i < nwatches; i++) {
        sa1_watch *w = &watches[i];
        uint32_t v;
        if (!watch_value(w, &v)) continue;
        if (!w->init) { w->init = 1; w->last = v; continue; }
        if (v != w->last) {
            sa1_log("watch: %s changed 0x%08x -> 0x%08x (%s)", w->spec, w->last, v, where);
            w->last = v;
        }
    }
}

void (*sa1_depth_hook)(void);
uintptr_t sa1_stack_floor = UINTPTR_MAX;
int sa1_probe_slow;

void sa1_call(sh4_ctx *c, uint32_t addr) {
    sa1_depth_probe();
    if (sa1_watch_addr) sa1_watch_check("indirect call");
    sa1_fn fn = sa1_fn_lookup(addr);
    if (fn == NULL) {
        sa1_trap(c, addr, "indirect call to an address with no function");
        return;
    }
    fn(c);
}

sa1_fn sa1_tail_target(sh4_ctx *c, uint32_t addr) {
    sa1_depth_probe();
    if (sa1_watch_addr) sa1_watch_check("indirect jump");
    sa1_fn fn = sa1_fn_lookup(addr);
    if (fn == NULL) sa1_trap(c, addr, "indirect jump to an address with no function");
    return fn;
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
typedef struct sa1_hit sa1_hit;   /* sh4_rt.h */
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

/* A native body the game polls without the frame advancing is a spin too:
 * SA1_SPIN_LIMIT consecutive hits of one entry with the frame counter
 * unchanged trap with a backtrace. */
struct sa1_hit_spin sa1_hit_spin;   /* the last entry, its frame, its repeats (sh4_rt.h) */

/* The frame counter is read only on a repeat of the last entry: a run starts
 * with the frame unknown and takes it at its first repeat. */
static inline void seam_hit_count(sa1_hit *h, uint32_t addr, const char *name) {
    sa1_depth_probe();
    if (sa1_watch_addr) sa1_watch_check(name);
    if (h) h->hits++;
    struct sa1_hit_spin *s = &sa1_hit_spin;
    if (addr != s->addr) { s->addr = addr; s->frame = UINT32_MAX; s->repeat = 0; return; }
    /* A repeat reads the frame only every 65536 repeats (the read at every
     * one was most of this path's 0.8 % of the PSP-3001's frame). */
    if ((++s->repeat & 0xFFFFu) != 0) return;
    uint32_t f = sa1_trace_frame ? sa1_trace_frame() : 0;
    if (f != s->frame) { s->frame = f; s->repeat = 0; return; }
    if (s->repeat >= SA1_SPIN_LIMIT) {
        sh4_ctx dummy = {0};
        sa1_trap(&dummy, addr, name);
    }
}

void sa1_seam_hit(uint32_t addr, const char *name) {
    seam_hit_count(sa1_hit_slot(addr, name), addr, name);
}

void sa1_seam_hit_at(sa1_hit **site, uint32_t addr, const char *name) {
    sa1_hit *h = *site;
    if (!h) h = *site = sa1_hit_slot(addr, name);
    seam_hit_count(h, addr, name);
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

/* ---- guest addresses outside RAM (SA1_RAM_CHECK) -------------------------- */
void (*sa1_badaddr_hook)(uint32_t addr);
#define BAD_MAX 64
static struct { uint32_t addr, hits, first_frame; int write; } bad_recs[BAD_MAX];
static uint32_t bad_n, bad_other;

void sa1_ram_badaddr(uint32_t a, uint32_t d, int write) {
    uint32_t addr = a + d;
    uint32_t f = sa1_trace_frame ? sa1_trace_frame() : 0;
    for (uint32_t i = 0; i < bad_n; i++) if (bad_recs[i].addr == addr) { bad_recs[i].hits++; return; }
    if (bad_n == BAD_MAX) { bad_other++; return; }
    bad_recs[bad_n].addr = addr; bad_recs[bad_n].hits = 1; bad_recs[bad_n].first_frame = f; bad_recs[bad_n].write = write; bad_n++;
    if (sa1_off(addr) >= SA1_NULL_OFF)
        sa1_log("guest %s outside RAM: 0x%08x (base 0x%08x + %u) at frame %u; a NULL pointer: sent to the null page", write ? "write" : "read", addr, a, d, f);
    else
        sa1_log("guest %s outside RAM: 0x%08x (base 0x%08x + %u) at frame %u; masked into RAM offset 0x%x", write ? "write" : "read", addr, a, d, f, addr & SA1_RAM_MASK);
    if (sa1_badaddr_hook) sa1_badaddr_hook(addr);
    if (getenv("SA1_RAM_TRAP")) { sa1_log("SA1_RAM_TRAP set: aborting"); abort(); }
}

void sa1_badaddr_report(void) {
    if (!bad_n) { sa1_log("ram check: no guest access outside RAM"); return; }
    sa1_log("ram check: %u distinct guest addresses outside RAM%s (each is a seam entry to add):", bad_n, bad_other ? " (table full, more not listed)" : "");
    for (uint32_t i = 0; i < bad_n; i++) sa1_log("  0x%08x  %s, %u hits, first at frame %u", bad_recs[i].addr, bad_recs[i].write ? "write" : "read", bad_recs[i].hits, bad_recs[i].first_frame);
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
#ifdef SA1_RAM_CHECK
    sa1_badaddr_report();
#endif
}

/* ---- function-execution trace (-finstrument-functions) ------------------ */
#define TRACE_BUCKETS 65536
typedef struct { void *fn; uint32_t first, hits; } trace_rec;
static trace_rec *trace_tab;
static uint32_t trace_n;
uint32_t (*sa1_trace_frame)(void);

__attribute__((no_instrument_function))
void __cyg_profile_func_enter(void *fn, void *callsite) {
    (void)callsite;
    if (!trace_tab) {
        trace_tab = (trace_rec *)calloc(TRACE_BUCKETS, sizeof(trace_rec));
        if (!trace_tab) return;
    }
    uint32_t i = (uint32_t)(((uintptr_t)fn >> 4) * 2654435761u) & (TRACE_BUCKETS - 1);
    for (;;) {
        trace_rec *r = &trace_tab[i];
        if (r->fn == fn) { r->hits++; return; }
        if (r->fn == NULL) {
            if (trace_n + 1 >= TRACE_BUCKETS) return;
            r->fn = fn; r->hits = 1; r->first = sa1_trace_frame ? sa1_trace_frame() : 0;
            trace_n++;
            return;
        }
        i = (i + 1) & (TRACE_BUCKETS - 1);
    }
}

__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void *fn, void *callsite) { (void)fn; (void)callsite; }

void sa1_trace_dump(void (*cb)(void *fn, uint32_t first_frame, uint32_t hits, void *arg), void *arg) {
    if (!trace_tab) return;
    for (uint32_t i = 0; i < TRACE_BUCKETS; i++)
        if (trace_tab[i].fn) cb(trace_tab[i].fn, trace_tab[i].first, trace_tab[i].hits, arg);
}
