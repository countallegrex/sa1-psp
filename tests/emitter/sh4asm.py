"""A tiny SH-4 assembler for the emitter tests.

It is driven by the same encoding table as the decoder (recomp/emitter/
sh4emit/isa.py), so it assembles exactly the syntax the decoder prints.  A
handful of hand-verified encodings from the SH-4 manual in run_tests.py
guard the shared table against typos.

Syntax: one instruction, label (`name:`) or directive per line; `;` or `!`
starts a comment.  Directives: `.word v`, `.long v`, `.float v`, `.align n`,
`.skip n`.  Sugar: `mov.l label,rN`, `mov.w label,rN`, `mova label,r0`
compute the PC-relative displacement from a label (the label must be
reachable and, for .l, 4-aligned relative to the instruction's PC & ~3).
Branch targets are labels or absolute hex addresses."""

import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "recomp", "emitter"))

from sh4emit import isa  # noqa: E402


def _syntax_regex(syntax):
    """Turn 'mov.l @({d4},pc),r{n}' into a regex with named groups."""
    out = ""
    i = 0
    while i < len(syntax):
        ch = syntax[i]
        if ch == "{":
            j = syntax.index("}", i)
            field = syntax[i + 1:j]
            out += "(?P<%s>-?(?:0x[0-9a-f]+|[0-9]+|[A-Za-z_.][A-Za-z0-9_.]*))" % field
            i = j + 1
        else:
            out += re.escape(ch)
            i += 1
    return re.compile("^" + out + "$")


_FORMS = [(f, _syntax_regex(f.syntax)) for f in isa.FORMS]
_SUGAR = {
    "mov.l": isa.BY_OP["MOV_L_PC"],
    "mov.w": isa.BY_OP["MOV_W_PC"],
    "mova": isa.BY_OP["MOVA"],
}


class AsmError(Exception):
    pass


def _num(text, labels=None):
    if isinstance(text, int):
        return text
    text = text.strip()
    if labels is not None and text in labels:
        return labels[text]
    neg = text.startswith("-")
    if neg:
        text = text[1:]
    v = int(text, 16) if text.lower().startswith("0x") else int(text, 10)
    return -v if neg else v


def split_labels(text):
    """Yield source lines with a leading 'label:' moved to its own line."""
    for l in text.splitlines():
        body = l.split(";")[0].split("!")[0].strip()
        if ":" in body and not body.endswith(":"):
            lab, rest = body.split(":", 1)
            if re.match(r"^[A-Za-z_.][A-Za-z0-9_.]*$", lab.strip()):
                yield lab.strip() + ":"
                yield rest.strip()
                continue
        yield l


def _norm(line):
    line = line.split(";")[0].split("!")[0].strip().lower()
    line = re.sub(r"\s+", " ", line)
    # 'mnemonic operands' -> remove spaces inside the operand list
    if " " in line:
        mn, ops = line.split(" ", 1)
        line = mn + " " + ops.replace(" ", "")
    return line


def assemble(text, base):
    """Return bytes for `text` placed at guest address `base`."""
    lines = [_norm(l) for l in split_labels(text)]
    # pass 1: addresses of labels
    labels = {}
    pc = base
    items = []
    for raw in lines:
        if not raw:
            continue
        if raw.endswith(":"):
            labels[raw[:-1]] = pc
            continue
        if raw.startswith(".align"):
            n = _num(raw.split(" ", 1)[1])
            while pc % n:
                items.append(("word", 0x0009, pc))
                pc += 2
            continue
        if raw.startswith(".skip"):
            n = _num(raw.split(" ", 1)[1])
            items.append(("skip", n, pc))
            pc += n
            continue
        if raw.startswith(".word"):
            items.append(("word", raw.split(" ", 1)[1], pc))
            pc += 2
            continue
        if raw.startswith(".long"):
            items.append(("long", raw.split(" ", 1)[1], pc))
            pc += 4
            continue
        if raw.startswith(".float"):
            items.append(("float", raw.split(" ", 1)[1], pc))
            pc += 4
            continue
        items.append(("ins", raw, pc))
        pc += 2
    # pass 2
    out = bytearray()
    for kind, val, at in items:
        if kind == "skip":
            out += bytes(val)
        elif kind == "word":
            out += struct.pack("<H", _num(val, labels) & 0xFFFF)
        elif kind == "long":
            out += struct.pack("<I", _num(val, labels) & 0xFFFFFFFF)
        elif kind == "float":
            out += struct.pack("<f", float(val))
        else:
            out += struct.pack("<H", encode(val, at, labels))
    return bytes(out)


