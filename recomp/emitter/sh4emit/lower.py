"""Per-instruction lowering: one SH-4 instruction -> C statements.

Every GPR is a uint32_t local r0..r15, every FP register a float local
(fr0..fr15 for bank 0, xf0..xf15 for bank 1), the flags are uint32_t
locals t/s/q/m, plus mach, macl, pr, gbr, fpul, fpscr, vbr.  Memory goes
through the RD*/WR* macros of runtime/sh4_rt.h.  Branches, calls and
returns are not lowered here: emit.py handles block terminators.

The lowering table in README.md is generated from LOWERING_DOC below."""

from .analysis import U
from .image import phys
from . import regs

RAM_RANGES = ((0x0C000000, 0x10000000),)   # physical: main RAM and its mirrors


def is_ram_const(addr):
    a = phys(addr)
    return any(lo <= a < hi for lo, hi in RAM_RANGES)


class Lowerer:
    def __init__(self, env, state_fn, notes):
        self.env = env
        self.frbank = 0
        self.pr = 0
        self.sz = 0
        self.notes = notes
        self.rt_pr = 0   # runtime-branched FP ops emitted
        self.rt_sz = 0

    # ---- register naming ------------------------------------------------
    def F(self, n):
        """C local for logical FRn under the current bank."""
        return ("xf%d" if self.frbank else "fr%d") % n

    def X(self, n):
        """C local for logical XFn under the current bank."""
        return ("fr%d" if self.frbank else "xf%d") % n

    def FP(self, field):
        """Pair (hi, lo) locals for a DR/XD selected by a 4-bit field under SZ=1."""
        base = field & ~1
        if field & 1:
            return self.X(base), self.X(base + 1)
        return self.F(base), self.F(base + 1)

    def DRn(self, field):
        base = field & ~1
        return self.F(base), self.F(base + 1)

    def set_state(self, st):
        pr, sz, fr = st
        self.pr, self.sz = pr, sz
        self.frbank = 0 if fr == U else fr

    # ---- memory access expressions ----------------------------------------
    def rd(self, size, base, disp, consts, signed=False):
        """Read expression; `base` is a C expression or a constant int."""
        macro = {1: "RD8", 2: "RD16", 4: "RD32", "f": "RDF"}[size]
        if signed:
            macro += "S"
        if isinstance(base, int):
            addr = (base + disp) & 0xFFFFFFFF
            if not is_ram_const(addr):
                cast = "(uint32_t)(int32_t)(int%d_t)" % (size * 8) if signed else ""
                if size == "f":
                    return "U2F(sa1_mmio_rd(0x%08xu, 4))" % addr
                return "%ssa1_mmio_rd(0x%08xu, %d)" % (cast, addr, size)
            return "%s(0x%08xu, 0)" % (macro, addr)
        return "%s(%s, %d)" % (macro, base, disp)

    def wr(self, size, base, disp, value):
        macro = {1: "WR8", 2: "WR16", 4: "WR32", "f": "WRF"}[size]
        if isinstance(base, int):
            addr = (base + disp) & 0xFFFFFFFF
            if not is_ram_const(addr):
                if size == "f":
                    return "sa1_mmio_wr(0x%08xu, 4, F2U(%s));" % (addr, value)
                return "sa1_mmio_wr(0x%08xu, %d, %s);" % (addr, size, value)
            return "%s(0x%08xu, 0, %s);" % (macro, addr, value)
        return "%s(%s, %d, %s);" % (macro, base, disp, value)

    def base(self, reg, consts):
        v = consts.get(reg)
        return v if v is not None else "r%d" % reg

    # ---- the table -------------------------------------------------------
    def lower(self, ins, consts, pc_after):
        """Return a list of C statements for a non-branch instruction.

        consts: GPR constants known before `ins`.  pc_after: address of the
        instruction after the branch group (for pr on calls; unused here)."""
        op = ins.op
        n, m, d = ins.n, ins.m, ins.d
        rn, rm = "r%d" % n, "r%d" % m
        env = self.env
        out = []
        E = out.append

        # ------------------------------------------------ data transfer
        if op == "MOV_I":
            E("%s = 0x%08xu;" % (rn, ins.simm & 0xFFFFFFFF))
        elif op in ("MOV_W_PC", "MOV_L_PC"):
            v = env.pool_word(ins)
            if v is not None:
                E("%s = 0x%08xu;" % (rn, v))
            else:
                a = ins.pcrel_address()
                E("%s = %s;" % (rn, self.rd(2 if op == "MOV_W_PC" else 4, a, 0, consts,
                                            signed=(op == "MOV_W_PC"))))
        elif op == "MOVA":
            E("r0 = 0x%08xu;" % ins.pcrel_address())
        elif op == "MOV":
            E("%s = %s;" % (rn, rm))
        elif op in ("MOV_B_S", "MOV_W_S", "MOV_L_S"):
            size = {"MOV_B_S": 1, "MOV_W_S": 2, "MOV_L_S": 4}[op]
            E(self.wr(size, self.base(n, consts), 0, rm))
        elif op in ("MOV_B_L", "MOV_W_L", "MOV_L_L"):
            size = {"MOV_B_L": 1, "MOV_W_L": 2, "MOV_L_L": 4}[op]
            E("%s = %s;" % (rn, self.rd(size, self.base(m, consts), 0, consts, signed=size < 4)))
        elif op in ("MOV_B_SD", "MOV_W_SD", "MOV_L_SD"):
            size = {"MOV_B_SD": 1, "MOV_W_SD": 2, "MOV_L_SD": 4}[op]
            E("%s -= %d;" % (rn, size))
            E(self.wr(size, rn, 0, rm))
        elif op in ("MOV_B_LI", "MOV_W_LI", "MOV_L_LI"):
            size = {"MOV_B_LI": 1, "MOV_W_LI": 2, "MOV_L_LI": 4}[op]
            E("%s = %s;" % (rn, self.rd(size, rm, 0, consts, signed=size < 4)))
            if n != m:
                E("%s += %d;" % (rm, size))
        elif op == "MOV_B_S_D":
            E(self.wr(1, self.base(n, consts), d, "r0"))
        elif op == "MOV_W_S_D":
            E(self.wr(2, self.base(n, consts), d * 2, "r0"))
        elif op == "MOV_L_S_D":
            E(self.wr(4, self.base(n, consts), d * 4, rm))
        elif op == "MOV_B_L_D":
            E("r0 = %s;" % self.rd(1, self.base(m, consts), d, consts, signed=True))
        elif op == "MOV_W_L_D":
            E("r0 = %s;" % self.rd(2, self.base(m, consts), d * 2, consts, signed=True))
        elif op == "MOV_L_L_D":
            E("%s = %s;" % (rn, self.rd(4, self.base(m, consts), d * 4, consts)))
        elif op in ("MOV_B_S_R0", "MOV_W_S_R0", "MOV_L_S_R0"):
            size = {"MOV_B_S_R0": 1, "MOV_W_S_R0": 2, "MOV_L_S_R0": 4}[op]
            E(self.wr(size, self.idx(0, n, consts), 0, rm))
        elif op in ("MOV_B_L_R0", "MOV_W_L_R0", "MOV_L_L_R0"):
            size = {"MOV_B_L_R0": 1, "MOV_W_L_R0": 2, "MOV_L_L_R0": 4}[op]
            E("%s = %s;" % (rn, self.rd(size, self.idx(0, m, consts), 0, consts, signed=size < 4)))
        elif op in ("MOV_B_S_G", "MOV_W_S_G", "MOV_L_S_G"):
            size = {"MOV_B_S_G": 1, "MOV_W_S_G": 2, "MOV_L_S_G": 4}[op]
            E(self.wr(size, "gbr", d * size, "r0"))
        elif op in ("MOV_B_L_G", "MOV_W_L_G", "MOV_L_L_G"):
            size = {"MOV_B_L_G": 1, "MOV_W_L_G": 2, "MOV_L_L_G": 4}[op]
            E("r0 = %s;" % self.rd(size, "gbr", d * size, consts, signed=size < 4))
        elif op == "MOVT":
            E("%s = t;" % rn)
        elif op == "SWAP_B":
            E("%s = (%s & 0xFFFF0000u) | ((%s & 0xFF) << 8) | ((%s >> 8) & 0xFF);" % (rn, rm, rm, rm))
        elif op == "SWAP_W":
            E("%s = (%s << 16) | (%s >> 16);" % (rn, rm, rm))
        elif op == "XTRCT":
            E("%s = (%s << 16) | (%s >> 16);" % (rn, rm, rn))
        # ------------------------------------------------ arithmetic
        elif op == "ADD":
            E("%s += %s;" % (rn, rm))
        elif op == "ADD_I":
            E("%s += 0x%08xu;" % (rn, ins.simm & 0xFFFFFFFF))
        elif op == "ADDC":
            E("ADDC(%s, %s);" % (rn, rm))
        elif op == "ADDV":
            E("ADDV(%s, %s);" % (rn, rm))
        elif op == "CMP_EQ_I":
            E("t = r0 == 0x%08xu;" % (ins.simm & 0xFFFFFFFF))
        elif op == "CMP_EQ":
            E("t = %s == %s;" % (rn, rm))
        elif op == "CMP_HS":
            E("t = %s >= %s;" % (rn, rm))
        elif op == "CMP_GE":
            E("t = (int32_t)%s >= (int32_t)%s;" % (rn, rm))
        elif op == "CMP_HI":
            E("t = %s > %s;" % (rn, rm))
        elif op == "CMP_GT":
            E("t = (int32_t)%s > (int32_t)%s;" % (rn, rm))
        elif op == "CMP_PL":
            E("t = (int32_t)%s > 0;" % rn)
        elif op == "CMP_PZ":
            E("t = (int32_t)%s >= 0;" % rn)
        elif op == "CMP_STR":
            E("CMPSTR(%s, %s);" % (rn, rm))
        elif op == "DIV1":
            E("DIV1(%s, %s);" % (rn, rm))
        elif op == "DIV0S":
            E("DIV0S(%s, %s);" % (rn, rm))
        elif op == "DIV0U":
            E("DIV0U();")
        elif op == "DMULS":
            E("{ int64_t p_ = (int64_t)(int32_t)%s * (int64_t)(int32_t)%s; "
              "macl = (uint32_t)p_; mach = (uint32_t)((uint64_t)p_ >> 32); }" % (rn, rm))
        elif op == "DMULU":
            E("{ uint64_t p_ = (uint64_t)%s * (uint64_t)%s; "
              "macl = (uint32_t)p_; mach = (uint32_t)(p_ >> 32); }" % (rn, rm))
        elif op == "DT":
            E("%s -= 1; t = %s == 0;" % (rn, rn))
        elif op == "EXTS_B":
            E("%s = (uint32_t)(int32_t)(int8_t)%s;" % (rn, rm))
        elif op == "EXTS_W":
            E("%s = (uint32_t)(int32_t)(int16_t)%s;" % (rn, rm))
        elif op == "EXTU_B":
            E("%s = %s & 0xFF;" % (rn, rm))
        elif op == "EXTU_W":
            E("%s = %s & 0xFFFF;" % (rn, rm))
        elif op == "MAC_L":
            E("{ uint32_t a_ = %s; %s += 4; uint32_t b_ = %s; %s += 4; MACL_STEP(a_, b_); }" % (
                self.rd(4, rm, 0, consts), rm, self.rd(4, rn, 0, consts), rn))
        elif op == "MAC_W":
            E("{ uint32_t a_ = %s; %s += 2; uint32_t b_ = %s; %s += 2; MACW_STEP(a_, b_); }" % (
                self.rd(2, rm, 0, consts), rm, self.rd(2, rn, 0, consts), rn))
        elif op == "MUL_L":
            E("macl = %s * %s;" % (rn, rm))
        elif op == "MULS_W":
            E("macl = (uint32_t)((int32_t)(int16_t)%s * (int32_t)(int16_t)%s);" % (rn, rm))
        elif op == "MULU_W":
            E("macl = (%s & 0xFFFF) * (%s & 0xFFFF);" % (rn, rm))
        elif op == "NEG":
            E("%s = 0u - %s;" % (rn, rm))
        elif op == "NEGC":
            E("NEGC(%s, %s);" % (rn, rm))
        elif op == "SUB":
            E("%s -= %s;" % (rn, rm))
        elif op == "SUBC":
            E("SUBC(%s, %s);" % (rn, rm))
        elif op == "SUBV":
            E("SUBV(%s, %s);" % (rn, rm))
        # ------------------------------------------------ logic
        elif op == "AND":
            E("%s &= %s;" % (rn, rm))
        elif op == "AND_I":
            E("r0 &= 0x%02xu;" % ins.i)
        elif op == "AND_B_G":
            E("WR8(gbr, r0, RD8(gbr, r0) & 0x%02xu);" % ins.i)
        elif op == "NOT":
            E("%s = ~%s;" % (rn, rm))
        elif op == "OR":
            E("%s |= %s;" % (rn, rm))
        elif op == "OR_I":
            E("r0 |= 0x%02xu;" % ins.i)
        elif op == "OR_B_G":
            E("WR8(gbr, r0, RD8(gbr, r0) | 0x%02xu);" % ins.i)
        elif op == "TAS_B":
            E("{ uint32_t b_ = RD8(%s, 0); t = b_ == 0; WR8(%s, 0, b_ | 0x80); }" % (rn, rn))
        elif op == "TST":
            E("t = (%s & %s) == 0;" % (rn, rm))
        elif op == "TST_I":
            E("t = (r0 & 0x%02xu) == 0;" % ins.i)
        elif op == "TST_B_G":
            E("t = (RD8(gbr, r0) & 0x%02xu) == 0;" % ins.i)
        elif op == "XOR":
            E("%s ^= %s;" % (rn, rm))
        elif op == "XOR_I":
            E("r0 ^= 0x%02xu;" % ins.i)
        elif op == "XOR_B_G":
            E("WR8(gbr, r0, RD8(gbr, r0) ^ 0x%02xu);" % ins.i)
        # ------------------------------------------------ shifts
        elif op == "ROTL":
            E("t = %s >> 31; %s = (%s << 1) | t;" % (rn, rn, rn))
        elif op == "ROTR":
            E("t = %s & 1; %s = (%s >> 1) | (t << 31);" % (rn, rn, rn))
        elif op == "ROTCL":
            E("ROTCL(%s);" % rn)
        elif op == "ROTCR":
            E("ROTCR(%s);" % rn)
        elif op == "SHAD":
            E("SHAD(%s, %s);" % (rn, rm))
        elif op == "SHLD":
            E("SHLD(%s, %s);" % (rn, rm))
        elif op == "SHAL":
            E("t = %s >> 31; %s <<= 1;" % (rn, rn))
        elif op == "SHAR":
            E("t = %s & 1; %s = (uint32_t)((int32_t)%s >> 1);" % (rn, rn, rn))
        elif op == "SHLL":
            E("t = %s >> 31; %s <<= 1;" % (rn, rn))
        elif op == "SHLR":
            E("t = %s & 1; %s >>= 1;" % (rn, rn))
        elif op in ("SHLL2", "SHLL8", "SHLL16"):
            E("%s <<= %d;" % (rn, int(op[4:])))
        elif op in ("SHLR2", "SHLR8", "SHLR16"):
            E("%s >>= %d;" % (rn, int(op[4:])))
        # ------------------------------------------------ system
        elif op == "CLRMAC":
            E("mach = 0; macl = 0;")
        elif op == "CLRS":
            E("s = 0;")
        elif op == "CLRT":
            E("t = 0;")
        elif op == "SETS":
            E("s = 1;")
        elif op == "SETT":
            E("t = 1;")
        elif op == "LDC_SR":
            E("t = %s & 1; s = (%s >> 1) & 1; q = (%s >> 8) & 1; m = (%s >> 9) & 1;" % (rm, rm, rm, rm))
        elif op == "LDC_GBR":
            E("gbr = %s;" % rm)
        elif op == "LDC_VBR":
            E("vbr = %s;" % rm)
        elif op in ("LDC_SSR", "LDC_SPC", "LDC_DBR", "LDC_BANK"):
            E("/* %s: privileged, ignored */" % ins.text())
        elif op == "LDCL_SR":
            E("{ uint32_t v_ = %s; t = v_ & 1; s = (v_ >> 1) & 1; q = (v_ >> 8) & 1; m = (v_ >> 9) & 1; } %s += 4;" % (
                self.rd(4, rm, 0, consts), rm))
        elif op == "LDCL_GBR":
            E("gbr = %s; %s += 4;" % (self.rd(4, rm, 0, consts), rm))
        elif op == "LDCL_VBR":
            E("vbr = %s; %s += 4;" % (self.rd(4, rm, 0, consts), rm))
        elif op in ("LDCL_SSR", "LDCL_SPC", "LDCL_DBR", "LDCL_BANK"):
            E("%s += 4; /* %s: privileged, value dropped */" % (rm, ins.text()))
        elif op in ("LDS_MACH", "LDS_MACL", "LDS_PR", "LDS_FPUL"):
            E("%s = %s;" % (op[4:].lower(), rm))
        elif op == "LDS_FPSCR":
            E("fpscr = %s;" % rm)
        elif op in ("LDSL_MACH", "LDSL_MACL", "LDSL_PR", "LDSL_FPUL", "LDSL_FPSCR"):
            E("%s = %s; %s += 4;" % (op[5:].lower(), self.rd(4, rm, 0, consts), rm))
        elif op == "STC_SR":
            E("%s = SR_FROM_FLAGS();" % rn)
        elif op == "STC_GBR":
            E("%s = gbr;" % rn)
        elif op == "STC_VBR":
            E("%s = vbr;" % rn)
        elif op in ("STC_SSR", "STC_SPC", "STC_SGR", "STC_DBR", "STC_BANK"):
            E("%s = 0; /* %s: privileged */" % (rn, ins.text()))
        elif op == "STCL_SR":
            E("%s -= 4; %s" % (rn, self.wr(4, rn, 0, "SR_FROM_FLAGS()")))
        elif op == "STCL_GBR":
            E("%s -= 4; %s" % (rn, self.wr(4, rn, 0, "gbr")))
        elif op == "STCL_VBR":
            E("%s -= 4; %s" % (rn, self.wr(4, rn, 0, "vbr")))
        elif op in ("STCL_SSR", "STCL_SPC", "STCL_SGR", "STCL_DBR", "STCL_BANK"):
            E("%s -= 4; %s /* %s: privileged */" % (rn, self.wr(4, rn, 0, "0u"), ins.text()))
        elif op in ("STS_MACH", "STS_MACL", "STS_PR", "STS_FPUL", "STS_FPSCR"):
            E("%s = %s;" % (rn, op[4:].lower()))
        elif op in ("STSL_MACH", "STSL_MACL", "STSL_PR", "STSL_FPUL", "STSL_FPSCR"):
            E("%s -= 4; %s" % (rn, self.wr(4, rn, 0, op[5:].lower())))
        elif op == "MOVCA_L":
            E(self.wr(4, self.base(n, consts), 0, "r0"))
        elif op in ("OCBI", "OCBP", "OCBWB", "PREF", "NOP", "LDTLB"):
            if op != "NOP":
                E("/* %s */" % ins.text())
        elif op in ("SLEEP", "TRAPA", "INVALID", "RTE"):
            E('sa1_trap(c, 0x%08xu, "%s");' % (ins.pc, ins.text().replace('"', "'")))
        # ------------------------------------------------ floating point
        elif op == "FLDI0":
            E("%s = 0.0f;" % self.F(n))
        elif op == "FLDI1":
            E("%s = 1.0f;" % self.F(n))
        elif op == "FMOV":
            self.fmov_reg(E, n, m)
        elif op in ("FMOV_L", "FMOV_L_R0", "FMOV_LI"):
            base = self.idx(0, m, consts) if op == "FMOV_L_R0" else self.base(m, consts)
            self.fmov_load(E, n, base, consts)
            if op == "FMOV_LI":
                E("%s += %s;" % (rm, self.sz_bytes()))
        elif op in ("FMOV_S", "FMOV_SD", "FMOV_S_R0"):
            if op == "FMOV_SD":
                E("%s -= %s;" % (rn, self.sz_bytes()))
                base = rn
            elif op == "FMOV_S_R0":
                base = self.idx(0, n, consts)
            else:
                base = self.base(n, consts)
            self.fmov_store(E, m, base)
        elif op == "FLDS":
            E("fpul = F2U(%s);" % self.F(m))
        elif op == "FSTS":
            E("%s = U2F(fpul);" % self.F(n))
        elif op == "FABS":
            self.fp_unary(E, n, "FABS_F(%s)", "fabs(%s)")
        elif op == "FNEG":
            self.fp_unary(E, n, "FNEG_F(%s)", "-(%s)")
        elif op == "FSQRT":
            self.fp_unary(E, n, "sqrtf(%s)", "sqrt(%s)")
        elif op in ("FADD", "FSUB", "FMUL", "FDIV"):
            sym = {"FADD": "+", "FSUB": "-", "FMUL": "*", "FDIV": "/"}[op]
            self.fp_binary(E, n, m, sym)
        elif op in ("FCMP_EQ", "FCMP_GT"):
            sym = "==" if op == "FCMP_EQ" else ">"
            single = "t = %s %s %s;" % (self.F(n), sym, self.F(m))
            dbl = "t = DR(%s, %s) %s DR(%s, %s);" % (*self.DRn(n), sym, *self.DRn(m))
            self.pr_select(E, single, dbl)
        elif op == "FLOAT":
            single = "%s = (float)(int32_t)fpul;" % self.F(n)
            dbl = "DR_SET(%s, %s, (double)(int32_t)fpul);" % self.DRn(n)
            self.pr_select(E, single, dbl)
        elif op == "FTRC":
            single = "fpul = FTRC(%s);" % self.F(m)
            dbl = "fpul = FTRC_D(DR(%s, %s));" % self.DRn(m)
            self.pr_select(E, single, dbl)
        elif op == "FMAC":
            E("%s = %s * %s + %s;" % (self.F(n), self.F(0), self.F(m), self.F(n)))
        elif op == "FCNVDS":
            E("fpul = F2U((float)DR(%s, %s));" % self.DRn(m))
        elif op == "FCNVSD":
            E("DR_SET(%s, %s, (double)U2F(fpul));" % self.DRn(n))
        elif op == "FIPR":
            a = [self.F(m * 4 + k) for k in range(4)]
            b = [self.F(n * 4 + k) for k in range(4)]
            E("%s = FIPR(%s, %s);" % (self.F(n * 4 + 3), ", ".join(a), ", ".join(b)))
        elif op == "FTRV":
            v = [self.F(n * 4 + k) for k in range(4)]
            E("FTRV(%s, %s);" % ("xf" if not self.frbank else "fr", ", ".join(v)))
        elif op == "FRCHG":
            E("fpscr ^= FPSCR_FR;")
        elif op == "FSCHG":
            E("fpscr ^= FPSCR_SZ;")
        elif op == "FSCA":
            E("FSCA(fpul, %s, %s);" % (self.F(n * 2), self.F(n * 2 + 1)))
        elif op == "FSRRA":
            E("%s = FSRRA(%s);" % (self.F(n), self.F(n)))
        # ------------------------------------------------ branches (not here)
        elif op in ("BF", "BF_S", "BT", "BT_S", "BRA", "BRAF", "BSR", "BSRF", "JMP", "JSR", "RTS"):
            raise ValueError("branch %s reached the instruction lowerer" % op)
        else:
            raise KeyError("no lowering for %s" % op)
        return out

    # ---- FP helpers --------------------------------------------------------
    def idx(self, a, b, consts):
        """Address expression r_a + r_b (constant-folded when both known)."""
        va, vb = consts.get(a), consts.get(b)
        if va is not None and vb is not None:
            return (va + vb) & 0xFFFFFFFF
        return "r%d + r%d" % (a, b)

    def sz_bytes(self):
        if self.sz == U:
            return "((fpscr & FPSCR_SZ) ? 8 : 4)"
        return "8" if self.sz else "4"

    def pr_select(self, E, single, dbl):
        if self.pr == 0:
            E(single)
        elif self.pr == 1:
            E(dbl)
        else:
            self.rt_pr += 1
            E("if (fpscr & FPSCR_PR) { %s } else { %s }" % (dbl, single))

    def sz_select(self, E, single, pair):
        if self.sz == 0:
            E(single)
        elif self.sz == 1:
            E(pair)
        else:
            self.rt_sz += 1
            E("if (fpscr & FPSCR_SZ) { %s } else { %s }" % (pair, single))

    def fp_unary(self, E, n, single_fmt, dbl_fmt):
        single = "%s = %s;" % (self.F(n), single_fmt % self.F(n))
        hi, lo = self.DRn(n)
        dbl = "DR_SET(%s, %s, %s);" % (hi, lo, dbl_fmt % ("DR(%s, %s)" % (hi, lo)))
        self.pr_select(E, single, dbl)

    def fp_binary(self, E, n, m, sym):
        single = "%s = %s %s %s;" % (self.F(n), self.F(n), sym, self.F(m))
        hn, ln = self.DRn(n)
        hm, lm = self.DRn(m)
        dbl = "DR_SET(%s, %s, DR(%s, %s) %s DR(%s, %s));" % (hn, ln, hn, ln, sym, hm, lm)
        self.pr_select(E, single, dbl)

    def fmov_reg(self, E, n, m):
        single = "%s = %s;" % (self.F(n), self.F(m))
        (hn, ln), (hm, lm) = self.FP(n), self.FP(m)
        pair = "%s = %s; %s = %s;" % (hn, hm, ln, lm)
        self.sz_select(E, single, pair)

    def fmov_load(self, E, n, base, consts):
        single = "%s = %s;" % (self.F(n), self.rd("f", base, 0, consts))
        hi, lo = self.FP(n)
        pair = "%s = %s; %s = %s;" % (hi, self.rd("f", base, 0, consts), lo, self.rd("f", base, 4, consts))
        self.sz_select(E, single, pair)

    def fmov_store(self, E, m, base):
        single = self.wr("f", base, 0, self.F(m))
        hi, lo = self.FP(m)
        pair = "%s %s" % (self.wr("f", base, 0, hi), self.wr("f", base, 4, lo))
        self.sz_select(E, single, pair)


