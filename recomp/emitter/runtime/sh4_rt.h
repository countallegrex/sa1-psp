/* sh4_rt.h - runtime contract between recompiled SH-4 code and the HLE layer.
 *
 * Part of sa1-psp (MIT).  Written from the SH-4 software manual; no code
 * from any other recompiler.  Everything the emitter generates refers only
 * to the names in this header (see recomp/emitter/README.md, "ABI").
 *
 * Requirements on the C compiler: little-endian target, IEEE-754 float and
 * double, -fno-strict-aliasing or support for __attribute__((may_alias)),
 * and -ffp-contract=off (fmac goes through FMAC_F below, never a contraction).
 */
#ifndef SH4_RT_H
#define SH4_RT_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- guest register file: what lives in memory between C functions ---- */
typedef struct sh4_ctx {
    uint32_t r[16];
    float fr[16];      /* FPSCR.FR = 0 bank */
    float xf[16];      /* FPSCR.FR = 1 bank; XMTRX for ftrv */
    uint32_t t, s, q, m;
    uint32_t mach, macl, pr, gbr, fpul, fpscr, vbr;
} sh4_ctx;

typedef void (*sa1_fn)(sh4_ctx *c);

typedef struct sa1_fn_entry {
    uint32_t addr;     /* physical (29-bit) guest address of the entry */
    sa1_fn fn;
} sa1_fn_entry;

/* ---- guest RAM: one flat array, guest 0x?C000000 + off -> sa1_ram[off] ---
 * The console's 16 MB are followed by SA1_RAM_EXTRA bytes the HLE uses for
 * the KAMUI vertex buffers (guest addresses 0x8D000000 + off, which the
 * game never forms itself; on the console they mirror 0x8C000000).  The
 * mask therefore keeps 25 bits; an access above 16 MB + EXTRA is
 * undefined, as it would be on the console. */
#define SA1_RAM_SIZE  0x01000000u
#define SA1_RAM_EXTRA 0x00200000u
#define SA1_RAM_SLACK 0x00010000u   /* so base-masked + small disp never overruns */
#define SA1_RAM_MASK  0x01FFFFFFu
/* The desktop allocates the whole 32 MB window the mask can reach (untouched
 * pages cost nothing there): a few game accesses to control registers
 * (0xFFxxxxxx) through a register, not a constant, mask into its top.  The
 * PSP allocates SIZE + EXTRA only (SA1_COMPACT_RAM below). */
/* The null page.  Area 0 (guest 0x00000000-0x01FFFFFF, the boot ROM, and
 * its P1 alias 0x80000000) is where a NULL pointer points.  On the console
 * a write there is dropped and a read returns ROM bytes; under the flat
 * mask alone both would hit RAM offset 0, where the BIOS syscall vectors
 * live (0x8C0000B0-BC, E0): the Emerald Coast dolphin's motion callback
 * writes a 256-byte block through a NULL pointer, and the second exit-test
 * run died 4,800 frames later calling through a clobbered vector
 * (docs/m2-render-report.md section 9).  So area-0 accesses go to their
 * own 2 MB above the window: a read sees what was written there, never
 * RAM.  The test (a & 0x3E000000) == 0 catches P0 and P1 area 0 and leaves
 * the store queues (0xE0...) and RAM (0x0C/0x8C/0xAC) on the mask; P2 area
 * 0 (0xA0...) is not caught (the game never forms it).  One test and a
 * conditional move per access; constant addresses fold away. */
#define SA1_NULL_SIZE 0x00200000u
/* The PSP (SA1_COMPACT_RAM, on by default there) cannot spend 14 MB on the
 * unused top of the 32 MB window: it allocates RAM + EXTRA, then the null
 * page, 20 MB in all, and sends every masked offset past RAM + EXTRA (the
 * control-register accesses above) to the null page as well.  One compare
 * more per access than the desktop's layout; constant addresses still fold. */
