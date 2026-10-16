"""Guest register numbering and per-instruction use/def sets.

Registers are numbered so that a set of them fits in a Python int bitmask:

  0..15   r0..r15
  16..31  FR0..FR15 of the *current* bank (logical)
  32..47  XF0..XF15, the other bank (logical)
  48..    T S Q M MACH MACL PR GBR FPUL FPSCR VBR

The FR/XF split is logical: which physical C local a logical FR maps to
depends on FPSCR.FR, which analysis.py tracks statically per block and
resolves with phys_map().  use/def sets depend on FPSCR.SZ (pair moves) and
FPSCR.PR (double-precision arithmetic), so effects() takes both.
"""

R = list(range(16))
FR0 = 16
XF0 = 32
T, S, Q, M, MACH, MACL, PR, GBR, FPUL, FPSCR, VBR = range(48, 59)
NREGS = 59
ALL = (1 << NREGS) - 1

SPECIAL_NAMES = {T: "t", S: "s", Q: "q", M: "m", MACH: "mach", MACL: "macl",
                 PR: "pr", GBR: "gbr", FPUL: "fpul", FPSCR: "fpscr", VBR: "vbr"}


def name(reg):
    """C local name of a *physical* register id."""
    if reg < 16:
        return "r%d" % reg
    if reg < 32:
        return "fr%d" % (reg - 16)
    if reg < 48:
        return "xf%d" % (reg - 32)
    return SPECIAL_NAMES[reg]


def ctx_field(reg):
    """Expression for the register inside the sh4_ctx struct."""
    if reg < 16:
        return "c->r[%d]" % reg
    if reg < 32:
        return "c->fr[%d]" % (reg - 16)
    if reg < 48:
        return "c->xf[%d]" % (reg - 32)
    return "c->" + SPECIAL_NAMES[reg]


def is_float(reg):
    return 16 <= reg < 48


def bit(reg):
    return 1 << reg


def bits(*regs):
    v = 0
    for r in regs:
        v |= 1 << r
    return v


def iter_bits(mask):
    i = 0
    while mask:
        if mask & 1:
            yield i
        mask >>= 1
        i += 1


def fr(n):
    return FR0 + n


def xf(n):
    return XF0 + n


def pair(n):
    """Logical regs of DRn / XDn addressed by a 4-bit field under SZ=1."""
    if n & 1:
        return bits(xf(n & ~1), xf((n & ~1) + 1))
    return bits(fr(n), fr(n + 1))


def dr(n):
    """Logical regs of DRn for PR=1 arithmetic (field low bit is 0)."""
    n &= ~1
    return bits(fr(n), fr(n + 1))


def fv(n):
    """FVn: n is already the 4-aligned base register number."""
    return bits(*(fr(n + k) for k in range(4)))


XF_ALL = bits(*(xf(k) for k in range(16)))
FR_ALL = bits(*(fr(k) for k in range(16)))
GPR_ALL = (1 << 16) - 1
# Registers the SH ABI treats as caller-saved; what a native HLE function
# may read (arguments, stack pointer) and write (results, scratch).
SEAM_IN = bits(4, 5, 6, 7, 15, GBR, *(fr(k) for k in range(4, 12)))
SEAM_OUT = bits(0, 1, 2, 3, 4, 5, 6, 7, T, MACH, MACL, FPUL,
                *(fr(k) for k in range(0, 12)))
# Callee-saved under the SH ABI: candidates for the push/pop idiom.
CALLEE_SAVED = bits(8, 9, 10, 11, 12, 13, 14, *(fr(k) for k in range(12, 16)))


