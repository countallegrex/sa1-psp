"""Program-level driver: builds every function's CFG, iterates the call
summaries to a fixpoint, and writes the C output.

Outputs (all under the chosen out dir; <unit> is "game" for the main image):
  <unit>_NNN.c    recompiled functions, chunked for parallel compilation
  <unit>.h        prototypes of every emitted function
  <unit>_table.c  sorted (address, function) table for indirect calls; for an
                  overlay also the sa1_overlay record the loader hook installs
  seam.h          prototypes of the HLE entry points (also copied to hle/)
  seam_stubs.c    logging stubs for them (main image only)
  report.txt      counts and notes
"""

import os
import time

from . import regs
from .regs import bit, iter_bits
from .image import phys
from .cfg import Env, FunctionCFG, const_step
from .analysis import FunctionAnalysis, Summaries, IDENTITY, U
from .lower import Lowerer

STORE_OPS = {
    "MOV_B_S": (1, "n", 0), "MOV_W_S": (2, "n", 0), "MOV_L_S": (4, "n", 0),
    "MOV_B_S_D": (1, "n", 1), "MOV_W_S_D": (2, "n", 2), "MOV_L_S_D": (4, "n", 4),
    "MOVCA_L": (4, "n", 0),
}


class Program:
    def __init__(self, image, code_lo, code_hi, functions, seam, sdk_range=None,
                 name_prefix="fn_", log=print, extern=None, wrap=None, keep=None):
        """functions: list of (entry, end, origin).  seam: {addr: (name, sites)}.
        extern: {addr: C name} of functions emitted in another unit (an
        overlay's calls into the main image); called directly, full sync.
        wrap: addresses of functions the HLE layer wraps.  The recompiled
        body is defined as <name>_orig; every call and the function table
        still use <name>, which hle/ defines (and which calls <name>_orig)."""
        self.image = image
        self.code_lo, self.code_hi = phys(code_lo), phys(code_hi)
        self.functions_in = [(phys(e), phys(end) if end else None, origin) for e, end, origin in functions]
        self.seam = {phys(a): v for a, v in seam.items()}
        self.sdk_range = tuple(phys(x) for x in sdk_range) if sdk_range else None
        self.prefix = name_prefix
        self.extern = {phys(a): n for a, n in (extern or {}).items()}
        self.wrap = {phys(a) for a in (wrap or ())}
        # Entries that stay C functions even when only a branch reaches them:
        # the Flycast trace's executed entries (recomp/sa1_us_1005/
        # trace_call_targets.txt: jsr/jmp targets), which are real call targets whatever the
        # discovery origin says.
        self.keep = {phys(a) for a in (keep or ())}
        self.log = log
        self.summaries = Summaries()
        self.cfgs = {}
        self.analyses = {}
        self.notes = []
        self.stats = {}

    # ---- names ------------------------------------------------------------
    def fn_name(self, addr):
        return "%s%08x" % (self.prefix, phys(addr) | 0x80000000)

    def def_name(self, addr):
        """Name of the C definition: fn_name, plus "_orig" when wrapped."""
        return self.fn_name(addr) + ("_orig" if phys(addr) in self.wrap else "")

    def seam_name(self, addr):
        return "sdk_%08x" % (phys(addr) | 0x80000000)

    def in_sdk(self, addr):
        return self.sdk_range is not None and self.sdk_range[0] <= phys(addr) < self.sdk_range[1]

    def pointer_words(self, candidates):
        """Candidates whose P1 address (0x8C...) is an aligned 32-bit word
        anywhere in the image: a function pointer in a data table, or in a
        literal pool that code stores into an object (an instruction pair
        that happens to spell one only costs an extra C function)."""
        import struct
        found = set()
        if not candidates:
            return found
        want = {(e | 0x80000000) & 0xFFFFFFFF for e in candidates}
        data = self.image.data
        for off in range(0, len(data) - 3, 4):
            w = struct.unpack_from("<I", data, off)[0]
            if w in want:
                found.add(phys(w))
        return found

    # ---- passes -----------------------------------------------------------
    def build(self):
        t0 = time.time()
        entries_all = [e for e, _, _ in self.functions_in]
        ends = {e: end for e, end, _ in self.functions_in}
        origin = {e: o for e, _, o in self.functions_in}

        # pass 1: everything is an entry; collect call targets and patched words
        env = Env(self.image, self.code_lo, self.code_hi, entries_all)
        called = set()
        cfgs = {}
        for e in entries_all:
            cfgs[e] = FunctionCFG(env, e, ends.get(e))
            for b in cfgs[e].blocks.values():
                if b.kind == "call" and b.target is not None:
                    called.add(b.target)
        patched = self.find_patched(cfgs.values(), env)
        self.stats["patched_words"] = len(patched)

        # pass 2: branch-origin entries nothing calls are local code of whoever
        # branches to them; everything else is a C function.  Kept as
        # functions anyway: entries the trace saw executed, and entries whose
        # address appears as a 32-bit word in the image (object function
        # tables in data reach them through sa1_call; M1 boot bug: the
        # Station Square object table pointed at merged entry 0x8C060460).
        branch_entries = {e for e in entries_all if origin.get(e) == "branch" and e not in called}
        referenced = self.pointer_words(branch_entries)
        self.stats["entries_kept_trace"] = len(branch_entries & self.keep)
        self.stats["entries_kept_pointer"] = len(referenced - self.keep)
        local_only = {e for e in branch_entries if e not in self.keep and e not in referenced}
        entries = [e for e in entries_all if e not in local_only]
        self.stats["entries_merged"] = len(local_only)
        env = Env(self.image, self.code_lo, self.code_hi, entries, patched)
        self.env = env
        work = list(entries)
        seen = set()
        while work:
            e = work.pop()
            if e in seen:
                continue
            seen.add(e)
            cfg = FunctionCFG(env, e, ends.get(e))
            self.cfgs[e] = cfg
            for x in cfg.extra_entries:
                if x not in seen and env.in_code(x):
                    env.entries.add(x)
                    work.append(x)
        # pass 3: merged entries that no emitted function reached may still be
        # pointer-table callbacks; give them a C function of their own.
        covered = set()
        for cfg in self.cfgs.values():
            covered.update(cfg.blocks)
        orphans = sorted(e for e in local_only if e not in covered)
        self.stats["entries_orphan"] = len(orphans)
        for e in orphans:
            env.entries.add(e)
        for e in orphans:
            if e not in seen:
                seen.add(e)
                self.cfgs[e] = FunctionCFG(env, e, ends.get(e))
        self.stats["entries_added"] = len(seen) - len(entries) - len(orphans)
        self.order = sorted(self.cfgs)
        self.log("cfg: %d functions in %.1fs" % (len(self.cfgs), time.time() - t0))

        # summaries to a fixpoint
        t0 = time.time()
        for rnd in range(8):
            changed = 0
            for e in self.order:
                fa = FunctionAnalysis(self.cfgs[e], self)
                self.analyses[e] = fa
                S = self.summaries
                if (S.live_in.get(e) != fa.summary_live_in or S.may_write.get(e) != fa.summary_may_write
                        or S.fpscr_ret.get(e, IDENTITY) != fa.fpscr_ret or S.preserved.get(e) != fa.preserved):
                    S.live_in[e] = fa.summary_live_in
                    S.may_write[e] = fa.summary_may_write
                    S.fpscr_ret[e] = fa.fpscr_ret
                    S.preserved[e] = fa.preserved
                    changed += 1
            self.log("analysis round %d: %d summaries changed (%.1fs)" % (rnd + 1, changed, time.time() - t0))
            if not changed:
                break

    def find_patched(self, cfgs, env):
        """Image words some store writes through a constant address."""
        patched = set()
        for cfg in cfgs:
            for b in cfg.blocks.values():
                consts = dict(b.consts_in)
                for ins in b.all_instrs():
                    spec = STORE_OPS.get(ins.op)
                    if spec is not None:
                        size, _, scale = spec
                        base = consts.get(ins.n)
                        if base is not None:
                            addr = phys((base + ins.d * scale) & 0xFFFFFFFF)
                            if self.image.contains(addr, size):
                                patched.add(addr & ~3)
                    elif ins.op in ("MOV_B_S_R0", "MOV_W_S_R0", "MOV_L_S_R0"):
                        a, b0 = consts.get(0), consts.get(ins.n)
                        if a is not None and b0 is not None:
                            addr = phys((a + b0) & 0xFFFFFFFF)
                            if self.image.contains(addr, 4):
                                patched.add(addr & ~3)
                    consts = const_step(consts, ins, env)
        return patched

    # ---- emission ---------------------------------------------------------
    def emit_function(self, e):
        cfg, fa = self.cfgs[e], self.analyses[e]
        fe = FunctionEmitter(self, cfg, fa)
        return fe.emit()

    def write_all(self, out_dir, chunks=32, include_dir=None, unit="game", overlay=None):
        """overlay: None for the main image, else {"name", "table", "record",
        "code_lo", "code_hi"}: the table and the sa1_overlay record get those
        symbol names, and no seam files are written."""
        os.makedirs(out_dir, exist_ok=True)
        t0 = time.time()
        counts = {"instructions": 0, "blocks": 0, "calls_direct": 0, "calls_seam": 0,
                  "calls_indirect": 0, "tails": 0, "tails_seam": 0, "tails_sdk_dead": 0,
                  "ijumps": 0, "switches": 0, "switch_entries": 0, "switch_bounded": 0,
                  "rt_pr": 0, "rt_sz": 0, "fr_unknown": 0, "nonreset_calls": 0,
                  "fpscr_ret_nonidentity": 0, "fpscr_ret_unknown": 0, "stores_at_calls": 0,
                  "reloads_at_calls": 0, "entry_loads": 0, "exit_stores": 0, "cond_tails": 0,
                  "stops": 0, "folded_pools": 0, "unfolded_pools": 0}
        bodies = []
        total_bytes = 0
        for e in self.order:
            text, c = self.emit_function(e)
            bodies.append((e, text))
            total_bytes += len(text)
            for k, v in c.items():
                counts[k] = counts.get(k, 0) + v
        # chunk by size
        per = max(1, total_bytes // chunks + 1)
        files = []
        cur, cur_size, idx = [], 0, 0
        for e, text in bodies:
            cur.append(text)
            cur_size += len(text)
            if cur_size >= per:
                files.append(cur)
                cur, cur_size = [], 0
        if cur:
            files.append(cur)
        inc = '#include "sh4_rt.h"\n#include "%s.h"\n#include "seam.h"\n' % unit
        if overlay and self.extern:
            inc += '#include "game.h"\n'
        src_bytes = 0
        for i, texts in enumerate(files):
            path = os.path.join(out_dir, "%s_%03d.c" % (unit, i))
            with open(path, "w", newline="\n") as f:
                f.write("/* generated by recomp/emitter (sa1-psp); do not edit, do not commit */\n")
                f.write(inc)
                f.write("\n".join(texts))
            src_bytes += os.path.getsize(path)
        with open(os.path.join(out_dir, "%s.h" % unit), "w", newline="\n") as f:
            f.write("/* generated: prototypes of recompiled functions */\n#pragma once\n#include \"sh4_rt.h\"\n")
            if overlay:
                f.write("extern const sa1_overlay %s;\n" % overlay["record"])
            for e in self.order:
                f.write("void %s(sh4_ctx *c);\n" % self.fn_name(e))
                if e in self.wrap:
                    f.write("void %s(sh4_ctx *c);  /* recompiled body; the wrapper is in hle/ */\n"
                            % self.def_name(e))
        table = overlay["table"] if overlay else "sa1_fn_table"
        with open(os.path.join(out_dir, "%s_table.c" % unit), "w", newline="\n") as f:
            f.write("/* generated: function table for indirect calls */\n#include \"sh4_rt.h\"\n#include \"%s.h\"\n" % unit)
            f.write("%sconst sa1_fn_entry %s[] = {\n" % ("static " if overlay else "", table))
            for e in self.order:
                f.write("    {0x%08xu, %s},\n" % (e, self.fn_name(e)))
            if overlay:
                f.write("};\nconst sa1_overlay %s = {\"%s\", 0x%08xu, 0x%08xu, %s, %d};\n" % (
                    overlay["record"], overlay["name"], phys(overlay["code_lo"]), phys(overlay["code_hi"]),
                    table, len(self.order)))
            else:
                f.write("};\nconst uint32_t sa1_fn_count = %d;\n" % len(self.order))
        if not overlay:
            self.write_seam(out_dir)
        counts["functions"] = len(self.order)
        counts["wrapped"] = len(self.wrap & set(self.order))
        counts["source_bytes"] = src_bytes
        counts["chunks"] = len(files)
        counts.update(self.stats)
        counts["emit_seconds"] = round(time.time() - t0, 1)
        self.counts = counts
        with open(os.path.join(out_dir, "report.txt"), "w", newline="\n") as f:
            for k in sorted(counts):
                f.write("%s %s\n" % (k, counts[k]))
            for e in self.order:
                for pc, msg in self.cfgs[e].notes + self.analyses[e].notes:
                    f.write("note: %08x: %s\n" % (pc, msg))
        return counts

    def write_seam(self, out_dir):
        items = sorted(self.seam.items(), key=lambda kv: -kv[1][1])
        with open(os.path.join(out_dir, "seam.h"), "w", newline="\n") as f:
            f.write("/* generated by recomp/emitter from recomp/sa1_us_1005/sdk_entry_points.tsv.\n"
                    " * One prototype per Katana SDK entry point the game calls.  The HLE layer\n"
                    " * implements these; until then seam_stubs.c logs and returns 0.\n"
                    " * Arguments arrive in c->r[4..7] / c->fr[4..11] and on the guest stack\n"
                    " * (c->r[15]); results go to c->r[0] / c->fr[0] (SH ABI). */\n")
            f.write("#pragma once\n#include \"sh4_rt.h\"\n\n")
            for addr, (name, sites) in items:
                f.write("void %s(sh4_ctx *c);  /* %-40s %5d sites */\n" % (self.seam_name(addr), name or "-", sites))
        with open(os.path.join(out_dir, "seam_stubs.c"), "w", newline="\n") as f:
            f.write("/* generated: logging stubs for every seam entry point.  Replace one by\n"
                    " * one with real HLE in hle/: the stubs are weak symbols, so a strong\n"
                    " * definition anywhere in the link replaces the stub. */\n")
            f.write("#include \"seam.h\"\n\nvoid sa1_seam_stub(sh4_ctx *c, uint32_t addr, const char *name);\n\n"
                    "/* Weak, so a real definition in hle/ wins at link time without editing\n"
                    " * this file (clang/COFF, gcc/ELF and psp-gcc all honour it). */\n"
                    "#define SA1_WEAK __attribute__((weak))\n\n")
            for addr, (name, sites) in items:
                f.write("SA1_WEAK void %s(sh4_ctx *c) { sa1_seam_stub(c, 0x%08xu, \"%s\"); c->r[0] = 0; }\n" % (
                    self.seam_name(addr), addr | 0x80000000, name or ""))
            f.write("\nconst sa1_fn_entry sa1_seam_table[] = {\n")
            for addr in sorted(self.seam):
                f.write("    {0x%08xu, %s},\n" % (addr, self.seam_name(addr)))
            f.write("};\nconst uint32_t sa1_seam_count = %d;\n" % len(self.seam))


class FunctionEmitter:
    def __init__(self, prog, cfg, fa):
        self.prog, self.cfg, self.fa = prog, cfg, fa
        self.env = cfg.env
        self.lw = Lowerer(cfg.env, None, fa.notes)
        self.counts = {}

    def count(self, k, v=1):
        self.counts[k] = self.counts.get(k, 0) + v

    def label(self, addr):
        return "L_%08x" % addr

    def store(self, mask):
        return ["%s = %s;" % (regs.ctx_field(r), regs.name(r)) for r in iter_bits(mask & self.fa.used)]

    def load(self, mask):
        return ["%s = %s;" % (regs.name(r), regs.ctx_field(r)) for r in iter_bits(mask & self.fa.used)]

    def emit(self):
        cfg, fa, prog = self.cfg, self.fa, self.prog
        L = []
        name = prog.def_name(cfg.entry)
        L.append("")
        L.append("void %s(sh4_ctx *c) {" % name)
        ints = [regs.name(r) for r in iter_bits(fa.used) if not regs.is_float(r)]
        flts = [regs.name(r) for r in iter_bits(fa.used) if regs.is_float(r)]
        if ints:
            L.append("    uint32_t " + ", ".join(ints) + ";")
        if flts:
            L.append("    float " + ", ".join(flts) + ";")
        loads = self.load(fa.entry_loads())
        self.count("entry_loads", len(loads))
        L.extend("    " + x for x in loads)
        if fa.used & bit(regs.PR):
            L.append("    pr = 0;")
        order = cfg.order
        if order[0] != cfg.entry:
            L.append("    goto %s;" % self.label(cfg.entry))
        for i, s in enumerate(order):
            b = cfg.blocks[s]
            nxt = order[i + 1] if i + 1 < len(order) else None
            L.append("%s:" % self.label(s))
            L.extend("    " + x for x in self.emit_block(b, nxt))
        L.append("}")
        self.count("instructions", cfg.instruction_count())
        self.count("blocks", len(cfg.blocks))
        self.count("rt_pr", self.lw.rt_pr)
        self.count("rt_sz", self.lw.rt_sz)
        self.count("fr_unknown", fa.fr_unknown)
        self.count("nonreset_calls", fa.nonreset_calls)
        if fa.fpscr_ret != IDENTITY:
            self.count("fpscr_ret_nonidentity")
            if U in fa.fpscr_ret:
                self.count("fpscr_ret_unknown")
        return "\n".join(L), self.counts

    def lower_seq(self, b, instrs_states, consts):
        """Lower straight-line instructions, threading constants."""
        out = []
        for ins, st in instrs_states:
            self.lw.set_state(st)
            if ins.op in ("MOV_W_PC", "MOV_L_PC"):
                if self.env.pool_word(ins) is not None:
                    self.count("folded_pools")
                else:
                    self.count("unfolded_pools")
            stmts = self.lw.lower(ins, consts, None)
            out.append("/* %08x %s */ %s" % (ins.pc, ins.text(), " ".join(stmts)) if stmts
                       else "/* %08x %s */" % (ins.pc, ins.text()))
            consts = const_step(consts, ins, self.env)
        return out, consts

    def emit_block(self, b, nxt):
        fa, cfg = self.fa, self.cfg
        s = b.start
        states = fa.states[s]
        n_body = len(b.instrs)
        out, consts = self.lower_seq(b, zip(b.instrs, states[:n_body]), dict(b.consts_in))
        slot_state = states[n_body] if b.slot is not None else None
        kind = b.kind

        def slot_code(extra_consts=None):
            if b.slot is None:
                return []
            code, _ = self.lower_seq(b, [(b.slot, slot_state)], consts)
            return code

        def slot_defs(reg):
            if b.slot is None:
                return False
            idx = n_body
            return bool(fa.ud[s][idx][1] & bit(reg))

        def go(target):
            return [] if target == nxt else ["goto %s;" % self.label(target)]

        def callee_code(ci, target_expr=None):
            """Statement that performs the call."""
            if ci.kind == "direct":
                return "%s(c);" % ci.name
            if ci.kind == "seam":
                return "%s(c);" % ci.name
            if ci.kind == "sdk_dead":
                return 'sa1_trap(c, 0x%08xu, "jump into replaced SDK code 0x%08x");' % (b.branch.pc, ci.target)
            return "sa1_call(c, %s);" % (target_expr if target_expr else "0x%08xu" % ci.target)

        def tail_code(ci, target_expr=None):
            """Statements that perform a tail call (sh4_rt.h SA1_TAIL: a guaranteed
            tail call, so jump loops between emitted functions use no host stack).
            Ends the block: no statement may follow."""
            if ci.kind in ("direct", "seam"):
                return ["SA1_TAIL(%s(c));" % ci.name]
            if ci.kind == "sdk_dead":
                return [callee_code(ci), "return;"]
            tgt = target_expr if target_expr else "0x%08xu" % ci.target
            return ["{ sa1_fn f_ = sa1_tail_target(c, %s); if (f_) SA1_TAIL(f_(c)); }" % tgt, "return;"]

        if kind == "fall":
            out += go(b.succs[0])
        elif kind == "jump":
            out += slot_code()
            out += go(b.target)
        elif kind == "cond":
            cond = "t"
            if slot_defs(regs.T):
                out.append("{ uint32_t cnd_ = t;")
                cond = "cnd_"
            if b.branch.op in ("BF", "BF_S"):
                cond = "!" + cond
            out += slot_code()
            if b.target in b.succs:
                out.append("if (%s) goto %s;" % (cond, self.label(b.target)))
            else:
                # conditional branch out of the function: a tail call
                self.count("cond_tails")
                out.append("if (%s) {" % cond)
                out += ["    " + x for x in self.store(fa.dirty_out[s] & ~bit(regs.PR))]
                out += ["    " + x for x in self.tail_call_stmts(b.target, b.branch.pc)]
                out.append("}")
            if cond.endswith("cnd_"):
                out.append("}")
            fall = b.succs[0]
            out += go(fall)
        elif kind == "call":
            ci = fa.calls[s]
            tgt_expr = None
            if ci.kind == "indirect" and b.call_reg is not None:
                reg = "r%d" % b.call_reg
                if b.branch.op == "BSRF":
                    expr = "0x%08xu + %s" % ((b.branch.pc + 4) & 0xFFFFFFFF, reg)
                else:
                    expr = reg
                out.append("{ uint32_t tgt_ = %s;" % expr)
                tgt_expr = "tgt_"
            out += slot_code()
            store, reload = fa.sync_at_call(s)
            st = self.store(store)
            rl = self.load(reload)
            self.count("stores_at_calls", len(st))
            self.count("reloads_at_calls", len(rl))
            self.count({"direct": "calls_direct", "seam": "calls_seam", "indirect": "calls_indirect",
                        "sdk_dead": "calls_indirect"}[ci.kind])
            out += st
            out.append(callee_code(ci, tgt_expr))
            if fa.used & bit(regs.PR):
                out.append("pr = 0x%08xu;" % (b.end | 0x80000000))
            out += rl
            if tgt_expr:
                out.append("}")
            out += go(b.succs[0])
        elif kind == "ret":
            out += slot_code()
            st = self.store(fa.store_at_exit(s))
            self.count("exit_stores", len(st))
            out += st
            out.append("return;")
        elif kind == "tail":
            out += slot_code()
            ci = fa.calls[s]
            out += self.store(fa.dirty_out[s] & ~bit(regs.PR))
            self.count({"direct": "tails", "seam": "tails_seam", "indirect": "tails",
                        "sdk_dead": "tails_sdk_dead"}[ci.kind])
            out += tail_code(ci)
        elif kind == "ijump":
            reg = "r%d" % b.call_reg
            expr = reg if b.branch.op == "JMP" else "0x%08xu + %s" % ((b.branch.pc + 4) & 0xFFFFFFFF, reg)
            out.append("{ uint32_t tgt_ = %s;" % expr)
            out += slot_code()
            out += self.store(fa.dirty_out[s] & ~bit(regs.PR))
            self.count("ijumps")
            out.append("{ sa1_fn f_ = sa1_tail_target(c, tgt_); if (f_) SA1_TAIL(f_(c)); }")
            out.append("return; }")
        elif kind == "switch":
            reg = "r%d" % b.branch.n
            tbl = b.table
            self.count("switches")
            self.count("switch_entries", len(tbl.targets))
            if tbl.bounded:
                self.count("switch_bounded")
            key = reg
            if slot_defs(b.branch.n):
                out.append("{ uint32_t sw_ = %s;" % reg)
                key = "sw_"
            out += slot_code()
            out.append("switch (%s) {" % key)
            seen = set()
            img = self.env.image
            for k, t in enumerate(tbl.targets):
                addr = tbl.table + k * tbl.entry_size
                if tbl.entry_size == 1:
                    val = img.s8(addr) & 0xFFFFFFFF
                elif tbl.entry_size == 2:
                    val = img.s16(addr) & 0xFFFFFFFF
                else:
                    val = img.u32(addr)
                if val in seen:
                    continue
                seen.add(val)
                if t in self.cfg.blocks:
                    out.append("    case 0x%08xu: goto %s;" % (val, self.label(t)))
                else:
                    out.append("    case 0x%08xu: %s %s" % (
                        val, " ".join(self.store(fa.dirty_out[s] & ~bit(regs.PR))), " ".join(self.tail_call_stmts(t, b.branch.pc))))
            out.append('    default: sa1_trap(c, 0x%08xu, "switch value outside the recovered table"); return;' % b.branch.pc)
            out.append("}")
            if key == "sw_":
                out.append("}")
        elif kind == "stop":
            self.count("stops")
            out += self.store(fa.dirty_out[s] & ~bit(regs.PR))
            out.append('sa1_trap(c, 0x%08xu, "%s"); return;' % (b.end, b.note or "end of decodable code"))
        else:
            raise ValueError(kind)
        return out

    def tail_call_stmts(self, target, pc):
        """A tail call out of a conditional branch (sh4_rt.h SA1_TAIL)."""
        prog = self.prog
        if prog.env.in_code(target):
            return ["SA1_TAIL(%s(c));" % prog.fn_name(target)]
        if target in prog.extern:
            return ["SA1_TAIL(%s(c));" % prog.extern[target]]
        if target in prog.seam:
            return ["SA1_TAIL(%s(c));" % prog.seam_name(target)]
        if prog.in_sdk(target):
            return ['sa1_trap(c, 0x%08xu, "jump into replaced SDK code 0x%08x");' % (pc, target), "return;"]
        return ["{ sa1_fn f_ = sa1_tail_target(c, 0x%08xu); if (f_) SA1_TAIL(f_(c)); }" % target, "return;"]