#if defined(__psp__) && !defined(SA1_COMPACT_RAM)
#define SA1_COMPACT_RAM 1
#endif
#ifdef SA1_COMPACT_RAM
#define SA1_NULL_OFF  (SA1_RAM_SIZE + SA1_RAM_EXTRA + SA1_RAM_SLACK)
#define SA1_RAM_ALLOC (SA1_NULL_OFF + SA1_NULL_SIZE + SA1_RAM_SLACK)
extern uint8_t *sa1_ram;
/* SA1_OFF_AREA_TEST_ONLY (the PSP's recompiled game code, unless
 * SA1_PSP_GAME_FULL_OFF): area 0 still goes to the null page, but the
 * compare against RAM + EXTRA is left out.  It cost 6.8 ms of a 74 ms
 * Station Square frame on the PSP-3001 (2026-10-21), and the game code
 * never had it before then (it built against a stale copy of this header):
 * a masked offset past RAM + EXTRA, which no run has shown, would land past
 * the allocation, as it always did there. */
static inline uint32_t sa1_off(uint32_t a) {
    uint32_t off = a & SA1_RAM_MASK;
#ifdef SA1_OFF_AREA_TEST_ONLY
    return (a & 0x3E000000u) ? off : (a & (SA1_NULL_SIZE - 1u)) + SA1_NULL_OFF;
#else
    return ((a & 0x3E000000u) && off < SA1_RAM_SIZE + SA1_RAM_EXTRA) ? off : (a & (SA1_NULL_SIZE - 1u)) + SA1_NULL_OFF;
#endif
}
#else
#define SA1_NULL_OFF  (2u * SA1_RAM_SIZE)
#define SA1_RAM_ALLOC (2u * SA1_RAM_SIZE + SA1_NULL_SIZE + SA1_RAM_SLACK)
extern uint8_t *sa1_ram;
static inline uint32_t sa1_off(uint32_t a) {
    uint32_t off = a & SA1_RAM_MASK;
    return (a & 0x3E000000u) ? off : (a & (SA1_NULL_SIZE - 1u)) + SA1_NULL_OFF;
}
#endif

typedef uint8_t  __attribute__((may_alias)) sa1_u8a;
typedef uint16_t __attribute__((may_alias)) sa1_u16a;
typedef uint32_t __attribute__((may_alias)) sa1_u32a;
typedef float    __attribute__((may_alias)) sa1_f32a;

/* The displacement is added after masking the base, which lets the compiler
 * share one masked base across @(disp,Rn) accesses.  Differs from hardware
 * only for an access whose base+disp crosses the 16 MB wrap, which nothing
 * does. */
#ifdef SA1_RAM_CHECK
/* Debug builds (the desktop Debug configuration, or -DSA1_RAM_CHECK=ON):
 * every access checks that the guest address is RAM (P0/P1/P2 spellings
 * of the 16 MB plus the HLE's extra area) or a store-queue address (the
 * TA writers).  Anything else, a hardware register reached through a
 * register the emitter could not see (the M2 hang, docs/m2-render-report.md
 * section 7), would silently land in RAM under the mask: it is logged once
 * per address with the guest call stack (sa1_badaddr_hook), counted, and
 * listed by sa1_seam_report; SA1_RAM_TRAP=1 aborts on the first one.  The
 * access itself goes where every build sends it (the masked RAM byte, or
 * the null page for area 0), so a run behaves as a release one. */
void sa1_ram_badaddr(uint32_t a, uint32_t d, int write);
static inline int sa1_addr_ok(uint32_t a) {
    return ((a & 0xDFFFFFFFu) - 0x8C000000u) < SA1_RAM_SIZE + SA1_RAM_EXTRA   /* 0x8C/0xAC: RAM and the extra area */
        || (a - 0x0C000000u) < SA1_RAM_SIZE + SA1_RAM_EXTRA                   /* 0x0C: physical */
        || (a & 0xFC000000u) == 0xE0000000u;                                   /* store queues */
}
static inline uint8_t *sa1_ramp_checked(uint32_t a, int32_t d, int write) {
    if (__builtin_expect(!sa1_addr_ok(a), 0)) sa1_ram_badaddr(a, (uint32_t)d, write);
    return sa1_ram + (sa1_off((uint32_t)(a)) + d);
}
#define RAMP(a, d)   sa1_ramp_checked((uint32_t)(a), (int32_t)(d), 0)
#define RAMPW(a, d)  sa1_ramp_checked((uint32_t)(a), (int32_t)(d), 1)
#else
#define RAMP(a, d)   (sa1_ram + (sa1_off((uint32_t)(a)) + (d)))
#define RAMPW(a, d)  RAMP(a, d)
#endif
#define RD8(a, d)    ((uint32_t)*(const sa1_u8a *)RAMP(a, d))
#define RD8S(a, d)   ((uint32_t)(int32_t)(int8_t)*(const sa1_u8a *)RAMP(a, d))
#define RD16(a, d)   ((uint32_t)*(const sa1_u16a *)RAMP(a, d))
#define RD16S(a, d)  ((uint32_t)(int32_t)(int16_t)*(const sa1_u16a *)RAMP(a, d))
#define RD32(a, d)   (*(const sa1_u32a *)RAMP(a, d))
#define RDF(a, d)    (*(const sa1_f32a *)RAMP(a, d))
#define WR8(a, d, v)  (*(sa1_u8a *)RAMPW(a, d) = (uint8_t)(v))
#define WR16(a, d, v) (*(sa1_u16a *)RAMPW(a, d) = (uint16_t)(v))
#define WR32(a, d, v) (*(sa1_u32a *)RAMPW(a, d) = (uint32_t)(v))
#define WRF(a, d, v)  (*(sa1_f32a *)RAMPW(a, d) = (v))

