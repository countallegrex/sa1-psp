#!/usr/bin/env python3
"""Emitter test harness.

Each case is a hand-written SH-4 snippet.  The snippets are assembled into a
small image (no game data involved), run through the emitter exactly like
the game block, compiled with the host C compiler together with the runtime,
and executed; register and memory results are compared with expectations.

    python tests/emitter/run_tests.py [-k substring] [--cc clang] [--keep]

Exit status 0 when every case passes.  Needs a C compiler on PATH (clang,
gcc or cc) and nothing else."""

import argparse
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
EMITTER = os.path.join(HERE, "..", "..", "recomp", "emitter")
sys.path.insert(0, EMITTER)
sys.path.insert(0, HERE)

from sh4emit import isa  # noqa: E402
from sh4emit.decode import Instr  # noqa: E402
from sh4emit.image import Image  # noqa: E402
from sh4emit.emit import Program  # noqa: E402
import sh4asm  # noqa: E402

BASE = 0x8C010000          # where the test image lives (like 1ST_READ.BIN)
STRIDE = 0x200             # bytes per case
DATA = 0x8C200000          # scratch RAM the cases may use
SEAM_ADDR = 0x8C5FB000     # a fake SDK entry point implemented in C below
SEAM2_ADDR = 0x8C5FB010

# ---------------------------------------------------------------- encodings
# (word, text) pairs from the SH-4 software manual's instruction listing.
KNOWN_ENCODINGS = [
    (0x321C, "add r1,r2"), (0x7A05, "add #5,r10"), (0x30FE, "addc r15,r0"),
    (0x9001, "mov.w @(2,pc),r0"), (0xD102, "mov.l @(8,pc),r1"), (0xE1FF, "mov #-1,r1"),
    (0x6213, "mov r1,r2"), (0x2120, "mov.b r2,@r1"), (0x2121, "mov.w r2,@r1"),
    (0x2122, "mov.l r2,@r1"), (0x6120, "mov.b @r2,r1"), (0x6122, "mov.l @r2,r1"),
    (0x2F26, "mov.l r2,@-r15"), (0x62F6, "mov.l @r15+,r2"), (0x8012, "mov.b r0,@(2,r1)"),
    (0x8114, "mov.w r0,@(8,r1)"), (0x1123, "mov.l r2,@(12,r1)"), (0x8412, "mov.b @(2,r1),r0"),
    (0x5123, "mov.l @(12,r2),r1"), (0x0124, "mov.b r2,@(r0,r1)"), (0x012E, "mov.l @(r0,r2),r1"),
    (0xC004, "mov.b r0,@(4,gbr)"), (0xC602, "mov.l @(8,gbr),r0"), (0xC703, "mova @(12,pc),r0"),
    (0x0129, "movt r1"), (0x6128, "swap.b r2,r1"), (0x212D, "xtrct r2,r1"),
    (0x8805, "cmp/eq #5,r0"), (0x3120, "cmp/eq r2,r1"), (0x3126, "cmp/hi r2,r1"),
    (0x4115, "cmp/pl r1"), (0x4111, "cmp/pz r1"), (0x3124, "div1 r2,r1"), (0x2127, "div0s r2,r1"),
    (0x0019, "div0u"), (0x312D, "dmuls.l r2,r1"), (0x4110, "dt r1"), (0x612E, "exts.b r2,r1"),
    (0x012F, "mac.l @r2+,@r1+"), (0x412F, "mac.w @r2+,@r1+"), (0x0127, "mul.l r2,r1"),
    (0x612B, "neg r2,r1"), (0x3128, "sub r2,r1"), (0x2129, "and r2,r1"), (0xC90F, "and #15,r0"),
    (0xCD0F, "and.b #15,@(r0,gbr)"), (0x6127, "not r2,r1"), (0x411B, "tas.b @r1"),
    (0x2128, "tst r2,r1"), (0xC880, "tst #128,r0"), (0x4104, "rotl r1"), (0x4124, "rotcl r1"),
    (0x412C, "shad r2,r1"), (0x4120, "shal r1"), (0x4121, "shar r1"), (0x412D, "shld r2,r1"),
    (0x4100, "shll r1"), (0x4108, "shll2 r1"), (0x4118, "shll8 r1"), (0x4128, "shll16 r1"),
    (0x4101, "shlr r1"), (0x8B05, "bf 0x8c01000e"), (0x8F05, "bf/s 0x8c01000e"),
    (0x8905, "bt 0x8c01000e"), (0x8D05, "bt/s 0x8c01000e"), (0xA005, "bra 0x8c01000e"),
    (0x0123, "braf r1"), (0xB005, "bsr 0x8c01000e"), (0x0103, "bsrf r1"), (0x412B, "jmp @r1"),
    (0x410B, "jsr @r1"), (0x000B, "rts"), (0x0028, "clrmac"), (0x0048, "clrs"), (0x0008, "clrt"),
    (0x410E, "ldc r1,sr"), (0x411E, "ldc r1,gbr"), (0x4107, "ldc.l @r1+,sr"), (0x410A, "lds r1,mach"),
    (0x411A, "lds r1,macl"), (0x412A, "lds r1,pr"), (0x4126, "lds.l @r1+,pr"), (0x0009, "nop"),
    (0x002B, "rte"), (0x0058, "sets"), (0x0018, "sett"), (0x0102, "stc sr,r1"), (0x0112, "stc gbr,r1"),
    (0x010A, "sts mach,r1"), (0x012A, "sts pr,r1"), (0x4122, "sts.l pr,@-r1"), (0xC305, "trapa #5"),
    (0xF18D, "fldi0 fr1"), (0xF19D, "fldi1 fr1"), (0xF12C, "fmov fr2,fr1"), (0xF128, "fmov.s @r2,fr1"),
    (0xF126, "fmov.s @(r0,r2),fr1"), (0xF129, "fmov.s @r2+,fr1"), (0xF12A, "fmov.s fr2,@r1"),
    (0xF12B, "fmov.s fr2,@-r1"), (0xF127, "fmov.s fr2,@(r0,r1)"), (0xF11D, "flds fr1,fpul"),
    (0xF10D, "fsts fpul,fr1"), (0xF15D, "fabs fr1"), (0xF120, "fadd fr2,fr1"), (0xF124, "fcmp/eq fr2,fr1"),
    (0xF125, "fcmp/gt fr2,fr1"), (0xF123, "fdiv fr2,fr1"), (0xF12D, "float fpul,fr1"),
    (0xF12E, "fmac fr0,fr2,fr1"), (0xF122, "fmul fr2,fr1"), (0xF14D, "fneg fr1"), (0xF16D, "fsqrt fr1"),
    (0xF121, "fsub fr2,fr1"), (0xF13D, "ftrc fr1,fpul"), (0xF2BD, "fcnvds dr2,fpul"),
    (0xF2AD, "fcnvsd fpul,dr2"), (0x416A, "lds r1,fpscr"), (0x415A, "lds r1,fpul"),
    (0x4166, "lds.l @r1+,fpscr"), (0x016A, "sts fpscr,r1"), (0x015A, "sts fpul,r1"),
    (0x4162, "sts.l fpscr,@-r1"), (0xF4ED, "fipr fv0,fv4"), (0xF9FD, "ftrv xmtrx,fv8"),
    (0xFBFD, "frchg"), (0xF3FD, "fschg"), (0xF2FD, "fsca fpul,dr2"), (0xF17D, "fsrra fr1"),
]

