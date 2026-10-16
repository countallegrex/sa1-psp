"""Decode 16-bit SH-4 words into Instr records."""

from .isa import build_decode_table, DELAYED, TERMINATORS, CALLS, COND_BRANCHES

_TABLE = build_decode_table()


def sext(value, bits):
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


class Instr:
    __slots__ = ("pc", "word", "op", "n", "m", "d", "i", "form")

    def __init__(self, pc, word):
        self.pc = pc
        self.word = word
        form = _TABLE[word]
        self.form = form
        if form is None:
            self.op = "INVALID"
            self.n = self.m = self.d = self.i = 0
            return
        f = form.extract(word)
        self.op = form.op
        self.n = f.get("n", 0)
        self.m = f.get("m", 0)
        self.d = f.get("d", 0)
        self.i = f.get("i", 0)

    # ---- derived operands -------------------------------------------------
    @property
    def simm(self):
        return sext(self.i, 8)

    @property
    def is_delayed(self):
        return self.op in DELAYED

    @property
    def is_terminator(self):
        return self.op in TERMINATORS

    @property
    def is_call(self):
        return self.op in CALLS

    @property
    def is_cond(self):
        return self.op in COND_BRANCHES

    def branch_target(self):
        """Static target of bra/bsr/bt/bf (None for register forms)."""
        if self.op in ("BRA", "BSR"):
            return (self.pc + 4 + sext(self.d, 12) * 2) & 0xFFFFFFFF
        if self.op in COND_BRANCHES:
            return (self.pc + 4 + sext(self.d, 8) * 2) & 0xFFFFFFFF
        return None

    def pcrel_address(self):
        """Address read by mov.w/mov.l @(disp,pc) and mova."""
        if self.op == "MOV_W_PC":
            return (self.pc + 4 + self.d * 2) & 0xFFFFFFFF
        if self.op in ("MOV_L_PC", "MOVA"):
            return ((self.pc & ~3) + 4 + self.d * 4) & 0xFFFFFFFF
        return None

    def text(self):
        """objdump-like rendering, for diagnostics and the test assembler."""
        if self.form is None:
            return ".word 0x%04x" % self.word
        s = self.form.syntax
        t = self.branch_target()
        return s.format(
            n=self.n, m=self.m, simm=self.simm, imm=self.i,
            d1=self.d, d2=self.d * 2, d4=self.d * 4,
            n2=self.n * 2, m2=self.m * 2, n4=self.n * 4, m4=self.m * 4,
            target=("0x%08x" % t) if t is not None else "?",
        )

    def __repr__(self):
        return "<%08x %04x %s>" % (self.pc, self.word, self.text())


def decode(pc, word):
    return Instr(pc, word)
