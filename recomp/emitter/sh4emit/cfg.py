"""Per-function control-flow graph: basic blocks, delay slots, switch
tables, intra-function constant propagation and call-target resolution.

A function is everything reachable from its entry by following control
flow inside the code range, stopping at other known function entries
(which become tail calls) and at the code range boundary.  Discovery's
function extents are used only as hints (switch-table target ranges)."""

from .decode import Instr, sext
from .image import phys
from . import regs
from .regs import bit, effects

CONST_OPS = {
    "MOV_I", "MOV_W_PC", "MOV_L_PC", "MOVA", "MOV", "ADD_I", "ADD", "SUB",
    "SHLL", "SHLL2", "SHLL8", "SHLL16", "SHLR", "SHLR2", "SHLR8", "SHLR16",
    "NEG", "NOT", "EXTU_B", "EXTU_W", "EXTS_B", "EXTS_W", "AND", "OR", "XOR",
    "AND_I", "OR_I", "XOR_I", "SWAP_W", "SWAP_B", "XTRCT", "SHAR", "SHAL",
}
MASK32 = 0xFFFFFFFF


class SwitchTable:
    __slots__ = ("table", "entry_size", "mode", "targets", "bounded", "index_reg")

    def __init__(self, table, entry_size, mode, targets, bounded):
        self.table = table
        self.entry_size = entry_size
        self.mode = mode          # 'braf' | 'abs' | 'rel'
        self.targets = targets    # list of absolute (phys) targets in table order
        self.bounded = bounded    # True when a compare gave the entry count


class Block:
    __slots__ = ("start", "instrs", "branch", "slot", "end", "succs", "kind",
                 "target", "table", "call_reg", "consts_in", "consts_out",
                 "preds", "note")

    def __init__(self, start):
        self.start = start
        self.instrs = []
        self.branch = None
        self.slot = None
        self.end = start
        self.succs = []
        self.kind = "fall"
        self.target = None
        self.table = None
        self.call_reg = None
        self.consts_in = {}
        self.consts_out = {}
        self.preds = []
        self.note = None

    def all_instrs(self):
        out = list(self.instrs)
        if self.slot is not None:
            out.append(self.slot)
        if self.branch is not None:
            out.append(self.branch)
        return out

    def __repr__(self):
        return "<Block %08x %s %s>" % (self.start, self.kind, ["%08x" % s for s in self.succs])


class Env:
    """What the CFG builder needs to know about the surrounding program."""

    def __init__(self, image, code_lo, code_hi, entries, patched=frozenset()):
        self.image = image
        self.code_lo = phys(code_lo)
        self.code_hi = phys(code_hi)
        self.entries = set(phys(e) for e in entries)
        self.patched = set(patched)   # pool words never to fold

    def in_code(self, addr):
        a = phys(addr)
        return self.code_lo <= a < self.code_hi

    def pool_word(self, ins):
        """Folded value of a PC-relative load, or None when it must read memory."""
        a = ins.pcrel_address()
        if ins.op == "MOVA":
            return a
        if not self.image.contains(a, 4 if ins.op == "MOV_L_PC" else 2):
            return None
        if phys(a) & ~3 in self.patched or phys(a) in self.patched:
            return None
        if ins.op == "MOV_L_PC":
            return self.image.u32(a)
        return self.image.s16(a) & MASK32