# ------------------------------------------------------------------- cases
# Each case: name, asm, setup dict, expect dict.  Keys: r0..r15, t, s, q, m,
# mach, macl, gbr, fpul, fpscr, fr0..fr15, xf0..xf15, and mem32/mem16/mem8
# at an address ("mem32@0x8c200000").  Float expectations may be exact
# numbers or ("approx", value, tolerance).
CASES = []


def case(name, asm, setup=None, expect=None, calls=None, entry=0, extra=(), branch=()):
    """entry: byte offset of the function the harness calls; extra: offsets
    of further function entries inside this case's region; branch: offsets
    of entries given the discovery origin "branch" (a branch target that may
    or may not be a real function)."""
    CASES.append({"name": name, "asm": asm, "setup": setup or {}, "expect": expect or {},
                  "calls": calls or [], "entry": entry, "extra": list(extra), "branch": list(branch)})


RET = "\n rts\n nop\n"

case("mov_imm", "mov #-3,r0\n mov #100,r1\n mov r1,r2" + RET,
     {}, {"r0": 0xFFFFFFFD, "r1": 100, "r2": 100})
case("add_sub_logic", "add r5,r4\n sub r6,r4\n and r7,r5\n or r7,r6\n xor r6,r7\n not r4,r8\n neg r5,r9" + RET,
     {"r4": 10, "r5": 0xF0F0, "r6": 3, "r7": 0x0FF0},
     {"r4": 0xF0F7, "r5": 0x00F0, "r6": 0x0FF3, "r7": 0x0003, "r8": 0xFFFF0F08, "r9": 0xFFFFFF10})
case("add_imm_wrap", "add #-1,r4\n add #127,r5\n add #-128,r6" + RET,
     {"r4": 0, "r5": 1, "r6": 0x80000000},
     {"r4": 0xFFFFFFFF, "r5": 128, "r6": 0x7FFFFF80})
case("addc_chain", "clrt\n addc r6,r4\n addc r7,r5\n movt r0" + RET,
     {"r4": 0xFFFFFFFF, "r6": 1, "r5": 5, "r7": 7},
     {"r4": 0, "r5": 13, "r0": 0, "t": 0})
case("addc_carry_in_overflow", "sett\n addc r5,r4" + RET,
     {"r4": 0xFFFFFFFF, "r5": 0}, {"r4": 0, "t": 1})
case("subc", "sett\n subc r5,r4\n movt r1\n clrt\n subc r7,r6" + RET,
     {"r4": 10, "r5": 3, "r6": 0, "r7": 1}, {"r4": 6, "r1": 0, "r6": 0xFFFFFFFF, "t": 1})
case("addv_subv", "addv r5,r4\n movt r0\n subv r7,r6\n movt r1\n addv r9,r8\n movt r2" + RET,
     {"r4": 0x7FFFFFFF, "r5": 1, "r6": 0x80000000, "r7": 1, "r8": 5, "r9": 6},
     {"r0": 1, "r1": 1, "r2": 0, "r4": 0x80000000, "r6": 0x7FFFFFFF, "r8": 11})
case("negc", "clrt\n negc r4,r0\n movt r1\n sett\n negc r5,r2\n movt r3" + RET,
     {"r4": 1, "r5": 0}, {"r0": 0xFFFFFFFF, "r1": 1, "r2": 0xFFFFFFFF, "r3": 1})
case("cmp_forms", """
 cmp/eq r5,r4
 movt r0
 cmp/hs r5,r4
 movt r1
 cmp/ge r5,r4
 movt r2
 cmp/hi r5,r4
 movt r3
 cmp/gt r5,r4
 movt r6
 cmp/pl r4
 movt r7
 cmp/pz r4
 movt r8
 mov #7,r0
 cmp/eq #7,r0
 movt r9
 cmp/str r10,r11
 movt r12
""" + RET, {"r4": 0xFFFFFFFF, "r5": 1, "r10": 0x11223344, "r11": 0x55663377},
     {"r1": 1, "r2": 0, "r3": 1, "r6": 0, "r7": 0, "r8": 0, "r9": 1, "r12": 1})
case("dt_loop", "mov #0,r0\n mov #10,r1\nloop:\n add #3,r0\n dt r1\n bf loop" + RET,
     {}, {"r0": 30, "r1": 0, "t": 1})