/* Accesses whose address is a compile-time constant outside RAM (hardware
 * registers the A-tail Katana pieces and the 8 TA-writing functions touch). */
uint32_t sa1_mmio_rd(uint32_t addr, int size);
void sa1_mmio_wr(uint32_t addr, int size, uint32_t value);
/* The HLE layer serves the few registers the game touches directly (timer
 * unit, cable pins, G2 FIFO status); set handled when the address is one. */
extern uint32_t (*sa1_mmio_hook)(uint32_t addr, int size, int *handled);
extern void (*sa1_mmio_wr_hook)(uint32_t addr, int size, uint32_t value, int *handled);

/* ---- control flow ------------------------------------------------------ */
void sa1_call(sh4_ctx *c, uint32_t addr);               /* function table / seam */
void sa1_trap(sh4_ctx *c, uint32_t pc, const char *what); /* unimplemented: logs, aborts */
sa1_fn sa1_fn_lookup(uint32_t addr);                   /* NULL when unknown */
/* A jump out of a function (bra / jmp to another function, or a jmp @Rn)
 * is a tail call, and it must stay one: the game writes loops as jumps
 * between code the emitter split into functions, and each nested host call
 * would cost a frame (the printf-style formatter 0x8C095B60 looped 1,786
 * times through four such jumps and overflowed the root coroutine's 1 MB
 * host stack in the maintainer's fourth exit-test run: docs/m2-render-report.md
 * section 10).  clang's musttail guarantees the jump at every -O level and
 * under -finstrument-functions, and GCC has it from 15 (psp-gcc 15.2 emits
 * a plain j at -O0 and -Os); other compilers get a plain call.
 * sa1_tail_target does what sa1_call does before the call (the hooks and
 * the lookup; it traps and returns NULL for an unknown address). */
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 15)
#define SA1_TAIL(call) { __attribute__((musttail)) return call; }   /* a block: after a label the bare attribute reads as a declaration */
#else
#define SA1_TAIL(call) do { call; return; } while (0)
#endif
sa1_fn sa1_tail_target(sh4_ctx *c, uint32_t addr);
void sa1_set_log_sink(void (*sink)(const char *line)); /* default: stderr */
void sa1_log(const char *fmt, ...) __attribute__((format(printf, 1, 2))); /* one line to the sink */
void sa1_seam_stub(sh4_ctx *c, uint32_t addr, const char *name);
/* Per-entry hit counters behind sa1_seam_stub and sa1_seam_hit: the log
 * prints the busiest callees at exit (sa1_seam_report).  A real HLE body
 * calls sa1_seam_hit so it is counted too. */
void sa1_seam_hit(uint32_t addr, const char *name);
/* The same with the counter's slot cached at the call site (HLE_ENTER): *site
 * starts NULL and is filled on the first call. */
struct sa1_hit;
void sa1_seam_hit_at(struct sa1_hit **site, uint32_t addr, const char *name);
void sa1_seam_report(int top);
/* Native functions installed at run time for guest addresses that have no
 * emitted code (BIOS syscall vectors, host-provided callbacks).  Checked
 * before the overlay slots and the main table. */
void sa1_register_fn(uint32_t addr, sa1_fn fn);
/* Debug aid: when sa1_watch_addr is set, sa1_watch_check() logs (once per
 * change) whenever the 32-bit word there differs from its last value.  The
 * runtime calls it at every seam hit and indirect call; the backend may
 * call it per frame. */
