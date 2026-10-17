"""Dataflow over one function's CFG, with program-wide call summaries.

Per function:
  * FPSCR state (PR, SZ, FR) per instruction, each bit 0 / 1 / U(nknown),
    from the assumed entry state (0,0,0) = Katana reset mode.
  * physical use/def sets per instruction (FP bank resolved).
  * dirty sets: registers whose C local may differ from the ctx struct.
  * liveness, with calls using/defining their callee's summary sets.
  * the store/reload sets at every call and return.

Program-wide (iterated to a fixpoint by Program):
  * live_in(f): registers f may read before writing.
  * may_write(f): registers f may leave changed in the ctx struct.
  * fpscr_ret(f): the FPSCR state f returns with.
  * preserved(f): callee-saved registers f saves and restores (push/pop idiom).
"""

from . import regs
from .regs import bit, bits, effects, iter_bits
from .image import phys

U = 2  # unknown bit value

IDENTITY = (0, 0, 0)
UNKNOWN = (U, U, U)


def join_bit(a, b):
    return a if a == b else U


def join_state(a, b):
    if a is None:
        return b
    if b is None:
        return a
    return (join_bit(a[0], b[0]), join_bit(a[1], b[1]), join_bit(a[2], b[2]))


def toggle(v):
    return v if v == U else v ^ 1


class CallInfo:
    """What the emitter needs at a call or tail-call site."""
    __slots__ = ("kind", "target", "name", "live_in", "may_write", "fpscr")

    def __init__(self, kind, target, name, live_in, may_write, fpscr):
        self.kind = kind            # 'direct' | 'seam' | 'indirect' | 'sdk_dead'
        self.target = target
        self.name = name
        self.live_in = live_in
        self.may_write = may_write
        self.fpscr = fpscr


class Summaries:
    """Program-wide per-function facts, keyed by phys entry address."""

    def __init__(self):
        self.live_in = {}
        self.may_write = {}
        self.fpscr_ret = {}
        self.preserved = {}

    def get(self, entry):
        return (self.live_in.get(entry, 0), self.may_write.get(entry, 0),
                self.fpscr_ret.get(entry, IDENTITY))


