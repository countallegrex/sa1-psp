#!/usr/bin/env python3
"""Read a Windows minidump of sa1psp: modules, the exception record and
registers, and the faulting thread's stack as exe offsets to symbolise.

  python -I tools/minidump.py DUMP [--stack-words N]

Prints the sa1psp.exe module (base, size, link timestamp), the exception
(code, address, parameters; for an access violation whether it was a read
or a write and where), the registers of the faulting thread, and every
word of its captured stack that points into the executable (the return
addresses, in order from the top of the stack, then a histogram: a deep
recursion shows as a few offsets repeated thousands of times).  Symbolise
an offset against a build of the same commit and options with

  llvm-symbolizer --obj=build-desktop/backend/desktop/sa1psp.exe 0x140000000+OFFSET

(the image base is in the module line).  The dump itself holds guest RAM
and is game state: keep it outside the repository.

Part of sa1-psp (MIT).
"""
import collections
import struct
import sys


def read_dump(path):
    d = open(path, "rb").read()
    sig, ver, nstreams, dir_rva = struct.unpack_from("<4sIII", d, 0)
    if sig != b"MDMP":
        sys.exit("%s: not a minidump" % path)
    streams = {}
    for i in range(nstreams):
        t, size, rva = struct.unpack_from("<III", d, dir_rva + 12 * i)
        streams.setdefault(t, []).append((size, rva))
    return d, streams


def modules(d, streams):
    out = []
    if 4 not in streams:
        return out
    size, rva = streams[4][0]
    n = struct.unpack_from("<I", d, rva)[0]
    for i in range(n):
        base, msize, cksum, tstamp, name_rva = struct.unpack_from("<QIIII", d, rva + 4 + i * 108)
        nlen = struct.unpack_from("<I", d, name_rva)[0]
        name = d[name_rva + 4:name_rva + 4 + nlen].decode("utf-16le", "replace")
        out.append((base, msize, tstamp, name))
    return out


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        print(__doc__)
        return 2
    nwords = int(argv[argv.index("--stack-words") + 1]) if "--stack-words" in argv else 200000
    d, streams = read_dump(argv[0])
    exe = None
    for base, msize, tstamp, name in modules(d, streams):
        short = name.split("\\")[-1]
        if "sa1psp" in short.lower():
            exe = (base, msize)
            print("module %s base 0x%x size 0x%x link timestamp 0x%x" % (short, base, msize, tstamp))
        elif short.lower() in ("ntdll.dll", "kernel32.dll", "kernelbase.dll", "sdl2.dll"):
            print("module %s base 0x%x size 0x%x" % (short, base, msize))
    tid = None
    if 6 in streams:
        size, rva = streams[6][0]
        tid, _, code, flags, record, addr, nparams = struct.unpack_from("<IIIIQQI", d, rva)
        params = struct.unpack_from("<15Q", d, rva + 40)
        print("exception: thread %d code 0x%08x at 0x%x" % (tid, code, addr) +
              (" (exe+0x%x)" % (addr - exe[0]) if exe and exe[0] <= addr < exe[0] + exe[1] else ""))
        if code == 0xC0000005 and nparams >= 2:
            print("  access violation: %s of 0x%x" % ("write" if params[0] == 1 else "read" if params[0] == 0 else "execute", params[1]))
        csize, crva = struct.unpack_from("<II", d, rva + 160)
        ctx = d[crva:crva + csize]
        names = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"]
        if len(ctx) >= 0x78 + 8 * len(names):
            regs = {nm: struct.unpack_from("<Q", ctx, 0x78 + 8 * i)[0] for i, nm in enumerate(names)}
            print("  " + " ".join("%s=%x" % (k, v) for k, v in regs.items()))
    if 3 in streams and exe:
        size, rva = streams[3][0]
        n = struct.unpack_from("<I", d, rva)[0]
        for i in range(n):
            o = rva + 4 + i * 48
            t, susp, pc, prio, teb, sstart, ssize, srva, csize, crva = struct.unpack_from("<IIIIQQIIII", d, o)
            if t != tid:
                continue
            mem = d[srva:srva + ssize]
            print("thread %d stack 0x%x size 0x%x (captured %d bytes)" % (t, sstart, ssize, len(mem)))
            hist = collections.Counter()
            seq = []
            for k in range(0, min(len(mem), nwords * 8) - 7, 8):
                v = struct.unpack_from("<Q", mem, k)[0]
                if exe[0] <= v < exe[0] + exe[1]:
                    off = v - exe[0]
                    hist[off] += 1
                    if len(seq) < 48:
                        seq.append((sstart + k, off))
            print("  return-address candidates from the top of the stack (stack address, exe offset):")
            for a, off in seq:
                print("    0x%x  exe+0x%x" % (a, off))
            print("  most frequent exe offsets on the captured stack:")
            for off, c in hist.most_common(16):
                print("    exe+0x%x  x%d" % (off, c))
    return 0


if __name__ == "__main__":
    sys.exit(main())