extern uint32_t sa1_watch_addr;          /* set directly: one plain address, or use sa1_watch_add */
/* Up to 8 watches; a spec is "addr[:off[:off...]]": the chain of 32-bit
 * loads *(…*(*(addr) + off) + off…), logged whenever its value changes. */
int sa1_watch_add(const char *spec);
void sa1_watch_check(const char *where);
/* Function-execution trace (debug builds compiled with -finstrument-functions):
 * records every host function entered, with the guest frame it was first
 * seen in and a hit count.  sa1_trace_dump calls back once per function. */
void sa1_trace_dump(void (*cb)(void *fn, uint32_t first_frame, uint32_t hits, void *arg), void *arg);
extern uint32_t (*sa1_trace_frame)(void);
/* Called by sa1_trap before abort(), if set (the backend prints its report). */
extern void (*sa1_trap_hook)(sh4_ctx *c, uint32_t pc, const char *what);
/* Called at every indirect call and HLE entry, if set: the backend's host
 * stack probe (depth high-water mark; traps before an unbounded guest
 * recursion overflows the host stack). */
extern void (*sa1_depth_hook)(void);

/* HLE_ENTER's common case inline: an entry other than the last one counted
 * only bumps its counter and restarts the spin check, which is all
 * sa1_seam_hit_at does then besides the stack probe (that one still runs at
 * every repeat and at every indirect call).  A repeat, a watch, or the first
 * call of a site (slot not cached yet) take the full path. */
struct sa1_hit { uint32_t addr; uint32_t hits; const char *name; int logged; };
struct sa1_hit_spin { uint32_t addr, frame, repeat; };
extern struct sa1_hit_spin sa1_hit_spin;
static inline void sa1_seam_enter(struct sa1_hit **site, uint32_t addr, const char *name) {
    struct sa1_hit *h = *site;
    if (__builtin_expect(h != 0 && addr != sa1_hit_spin.addr && !sa1_watch_addr, 1)) {
        h->hits++;
        sa1_hit_spin.addr = addr; sa1_hit_spin.frame = UINT32_MAX; sa1_hit_spin.repeat = 0;
        return;
    }
    sa1_seam_hit_at(site, addr, name);
}
/* Called on the first access to each guest address outside RAM (SA1_RAM_CHECK
 * builds); the desktop backend prints the host backtrace, which names the
 * recompiled functions on the guest call stack. */
extern void (*sa1_badaddr_hook)(uint32_t addr);
void sa1_badaddr_report(void);     /* the addresses hit so far, with counts (no-op without SA1_RAM_CHECK) */
extern const sa1_fn_entry sa1_fn_table[];
extern const uint32_t sa1_fn_count;
extern const sa1_fn_entry sa1_seam_table[];
extern const uint32_t sa1_seam_count;

/* ---- code overlays (docs/m1-overlays.md) --------------------------------
 * The game loads code from disc into fixed slots, one file at a time per
 * slot.  Each recompiled overlay has its own function table; the loader hook
 * installs it when the game copies the file in and clears it (NULL) when the
 * slot is reused.  sa1_fn_lookup checks the installed overlays (by their code
 * range) before the main table. */
typedef struct sa1_overlay {
    const char *name;            /* disc file, e.g. "ADV00" */
    uint32_t code_lo, code_hi;   /* physical range of the overlay's code */
    const sa1_fn_entry *fns;     /* sorted by addr */
    uint32_t count;
} sa1_overlay;

/* 0x8C900000, 0x8CB80000, and 0x8C920000, where the Egg Carrier field
 * (ADV0100/ADV0130) loads its code next to ADV01OBJ in the area slot. */
enum { SA1_SLOT_AREA, SA1_SLOT_EVENT, SA1_SLOT_AREA2, SA1_SLOT_COUNT };
void sa1_overlay_set(int slot, const sa1_overlay *ov);
const sa1_overlay *sa1_overlay_get(int slot);

/* ---- integer helpers (operate on the caller's locals by name) ---------- */
#define ADDC(n, m) do { uint32_t t0_ = (n) + (m); uint32_t c_ = t0_ < (n); \
    (n) = t0_ + t; t = c_ | ((n) < t0_); } while (0)
#define ADDV(n, m) do { int32_t a_ = (int32_t)(n), b_ = (int32_t)(m); \
    int32_t r_ = (int32_t)((uint32_t)a_ + (uint32_t)b_); \
    t = ((a_ ^ r_) & (b_ ^ r_)) < 0; (n) = (uint32_t)r_; } while (0)