case("ext_swap_xtrct", """
 exts.b r4,r0
 exts.w r4,r1
 extu.b r4,r2
 extu.w r4,r3
 swap.b r4,r5
 swap.w r4,r6
 mov r4,r7
 xtrct r8,r7
""" + RET, {"r4": 0x1234F8F0, "r8": 0xAABBCCDD},
     {"r0": 0xFFFFFFF0, "r1": 0xFFFFF8F0, "r2": 0xF0, "r3": 0xF8F0, "r5": 0x1234F0F8,
      "r6": 0xF8F01234, "r7": 0xCCDD1234})
case("shifts", """
 mov r4,r0
 shll r0
 movt r1
 mov r4,r2
 shlr r2
 movt r3
 mov r4,r5
 shar r5
 mov r4,r6
 shal r6
 mov r4,r7
 rotl r7
 mov r4,r8
 rotr r8
 mov r4,r9
 shll2 r9
 mov r4,r10
 shlr8 r10
 mov r4,r11
 shll16 r11
 mov r4,r12
 shlr16 r12
""" + RET, {"r4": 0x80000001},
     {"r0": 2, "r1": 1, "r2": 0x40000000, "r3": 1, "r5": 0xC0000000, "r6": 2, "r7": 3,
      "r8": 0xC0000000, "r9": 4, "r10": 0x00800000, "r11": 0x00010000, "r12": 0x8000})
case("rotc", "clrt\n rotcl r4\n movt r0\n sett\n rotcr r5\n movt r1" + RET,
     {"r4": 0x80000001, "r5": 0x00000001}, {"r4": 2, "r0": 1, "r5": 0x80000000, "r1": 1})
case("shad_shld", """
 mov r4,r0
 shad r5,r0
 mov r4,r1
 shad r6,r1
 mov r4,r2
 shad r7,r2
 mov r4,r3
 shld r6,r3
 mov r4,r8
 shld r7,r8
 mov r4,r9
 shld r5,r9
""" + RET, {"r4": 0x80000010, "r5": 4, "r6": -4 & 0xFFFFFFFF, "r7": -32 & 0xFFFFFFFF},
     {"r0": 0x00000100, "r1": 0xF8000001, "r2": 0xFFFFFFFF, "r3": 0x08000001, "r8": 0, "r9": 0x100})
case("loads_stores", """
 mov.l r5,@r4
 mov r5,r0
 mov.w r0,@(4,r4)
 mov.b r0,@(6,r4)
 mov.l @r4,r0
 mov.w @(4,r4),r0
 mov r0,r1
 mov.b @(6,r4),r0
 mov r0,r2
 mov #8,r0
 mov.l r5,@(r0,r4)
 mov.l @(r0,r4),r3
 mov.w @(r0,r4),r6
 mov.b @(r0,r4),r7
 mov.w r5,@r4
 mov.w @r4,r8
 mov.b r5,@r4
 mov.b @r4,r9
""" + RET, {"r4": DATA, "r5": 0xFFFF8081},
     {"r1": 0xFFFF8081, "r2": 0xFFFFFF81, "r3": 0xFFFF8081, "r6": 0xFFFF8081, "r7": 0xFFFFFF81,
      "r8": 0xFFFF8081, "r9": 0xFFFFFF81,
      "mem32@%#x" % DATA: 0xFFFF8081, "mem16@%#x" % (DATA + 4): 0x8081, "mem8@%#x" % (DATA + 6): 0x81})
case("mov_disp_forms", "mov.l @(8,r4),r0\n mov.w @(2,r4),r0\n mov r0,r1\n mov.b @(1,r4),r0\n mov.l r1,@(12,r4)" + RET,
     {"r4": DATA, "mem32@%#x" % (DATA + 8): 0x01020304, "mem32@%#x" % DATA: 0x80FF7F00},
     {"r1": 0xFFFF80FF, "r0": 0x7F, "mem32@%#x" % (DATA + 12): 0xFFFF80FF})
case("predec_postinc", """
 mov.l r5,@-r4
 mov.w r5,@-r4
 mov.b r5,@-r4
 mov r4,r6
 mov.b @r6+,r0
 mov.w @r6+,r1
 mov.l @r6+,r2
 mov r4,r3
 add #3,r3
 mov.l @r3+,r3
""" + RET, {"r4": DATA + 16, "r5": 0x12345678},
     {"r4": DATA + 9, "r6": DATA + 16, "r0": 0x78, "r1": 0x5678, "r2": 0x12345678,
      "r3": 0x12345678})
case("gbr_forms", """
 mov.l r0,@(4,gbr)
 mov.w r0,@(2,gbr)
 mov.b r0,@(1,gbr)
 mov.l @(4,gbr),r0
 mov r0,r1
 mov.b @(1,gbr),r0
 mov r0,r2
 mov #0,r0
 and.b #15,@(r0,gbr)
 or.b #128,@(r0,gbr)
 tst.b #128,@(r0,gbr)
 movt r3
 xor.b #255,@(r0,gbr)
 mov.b @(0,gbr),r0
""" + RET, {"gbr": DATA, "r0": 0xDEADBEEF, "mem32@%#x" % DATA: 0x000000FF},
     {"r1": 0xDEADBEEF, "r2": 0xFFFFFFEF, "r3": 0, "r0": 0x70})