def const_step(consts, ins, env, is_call=False):
    """Apply one instruction to a {reg: value} map (GPRs only)."""
    op = ins.op
    n, m = ins.n, ins.m
    v = None
    if op == "MOV_I":
        v = ins.simm & MASK32
    elif op in ("MOV_W_PC", "MOV_L_PC"):
        v = env.pool_word(ins)
    elif op == "MOVA":
        v = ins.pcrel_address()
    elif op == "MOV":
        v = consts.get(m)
    elif op == "ADD_I":
        if n in consts:
            v = (consts[n] + ins.simm) & MASK32
    elif op in ("ADD", "SUB", "AND", "OR", "XOR"):
        if n in consts and m in consts:
            a, b = consts[n], consts[m]
            v = {"ADD": a + b, "SUB": a - b, "AND": a & b, "OR": a | b, "XOR": a ^ b}[op] & MASK32
    elif op in ("AND_I", "OR_I", "XOR_I"):
        if 0 in consts:
            a = consts[0]
            v = {"AND_I": a & ins.i, "OR_I": a | ins.i, "XOR_I": a ^ ins.i}[op]
    elif op in ("SHLL", "SHLL2", "SHLL8", "SHLL16", "SHAL"):
        if n in consts:
            v = (consts[n] << {"SHLL": 1, "SHAL": 1, "SHLL2": 2, "SHLL8": 8, "SHLL16": 16}[op]) & MASK32
    elif op in ("SHLR", "SHLR2", "SHLR8", "SHLR16"):
        if n in consts:
            v = consts[n] >> {"SHLR": 1, "SHLR2": 2, "SHLR8": 8, "SHLR16": 16}[op]
    elif op == "SHAR":
        if n in consts:
            v = (sext(consts[n], 32) >> 1) & MASK32
    elif op == "NEG":
        if m in consts:
            v = (-consts[m]) & MASK32
    elif op == "NOT":
        if m in consts:
            v = (~consts[m]) & MASK32
    elif op == "EXTU_B":
        if m in consts:
            v = consts[m] & 0xFF
    elif op == "EXTU_W":
        if m in consts:
            v = consts[m] & 0xFFFF
    elif op == "EXTS_B":
        if m in consts:
            v = sext(consts[m] & 0xFF, 8) & MASK32
    elif op == "EXTS_W":
        if m in consts:
            v = sext(consts[m] & 0xFFFF, 16) & MASK32
    elif op == "SWAP_W":
        if m in consts:
            a = consts[m]
            v = ((a << 16) | (a >> 16)) & MASK32
    elif op == "SWAP_B":
        if m in consts:
            a = consts[m]
            v = (a & 0xFFFF0000) | ((a & 0xFF) << 8) | ((a >> 8) & 0xFF)
    elif op == "XTRCT":
        if n in consts and m in consts:
            v = ((consts[m] << 16) | (consts[n] >> 16)) & MASK32
    _, defs = effects(ins, 1, 1)
    out = dict(consts)
    for r in regs.iter_bits(defs & regs.GPR_ALL):
        out.pop(r, None)
    if is_call:
        # SHC keeps r8-r14 and r15 across calls (SH ABI callee-saved); the
        # emitter trusts that for constant tracking, see README "assumptions".
        out = {r: v for r, v in out.items() if r >= 8}
    if v is not None and op in CONST_OPS:
        dest = 0 if op in ("MOVA", "AND_I", "OR_I", "XOR_I") else n
        out[dest] = v
    return out