#define SUBC(n, m) do { uint32_t t0_ = (n) - (m); uint32_t c_ = t0_ > (n); \
    uint32_t r_ = t0_ - t; t = c_ | (r_ > t0_); (n) = r_; } while (0)
#define SUBV(n, m) do { int32_t a_ = (int32_t)(n), b_ = (int32_t)(m); \
    int32_t r_ = (int32_t)((uint32_t)a_ - (uint32_t)b_); \
    t = ((a_ ^ b_) & (a_ ^ r_)) < 0; (n) = (uint32_t)r_; } while (0)
#define NEGC(n, m) do { uint32_t t0_ = 0u - (m); uint32_t r_ = t0_ - t; \
    t = (0u < t0_) | (t0_ < r_); (n) = r_; } while (0)
#define DIV0U() do { q = 0; m = 0; t = 0; } while (0)
#define DIV0S(n, mm) do { q = (n) >> 31; m = (mm) >> 31; t = q ^ m; } while (0)
/* DIV1, step of the restoring division from the SH-4 manual. */
#define DIV1(n, mm) do { \
    uint32_t oq_ = q, t0_, t1_; \
    q = (n) >> 31; (n) = ((n) << 1) | t; \
    if (oq_ == m) { t0_ = (n); (n) -= (mm); t1_ = (n) > t0_; } \
    else          { t0_ = (n); (n) += (mm); t1_ = (n) < t0_; } \
    if (oq_ == 0) { q = (m == 0) ? (q ? !t1_ : t1_) : (q ? t1_ : !t1_); } \
    else          { q = (m == 0) ? (q ? !t1_ : t1_) : (q ? t1_ : !t1_); } \
    t = (q == m); } while (0)
#define ROTCL(n) do { uint32_t c_ = (n) >> 31; (n) = ((n) << 1) | t; t = c_; } while (0)
#define ROTCR(n) do { uint32_t c_ = (n) & 1; (n) = ((n) >> 1) | (t << 31); t = c_; } while (0)
#define SHAD(n, m) do { uint32_t s_ = (m); if (!(s_ & 0x80000000u)) (n) <<= (s_ & 31); \
    else if ((s_ & 31) == 0) (n) = (uint32_t)((int32_t)(n) >> 31); \
    else (n) = (uint32_t)((int32_t)(n) >> ((~s_ & 31) + 1)); } while (0)
#define SHLD(n, m) do { uint32_t s_ = (m); if (!(s_ & 0x80000000u)) (n) <<= (s_ & 31); \
    else if ((s_ & 31) == 0) (n) = 0; \
    else (n) >>= ((~s_ & 31) + 1); } while (0)
#define CMPSTR(n, m) do { uint32_t x_ = (n) ^ (m); \
    t = !(x_ & 0xFF) || !(x_ & 0xFF00) || !(x_ & 0xFF0000) || !(x_ & 0xFF000000u); } while (0)
/* mac.l: 64-bit accumulate, 48-bit saturation when S=1. */
#define MACL_STEP(a, b) do { \
    int64_t p_ = (int64_t)(int32_t)(a) * (int64_t)(int32_t)(b); \
    int64_t acc_ = (int64_t)(((uint64_t)mach << 32) | macl); \
    if (s) { acc_ = (int64_t)(((uint64_t)(mach & 0xFFFF) << 32) | macl); \
             if (mach & 0x8000) acc_ -= (int64_t)1 << 48; \
             acc_ += p_; \
             if (acc_ > (int64_t)0x00007FFFFFFFFFFFLL) acc_ = (int64_t)0x00007FFFFFFFFFFFLL; \
             else if (acc_ < -((int64_t)1 << 47)) acc_ = -((int64_t)1 << 47); } \
    else acc_ += p_; \
    mach = (uint32_t)((uint64_t)acc_ >> 32); macl = (uint32_t)acc_; } while (0)
/* mac.w: 16x16 products; S=1 saturates to 32 bits in MACL and flags MACH. */
#define MACW_STEP(a, b) do { \
    int32_t p_ = (int32_t)(int16_t)(a) * (int32_t)(int16_t)(b); \
    if (s) { int64_t r_ = (int64_t)(int32_t)macl + p_; \
             if (r_ > 0x7FFFFFFFLL) { macl = 0x7FFFFFFFu; mach |= 1; } \
             else if (r_ < -0x80000000LL) { macl = 0x80000000u; mach |= 1; } \
             else macl = (uint32_t)r_; } \
    else { int64_t acc_ = (int64_t)(((uint64_t)mach << 32) | macl) + p_; \
           mach = (uint32_t)((uint64_t)acc_ >> 32); macl = (uint32_t)acc_; } } while (0)

