"""SH-4 instruction set table.

One row per instruction form: the 16-bit encoding pattern (from the SH-4
software manual, Renesas REJ09B0318), a short operation id, and a syntax
string.  The same table drives the decoder (decode.py) and the test
assembler (tests/emitter/sh4asm.py), so an encoding typo would hide in both;
tests/emitter/run_tests.py therefore checks hand-verified encodings taken
from the manual's opcode listing against this table.

Pattern letters: n = Rn field, m = Rm field, d = displacement, i = immediate,
0/1 = fixed bits.  Field widths follow from the number of letters.
"""

# (pattern, op, syntax)
TABLE = [
    # ---- data transfer ----
    ("1110nnnniiiiiiii", "MOV_I",      "mov #{simm},r{n}"),
    ("1001nnnndddddddd", "MOV_W_PC",   "mov.w @({d2},pc),r{n}"),
    ("1101nnnndddddddd", "MOV_L_PC",   "mov.l @({d4},pc),r{n}"),
    ("0110nnnnmmmm0011", "MOV",        "mov r{m},r{n}"),
    ("0010nnnnmmmm0000", "MOV_B_S",    "mov.b r{m},@r{n}"),
    ("0010nnnnmmmm0001", "MOV_W_S",    "mov.w r{m},@r{n}"),
    ("0010nnnnmmmm0010", "MOV_L_S",    "mov.l r{m},@r{n}"),
    ("0110nnnnmmmm0000", "MOV_B_L",    "mov.b @r{m},r{n}"),
    ("0110nnnnmmmm0001", "MOV_W_L",    "mov.w @r{m},r{n}"),
    ("0110nnnnmmmm0010", "MOV_L_L",    "mov.l @r{m},r{n}"),
    ("0010nnnnmmmm0100", "MOV_B_SD",   "mov.b r{m},@-r{n}"),
    ("0010nnnnmmmm0101", "MOV_W_SD",   "mov.w r{m},@-r{n}"),
    ("0010nnnnmmmm0110", "MOV_L_SD",   "mov.l r{m},@-r{n}"),
    ("0110nnnnmmmm0100", "MOV_B_LI",   "mov.b @r{m}+,r{n}"),
    ("0110nnnnmmmm0101", "MOV_W_LI",   "mov.w @r{m}+,r{n}"),
    ("0110nnnnmmmm0110", "MOV_L_LI",   "mov.l @r{m}+,r{n}"),
    ("10000000nnnndddd", "MOV_B_S_D",  "mov.b r0,@({d1},r{n})"),
    ("10000001nnnndddd", "MOV_W_S_D",  "mov.w r0,@({d2},r{n})"),
    ("0001nnnnmmmmdddd", "MOV_L_S_D",  "mov.l r{m},@({d4},r{n})"),
    ("10000100mmmmdddd", "MOV_B_L_D",  "mov.b @({d1},r{m}),r0"),
    ("10000101mmmmdddd", "MOV_W_L_D",  "mov.w @({d2},r{m}),r0"),
    ("0101nnnnmmmmdddd", "MOV_L_L_D",  "mov.l @({d4},r{m}),r{n}"),
    ("0000nnnnmmmm0100", "MOV_B_S_R0", "mov.b r{m},@(r0,r{n})"),
    ("0000nnnnmmmm0101", "MOV_W_S_R0", "mov.w r{m},@(r0,r{n})"),
    ("0000nnnnmmmm0110", "MOV_L_S_R0", "mov.l r{m},@(r0,r{n})"),
    ("0000nnnnmmmm1100", "MOV_B_L_R0", "mov.b @(r0,r{m}),r{n}"),
    ("0000nnnnmmmm1101", "MOV_W_L_R0", "mov.w @(r0,r{m}),r{n}"),
    ("0000nnnnmmmm1110", "MOV_L_L_R0", "mov.l @(r0,r{m}),r{n}"),
    ("11000000dddddddd", "MOV_B_S_G",  "mov.b r0,@({d1},gbr)"),
    ("11000001dddddddd", "MOV_W_S_G",  "mov.w r0,@({d2},gbr)"),
    ("11000010dddddddd", "MOV_L_S_G",  "mov.l r0,@({d4},gbr)"),
    ("11000100dddddddd", "MOV_B_L_G",  "mov.b @({d1},gbr),r0"),
    ("11000101dddddddd", "MOV_W_L_G",  "mov.w @({d2},gbr),r0"),
    ("11000110dddddddd", "MOV_L_L_G",  "mov.l @({d4},gbr),r0"),
    ("11000111dddddddd", "MOVA",       "mova @({d4},pc),r0"),
    ("0000nnnn00101001", "MOVT",       "movt r{n}"),
    ("0110nnnnmmmm1000", "SWAP_B",     "swap.b r{m},r{n}"),
    ("0110nnnnmmmm1001", "SWAP_W",     "swap.w r{m},r{n}"),
    ("0010nnnnmmmm1101", "XTRCT",      "xtrct r{m},r{n}"),
    # ---- arithmetic ----
    ("0011nnnnmmmm1100", "ADD",        "add r{m},r{n}"),
    ("0111nnnniiiiiiii", "ADD_I",      "add #{simm},r{n}"),
    ("0011nnnnmmmm1110", "ADDC",       "addc r{m},r{n}"),
    ("0011nnnnmmmm1111", "ADDV",       "addv r{m},r{n}"),
    ("10001000iiiiiiii", "CMP_EQ_I",   "cmp/eq #{simm},r0"),
    ("0011nnnnmmmm0000", "CMP_EQ",     "cmp/eq r{m},r{n}"),
    ("0011nnnnmmmm0010", "CMP_HS",     "cmp/hs r{m},r{n}"),
    ("0011nnnnmmmm0011", "CMP_GE",     "cmp/ge r{m},r{n}"),
    ("0011nnnnmmmm0110", "CMP_HI",     "cmp/hi r{m},r{n}"),
    ("0011nnnnmmmm0111", "CMP_GT",     "cmp/gt r{m},r{n}"),
    ("0100nnnn00010101", "CMP_PL",     "cmp/pl r{n}"),
    ("0100nnnn00010001", "CMP_PZ",     "cmp/pz r{n}"),
    ("0010nnnnmmmm1100", "CMP_STR",    "cmp/str r{m},r{n}"),
    ("0011nnnnmmmm0100", "DIV1",       "div1 r{m},r{n}"),
    ("0010nnnnmmmm0111", "DIV0S",      "div0s r{m},r{n}"),
    ("0000000000011001", "DIV0U",      "div0u"),
    ("0011nnnnmmmm1101", "DMULS",      "dmuls.l r{m},r{n}"),
    ("0011nnnnmmmm0101", "DMULU",      "dmulu.l r{m},r{n}"),
    ("0100nnnn00010000", "DT",         "dt r{n}"),
    ("0110nnnnmmmm1110", "EXTS_B",     "exts.b r{m},r{n}"),
    ("0110nnnnmmmm1111", "EXTS_W",     "exts.w r{m},r{n}"),
    ("0110nnnnmmmm1100", "EXTU_B",     "extu.b r{m},r{n}"),
    ("0110nnnnmmmm1101", "EXTU_W",     "extu.w r{m},r{n}"),
    ("0000nnnnmmmm1111", "MAC_L",      "mac.l @r{m}+,@r{n}+"),
    ("0100nnnnmmmm1111", "MAC_W",      "mac.w @r{m}+,@r{n}+"),
    ("0000nnnnmmmm0111", "MUL_L",      "mul.l r{m},r{n}"),
    ("0010nnnnmmmm1111", "MULS_W",     "muls.w r{m},r{n}"),
    ("0010nnnnmmmm1110", "MULU_W",     "mulu.w r{m},r{n}"),
    ("0110nnnnmmmm1011", "NEG",        "neg r{m},r{n}"),
    ("0110nnnnmmmm1010", "NEGC",       "negc r{m},r{n}"),
    ("0011nnnnmmmm1000", "SUB",        "sub r{m},r{n}"),
    ("0011nnnnmmmm1010", "SUBC",       "subc r{m},r{n}"),
    ("0011nnnnmmmm1011", "SUBV",       "subv r{m},r{n}"),
    # ---- logic ----
    ("0010nnnnmmmm1001", "AND",        "and r{m},r{n}"),
    ("11001001iiiiiiii", "AND_I",      "and #{imm},r0"),
    ("11001101iiiiiiii", "AND_B_G",    "and.b #{imm},@(r0,gbr)"),
    ("0110nnnnmmmm0111", "NOT",        "not r{m},r{n}"),
    ("0010nnnnmmmm1011", "OR",         "or r{m},r{n}"),
    ("11001011iiiiiiii", "OR_I",       "or #{imm},r0"),
    ("11001111iiiiiiii", "OR_B_G",     "or.b #{imm},@(r0,gbr)"),
    ("0100nnnn00011011", "TAS_B",      "tas.b @r{n}"),
    ("0010nnnnmmmm1000", "TST",        "tst r{m},r{n}"),
    ("11001000iiiiiiii", "TST_I",      "tst #{imm},r0"),
    ("11001100iiiiiiii", "TST_B_G",    "tst.b #{imm},@(r0,gbr)"),
    ("0010nnnnmmmm1010", "XOR",        "xor r{m},r{n}"),
    ("11001010iiiiiiii", "XOR_I",      "xor #{imm},r0"),
    ("11001110iiiiiiii", "XOR_B_G",    "xor.b #{imm},@(r0,gbr)"),
    # ---- shift ----
    ("0100nnnn00000100", "ROTL",       "rotl r{n}"),
    ("0100nnnn00000101", "ROTR",       "rotr r{n}"),
    ("0100nnnn00100100", "ROTCL",      "rotcl r{n}"),
    ("0100nnnn00100101", "ROTCR",      "rotcr r{n}"),
    ("0100nnnnmmmm1100", "SHAD",       "shad r{m},r{n}"),
    ("0100nnnn00100000", "SHAL",       "shal r{n}"),
    ("0100nnnn00100001", "SHAR",       "shar r{n}"),
    ("0100nnnnmmmm1101", "SHLD",       "shld r{m},r{n}"),
    ("0100nnnn00000000", "SHLL",       "shll r{n}"),
    ("0100nnnn00000001", "SHLR",       "shlr r{n}"),
    ("0100nnnn00001000", "SHLL2",      "shll2 r{n}"),
    ("0100nnnn00001001", "SHLR2",      "shlr2 r{n}"),
    ("0100nnnn00011000", "SHLL8",      "shll8 r{n}"),
    ("0100nnnn00011001", "SHLR8",      "shlr8 r{n}"),
    ("0100nnnn00101000", "SHLL16",     "shll16 r{n}"),
    ("0100nnnn00101001", "SHLR16",     "shlr16 r{n}"),
    # ---- branch ----
    ("10001011dddddddd", "BF",         "bf {target}"),
    ("10001111dddddddd", "BF_S",       "bf/s {target}"),
    ("10001001dddddddd", "BT",         "bt {target}"),
    ("10001101dddddddd", "BT_S",       "bt/s {target}"),
    ("1010dddddddddddd", "BRA",        "bra {target}"),
    ("0000nnnn00100011", "BRAF",       "braf r{n}"),
    ("1011dddddddddddd", "BSR",        "bsr {target}"),
    ("0000nnnn00000011", "BSRF",       "bsrf r{n}"),
    ("0100nnnn00101011", "JMP",        "jmp @r{n}"),
    ("0100nnnn00001011", "JSR",        "jsr @r{n}"),
    ("0000000000001011", "RTS",        "rts"),
    # ---- system ----
    ("0000000000101000", "CLRMAC",     "clrmac"),
    ("0000000001001000", "CLRS",       "clrs"),
    ("0000000000001000", "CLRT",       "clrt"),
    ("0100mmmm00001110", "LDC_SR",     "ldc r{m},sr"),
    ("0100mmmm00011110", "LDC_GBR",    "ldc r{m},gbr"),
    ("0100mmmm00101110", "LDC_VBR",    "ldc r{m},vbr"),
    ("0100mmmm00111110", "LDC_SSR",    "ldc r{m},ssr"),
    ("0100mmmm01001110", "LDC_SPC",    "ldc r{m},spc"),
    ("0100mmmm11111010", "LDC_DBR",    "ldc r{m},dbr"),
    ("0100mmmm1nnn1110", "LDC_BANK",   "ldc r{m},r{n}_bank"),
    ("0100mmmm00000111", "LDCL_SR",    "ldc.l @r{m}+,sr"),
    ("0100mmmm00010111", "LDCL_GBR",   "ldc.l @r{m}+,gbr"),
    ("0100mmmm00100111", "LDCL_VBR",   "ldc.l @r{m}+,vbr"),
    ("0100mmmm00110111", "LDCL_SSR",   "ldc.l @r{m}+,ssr"),
    ("0100mmmm01000111", "LDCL_SPC",   "ldc.l @r{m}+,spc"),
    ("0100mmmm11110110", "LDCL_DBR",   "ldc.l @r{m}+,dbr"),
    ("0100mmmm1nnn0111", "LDCL_BANK",  "ldc.l @r{m}+,r{n}_bank"),
    ("0100mmmm00001010", "LDS_MACH",   "lds r{m},mach"),
    ("0100mmmm00011010", "LDS_MACL",   "lds r{m},macl"),
    ("0100mmmm00101010", "LDS_PR",     "lds r{m},pr"),
    ("0100mmmm00000110", "LDSL_MACH",  "lds.l @r{m}+,mach"),
    ("0100mmmm00010110", "LDSL_MACL",  "lds.l @r{m}+,macl"),
    ("0100mmmm00100110", "LDSL_PR",    "lds.l @r{m}+,pr"),
    ("0000000000111000", "LDTLB",      "ldtlb"),
    ("0000nnnn11000011", "MOVCA_L",    "movca.l r0,@r{n}"),
    ("0000000000001001", "NOP",        "nop"),
    ("0000nnnn10010011", "OCBI",       "ocbi @r{n}"),
    ("0000nnnn10100011", "OCBP",       "ocbp @r{n}"),
    ("0000nnnn10110011", "OCBWB",      "ocbwb @r{n}"),
    ("0000nnnn10000011", "PREF",       "pref @r{n}"),
    ("0000000000101011", "RTE",        "rte"),
    ("0000000001011000", "SETS",       "sets"),
    ("0000000000011000", "SETT",       "sett"),
    ("0000000000011011", "SLEEP",      "sleep"),
    ("0000nnnn00000010", "STC_SR",     "stc sr,r{n}"),
    ("0000nnnn00010010", "STC_GBR",    "stc gbr,r{n}"),
    ("0000nnnn00100010", "STC_VBR",    "stc vbr,r{n}"),
    ("0000nnnn00110010", "STC_SSR",    "stc ssr,r{n}"),
    ("0000nnnn01000010", "STC_SPC",    "stc spc,r{n}"),
    ("0000nnnn00111010", "STC_SGR",    "stc sgr,r{n}"),
    ("0000nnnn11111010", "STC_DBR",    "stc dbr,r{n}"),
    ("0000nnnn1mmm0010", "STC_BANK",   "stc r{m}_bank,r{n}"),
    ("0100nnnn00000011", "STCL_SR",    "stc.l sr,@-r{n}"),
    ("0100nnnn00010011", "STCL_GBR",   "stc.l gbr,@-r{n}"),
    ("0100nnnn00100011", "STCL_VBR",   "stc.l vbr,@-r{n}"),
    ("0100nnnn00110011", "STCL_SSR",   "stc.l ssr,@-r{n}"),
    ("0100nnnn01000011", "STCL_SPC",   "stc.l spc,@-r{n}"),
    ("0100nnnn00110010", "STCL_SGR",   "stc.l sgr,@-r{n}"),
    ("0100nnnn11110010", "STCL_DBR",   "stc.l dbr,@-r{n}"),
    ("0100nnnn1mmm0011", "STCL_BANK",  "stc.l r{m}_bank,@-r{n}"),
    ("0000nnnn00001010", "STS_MACH",   "sts mach,r{n}"),
    ("0000nnnn00011010", "STS_MACL",   "sts macl,r{n}"),
    ("0000nnnn00101010", "STS_PR",     "sts pr,r{n}"),
    ("0100nnnn00000010", "STSL_MACH",  "sts.l mach,@-r{n}"),
    ("0100nnnn00010010", "STSL_MACL",  "sts.l macl,@-r{n}"),
    ("0100nnnn00100010", "STSL_PR",    "sts.l pr,@-r{n}"),
    ("11000011iiiiiiii", "TRAPA",      "trapa #{imm}"),
    # ---- floating point ----
    ("1111nnnn10001101", "FLDI0",      "fldi0 fr{n}"),
    ("1111nnnn10011101", "FLDI1",      "fldi1 fr{n}"),
    ("1111nnnnmmmm1100", "FMOV",       "fmov fr{m},fr{n}"),
    ("1111nnnnmmmm1000", "FMOV_L",     "fmov.s @r{m},fr{n}"),
    ("1111nnnnmmmm0110", "FMOV_L_R0",  "fmov.s @(r0,r{m}),fr{n}"),
    ("1111nnnnmmmm1001", "FMOV_LI",    "fmov.s @r{m}+,fr{n}"),
    ("1111nnnnmmmm1010", "FMOV_S",     "fmov.s fr{m},@r{n}"),
    ("1111nnnnmmmm1011", "FMOV_SD",    "fmov.s fr{m},@-r{n}"),
    ("1111nnnnmmmm0111", "FMOV_S_R0",  "fmov.s fr{m},@(r0,r{n})"),
    ("1111mmmm00011101", "FLDS",       "flds fr{m},fpul"),
    ("1111nnnn00001101", "FSTS",       "fsts fpul,fr{n}"),
    ("1111nnnn01011101", "FABS",       "fabs fr{n}"),
    ("1111nnnnmmmm0000", "FADD",       "fadd fr{m},fr{n}"),
    ("1111nnnnmmmm0100", "FCMP_EQ",    "fcmp/eq fr{m},fr{n}"),
    ("1111nnnnmmmm0101", "FCMP_GT",    "fcmp/gt fr{m},fr{n}"),
    ("1111nnnnmmmm0011", "FDIV",       "fdiv fr{m},fr{n}"),
    ("1111nnnn00101101", "FLOAT",      "float fpul,fr{n}"),
    ("1111nnnnmmmm1110", "FMAC",       "fmac fr0,fr{m},fr{n}"),
    ("1111nnnnmmmm0010", "FMUL",       "fmul fr{m},fr{n}"),
    ("1111nnnn01001101", "FNEG",       "fneg fr{n}"),
    ("1111nnnn01101101", "FSQRT",      "fsqrt fr{n}"),
    ("1111nnnnmmmm0001", "FSUB",       "fsub fr{m},fr{n}"),
    ("1111mmmm00111101", "FTRC",       "ftrc fr{m},fpul"),
    ("1111mmm010111101", "FCNVDS",     "fcnvds dr{m2},fpul"),
    ("1111nnn010101101", "FCNVSD",     "fcnvsd fpul,dr{n2}"),
    ("0100mmmm01101010", "LDS_FPSCR",  "lds r{m},fpscr"),
    ("0100mmmm01011010", "LDS_FPUL",   "lds r{m},fpul"),
    ("0100mmmm01100110", "LDSL_FPSCR", "lds.l @r{m}+,fpscr"),
    ("0100mmmm01010110", "LDSL_FPUL",  "lds.l @r{m}+,fpul"),
    ("0000nnnn01101010", "STS_FPSCR",  "sts fpscr,r{n}"),
    ("0000nnnn01011010", "STS_FPUL",   "sts fpul,r{n}"),
    ("0100nnnn01100010", "STSL_FPSCR", "sts.l fpscr,@-r{n}"),
    ("0100nnnn01010010", "STSL_FPUL",  "sts.l fpul,@-r{n}"),
    ("1111nnmm11101101", "FIPR",       "fipr fv{m4},fv{n4}"),
    ("1111nn0111111101", "FTRV",       "ftrv xmtrx,fv{n4}"),
    ("1111101111111101", "FRCHG",      "frchg"),
    ("1111001111111101", "FSCHG",      "fschg"),
    ("1111nnn011111101", "FSCA",       "fsca fpul,dr{n2}"),
    ("1111nnnn01111101", "FSRRA",      "fsrra fr{n}"),
]