class FunctionCFG:
    def __init__(self, env, entry, hint_end=None):
        self.env = env
        self.entry = phys(entry)
        self.hint_end = phys(hint_end) if hint_end else None
        self.blocks = {}
        self.addr2block = {}
        self.notes = []
        self.extra_entries = set()   # call targets that are not known entries
        self.build()
        self.propagate_consts()
        self.resolve_calls()

    # ------------------------------------------------------------------ build
    def note(self, pc, msg):
        self.notes.append((pc, msg))

    def is_local_target(self, target):
        t = phys(target)
        if not self.env.in_code(t):
            return False
        if t == self.entry:
            return True
        return t not in self.env.entries

    def build(self):
        work = [self.entry]
        self.drain(work)
        while True:
            self.finish()
            work = []
            for b in list(self.blocks.values()):
                if b.kind != "ijump" or b.table is False:
                    continue
                table = self.recover_switch(b, b.branch)
                if table is None:
                    b.table = False   # tried, do not retry
                    continue
                b.kind, b.table, b.call_reg = "switch", table, None
                for t in table.targets:
                    if self.is_local_target(t) and t not in b.succs:
                        b.succs.append(t)
                        work.append(t)
            if not work:
                break
            self.drain(work)
        for b in self.blocks.values():
            if b.table is False:
                b.table = None
        self.finish()

    def drain(self, work):
        env = self.env
        img = env.image
        while work:
            pc = phys(work.pop())
            if pc in self.blocks:
                continue
            owner = self.addr2block.get(pc)
            if owner is not None and owner.start != pc:
                self.split(owner, pc)
                continue
            b = Block(pc)
            self.blocks[pc] = b
            while True:
                if pc != b.start and pc in self.blocks:
                    b.kind, b.succs = "fall", [pc]
                    break
                if pc != b.start and pc in env.entries:
                    b.kind, b.target = "tail", pc
                    break
                if pc != b.start and pc in self.addr2block:
                    # ran into the middle of an already decoded block: split it
                    self.split(self.addr2block[pc], pc)
                    b.kind, b.succs = "fall", [pc]
                    break
                if not env.in_code(pc) or not img.contains(pc, 2):
                    b.kind = "stop"
                    b.note = "fell off the code range"
                    break
                ins = Instr(pc, img.u16(pc))
                if ins.op == "INVALID":
                    b.instrs.append(ins)
                    b.kind = "stop"
                    b.note = "invalid instruction"
                    self.note(pc, ".word 0x%04x: invalid instruction, emitted as trap" % ins.word)
                    pc += 2
                    break
                if not (ins.is_delayed or ins.is_cond):
                    b.instrs.append(ins)
                    self.addr2block[pc] = b
                    pc += 2
                    if ins.op in ("SLEEP",):
                        b.kind = "stop"
                        break
                    continue
                # ---- a branch ----
                b.branch = ins
                self.addr2block[pc] = b
                nxt = pc + 2
                if ins.is_delayed:
                    if img.contains(nxt, 2) and env.in_code(nxt):
                        slot = Instr(nxt, img.u16(nxt))
                        if slot.op in ("INVALID",) or slot.is_delayed or slot.is_cond:
                            self.note(nxt, "illegal delay slot %s, treated as nop" % slot.text())
                            slot = Instr(nxt, 0x0009)
                        b.slot = slot
                    else:
                        b.slot = Instr(nxt, 0x0009)
                    nxt += 2
                b.end = nxt
                self.classify_branch(b, ins, nxt, work)
                break
            if b.end < pc:
                b.end = pc

    def classify_branch(self, b, ins, nxt, work):
        op = ins.op
        if op in ("BT", "BF", "BT_S", "BF_S"):
            t = phys(ins.branch_target())
            b.kind = "cond"
            b.target = t
            b.succs = [nxt]
            if self.is_local_target(t):
                b.succs.append(t)
                work.append(t)
            work.append(nxt)
        elif op == "BRA":
            t = phys(ins.branch_target())
            b.target = t
            if self.is_local_target(t):
                b.kind, b.succs = "jump", [t]
                work.append(t)
            else:
                b.kind = "tail"
        elif op == "BSR":
            b.kind = "call"
            b.target = phys(ins.branch_target())
            b.succs = [nxt]
            work.append(nxt)
        elif op in ("JSR", "BSRF"):
            b.kind = "call"
            b.call_reg = ins.n
            b.succs = [nxt]
            work.append(nxt)
        elif op == "RTS":
            b.kind = "ret"
        elif op == "RTE":
            b.kind = "stop"
            b.note = "rte"
            self.note(ins.pc, "rte in game code: emitted as trap")
        elif op in ("JMP", "BRAF"):
            consts = self.block_consts(b)
            v = consts.get(ins.n)
            if v is not None:
                t = phys(v if op == "JMP" else (ins.pc + 4 + v) & MASK32)
                b.target = t
                if self.is_local_target(t):
                    b.kind, b.succs = "jump", [t]
                    work.append(t)
                else:
                    b.kind = "tail"
            else:
                b.kind = "ijump"       # switch recovery runs in build()'s second phase
                b.call_reg = ins.n

    def split(self, b, at):
        """Split block b at instruction address `at` (which is inside it)."""
        idx = next(i for i, ins in enumerate(b.instrs) if ins.pc == at) if any(
            ins.pc == at for ins in b.instrs) else None
        nb = Block(at)
        if idx is None:
            # the target is the branch instruction itself
            assert b.branch is not None and b.branch.pc == at, (hex(b.start), hex(at))
            nb.instrs = []
        else:
            nb.instrs = b.instrs[idx:]
            b.instrs = b.instrs[:idx]
        nb.branch, nb.slot, nb.end = b.branch, b.slot, b.end
        nb.succs, nb.kind, nb.target, nb.table, nb.call_reg, nb.note = (
            b.succs, b.kind, b.target, b.table, b.call_reg, b.note)
        b.branch = b.slot = None
        b.end = at
        b.succs, b.kind, b.target, b.table, b.call_reg, b.note = [at], "fall", None, None, None, None
        for ins in nb.instrs:
            self.addr2block[ins.pc] = nb
        if nb.branch is not None:
            self.addr2block[nb.branch.pc] = nb
        self.blocks[at] = nb

    def finish(self):
        for b in self.blocks.values():
            b.preds = []
        for b in self.blocks.values():
            for s in b.succs:
                if s in self.blocks:
                    self.blocks[s].preds.append(b.start)
        self.order = sorted(self.blocks)
        # reverse post-order for dataflow
        seen, post = set(), []

        def dfs(start):
            stack = [(start, iter(self.blocks[start].succs))]
            seen.add(start)
            while stack:
                node, it = stack[-1]
                for s in it:
                    if s not in seen:
                        seen.add(s)
                        stack.append((s, iter(self.blocks[s].succs)))
                        break
                else:
                    post.append(node)
                    stack.pop()
        dfs(self.entry)
        for s in self.order:
            if s not in seen:
                dfs(s)
        self.rpo = list(reversed(post))

    # ------------------------------------------------------------- constants
    def block_consts(self, b, upto=None, start=None):
        """Constants after b's straight-line instructions (or the first
        `upto` of them), starting from b.consts_in or `start`."""
        consts = dict(b.consts_in if start is None else start)
        seq = b.instrs if upto is None else b.instrs[:upto]
        for ins in seq:
            consts = const_step(consts, ins, self.env)
        return consts

    def propagate_consts(self):
        """Forward dataflow of GPR constants across blocks (meet = agree)."""
        for b in self.blocks.values():
            b.consts_in = {}
            b.consts_out = None
        changed = True
        rounds = 0
        while changed and rounds < 50:
            changed = False
            rounds += 1
            for s in self.rpo:
                b = self.blocks[s]
                if s == self.entry or not b.preds:
                    cin = {}
                else:
                    cin = None
                    for p in b.preds:
                        pout = self.blocks[p].consts_out
                        if pout is None:
                            continue
                        if cin is None:
                            cin = dict(pout)
                        else:
                            cin = {k: v for k, v in cin.items() if pout.get(k) == v}
                    if cin is None:
                        cin = {}
                b.consts_in = cin
                out = self.block_consts(b)
                if b.slot is not None:
                    out = const_step(out, b.slot, self.env)
                if b.kind == "call":
                    out = {r: v for r, v in out.items() if r >= 8}
                if out != b.consts_out:
                    b.consts_out = out
                    changed = True

    def resolve_calls(self):
        """Fill b.target for register calls whose register is constant."""
        for b in self.blocks.values():
            if b.kind == "call" and b.call_reg is not None:
                consts = self.block_consts(b)
                v = consts.get(b.call_reg)
                if v is not None:
                    if b.branch.op == "BSRF":
                        v = (b.branch.pc + 4 + v) & MASK32
                    b.target = phys(v)
            if b.kind == "call" and b.target is not None:
                if self.env.in_code(b.target) and b.target not in self.env.entries:
                    self.extra_entries.add(b.target)
            if b.kind == "ijump":
                consts = self.block_consts(b)
                v = consts.get(b.call_reg)
                if v is not None:
                    if b.branch.op == "BRAF":
                        v = (b.branch.pc + 4 + v) & MASK32
                    t = phys(v)
                    b.target = t
                    b.kind = "tail"
                    if self.env.in_code(t) and t not in self.env.entries and t != self.entry:
                        self.extra_entries.add(t)

    # --------------------------------------------------------- switch tables
    def recover_switch(self, b, jump):
        env, img = self.env, self.env.image
        reg = jump.n
        instrs = b.instrs
        idx = len(instrs) - 1
        rel_table = False
        while idx >= 0:
            _, d = effects(instrs[idx], 1, 1)
            if d & bit(reg):
                break
            idx -= 1
        if idx < 0:
            return None
        ins = instrs[idx]
        if ins.op == "MOV" and ins.n == reg:
            reg = ins.m
            idx -= 1
            while idx >= 0 and not (effects(instrs[idx], 1, 1)[1] & bit(reg)):
                idx -= 1
            if idx < 0:
                return None
            ins = instrs[idx]
        if ins.op == "ADD" and ins.n == reg and jump.op == "JMP":
            base_reg = ins.m
            rel_table = True
            idx -= 1
            while idx >= 0 and not (effects(instrs[idx], 1, 1)[1] & bit(reg)):
                idx -= 1
            if idx < 0:
                return None
            ins = instrs[idx]
        if ins.op not in ("MOV_B_L_R0", "MOV_W_L_R0", "MOV_L_L_R0") or ins.n != reg:
            return None
        size = {"MOV_B_L_R0": 1, "MOV_W_L_R0": 2, "MOV_L_L_R0": 4}[ins.op]
        consts = self.block_consts(b, upto=idx)
        candidates = []
        for r in (0, ins.m):
            v = consts.get(r)
            if v is not None and img.contains(v, size):
                candidates.append(v)
        if rel_table:
            v = consts.get(base_reg)
            if v is None:
                return None
            # the table base register must also be the add operand
            candidates = [v] + [c for c in candidates if c != v]
        if not candidates:
            return None
        table = phys(candidates[0])
        if jump.op == "BRAF":
            mode = "braf"
        else:
            mode = "rel" if rel_table else "abs"
        if mode == "abs" and size != 4:
            return None
        bound = self.switch_bound(b)
        lo, hi = self.target_range()
        targets = []
        addr = table
        code_addrs = set(self.addr2block)
        limit = bound if bound is not None else 512
        while len(targets) < limit:
            if not img.contains(addr, size):
                break
            if bound is None and addr in code_addrs and addr != table:
                break
            if bound is None and targets:
                later = [t for t in targets if t > table]
                if later and addr >= min(later):
                    break  # the table has run into its first case body
            if size == 1:
                e = img.s8(addr)
            elif size == 2:
                e = img.s16(addr)
            else:
                e = img.u32(addr)
            if mode == "braf":
                t = (jump.pc + 4 + e) & MASK32
            elif mode == "rel":
                t = (table + e) & MASK32
            else:
                t = e
            t = phys(t)
            if t & 1 or not (lo <= t < hi):
                break
            targets.append(t)
            addr += size
        if not targets:
            return None
        self.note(jump.pc, "switch: %d entries of %d bytes at %08x (%s%s)" % (
            len(targets), size, table, mode, ", bounded" if bound is not None else ""))
        return SwitchTable(table, size, mode, targets, bound is not None)

    def target_range(self):
        lo = self.entry
        hi = self.hint_end if self.hint_end and self.hint_end > lo else lo + 0x2000
        hi = max(hi, max(self.blocks) + 2)
        # SHC can place the whole function before a late-discovered entry;
        # allow targets a little before the entry too.
        return max(self.env.code_lo, lo - 0x400), min(self.env.code_hi, hi + 0x400)

    def switch_bound(self, b):
        """Entry count from a compare guarding the predecessor edge, or None."""
        for p in b.preds:
            pb = self.blocks[p]
            if pb.kind != "cond" or pb.branch is None:
                continue
            # which edge leads to b, and what T value takes it?
            taken_to_b = pb.target == b.start
            fall_to_b = pb.end == b.start
            if taken_to_b == fall_to_b:
                continue
            branch_on_true = pb.branch.op in ("BT", "BT_S")
            t_value = branch_on_true if taken_to_b else (not branch_on_true)
            # last T-defining instruction
            cmp = None
            for ins in reversed(pb.instrs):
                if effects(ins, 1, 1)[1] & bit(regs.T):
                    cmp = ins
                    break
            if cmp is None or cmp.op not in ("CMP_HI", "CMP_HS", "CMP_GT", "CMP_GE"):
                continue
            consts = self.block_consts(pb, upto=pb.instrs.index(cmp))
            bound = consts.get(cmp.m)
            if bound is None:
                continue
            if t_value:
                continue  # we are on the "index too large" side
            if cmp.op in ("CMP_HI", "CMP_GT"):
                return bound + 1
            return bound
        return None

    # ------------------------------------------------------------ utilities
    def instruction_count(self):
        n = 0
        for b in self.blocks.values():
            n += len(b.instrs) + (1 if b.branch else 0) + (1 if b.slot else 0)
        return n
