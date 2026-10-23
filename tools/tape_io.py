#!/usr/bin/env python3
"""Read and write sa1psp input tapes (format 1, tests/replay/README.md).

  tape_io.py info FILE           header, length, number of records
  tape_io.py normalize IN OUT    re-encode (run-length, canonical spacing)

A tape is a header (key value lines) and records
  FRAME BUTTONS(hex) STICK_X STICK_Y LTRIG RTRIG [COUNT]
each covering COUNT frames from FRAME; uncovered frames are a neutral pad.
"slow-substep F" is the one header key that may repeat (Tape.slow).

Part of sa1-psp (MIT).
"""
import sys

FORMAT = 1
NEUTRAL = (0, 0, 0, 0, 0)


class Tape:
    def __init__(self):
        self.header = {}           # key -> value (strings), in file order
        self.pads = {}             # frame -> (buttons, x, y, lt, rt); neutral frames may be absent
        self.end = 0               # last frame defined
        self.slow = []             # "slow-substep" frames (tools/tape_diff.py slow)

    def pad(self, frame):
        return self.pads.get(frame, NEUTRAL)


def read(path):
    t = Tape()
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            p = line.split("#", 1)[0].split()
            if not p:
                continue
            if p[0].isdigit():
                if len(p) < 6:
                    raise ValueError("%s:%d: bad record" % (path, n))
                frame, b = int(p[0]), int(p[1], 16)
                x, y, lt, rt = (int(v) for v in p[2:6])
                count = int(p[6]) if len(p) > 6 else 1
                if not (-128 <= x <= 127 and -128 <= y <= 127 and 0 <= lt <= 255 and 0 <= rt <= 255 and count > 0):
                    raise ValueError("%s:%d: value out of range" % (path, n))
                for i in range(count):
                    t.pads[frame + i] = (b, x, y, lt, rt)
                t.end = max(t.end, frame + count - 1)
            elif p[0] == "slow-substep":
                t.slow.append(int(p[1], 0))
            elif p[0] == "end":
                t.end = max(t.end, int(p[1], 0))
            else:
                t.header[p[0]] = " ".join(p[1:])
    if int(t.header.get("sa1psp-tape", "0")) != FORMAT:
        raise ValueError("%s: not a format-%d tape" % (path, FORMAT))
    return t


def encode(t):
    keys = ["sa1psp-tape", "title-sha256", "epoch", "tool"]
    lines = ["%s %s" % (k, t.header[k]) for k in keys if k in t.header]
    lines += ["%s %s" % (k, v) for k, v in t.header.items() if k not in keys]
    lines += ["slow-substep %d" % f for f in sorted(set(t.slow))]
    lines.append("# FRAME BUTTONS STICK_X STICK_Y LTRIG RTRIG [COUNT]")
    frame = 1
    while frame <= t.end:
        pad, start = t.pad(frame), frame
        while frame + 1 <= t.end and t.pad(frame + 1) == pad:
            frame += 1
        count = frame - start + 1
        lines.append("%d %x %d %d %d %d" % ((start,) + pad) + (" %d" % count if count > 1 else ""))
        frame += 1
    lines.append("end %d" % t.end)
    return "\n".join(lines) + "\n"


def write(path, t):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(encode(t))


def main(argv):
    if len(argv) >= 2 and argv[0] == "info":
        t = read(argv[1])
        for k, v in t.header.items():
            print("%-14s %s" % (k, v))
        for f in t.slow:
            print("%-14s %d" % ("slow-substep", f))
        print("%-14s %d (%.1f s at 60 Hz)" % ("frames", t.end, t.end / 60))
        return 0
    if len(argv) >= 3 and argv[0] == "normalize":
        write(argv[2], read(argv[1]))
        return 0
    print(__doc__.split("\n\n")[1])
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
