/* sh4_rt.h - runtime contract between recompiled SH-4 code and the HLE layer.
 *
 * Part of sa1-psp (MIT).  Written from the SH-4 software manual; no code
 * from any other recompiler.  Everything the emitter generates refers only
 * to the names in this header (see recomp/emitter/README.md, "ABI").
 *
 * Requirements on the C compiler: little-endian target, IEEE-754 float and
 * double, -fno-strict-aliasing or support for __attribute__((may_alias)),
 * and -ffp-contract=off so that fmac is two roundings like the hardware.
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

/* ---- guest RAM: one flat array, guest 0x?C000000 + off -> sa1_ram[off] --- */
#define SA1_RAM_SIZE  0x01000000u
#define SA1_RAM_SLACK 0x00010000u   /* so base-masked + small disp never overruns */
#define SA1_RAM_MASK  0x00FFFFFFu
extern uint8_t *sa1_ram;

typedef uint8_t  __attribute__((may_alias)) sa1_u8a;
typedef uint16_t __attribute__((may_alias)) sa1_u16a;
typedef uint32_t __attribute__((may_alias)) sa1_u32a;
typedef float    __attribute__((may_alias)) sa1_f32a;

/* The displacement is added after masking the base, which lets the compiler
 * share one masked base across @(disp,Rn) accesses.  Differs from hardware
 * only for an access whose base+disp crosses the 16 MB wrap, which nothing
 * does. */
#define RAMP(a, d)   (sa1_ram + (((uint32_t)(a) & SA1_RAM_MASK) + (d)))
#define RD8(a, d)    ((uint32_t)*(const sa1_u8a *)RAMP(a, d))
#define RD8S(a, d)   ((uint32_t)(int32_t)(int8_t)*(const sa1_u8a *)RAMP(a, d))
#define RD16(a, d)   ((uint32_t)*(const sa1_u16a *)RAMP(a, d))
#define RD16S(a, d)  ((uint32_t)(int32_t)(int16_t)*(const sa1_u16a *)RAMP(a, d))
#define RD32(a, d)   (*(const sa1_u32a *)RAMP(a, d))
#define RDF(a, d)    (*(const sa1_f32a *)RAMP(a, d))
#define WR8(a, d, v)  (*(sa1_u8a *)RAMP(a, d) = (uint8_t)(v))
#define WR16(a, d, v) (*(sa1_u16a *)RAMP(a, d) = (uint16_t)(v))
#define WR32(a, d, v) (*(sa1_u32a *)RAMP(a, d) = (uint32_t)(v))
#define WRF(a, d, v)  (*(sa1_f32a *)RAMP(a, d) = (v))

/* Accesses whose address is a compile-time constant outside RAM (hardware
 * registers the A-tail Katana pieces and the 8 TA-writing functions touch). */
uint32_t sa1_mmio_rd(uint32_t addr, int size);
void sa1_mmio_wr(uint32_t addr, int size, uint32_t value);

/* ---- control flow ------------------------------------------------------ */
void sa1_call(sh4_ctx *c, uint32_t addr);               /* function table / seam */
void sa1_trap(sh4_ctx *c, uint32_t pc, const char *what); /* unimplemented: logs, aborts */
sa1_fn sa1_fn_lookup(uint32_t addr);                   /* NULL when unknown */
void sa1_set_log_sink(void (*sink)(const char *line)); /* default: stderr */
void sa1_seam_stub(sh4_ctx *c, uint32_t addr, const char *name);
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

enum { SA1_SLOT_AREA, SA1_SLOT_EVENT, SA1_SLOT_COUNT };   /* 0x8C900000, 0x8CB80000 */
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