class FunctionAnalysis:
    def __init__(self, cfg, program):
        self.cfg = cfg
        self.prog = program
        self.env = cfg.env
        self.notes = []
        self.states = {}     # block start -> list of states, one per all_instrs()
        self.state_in = {}
        self.state_out = {}
        self.calls = {}      # block start -> CallInfo for call/tail/ijump blocks
        self.used = 0        # physical registers mentioned anywhere
        self.nonreset_calls = 0
        self.fr_unknown = 0
        self.rt_branches = 0
        self.run()

    # ------------------------------------------------------------- helpers
    def note(self, pc, msg):
        self.notes.append((pc, msg))

    def call_info(self, b):
        """Classify the callee of a call/tail/ijump block."""
        env = self.env
        t = b.target
        if b.kind in ("call", "tail") and t is not None:
            if env.in_code(t):
                li, mw, fp = self.prog.summaries.get(t)
                pres = self.prog.summaries.preserved.get(t, 0)
                return CallInfo("direct", t, self.prog.fn_name(t), li & ~pres, mw & ~pres, fp)
            if t in self.prog.extern:
                # emitted in another unit (overlay -> main image): no summary here
                return CallInfo("direct", t, self.prog.extern[t], regs.ALL, regs.ALL, IDENTITY)
            if t in self.prog.seam:
                return CallInfo("seam", t, self.prog.seam_name(t), regs.SEAM_IN, regs.SEAM_OUT, IDENTITY)
            if self.prog.in_sdk(t):
                return CallInfo("sdk_dead", t, None, regs.ALL, regs.ALL, IDENTITY)
            return CallInfo("indirect", t, None, regs.ALL, regs.ALL, IDENTITY)
        return CallInfo("indirect", None, None, regs.ALL, regs.ALL, IDENTITY)

    # ----------------------------------------------------------- FPSCR pass
    def fpscr_transfer(self, state, ins, consts, tags):
        """Next FPSCR state after `ins`.  `tags` maps GPRs holding a copy of
        FPSCR (from sts fpscr) to the state they carry, so that the SHC
        idiom `sts fpscr,rX; and/or rY,rX; lds rX,fpscr` (rounding-mode
        changes) keeps PR/SZ/FR known.  tags is updated in place."""
        op = ins.op
        pr, sz, fr = state
        n, m = ins.n, ins.m
        if op == "STS_FPSCR":
            tags[n] = state
            return state
        if op == "MOV":
            if m in tags:
                tags[n] = tags[m]
            else:
                tags.pop(n, None)
            return state
        if op in ("AND", "OR", "XOR"):
            src = None
            if n in tags and m in consts:
                src, c = tags[n], consts[m]
            elif m in tags and n in consts:
                src, c = tags[m], consts[n]
            if src is not None:
                bitsv = ((c >> 19) & 1, (c >> 20) & 1, (c >> 21) & 1)
                out = []
                for v, cb in zip(src, bitsv):
                    if op == "AND":
                        out.append(0 if cb == 0 else v)
                    elif op == "OR":
                        out.append(1 if cb == 1 else v)
                    else:
                        out.append(v if cb == 0 else toggle(v))
                tags[n] = tuple(out)
            else:
                tags.pop(n, None)
            return state
        if op in ("AND_I", "OR_I", "XOR_I"):
            return state          # 8-bit immediates cannot reach bits 19-21
        if op == "FSCHG":
            return (pr, toggle(sz), fr)
        if op == "FRCHG":
            return (pr, sz, toggle(fr))
        if op in ("LDS_FPSCR", "LDSL_FPSCR"):
            if op == "LDS_FPSCR" and m in tags:
                return tags[m]
            if op == "LDS_FPSCR" and m in consts:
                v = consts[m]
                return ((v >> 19) & 1, (v >> 20) & 1, (v >> 21) & 1)
            if op == "LDSL_FPSCR" and m == 15 and self.saves_fpscr:
                return IDENTITY   # restore of the value saved at entry
            self.note(ins.pc, "%s: FPSCR becomes unknown here" % ins.text())
            return UNKNOWN
        _, d = effects(ins, 1, 1)
        for r in iter_bits(d & regs.GPR_ALL):
            tags.pop(r, None)
        return state

    def fpscr_pass(self):
        cfg = self.cfg
        self.saves_fpscr = any(ins.op == "STSL_FPSCR" and ins.n == 15
                               for b in cfg.blocks.values() for ins in b.all_instrs())
        state_out = {}
        state_in = {}
        changed = True
        rounds = 0
        while changed and rounds < 64:
            changed = False
            rounds += 1
            for s in cfg.rpo:
                b = cfg.blocks[s]
                if s == cfg.entry:
                    st = IDENTITY
                else:
                    st = None
                    for p in b.preds:
                        if p in state_out:
                            st = join_state(st, state_out[p])
                    if st is None:
                        st = IDENTITY
                state_in[s] = st
                seq = []
                consts = dict(b.consts_in)
                tags = {}
                from .cfg import const_step
                for ins in b.instrs:
                    seq.append(st)
                    st = self.fpscr_transfer(st, ins, consts, tags)
                    consts = const_step(consts, ins, self.env)
                if b.slot is not None:
                    seq.append(st)
                    st = self.fpscr_transfer(st, b.slot, consts, tags)
                if b.branch is not None:
                    seq.append(st)
                if b.kind == "call":
                    ci = self.call_info(b)
                    if ci.fpscr != IDENTITY:
                        st = ci.fpscr
                self.states[s] = seq
                if state_out.get(s) != st:
                    state_out[s] = st
                    changed = True
        self.state_in, self.state_out = state_in, state_out
        self.notes = sorted(set(self.notes))
        # calls made while not in the assumed entry mode break the policy
        for s, b in cfg.blocks.items():
            if b.kind in ("call", "tail") and state_out.get(s, IDENTITY) != IDENTITY:
                self.nonreset_calls += 1
                self.note(b.branch.pc if b.branch else b.end,
                          "call with FPSCR state %s (assumed-entry policy violated)" % (state_out[s],))
        # return state summary
        ret = None
        for b in cfg.blocks.values():
            if b.kind == "ret":
                ret = join_state(ret, state_out[b.start])
            elif b.kind in ("tail", "ijump"):
                ci = self.call_info(b)
                if ci.fpscr == IDENTITY:
                    ret = join_state(ret, state_out[b.start])
                else:
                    ret = join_state(ret, ci.fpscr)
        self.fpscr_ret = ret if ret is not None else IDENTITY

    # ---------------------------------------------------------- use/def pass
    def usedef(self, ins, st):
        pr, sz, fr = st
        if sz == U or pr == U:
            u = d = 0
            for szv in ((0, 1) if sz == U else (sz,)):
                for prv in ((0, 1) if pr == U else (pr,)):
                    uu, dd = effects(ins, szv, prv)
                    u |= uu
                    d |= dd
            if ins.op in ("FMOV", "FMOV_L", "FMOV_L_R0", "FMOV_LI", "FMOV_S", "FMOV_SD",
                          "FMOV_S_R0", "FABS", "FNEG", "FSQRT", "FADD", "FSUB", "FMUL",
                          "FDIV", "FCMP_EQ", "FCMP_GT", "FLOAT", "FTRC"):
                self.rt_branches += 1
                u |= bit(regs.FPSCR)
        else:
            u, d = effects(ins, sz, pr)
        if fr == U:
            self.fr_unknown += 1
            fr = 0
        return regs.phys_map(u, fr), regs.phys_map(d, fr)

    def usedef_pass(self):
        self.ud = {}   # block -> list of (use, def) per all_instrs
        used = 0
        for s, b in self.cfg.blocks.items():
            seq = []
            for ins, st in zip(b.all_instrs(), self.states[s]):
                u, d = self.usedef(ins, st)
                if ins.op in ("FSCHG", "FRCHG", "LDS_FPSCR", "LDSL_FPSCR", "STS_FPSCR", "STSL_FPSCR"):
                    u |= bit(regs.FPSCR)
                    d |= bit(regs.FPSCR)
                seq.append((u, d))
                used |= u | d
            if b.slot is not None and b.branch is not None and len(seq) >= 2:
                # the branch reads T / its register before the slot runs
                bu, bd = seq[-1]
                su, sd = seq[-2]
                seq[-2] = (su | bu, sd)
                if b.kind == "switch" or b.kind == "ijump" or (b.kind == "call" and b.call_reg is not None):
                    reg = b.call_reg if b.call_reg is not None else b.branch.n
                    seq[-2] = (seq[-2][0] | bit(reg), sd)
            self.ud[s] = seq
            if b.kind in ("call", "tail", "ijump"):
                self.calls[s] = self.call_info(b)
        self.used = used

    # -------------------------------------------------- preserved registers
    def preserved_pass(self):
        """Callee-saved registers that are only written by pops from r15 and
        are pushed to r15 somewhere: the SHC prologue/epilogue idiom."""
        pushed = 0
        popped_only = regs.CALLEE_SAVED
        for s, b in self.cfg.blocks.items():
            for ins, (u, d) in zip(b.all_instrs(), self.ud[s]):
                if ins.op == "MOV_L_SD" and ins.n == 15 and ins.m >= 8:
                    pushed |= bit(ins.m)
                elif ins.op == "FMOV_SD" and ins.n == 15:
                    pushed |= u & ~bit(15) & regs.CALLEE_SAVED
                is_pop = (ins.op == "MOV_L_LI" and ins.m == 15) or (ins.op == "FMOV_LI" and ins.m == 15)
                if not is_pop:
                    popped_only &= ~(d & regs.CALLEE_SAVED)
        pres = pushed & popped_only
        # a function that calls something which may clobber a register it does
        # not itself save does not preserve it; handled by may_write's union.
        self.preserved = pres

    # ------------------------------------------------------ dirty + liveness
    def instr_sync(self, b, i):
        """For instruction i of block b: (uses, defs, is_call, callinfo)."""
        return self.ud[b.start][i]

    def dirty_pass(self):
        cfg = self.cfg
        din, dout = {}, {}
        changed = True
        while changed:
            changed = False
            for s in cfg.rpo:
                b = cfg.blocks[s]
                d = 0
                for p in b.preds:
                    d |= dout.get(p, 0)
                if s == cfg.entry:
                    d |= 0
                din[s] = d
                for (u, dd) in self.ud[s]:
                    d |= dd
                if b.kind == "call":
                    ci = self.calls[s]
                    d &= ~ci.live_in
                    d &= ~ci.may_write
                if dout.get(s) != d:
                    dout[s] = d
                    changed = True
        self.dirty_in, self.dirty_out = din, dout

    def live_pass(self):
        cfg = self.cfg
        lin, lout = {}, {}
        changed = True
        order = list(reversed(cfg.rpo))
        while changed:
            changed = False
            for s in order:
                b = cfg.blocks[s]
                live = 0
                for t in b.succs:
                    live |= lin.get(t, 0)
                if b.kind == "ret":
                    live |= self.dirty_out[s]          # everything we store back
                elif b.kind in ("tail", "ijump"):
                    live |= self.dirty_out[s]
                    live |= self.calls[s].live_in if b.kind == "tail" else regs.ALL
                    if b.kind == "ijump" or self.calls[s].kind == "indirect":
                        live |= bit(b.call_reg) if b.call_reg is not None else 0
                elif b.kind == "switch":
                    live |= bit(b.branch.n)
                elif b.kind == "stop":
                    live |= self.dirty_out[s]
                elif b.kind == "cond" and b.target is not None and b.target not in b.succs:
                    # conditional tail call to another function
                    live |= self.dirty_out[s] | regs.ALL
                lout[s] = live
                if b.kind == "call":
                    ci = self.calls[s]
                    live_after = live
                    live &= ~ci.may_write
                    # The callee reads its live-in registers from the context:
                    # a dirty one is stored at the site (read here), a clean one
                    # must already be right in the context, which makes it
                    # live-in of this function too (pass-through arguments).
                    # Registers reloaded after the call are read here only
                    # when dirty (sync_at_call stores them).
                    live |= ci.live_in | (live_after & ci.may_write & self.dirty_before_call(s))
                    if ci.kind == "indirect" and b.call_reg is not None:
                        live |= bit(b.call_reg)
                for (u, d) in reversed(self.ud[s]):
                    live = (live & ~d) | u
                if lin.get(s) != live:
                    lin[s] = live
                    changed = True
        self.live_in, self.live_out = lin, lout

    def dirty_before_call(self, s):
        d = self.dirty_in[s]
        for (u, dd) in self.ud[s]:
            d |= dd
        return d

    # ------------------------------------------------------------------ run
    def run(self):
        self.fpscr_pass()
        self.usedef_pass()
        self.preserved_pass()
        self.dirty_pass()
        self.live_pass()
        # summaries for callers
        self.summary_live_in = self.live_in.get(self.cfg.entry, 0)
        mw = 0
        for s in self.cfg.blocks:
            for (u, d) in self.ud[s]:
                mw |= d
            if s in self.calls:
                mw |= self.calls[s].may_write
        # PR is saved/restored by every prologue but never consulted (rts is a C
        # return), so callers neither store nor reload it, and it is not loaded
        # at entry: emit.py initialises it to 0 when the function mentions it.
        self.summary_may_write = mw & ~self.preserved & ~bit(regs.PR)
        self.summary_live_in &= ~self.preserved & ~bit(regs.PR)

    # ------------------------------------------------------ emitter queries
    def sync_at_call(self, s):
        """(store_set, reload_set) for the call ending block s.

        Stored: dirty registers the callee may read, plus dirty registers we
        reload afterwards.  may_write is a may-set, so a reloaded register the
        callee happened not to write must already hold our value in the
        context; without that a callee-saved register held across a call to
        a function containing an unknown indirect call came back stale
        (tests/emitter: callee_saved_across_indirect)."""
        ci = self.calls[s]
        reload = self.live_out[s] & ci.may_write & ~bit(regs.PR)
        store = self.dirty_before_call(s) & (ci.live_in | reload) & ~bit(regs.PR)
        return store, reload

    def store_at_exit(self, s):
        return self.dirty_out[s] & ~bit(regs.PR)

    def entry_loads(self):
        return self.live_in.get(self.cfg.entry, 0) & self.used & ~bit(regs.PR)