def encode(line, pc, labels):
    mn = line.split(" ", 1)[0]
    ops = line.split(" ", 1)[1] if " " in line else ""
    # sugar: pc-relative loads by label
    if mn in _SUGAR and ops.count(",") == 1 and not ops.startswith("@") and not ops.startswith("#"):
        lab, reg = ops.split(",")
        if lab in labels:
            form = _SUGAR[mn]
            target = labels[lab]
            if mn == "mov.w":
                disp = target - (pc + 4)
                if disp < 0 or disp % 2 or disp // 2 > 255:
                    raise AsmError("%s: label out of range" % line)
                return form.encode(n=int(reg[1:]), d=disp // 2)
            disp = target - ((pc & ~3) + 4)
            if disp < 0 or disp % 4 or disp // 4 > 255:
                raise AsmError("%s: label out of range (disp %d)" % (line, disp))
            return form.encode(n=int(reg[1:]) if mn == "mov.l" else 0, d=disp // 4)
    for form, rx in _FORMS:
        mobj = rx.match(line)
        if not mobj:
            continue
        g = mobj.groupdict()
        vals = {}
        try:
            if "n" in g:
                vals["n"] = _num(g["n"])
            if "m" in g:
                vals["m"] = _num(g["m"])
            if "n2" in g:
                vals["n"] = _num(g["n2"]) // 2
            if "m2" in g:
                vals["m"] = _num(g["m2"]) // 2
            if "n4" in g:
                vals["n"] = _num(g["n4"]) // 4
            if "m4" in g:
                vals["m"] = _num(g["m4"]) // 4
            if "simm" in g:
                v = _num(g["simm"])
                if not -128 <= v <= 127:
                    raise AsmError("%s: immediate out of range" % line)
                vals["i"] = v & 0xFF
            if "imm" in g:
                vals["i"] = _num(g["imm"]) & 0xFF
            for key, scale in (("d1", 1), ("d2", 2), ("d4", 4)):
                if key in g:
                    v = _num(g[key])
                    if v % scale:
                        raise AsmError("%s: displacement not a multiple of %d" % (line, scale))
                    vals["d"] = v // scale
            if "target" in g:
                t = _num(g["target"], labels)
                disp = t - (pc + 4)
                if disp % 2:
                    raise AsmError("%s: odd branch target" % line)
                disp //= 2
                width = 12 if form.op in ("BRA", "BSR") else 8
                if not -(1 << (width - 1)) <= disp < (1 << (width - 1)):
                    raise AsmError("%s: branch out of range" % line)
                vals["d"] = disp & ((1 << width) - 1)
        except ValueError:
            continue
        return form.encode(**vals)
    # double / pair spellings: drN is the frN field, xdN the odd field N+1
    if re.search(r"(^|[^a-z])(dr|xd)[0-9]+", line):
        alt = re.sub(r"(^|[^a-z])dr([0-9]+)", lambda m: "%sfr%d" % (m.group(1), int(m.group(2))), line)
        alt = re.sub(r"(^|[^a-z])xd([0-9]+)", lambda m: "%sfr%d" % (m.group(1), int(m.group(2)) + 1), alt)
        if alt.startswith("fmov ") and "@" in alt:
            alt = alt.replace("fmov ", "fmov.s ", 1)
        if alt != line:
            return encode(alt, pc, labels)
    raise AsmError("cannot assemble: %r" % line)