/* ---- floating point helpers ------------------------------------------- */
static inline uint32_t sh4_f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float sh4_u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline double sh4_dr_get(float hi, float lo) {
    uint64_t u = ((uint64_t)sh4_f2u(hi) << 32) | sh4_f2u(lo); double d; memcpy(&d, &u, 8); return d;
}
#define DR_SET(hi, lo, v) do { double d_ = (v); uint64_t u_; memcpy(&u_, &d_, 8); \
    (hi) = sh4_u2f((uint32_t)(u_ >> 32)); (lo) = sh4_u2f((uint32_t)u_); } while (0)
#define F2U(f) sh4_f2u(f)
#define U2F(u) sh4_u2f(u)
#define DR(hi, lo) sh4_dr_get(hi, lo)
/* ftrc: round toward zero, saturating like the hardware (NaN -> INT_MIN). */
static inline uint32_t sh4_ftrc(float f) {
    if (!(f == f)) return 0x80000000u;
    if (f >= 2147483648.0f) return 0x7FFFFFFFu;
    if (f <= -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)f;
}
static inline uint32_t sh4_ftrc_d(double d) {
    if (!(d == d)) return 0x80000000u;
    if (d >= 2147483648.0) return 0x7FFFFFFFu;
    if (d <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)d;
}
#define FTRC(f) sh4_ftrc(f)
#define FTRC_D(d) sh4_ftrc_d(d)
/* fmac FR0,FRm,FRn: FRn = FR0 * FRm + FRn.  Flycast (the oracle) computes it
 * fused, one rounding (std::fma); with SH4_FMAC_FUSED (the desktop build) so
 * do we: the product of two floats is exact in a double, and under the
 * game's round-toward-zero (FPSCR.RM = 1) truncating the double sum and then
 * the float is one truncation, so the double form is the fused result
 * exactly (round-to-nearest tapes: off by one ulp in rare ties).  Without it
 * (the PSP, where doubles are soft-float): two roundings. */
#ifdef SH4_FMAC_FUSED
#define FMAC_F(a, b, c) ((float)((double)(a) * (double)(b) + (double)(c)))
#else
#define FMAC_F(a, b, c) ((a) * (b) + (c))
#endif
/* fsca: angle is the low 16 bits of FPUL, 65536 = one turn. */
#define FSCA(fp, sn, cs) do { float a_ = (float)((fp) & 0xFFFFu) * (float)(6.283185307179586 / 65536.0); \
    (sn) = sinf(a_); (cs) = cosf(a_); } while (0)
#define FSRRA(x) (1.0f / sqrtf(x))
#define FIPR(a0, a1, a2, a3, b0, b1, b2, b3) \
    ((a0) * (b0) + (a1) * (b1) + (a2) * (b2) + (a3) * (b3))
/* FTRV: v = XMTRX * v, XMTRX(row i, col j) = xf[j*4+i]. */
#define FTRV(x, v0, v1, v2, v3) do { float a_ = (v0), b_ = (v1), c_ = (v2), d_ = (v3); \
    (v0) = x##0 * a_ + x##4 * b_ + x##8  * c_ + x##12 * d_; \
    (v1) = x##1 * a_ + x##5 * b_ + x##9  * c_ + x##13 * d_; \
    (v2) = x##2 * a_ + x##6 * b_ + x##10 * c_ + x##14 * d_; \
    (v3) = x##3 * a_ + x##7 * b_ + x##11 * c_ + x##15 * d_; } while (0)
#define FABS_F(f) sh4_u2f(sh4_f2u(f) & 0x7FFFFFFFu)
#define FNEG_F(f) sh4_u2f(sh4_f2u(f) ^ 0x80000000u)

/* FPSCR bits */
#define FPSCR_PR 0x00080000u
#define FPSCR_SZ 0x00100000u
#define FPSCR_FR 0x00200000u
#define SR_FROM_FLAGS() (0x40000000u | (m << 9) | (q << 8) | (s << 1) | t)

#ifdef __cplusplus
}
#endif
#endif /* SH4_RT_H */