# Instructions with a delay slot.
DELAYED = {"BF_S", "BT_S", "BRA", "BRAF", "BSR", "BSRF", "JMP", "JSR", "RTS", "RTE"}
# Unconditional control transfers that end a block with no fall-through.
TERMINATORS = {"BRA", "BRAF", "JMP", "RTS", "RTE"}
CALLS = {"BSR", "BSRF", "JSR"}
COND_BRANCHES = {"BF", "BF_S", "BT", "BT_S"}
# Instructions the manual forbids in a delay slot (checked and reported only).
SLOT_ILLEGAL = DELAYED | COND_BRANCHES | {"MOV_W_PC", "MOV_L_PC", "MOVA", "TRAPA"}


def _fields(pattern):
    """Return {letter: (shift, width)} for the variable fields of a pattern."""
    out = {}
    for letter in "nmdi":
        bits = [15 - idx for idx, ch in enumerate(pattern) if ch == letter]
        if bits:
            out[letter] = (min(bits), len(bits))
    return out


def _fixed(pattern):
    mask = value = 0
    for idx, ch in enumerate(pattern):
        bit = 15 - idx
        if ch in "01":
            mask |= 1 << bit
            if ch == "1":
                value |= 1 << bit
    return mask, value


class Form:
    __slots__ = ("pattern", "op", "syntax", "fields", "mask", "value")

    def __init__(self, pattern, op, syntax):
        assert len(pattern) == 16, pattern
        self.pattern = pattern
        self.op = op
        self.syntax = syntax
        self.fields = _fields(pattern)
        self.mask, self.value = _fixed(pattern)

    def extract(self, word):
        out = {}
        for letter, (shift, width) in self.fields.items():
            out[letter] = (word >> shift) & ((1 << width) - 1)
        return out

    def encode(self, **vals):
        word = self.value
        for letter, (shift, width) in self.fields.items():
            v = vals.get(letter, 0) & ((1 << width) - 1)
            word |= v << shift
        return word


FORMS = [Form(*row) for row in TABLE]
BY_OP = {f.op: f for f in FORMS}
assert len(BY_OP) == len(FORMS), "duplicate op id"


def build_decode_table():
    """65,536-entry list mapping a word to its Form (or None).

    Forms with more fixed bits are filled last so that a specific pattern
    wins over a broader one that happens to overlap it.  Valid SH-4 forms do
    not overlap, so this only matters as a defence against table mistakes.
    """
    table = [None] * 65536
    for form in sorted(FORMS, key=lambda f: bin(f.mask).count("1")):
        free = [b for b in range(16) if not (form.mask >> b) & 1]
        for combo in range(1 << len(free)):
            word = form.value
            for k, bit in enumerate(free):
                if (combo >> k) & 1:
                    word |= 1 << bit
            table[word] = form
    return table