case("pool_fold_and_mova", """
 mov.l cst,r0
 mov.w wcst,r1
 mov r0,r2
 mova tbl,r0
 mov.l @r0,r3
 rts
 nop
 .align 4
cst: .long 0x89ABCDEF
wcst: .word 0x8000
 .align 4
tbl: .long 0x11111111
""", {}, {"r2": 0x89ABCDEF, "r1": 0xFFFF8000, "r3": 0x11111111})
case("bt_bf_delay", """
 sett
 bt/s taken
 clrt
 mov #1,r0
 rts
 nop
taken:
 mov #2,r0
 movt r1
 rts
 nop
""", {}, {"r0": 2, "r1": 0})
case("bra_delay_slot", "bra over\n mov #5,r0\n mov #9,r0\nover:\n add #1,r0" + RET, {}, {"r0": 6})
case("bsr_call", """
 mov #3,r4
 bsr sub
 mov #4,r5
 add #100,r0
 rts
 nop
sub:
 rts
 add r5,r4
 nop
""", {}, {"r0": 100, "r4": 7, "r5": 4}, )
case("bsr_call_result", """
 sts.l pr,@-r15
 mov #3,r4
 bsr sub
 mov #4,r5
 lds.l @r15+,pr
 rts
 add #100,r0
sub:
 mov r4,r0
 rts
 add r5,r0
""", {"r15": DATA + 0x100}, {"r0": 107, "r15": DATA + 0x100})
case("jmp_slot_writes_reg", """
 mova target,r0
 mov r0,r1
 jmp @r1
 mov #0,r1
 .align 4
target:
 mov #7,r0
 rts
 nop
""", {}, {"r0": 7, "r1": 0})
case("braf_bsrf", """
 mov #6,r1
 bsrf r1
 nop
 add #1,r0
 rts
 nop
 nop
 mov #40,r0
 rts
 nop
""", {}, {"r0": 41})
case("switch_table", """
 mov r4,r0
 mov #3,r1
 cmp/hs r1,r0
 bt deflt
 shll r0
 mov r0,r1
 mova tbl,r0
 mov.w @(r0,r1),r0
 braf r0
 nop
tbl:
 .word c0-tbl
 .word c1-tbl
 .word c2-tbl
c0:
 mov #10,r0
 rts
 nop
c1:
 mov #11,r0
 rts
 nop
c2:
 mov #12,r0
 rts
 nop
deflt:
 mov #99,r0
 rts
 nop
""", {"r4": 2}, {"r0": 12})
# the switch case above uses label arithmetic: handled by the assembler sugar below
case("div_unsigned", """
 ; r4 / r5 unsigned (manual's 64/32 sequence with r6 = 0 as the high word)
 mov #0,r6
 div0u
""" + "\n rotcl r4\n div1 r5,r6" * 32 + """
 rotcl r4
 mov r4,r0
""" + RET, {"r4": 100000, "r5": 7}, {"r0": 100000 // 7, "r6": 100000 % 7})
case("mul_forms", """
 mul.l r4,r5
 sts macl,r0
 muls.w r4,r5
 sts macl,r1
 mulu.w r4,r5
 sts macl,r2
 dmuls.l r4,r5
 sts mach,r3
 sts macl,r6
 dmulu.l r4,r5
 sts mach,r7
""" + RET, {"r4": 0xFFFFFFFE, "r5": 0x00010003},
     {"r0": (0xFFFFFFFE * 0x00010003) & 0xFFFFFFFF, "r1": (-2 * 3) & 0xFFFFFFFF, "r2": 0xFFFE * 3,
      "r3": ((-2 * 0x00010003) >> 32) & 0xFFFFFFFF, "r6": (-2 * 0x00010003) & 0xFFFFFFFF,
      "r7": (0xFFFFFFFE * 0x00010003) >> 32})
case("mac_l", """
 clrmac
 mac.l @r4+,@r5+
 mac.l @r4+,@r5+
 sts mach,r0
 sts macl,r1
""", {"r4": DATA, "r5": DATA + 16, "mem32@%#x" % DATA: 0x40000000, "mem32@%#x" % (DATA + 4): 2,
      "mem32@%#x" % (DATA + 16): 4, "mem32@%#x" % (DATA + 20): 0xFFFFFFFF},
     {"r0": 0, "r1": 0xFFFFFFFE, "r4": DATA + 8, "r5": DATA + 24})
CASES[-1]["asm"] += RET
case("mac_w_saturate", """
 sets
 clrmac
 lds r6,macl
 mac.w @r4+,@r5+
 sts macl,r0
 sts mach,r1
 clrs
""" + RET, {"r4": DATA, "r5": DATA + 8, "r6": 0x7FFFFFF0, "mem16@%#x" % DATA: 0x7FFF, "mem16@%#x" % (DATA + 8): 0x7FFF},
     {"r0": 0x7FFFFFFF, "r1": 1})
case("tas_tst", "tas.b @r4\n movt r0\n tas.b @r4\n movt r1\n mov.b @r4,r2\n tst #1,r0\n movt r3" + RET,
     {"r4": DATA, "mem8@%#x" % DATA: 0}, {"r0": 1, "r1": 0, "r2": 0xFFFFFF80, "r3": 0})
case("sr_flags_roundtrip", "sett\n stc sr,r0\n clrt\n ldc r0,sr\n movt r1\n stc gbr,r2\n ldc r3,gbr\n stc gbr,r4" + RET,
     {"gbr": 0x1234, "r3": 0x5678}, {"r1": 1, "r2": 0x1234, "r4": 0x5678})
case("mach_macl_pr_fpul", "lds r4,mach\n lds r5,macl\n sts mach,r0\n sts macl,r1\n lds r6,fpul\n sts fpul,r2" + RET,
     {"r4": 1, "r5": 2, "r6": 3}, {"r0": 1, "r1": 2, "r2": 3})
# ---- floating point
case("fp_basic", """
 fldi1 fr0
 fldi0 fr1
 fadd fr4,fr0
 fmov fr0,fr2
 fsub fr5,fr2
 fmul fr4,fr2
 fdiv fr5,fr2
 fmov fr4,fr3
 fmac fr0,fr5,fr3
 fneg fr6
 fabs fr7
 fsqrt fr8
""" + RET, {"fr4": 2.0, "fr5": 4.0, "fr6": 1.5, "fr7": -2.5, "fr8": 9.0},
     {"fr0": 3.0, "fr1": 0.0, "fr2": -0.5, "fr3": 14.0, "fr6": -1.5, "fr7": 2.5, "fr8": 3.0})
case("fp_cmp_conv", """
 fcmp/eq fr4,fr5
 movt r0
 fcmp/gt fr4,fr5
 movt r1
 fcmp/gt fr5,fr4
 movt r2
 ftrc fr6,fpul
 sts fpul,r3
 ftrc fr7,fpul
 sts fpul,r8
 ftrc fr9,fpul
 sts fpul,r10
 lds r11,fpul
 float fpul,fr0
 flds fr5,fpul
 sts fpul,r12
 lds r13,fpul
 fsts fpul,fr1
""" + RET, {"fr4": 1.0, "fr5": 2.0, "fr6": -3.7, "fr7": 3e9, "fr9": -3e9, "r11": -5 & 0xFFFFFFFF, "r13": 0x3F800000},
     {"r0": 0, "r1": 1, "r2": 0, "r3": -3 & 0xFFFFFFFF, "r8": 0x7FFFFFFF, "r10": 0x80000000,
      "fr0": -5.0, "r12": 0x40000000, "fr1": 1.0})
case("fp_mem", """
 fmov.s fr4,@r4
 fmov.s fr5,@-r5
 mov #8,r0
 fmov.s fr6,@(r0,r4)
 fmov.s @r4,fr0
 fmov.s @r5+,fr1
 fmov.s @(r0,r4),fr2
""" + RET, {"r4": DATA, "r5": DATA + 32, "fr4": 1.25, "fr5": -8.0, "fr6": 100.0},
     {"fr0": 1.25, "fr1": -8.0, "fr2": 100.0, "r5": DATA + 32, "mem32@%#x" % DATA: 0x3FA00000})
case("fp_special", """
 lds r4,fpul
 fsca fpul,dr0
 fsrra fr4
 fipr fv4,fv8
 ftrv xmtrx,fv12
""" + RET, {"r4": 0x4000, "fr4": 4.0, "fr5": 1.0, "fr6": 2.0, "fr7": 3.0,
            "fr8": 1.0, "fr9": 1.0, "fr10": 1.0, "fr11": 1.0,
            "fr12": 1.0, "fr13": 2.0, "fr14": 3.0, "fr15": 1.0,
            # XMTRX: identity except (row0,col3) = 10 -> xf12 = 10; row1 scaled by 2 -> xf5 = 2
            "xf0": 1.0, "xf5": 2.0, "xf10": 1.0, "xf15": 1.0, "xf12": 10.0},
     {"fr0": ("approx", 1.0, 1e-5), "fr1": ("approx", 0.0, 1e-5), "fr4": 0.5,
      "fr11": 0.5 + 1.0 + 2.0 + 3.0,
      "fr12": 11.0, "fr13": 4.0, "fr14": 3.0, "fr15": 1.0})
case("fschg_pairs", """
 fschg
 fmov dr4,dr0
 fmov.s @r4,fr2
 fmov dr2,@-r5
 fmov xd0,dr6
 fschg
 fmov.s fr8,@r6
""", {"r4": DATA, "r5": DATA + 32, "r6": DATA + 48, "fr4": 1.0, "fr5": 2.0,
      "mem32@%#x" % DATA: 0x40400000, "mem32@%#x" % (DATA + 4): 0x40800000,
      "xf0": 7.0, "xf1": 8.0, "fr8": 9.0},
     {"fr0": 1.0, "fr1": 2.0, "fr2": 3.0, "fr3": 4.0, "r5": DATA + 24,
      "mem32@%#x" % (DATA + 24): 0x40400000, "mem32@%#x" % (DATA + 28): 0x40800000,
      "fr6": 7.0, "fr7": 8.0, "mem32@%#x" % (DATA + 48): 0x41100000})
CASES[-1]["asm"] = CASES[-1]["asm"].replace("fmov dr4,dr0", "fmov fr4,fr0").replace(
    "fmov dr2,@-r5", "fmov.s fr2,@-r5").replace("fmov xd0,dr6", "fmov fr1,fr6") + RET
case("frchg_bank", """
 frchg
 fadd fr1,fr0
 frchg
 fmov fr0,fr2
""" + RET, {"xf0": 1.5, "xf1": 2.0, "fr0": 10.0}, {"xf0": 3.5, "fr2": 10.0})
case("double_pr", """
 mov.l prbit,r0
 lds r0,fpscr
 fadd dr2,dr0
 fmul dr4,dr0
 mov #0,r0
 lds r0,fpscr
 rts
 nop
 .align 4
prbit: .long 0x00080000
""", {"dr0": 1.5, "dr2": 2.25, "dr4": 2.0}, {"dr0": 7.5})
# ---- calls between recompiled functions and the register ABI
case("callee", """
 mov.l r8,@-r15
 mov #77,r8
 mov r4,r0
 add r5,r0
 mov #5,r1
 rts
 mov.l @r15+,r8
""", {"r4": 1, "r5": 2, "r15": DATA + 0x100}, {"r0": 3, "r1": 5})
case("caller_abi", """
 sts.l pr,@-r15
 mov #9,r8
 mov #1,r4
 mov.l fn,r1
 jsr @r1
 mov #2,r5
 mov r1,r2
 add r8,r0
 lds.l @r15+,pr
 rts
 nop
 .align 4
fn: .long FNADDR
""", {"r15": DATA + 0x100}, {"r0": 12, "r2": 5, "r8": 9}, calls=["callee"])
case("indirect_call", """
 sts.l pr,@-r15
 mov #1,r4
 jsr @r6
 mov #2,r5
 lds.l @r15+,pr
 rts
 nop
""", {"r6": None, "r15": DATA + 0x100}, {"r0": 3, "r1": 5}, calls=["callee"])
# A callee-saved register held across a call to a function that itself makes
# an unknown indirect call: mid's summary says "may write anything", so the
# outer function reloads r10 after the call; the reload is only correct if the
# outer function stored r10 first (M1 boot bug: SHC's sprintf lost its stream
# struct pointer this way).
case("callee_saved_across_indirect", """
 mov.l r10,@-r15
 sts.l pr,@-r15
 mov #7,r10
 bsr mid
 nop
 mov r10,r0
 lds.l @r15+,pr
 rts
 mov.l @r15+,r10
 .align 4
mid:
 sts.l pr,@-r15
 mov #1,r4
 jsr @r6
 mov #2,r5
 lds.l @r15+,pr
 rts
 nop
""", {"r6": None, "r15": DATA + 0x100}, {"r0": 7}, calls=["callee"], extra=(0x14,))
# An argument passed through untouched: outer sets r4, mid never mentions r4
# and calls callee, which reads it.  r4 must be live-in of mid so that outer
# stores it before the call (M1 boot bug: file names reached the loader
# wrappers as stale garbage).
case("passthrough_arg", """
 sts.l pr,@-r15
 mov #1,r4
 bsr mid
 nop
 lds.l @r15+,pr
 rts
 nop
 .align 4
mid:
 sts.l pr,@-r15
 mov.l fn,r1
 jsr @r1
 mov #2,r5
 lds.l @r15+,pr
 rts
 nop
 .align 4
fn: .long FNADDR
""", {"r4": 100, "r15": DATA + 0x100}, {"r0": 3}, calls=["callee"], extra=(0x10,))
# A bounded table (the compare gives the entry count) whose last entry is a
# function far outside this function's range: it must be kept and become a
# tail call (M1 boot bug: a 53-entry table in the character-select code was
# cut to 4 entries by the target-range heuristic).
case("switch_far_target", """
 mov r4,r0
 mov #3,r1
 cmp/hs r1,r0
 bt deflt
 shll2 r0
 mov.l tbladdr,r1
 mov.l @(r0,r1),r1
 jmp @r1
 mov #5,r5
 .align 4
tbladdr: .long tbl
tbl: .long c0
 .long c1
 .long FNADDR
c0:
 mov #20,r0
 rts
 nop
c1:
 mov #21,r0
 rts
 nop
deflt:
 mov #99,r0
 rts
 nop
""", {"r4": 2, "r15": DATA + 0x100}, {"r0": 7}, calls=["callee"])
# A discovery entry of origin "branch" that a branch reaches (so the emitter
# would fold it into its parent) but whose address also sits in a data
# table: the game calls it through that pointer, so it must stay a function.
# (M1 boot bug: Station Square's object table pointed at such an entry.)
case("branch_entry_in_table", """
 sts.l pr,@-r15
 clrt
 bt target
 mov.l tbladdr,r1
 mov.l @r1,r1
 jsr @r1
 mov #3,r4
 lds.l @r15+,pr
 rts
 nop
 .align 4
tbladdr: .long tbl
tbl: .long TARGET
target:
 mov r4,r0
 rts
 add #4,r0
""", {"r15": DATA + 0x100}, {"r0": 7}, branch=(0x1c,))
case("tail_call", """
 mov.l fn,r1
 jmp @r1
 mov #10,r4
 .align 4
fn: .long FNADDR
""", {"r5": 20}, {"r0": 30}, calls=["callee"])
case("seam_call", """
 sts.l pr,@-r15
 mov.l fn,r1
 mov #6,r4
 jsr @r1
 mov #7,r5
 lds.l @r15+,pr
 rts
 add #1,r0
 .align 4
fn: .long %#x
""" % SEAM_ADDR, {"r15": DATA + 0x100}, {"r0": 14})
case("patched_pool", """
 mov.l wrd,r0
 mov.l adr,r1
 mov #5,r2
 mov.l r2,@r1
 mov.l wrd,r3
 rts
 nop
 .align 4
wrd: .long 0x12345678
adr: .long wrd
""", {}, {"r0": 0x12345678, "r3": 5})
case("cond_branch_out_and_loop", """
 mov #0,r0
 mov #0,r1
top:
 add #1,r1
 mov r1,r2
 cmp/eq #5,r2
 bf/s top
 add r1,r0
 rts
 nop
""", {}, {"r0": 15, "r1": 5})
CASES[-1]["asm"] = CASES[-1]["asm"].replace("cmp/eq #5,r2", "mov r1,r0\n cmp/eq #5,r0\n mov r2,r0")
# fix: the loop above sums 1..5 into r0 with r0 reused; redo explicitly
CASES[-1]["asm"] = """
 mov #0,r3
 mov #0,r1
top:
 add #1,r1
 mov r1,r0
 cmp/eq #5,r0
 bf/s top
 add r1,r3
 mov r3,r0
 rts
 nop
"""
CASES[-1]["expect"] = {"r0": 15, "r1": 5}
case("switch_abs_table", """
 mov r4,r0
 shll2 r0
 mov.l tbladdr,r1
 mov.l @(r0,r1),r1
 jmp @r1
 nop
 .align 4
tbladdr: .long tbl
tbl: .long c0
 .long c1
 .long c2
c0:
 mov #20,r0
 rts
 nop
c1:
 mov #21,r0
 rts
 nop
c2:
 mov #22,r0
 rts
 nop
""", {"r4": 1}, {"r0": 21})
case("cond_tail_call", """
target:
 mov r4,r0
 add r5,r0
 mov #5,r1
 rts
 nop
 .skip 22
main:
 mov #1,r4
 mov #2,r5
 sett
 bt target
 mov #0,r0
 rts
 nop
""", {}, {"r0": 3, "r1": 5}, entry=0x20)
case("fpscr_save_restore", """
 sts.l fpscr,@-r15
 fschg
 fmov dr4,dr0
 lds.l @r15+,fpscr
 fmov fr6,fr2
""" + RET, {"r15": DATA + 0x100, "fr4": 1.0, "fr5": 2.0, "fr6": 3.0},
     {"fr0": 1.0, "fr1": 2.0, "fr2": 3.0, "r15": DATA + 0x100})
case("const_base_access", """
 mov.l adr,r1
 mov.l r4,@r1
 mov.l @(4,r1),r3
 mov.w @(8,r1),r0
 mov r0,r2
 mov r3,r0
 rts
 nop
 .align 4
adr: .long %#x
""" % DATA, {"r4": 0xCAFEF00D, "mem32@%#x" % (DATA + 4): 0x11223344, "mem16@%#x" % (DATA + 8): 0xFFFE},
     {"mem32@%#x" % DATA: 0xCAFEF00D, "r0": 0x11223344, "r2": 0xFFFFFFFE})


# --------------------------------------------------------------- building
def resolve_case_refs():
    """Fill call-target addresses: case i lives at BASE + i*STRIDE."""
    index = {c["name"]: i for i, c in enumerate(CASES)}
    for c in CASES:
        c["asm"] = c["asm"].replace("TARGET", "%#x" % (BASE + index[c["name"]] * STRIDE + 0x1c))
        for callee in c["calls"]:
            addr = BASE + index[callee] * STRIDE
            c["asm"] = c["asm"].replace("FNADDR", "%#x" % addr)
            if c["setup"].get("r6", 0) is None:
                c["setup"]["r6"] = addr


def build_image():
    resolve_case_refs()
    size = len(CASES) * STRIDE
    img = bytearray(size)
    for i, c in enumerate(CASES):
        at = BASE + i * STRIDE
        asm = c["asm"]
        # label arithmetic in .word directives for the switch case
        code = assemble_with_diffs(asm, at)
        if len(code) > STRIDE:
            raise SystemExit("case %s is %d bytes, over the %d stride" % (c["name"], len(code), STRIDE))
        img[i * STRIDE:i * STRIDE + len(code)] = code
    return bytes(img)


def assemble_with_diffs(asm, at):
    """sh4asm with support for `.word a-b` (label difference)."""
    if "-" not in asm or ".word" not in asm:
        return sh4asm.assemble(asm, at)
    # first pass with zeros to get labels, then substitute
    lines = asm.splitlines()
    tmp = []
    for l in lines:
        s = l.strip()
        if s.startswith(".word") and "-" in s:
            tmp.append(" .word 0")
        else:
            tmp.append(l)
    sh4asm.assemble("\n".join(tmp), at)   # validates
    # compute labels manually
    labels = {}
    pc = at
    expanded = [sh4asm._norm(l) for l in sh4asm.split_labels(asm)]
    for s in expanded:
        if not s:
            continue
        if s.endswith(":"):
            labels[s[:-1]] = pc
        elif s.startswith(".align"):
            n = int(s.split()[1])
            while pc % n:
                pc += 2
        elif s.startswith(".word"):
            pc += 2
        elif s.startswith(".long") or s.startswith(".float"):
            pc += 4
        elif s.startswith(".skip"):
            pc += int(s.split()[1])
        else:
            pc += 2
    out = []
    for l in lines:
        s = l.strip()
        if s.startswith(".word") and "-" in s:
            a, b = s.split()[1].split("-")
            out.append(" .word %d" % ((labels[a] - labels[b]) & 0xFFFF))
        else:
            out.append(l)
    return sh4asm.assemble("\n".join(out), at)


def write_main(path, prog):
    L = []
    L.append('#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <math.h>\n')
    L.append('#include "sh4_rt.h"\n#include "game.h"\n#include "seam.h"\n')
    L.append("static int failures;\n")
    L.append("void %s(sh4_ctx *c) { c->r[0] = c->r[4] + c->r[5]; }\n" % prog.seam_name(SEAM_ADDR))
    L.append("void %s(sh4_ctx *c) { c->r[0] = 0; }\n" % prog.seam_name(SEAM2_ADDR))
    L.append("const sa1_fn_entry sa1_seam_table[] = { {0x%08xu, %s}, {0x%08xu, %s} };\n"
             "const uint32_t sa1_seam_count = 2;\n" % (SEAM_ADDR & 0x1FFFFFFF, prog.seam_name(SEAM_ADDR),
                                                     SEAM2_ADDR & 0x1FFFFFFF, prog.seam_name(SEAM2_ADDR)))
    L.append("""
static void check_u32(const char *name, const char *what, uint32_t got, uint32_t want) {
    if (got != want) { printf("FAIL %s: %s = 0x%08x, want 0x%08x\\n", name, what, got, want); failures++; }
}
static void check_f32(const char *name, const char *what, float got, float want, float tol) {
    int ok = tol == 0 ? (got == want) : (fabsf(got - want) <= tol);
    if (!ok) { printf("FAIL %s: %s = %g, want %g\\n", name, what, got, want); failures++; }
}
static void check_f64(const char *name, const char *what, double got, double want) {
    if (got != want) { printf("FAIL %s: %s = %g, want %g\\n", name, what, got, want); failures++; }
}
""")
    L.append("int main(void) {\n    sa1_ram_set(calloc(SA1_RAM_ALLOC, 1));\n")
    L.append("    FILE *f = fopen(\"image.bin\", \"rb\"); if (!f) { puts(\"no image\"); return 2; }\n")
    L.append("    fread(sa1_ram + (0x%08xu & SA1_RAM_MASK), 1, %d, f); fclose(f);\n" % (BASE, len(CASES) * STRIDE))
    L.append("    sh4_ctx ctx;\n    int before = failures;\n")
    for i, c in enumerate(CASES):
        name = c["name"]
        L.append("    /* ---- %s ---- */\n    memset(&ctx, 0, sizeof ctx); before = failures;\n" % name)
        L.append("    memset(sa1_ram + (0x%08xu & SA1_RAM_MASK), 0, 0x1000);\n" % DATA)
        # R15 is always a P1 RAM address in the game (runtime/sh4_rt.h RAMK),
        # so every case gets a stack at the top of the scratch page.
        L.append("    ctx.r[15] = 0x%08xu;\n" % (DATA + 0xFF0))
        for k, v in c["setup"].items():
            L.append("    " + setup_stmt(k, v) + "\n")
        L.append("    %s(&ctx);\n" % prog.fn_name(BASE + i * STRIDE + c["entry"]))
        for k, v in c["expect"].items():
            L.append("    " + expect_stmt(name, k, v) + "\n")
        L.append('    if (failures == before) { passed++; printf("ok   %s\\n"); }\n' % name)
    L.append('    printf("%%d of %d cases passed\\n", passed);\n' % len(CASES))
    L.append("    return failures ? 1 : 0;\n}\n")
    text = "".join(L)
    text = text.replace("static int failures;\n", "static int failures, passed;\n")
    with open(path, "w", newline="\n") as f:
        f.write(text)


def lvalue(key):
    if key.startswith("mem32@"):
        return "RD32(%s, 0)" % key[6:], "u32"
    if key.startswith("mem16@"):
        return "RD16(%s, 0)" % key[6:], "u32"
    if key.startswith("mem8@"):
        return "RD8(%s, 0)" % key[5:], "u32"
    if key.startswith("fr"):
        return "ctx.fr[%d]" % int(key[2:]), "f32"
    if key.startswith("xf"):
        return "ctx.xf[%d]" % int(key[2:]), "f32"
    if key.startswith("dr"):
        n = int(key[2:])
        return "DR(ctx.fr[%d], ctx.fr[%d])" % (n, n + 1), "f64"
    if key.startswith("r"):
        return "ctx.r[%d]" % int(key[1:]), "u32"
    return "ctx.%s" % key, "u32"


def setup_stmt(key, v):
    lv, kind = lvalue(key)
    if key.startswith("mem32@"):
        return "WR32(%s, 0, 0x%08xu);" % (key[6:], v)
    if key.startswith("mem16@"):
        return "WR16(%s, 0, 0x%04xu);" % (key[6:], v)
    if key.startswith("mem8@"):
        return "WR8(%s, 0, 0x%02xu);" % (key[5:], v)
    if kind == "f32":
        return "%s = %rf;" % (lv, float(v))
    if kind == "f64":
        n = int(key[2:])
        return "DR_SET(ctx.fr[%d], ctx.fr[%d], %r);" % (n, n + 1, float(v))
    return "%s = 0x%08xu;" % (lv, v & 0xFFFFFFFF)


def expect_stmt(name, key, v):
    lv, kind = lvalue(key)
    if kind == "u32":
        return 'check_u32("%s", "%s", %s, 0x%08xu);' % (name, key, lv, v & 0xFFFFFFFF)
    if kind == "f64":
        return 'check_f64("%s", "%s", %s, %r);' % (name, key, lv, float(v))
    if isinstance(v, tuple):
        return 'check_f32("%s", "%s", %s, %rf, %rf);' % (name, key, lv, float(v[1]), float(v[2]))
    return 'check_f32("%s", "%s", %s, %rf, 0.0f);' % (name, key, lv, float(v))


def check_known_encodings():
    bad = 0
    for word, text in KNOWN_ENCODINGS:
        ins = Instr(0x8C010000, word)
        if ins.text() != text:
            print("FAIL decode 0x%04x: got %r want %r" % (word, ins.text(), text))
            bad += 1
        try:
            enc = sh4asm.encode(sh4asm._norm(text), 0x8C010000, {})
        except sh4asm.AsmError as e:
            print("FAIL assemble %r: %s" % (text, e))
            bad += 1
            continue
        if enc != word:
            print("FAIL assemble %r: got 0x%04x want 0x%04x" % (text, enc, word))
            bad += 1
    print("encodings: %d checked, %d bad" % (len(KNOWN_ENCODINGS), bad))
    return bad == 0


def find_cc(pref):
    for cc in ([pref] if pref else []) + ["clang", "gcc", "cc"]:
        if cc and shutil.which(cc):
            return cc
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", help="only cases whose name contains this")
    ap.add_argument("--cc")
    ap.add_argument("--keep", action="store_true", help="keep the build directory")
    ap.add_argument("--out", help="build directory (default: a temp dir)")
    args = ap.parse_args()

    ok = check_known_encodings()
    if args.k:
        keep = [c for c in CASES if args.k in c["name"] or c["name"] in sum((x["calls"] for x in CASES if args.k in x["name"]), [])]
        CASES[:] = keep
    image = build_image()
    img = Image(image, BASE)
    functions = []
    for i, c in enumerate(CASES):
        at = BASE + i * STRIDE
        for off in sorted({c["entry"], 0, *c["extra"], *c["branch"]}):
            functions.append((at + off, at + STRIDE, "branch" if off in c["branch"] else "call"))
    seam = {SEAM_ADDR: ("test_add", 1), SEAM2_ADDR: ("test_zero", 1)}
    prog = Program(img, BASE, BASE + len(CASES) * STRIDE, functions, seam,
                   sdk_range=(0x8C5FA000, 0x8C66C000), log=lambda *a: None)
    prog.build()
    out = args.out or tempfile.mkdtemp(prefix="sh4emit_test_")
    counts = prog.write_all(out, chunks=1)
    shutil.copy(os.path.join(EMITTER, "runtime", "sh4_rt.h"), out)
    shutil.copy(os.path.join(EMITTER, "runtime", "sh4_rt.c"), out)
    shutil.copy(os.path.join(EMITTER, "runtime", "sh4_trig.h"), out)
    with open(os.path.join(out, "image.bin"), "wb") as f:
        f.write(image)
    write_main(os.path.join(out, "test_main.c"), prog)
    cc = find_cc(args.cc)
    if not cc:
        sys.exit("no C compiler found")
    exe = os.path.join(out, "sh4emit_test" + (".exe" if os.name == "nt" else ""))
    srcs = [os.path.join(out, x) for x in ("test_main.c", "game_000.c", "game_table.c", "sh4_rt.c")]
    cmd = [cc, "-std=c11", "-O1", "-fno-strict-aliasing", "-ffp-contract=off", "-Wall", "-Wno-unused-label",
           "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-parentheses",
           "-D_CRT_SECURE_NO_WARNINGS", "-I", out, "-o", exe] + srcs
    if os.name != "nt":
        cmd.append("-lm")
    print("emitted %d functions, %d instructions; compiling with %s" % (counts["functions"], counts["instructions"], cc))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr)
        sys.exit("compile failed (build dir %s)" % out)
    if r.stderr.strip():
        print(r.stderr)
    r = subprocess.run([exe], cwd=out, capture_output=True, text=True)
    print(r.stdout, end="")
    if r.stderr.strip():
        print(r.stderr, end="")
    if r.returncode != 0:
        ok = False
    if not args.keep and not args.out:
        shutil.rmtree(out, ignore_errors=True)
    else:
        print("build dir:", out)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