# Human-readable lowering table for README.md (op -> C shape).
LOWERING_DOC = [
    ("mov #imm,Rn / mov Rm,Rn", "`rn = imm;` / `rn = rm;`"),
    ("mov.l @(disp,pc),Rn", "folded: `rn = <pool word>;` unless the word is run-time patched, then `rn = RD32(addr, 0);`"),
    ("mov.w @(disp,pc),Rn", "folded sign-extended word, same rule"),
    ("mova @(disp,pc),r0", "`r0 = <address>;`"),
    ("mov.{b,w,l} loads", "`rn = RD8S/RD16S/RD32(base, disp);` (byte/word sign-extend); `@Rm+` adds the size after (not when n == m)"),
    ("mov.{b,w,l} stores", "`WR8/16/32(base, disp, rm);`; `@-Rn` decrements first"),
    ("constant address outside RAM", "`sa1_mmio_rd(addr, size)` / `sa1_mmio_wr(addr, size, v)` (hardware registers)"),
    ("swap.b / swap.w / xtrct", "shift-and-mask expressions"),
    ("add / sub / and / or / xor / not / neg", "C operators on uint32_t"),
    ("addc / subc / negc", "`ADDC/SUBC/NEGC` macros, carry in `t`"),
    ("addv / subv", "`ADDV/SUBV` macros, overflow in `t`"),
    ("cmp/*", "`t = <relation>;` with `(int32_t)` casts for the signed forms"),
    ("cmp/str", "`CMPSTR` macro (any equal byte)"),
    ("div0u / div0s / div1", "`DIV0U/DIV0S/DIV1` macros over the `q`, `m`, `t` locals, manual's restoring step"),
    ("mul.l / muls.w / mulu.w", "`macl = ...;`"),
    ("dmuls.l / dmulu.l", "64-bit product into `mach:macl`"),
    ("mac.l / mac.w", "`MACL_STEP/MACW_STEP` macros (S-bit saturation included)"),
    ("dt", "`rn -= 1; t = rn == 0;`"),
    ("exts/extu", "casts and masks"),
    ("shll/shlr/shal/shar/rotl/rotr", "shift with `t` = shifted-out bit"),
    ("rotcl / rotcr", "`ROTCL/ROTCR` macros through `t`"),
    ("shad / shld", "`SHAD/SHLD` macros (sign of rm picks direction; -32 special case)"),
    ("shll2/8/16, shlr2/8/16", "plain shifts"),
    ("tst / tst #imm / tas.b", "`t = (a & b) == 0;`; tas.b reads, tests, sets bit 7"),
    ("and/or/xor/tst.b #imm,@(r0,gbr)", "read-modify-write through `gbr`"),
    ("movt / clrt / sett / clrs / sets / clrmac", "flag and MAC assignments"),
    ("ldc/stc sr", "only T, S, Q, M are modelled; `SR_FROM_FLAGS()` rebuilds a plausible SR"),
    ("ldc/stc gbr, vbr", "locals `gbr`, `vbr`"),
    ("ldc/stc ssr, spc, sgr, dbr, Rn_bank", "privileged: ignored (store) / zero (load), commented in the output"),
    ("lds/sts mach, macl, pr, fpul, fpscr (+.l forms)", "locals; the `.l` forms go through RD32/WR32 and move the pointer"),
    ("pref / ocbi / ocbp / ocbwb / ldtlb / nop", "no code (comment)"),
    ("movca.l r0,@Rn", "plain 32-bit store"),
    ("trapa / sleep / rte / invalid word", "`sa1_trap(c, pc, text)`"),
    ("bt / bf (+/s)", "`if (t) goto L;` with the slot before the test; T captured first when the slot writes it"),
    ("bra / jmp @Rn (constant)", "`goto L;` locally, else tail call: store dirty registers, `SA1_TAIL(fn(c))` (a guaranteed tail call)"),
    ("bsr / jsr @Rn / bsrf (constant target)", "store live-in-of-callee ∩ dirty, `fn_xxxx(c);` or `sdk_xxxx(c);`, reload live ∩ may-write-of-callee"),
    ("jsr / bsrf (unknown target)", "store all dirty, `sa1_call(c, target);`, reload all live"),
    ("jmp / braf through a recovered table", "`switch (rn) { case <entry>: goto L; ... default: sa1_trap }`"),
    ("jmp / braf unresolved", "store all dirty, `sa1_tail_target(c, target)` then `SA1_TAIL(f(c))`"),
    ("rts", "store dirty registers, `return;` (PR is not consulted)"),
    ("fmov FRm,FRn", "`frn = frm;`; under SZ=1 the pair (`DRn`/`XDn` by field low bit)"),
    ("fmov.s loads/stores", "`RDF/WRF`; under SZ=1 two words, register order = address order (see README)"),
    ("fldi0 / fldi1 / flds / fsts", "assignments, `F2U/U2F` for the bit moves"),
    ("fadd / fsub / fmul / fdiv", "float operators; under PR=1 `DR_SET(hi, lo, DR(..) op DR(..))`"),
    ("fmac", "`frn = fr0 * frm + frn;` (two roundings: compile with -ffp-contract=off)"),
    ("fabs / fneg", "sign-bit operations (`FABS_F/FNEG_F`), `fabs()` / `-` under PR=1"),
    ("fsqrt / fsrra", "`sqrtf` / `1.0f / sqrtf`"),
    ("fcmp/eq / fcmp/gt", "`t = a == b;` / `t = b > a;` (frn > frm)"),
    ("float / ftrc", "`(float)(int32_t)fpul` / `FTRC()` saturating round-toward-zero"),
    ("fcnvsd / fcnvds", "double <-> single through FPUL"),
    ("fipr FVm,FVn", "`FIPR` macro: 4-term dot product into fr[4n+3]"),
    ("ftrv xmtrx,FVn", "`FTRV` macro: XMTRX (xf bank, column-major) times the vector"),
    ("fsca fpul,DRn", "`FSCA` macro: sinf/cosf of (fpul & 0xFFFF) / 65536 turns"),
    ("fschg / frchg", "`fpscr ^= bit`; SZ and FR are also tracked statically so fmov and register naming need no runtime test when the state is known"),
]