def effects(ins, sz=0, pr=0):
    """Return (uses, defs) bitmasks for one instruction.

    sz/pr are the statically known FPSCR bits (0/1); pass 1 for the pair
    or double form when the state is unknown and the caller wants the
    conservative union (it should call twice and OR).
    """
    op = ins.op
    n, m = ins.n, ins.m
    u = d = 0

    def rn():
        return bit(n)

    def rm():
        return bit(m)

    # --- data transfer ---
    if op == "MOV_I" or op == "MOV_W_PC" or op == "MOV_L_PC":
        d = rn()
    elif op == "MOV":
        u, d = rm(), rn()
    elif op in ("MOV_B_S", "MOV_W_S", "MOV_L_S"):
        u = rm() | rn()
    elif op in ("MOV_B_L", "MOV_W_L", "MOV_L_L"):
        u, d = rm(), rn()
    elif op in ("MOV_B_SD", "MOV_W_SD", "MOV_L_SD"):
        u, d = rm() | rn(), rn()
    elif op in ("MOV_B_LI", "MOV_W_LI", "MOV_L_LI"):
        u, d = rm(), rn() | rm()
    elif op in ("MOV_B_S_D", "MOV_W_S_D"):
        u = bit(0) | rn()
    elif op == "MOV_L_S_D":
        u = rm() | rn()
    elif op in ("MOV_B_L_D", "MOV_W_L_D"):
        u, d = rm(), bit(0)
    elif op == "MOV_L_L_D":
        u, d = rm(), rn()
    elif op in ("MOV_B_S_R0", "MOV_W_S_R0", "MOV_L_S_R0"):
        u = bit(0) | rm() | rn()
    elif op in ("MOV_B_L_R0", "MOV_W_L_R0", "MOV_L_L_R0"):
        u, d = bit(0) | rm(), rn()
    elif op in ("MOV_B_S_G", "MOV_W_S_G", "MOV_L_S_G"):
        u = bit(0) | bit(GBR)
    elif op in ("MOV_B_L_G", "MOV_W_L_G", "MOV_L_L_G"):
        u, d = bit(GBR), bit(0)
    elif op == "MOVA":
        d = bit(0)
    elif op == "MOVT":
        u, d = bit(T), rn()
    elif op in ("SWAP_B", "SWAP_W", "XTRCT"):
        u, d = rm() | (rn() if op == "XTRCT" else 0), rn()
    # --- arithmetic ---
    elif op in ("ADD", "SUB", "MUL_L", "MULS_W", "MULU_W", "DMULS", "DMULU"):
        u = rm() | rn()
        d = rn() if op in ("ADD", "SUB") else bit(MACL) | (bit(MACH) if op in ("DMULS", "DMULU") else 0)
    elif op == "ADD_I":
        u, d = rn(), rn()
    elif op in ("ADDC", "SUBC"):
        u, d = rm() | rn() | bit(T), rn() | bit(T)
    elif op in ("ADDV", "SUBV"):
        u, d = rm() | rn(), rn() | bit(T)
    elif op == "CMP_EQ_I":
        u, d = bit(0), bit(T)
    elif op in ("CMP_EQ", "CMP_HS", "CMP_GE", "CMP_HI", "CMP_GT", "CMP_STR", "TST"):
        u, d = rm() | rn(), bit(T)
    elif op in ("CMP_PL", "CMP_PZ"):
        u, d = rn(), bit(T)
    elif op == "DIV1":
        u, d = rm() | rn() | bits(Q, M, T), rn() | bits(Q, T)
    elif op == "DIV0S":
        u, d = rm() | rn(), bits(Q, M, T)
    elif op == "DIV0U":
        d = bits(Q, M, T)
    elif op == "DT":
        u, d = rn(), rn() | bit(T)
    elif op in ("EXTS_B", "EXTS_W", "EXTU_B", "EXTU_W", "NEG", "NOT"):
        u, d = rm(), rn()
    elif op == "NEGC":
        u, d = rm() | bit(T), rn() | bit(T)
    elif op in ("MAC_L", "MAC_W"):
        u = rm() | rn() | bits(MACH, MACL, S)
        d = rm() | rn() | bits(MACH, MACL)
    # --- logic ---
    elif op in ("AND", "OR", "XOR"):
        u, d = rm() | rn(), rn()
    elif op in ("AND_I", "OR_I", "XOR_I"):
        u, d = bit(0), bit(0)
    elif op == "TST_I":
        u, d = bit(0), bit(T)
    elif op in ("AND_B_G", "OR_B_G", "XOR_B_G"):
        u = bit(0) | bit(GBR)
    elif op == "TST_B_G":
        u, d = bit(0) | bit(GBR), bit(T)
    elif op == "TAS_B":
        u, d = rn(), bit(T)
    # --- shifts ---
    elif op in ("ROTL", "ROTR", "SHAL", "SHAR", "SHLL", "SHLR"):
        u, d = rn(), rn() | bit(T)
    elif op in ("ROTCL", "ROTCR"):
        u, d = rn() | bit(T), rn() | bit(T)
    elif op in ("SHAD", "SHLD"):
        u, d = rm() | rn(), rn()
    elif op in ("SHLL2", "SHLR2", "SHLL8", "SHLR8", "SHLL16", "SHLR16"):
        u, d = rn(), rn()
    # --- branches ---
    elif op in ("BF", "BF_S", "BT", "BT_S"):
        u = bit(T)
    elif op in ("BRA",):
        pass
    elif op == "BSR":
        d = bit(PR)
    elif op in ("BRAF", "JMP"):
        u = rn()
    elif op in ("BSRF", "JSR"):
        u, d = rn(), bit(PR)
    elif op == "RTS":
        u = bit(PR)
    # --- system ---
    elif op == "CLRMAC":
        d = bits(MACH, MACL)
    elif op in ("CLRS", "SETS"):
        d = bit(S)
    elif op in ("CLRT", "SETT"):
        d = bit(T)
    elif op == "LDC_SR":
        u, d = rm(), bits(T, S, Q, M)
    elif op == "LDC_GBR":
        u, d = rm(), bit(GBR)
    elif op == "LDC_VBR":
        u, d = rm(), bit(VBR)
    elif op in ("LDC_SSR", "LDC_SPC", "LDC_DBR", "LDC_BANK"):
        u = rm()
    elif op == "LDCL_SR":
        u, d = rm(), rm() | bits(T, S, Q, M)
    elif op == "LDCL_GBR":
        u, d = rm(), rm() | bit(GBR)
    elif op == "LDCL_VBR":
        u, d = rm(), rm() | bit(VBR)
    elif op in ("LDCL_SSR", "LDCL_SPC", "LDCL_DBR", "LDCL_BANK"):
        u, d = rm(), rm()
    elif op in ("LDS_MACH", "LDS_MACL", "LDS_PR", "LDS_FPSCR", "LDS_FPUL"):
        u = rm()
        d = bit({"LDS_MACH": MACH, "LDS_MACL": MACL, "LDS_PR": PR,
                 "LDS_FPSCR": FPSCR, "LDS_FPUL": FPUL}[op])
    elif op in ("LDSL_MACH", "LDSL_MACL", "LDSL_PR", "LDSL_FPSCR", "LDSL_FPUL"):
        u = rm()
        d = rm() | bit({"LDSL_MACH": MACH, "LDSL_MACL": MACL, "LDSL_PR": PR,
                        "LDSL_FPSCR": FPSCR, "LDSL_FPUL": FPUL}[op])
    elif op in ("STC_SR",):
        u, d = bits(T, S, Q, M), rn()
    elif op == "STC_GBR":
        u, d = bit(GBR), rn()
    elif op == "STC_VBR":
        u, d = bit(VBR), rn()
    elif op in ("STC_SSR", "STC_SPC", "STC_SGR", "STC_DBR", "STC_BANK"):
        d = rn()
    elif op == "STCL_SR":
        u, d = rn() | bits(T, S, Q, M), rn()
    elif op == "STCL_GBR":
        u, d = rn() | bit(GBR), rn()
    elif op == "STCL_VBR":
        u, d = rn() | bit(VBR), rn()
    elif op in ("STCL_SSR", "STCL_SPC", "STCL_SGR", "STCL_DBR", "STCL_BANK"):
        u, d = rn(), rn()
    elif op in ("STS_MACH", "STS_MACL", "STS_PR", "STS_FPSCR", "STS_FPUL"):
        u = bit({"STS_MACH": MACH, "STS_MACL": MACL, "STS_PR": PR,
                 "STS_FPSCR": FPSCR, "STS_FPUL": FPUL}[op])
        d = rn()
    elif op in ("STSL_MACH", "STSL_MACL", "STSL_PR", "STSL_FPSCR", "STSL_FPUL"):
        u = rn() | bit({"STSL_MACH": MACH, "STSL_MACL": MACL, "STSL_PR": PR,
                        "STSL_FPSCR": FPSCR, "STSL_FPUL": FPUL}[op])
        d = rn()
    elif op in ("MOVCA_L",):
        u = bit(0) | rn()
    elif op in ("OCBI", "OCBP", "OCBWB", "PREF"):
        u = rn()
    elif op in ("NOP", "LDTLB", "RTE", "SLEEP", "TRAPA", "INVALID"):
        pass
    # --- floating point ---
    elif op in ("FLDI0", "FLDI1"):
        d = bit(fr(n))
    elif op == "FMOV":
        if sz:
            u, d = pair(m), pair(n)
        else:
            u, d = bit(fr(m)), bit(fr(n))
    elif op in ("FMOV_L", "FMOV_L_R0", "FMOV_LI"):
        u = rm() | (bit(0) if op == "FMOV_L_R0" else 0)
        d = pair(n) if sz else bit(fr(n))
        if op == "FMOV_LI":
            d |= rm()
    elif op in ("FMOV_S", "FMOV_SD", "FMOV_S_R0"):
        u = rn() | (bit(0) if op == "FMOV_S_R0" else 0) | (pair(m) if sz else bit(fr(m)))
        if op == "FMOV_SD":
            d = rn()
    elif op == "FLDS":
        u, d = bit(fr(m)), bit(FPUL)
    elif op == "FSTS":
        u, d = bit(FPUL), bit(fr(n))
    elif op in ("FABS", "FNEG", "FSQRT"):
        u = d = dr(n) if pr else bit(fr(n))
    elif op in ("FADD", "FSUB", "FMUL", "FDIV"):
        if pr:
            u, d = dr(m) | dr(n), dr(n)
        else:
            u, d = bit(fr(m)) | bit(fr(n)), bit(fr(n))
    elif op in ("FCMP_EQ", "FCMP_GT"):
        u = (dr(m) | dr(n)) if pr else (bit(fr(m)) | bit(fr(n)))
        d = bit(T)
    elif op == "FLOAT":
        u, d = bit(FPUL), (dr(n) if pr else bit(fr(n)))
    elif op == "FTRC":
        u, d = (dr(m) if pr else bit(fr(m))), bit(FPUL)
    elif op == "FMAC":
        u, d = bit(fr(0)) | bit(fr(m)) | bit(fr(n)), bit(fr(n))
    elif op == "FCNVDS":
        u, d = dr(m), bit(FPUL)
    elif op == "FCNVSD":
        u, d = bit(FPUL), dr(n)
    elif op == "FIPR":
        u, d = fv(m * 4) | fv(n * 4), bit(fr(n * 4 + 3))
    elif op == "FTRV":
        u, d = XF_ALL | fv(n * 4), fv(n * 4)
    elif op == "FRCHG":
        u, d = bit(FPSCR), bit(FPSCR)   # bank rename is handled statically
    elif op == "FSCHG":
        u, d = bit(FPSCR), bit(FPSCR)
    elif op == "FSCA":
        u, d = bit(FPUL), bits(fr(n * 2), fr(n * 2 + 1))
    elif op == "FSRRA":
        u = d = bit(fr(n))
    else:
        raise KeyError("effects: unhandled op %s" % op)
    return u, d


def phys_map(mask, frbank):
    """Map logical FR/XF bits to physical ones given FPSCR.FR."""
    if not frbank:
        return mask
    fr_part = (mask >> FR0) & 0xFFFF
    xf_part = (mask >> XF0) & 0xFFFF
    rest = mask & ~(FR_ALL | XF_ALL)
    return rest | (xf_part << FR0) | (fr_part << XF0)
