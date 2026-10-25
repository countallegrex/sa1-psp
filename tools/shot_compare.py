#!/usr/bin/env python3
"""Compare console screenshots (binary PPM, P6) against a reference.

    python tools/shot_compare.py REF.ppm NEW.ppm [REF2.ppm NEW2.ppm ...]
    python tools/shot_compare.py --flicker REF_N.ppm REF_N1.ppm NEW_N.ppm NEW_N1.ppm

Per pair it prints the share of pixels that differ at all (any channel), the
pixels differing by more than --level (default 32) in some channel, and the
largest 8-connected region of those, with its bounding box.  The picture
gate (orders v32): a pair FAILS when that region has --region (default 64)
pixels or more: a wrong, missing or mis-sorted triangle.  Speckle, dither
(1-2 steps of the 16-bit frame buffer) and sub-texel shifts pass.

--flicker takes two consecutive frames of each build and applies the same
region rule to the difference of their frame-to-frame changes: a region that
changes between the frames in one build and not in the other flickers.

Exit status: 0 every pair passes, 1 one fails, 2 bad input.

Part of sa1-psp (MIT).
"""
import argparse
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    if fields[0] != b"P6" or int(fields[3]) != 255:
        raise ValueError(f"{path}: not an 8-bit P6 PPM")
    w, h = int(fields[1]), int(fields[2])
    px = data[pos:pos + 3 * w * h]
    if len(px) != 3 * w * h:
        raise ValueError(f"{path}: truncated")
    return w, h, px


def channel_diff(a, b):
    """Per pixel, the largest channel difference of two equal-size images."""
    (w, h, pa), (w2, h2, pb) = a, b
    if (w, h) != (w2, h2):
        raise ValueError(f"sizes differ: {w}x{h} and {w2}x{h2}")
    d = bytearray(w * h)
    for i in range(w * h):
        j = 3 * i
        d[i] = max(abs(pa[j] - pb[j]), abs(pa[j + 1] - pb[j + 1]), abs(pa[j + 2] - pb[j + 2]))
    return w, h, d


def change_diff(a0, a1, b0, b1):
    """Per pixel, the largest channel difference between build a's frame-to-
    frame change and build b's (signed, so a change in one build only counts)."""
    imgs = (a0, a1, b0, b1)
    w, h = a0[0], a0[1]
    if any((im[0], im[1]) != (w, h) for im in imgs):
        raise ValueError("sizes differ")
    p0, p1, q0, q1 = (im[2] for im in imgs)
    d = bytearray(w * h)
    for i in range(w * h):
        m = 0
        for c in range(3):
            j = 3 * i + c
            v = abs((p1[j] - p0[j]) - (q1[j] - q0[j]))
            m = v if v > m else m
        d[i] = m if m < 255 else 255
    return w, h, d


def regions(w, h, d, level):
    """The 8-connected regions of pixels differing by more than level:
    (size, x0, y0, x1, y1) of the largest, and how many pixels in all."""
    seen = bytearray(w * h)
    best, total = (0, 0, 0, 0, 0), 0
    for start in range(w * h):
        if d[start] <= level or seen[start]:
            continue
        seen[start] = 1
        stack, n = [start], 0
        x0 = y0 = 1 << 30
        x1 = y1 = -1
        while stack:
            i = stack.pop()
            n += 1
            y, x = divmod(i, w)
            x0, x1, y0, y1 = min(x0, x), max(x1, x), min(y0, y), max(y1, y)
            for dy in (-1, 0, 1):
                yy = y + dy
                if yy < 0 or yy >= h:
                    continue
                for dx in (-1, 0, 1):
                    xx = x + dx
                    if xx < 0 or xx >= w:
                        continue
                    k = yy * w + xx
                    if d[k] > level and not seen[k]:
                        seen[k] = 1
                        stack.append(k)
        total += n
        if n > best[0]:
            best = (n, x0, y0, x1, y1)
    return best, total


def report(name, w, h, d, level, region):
    differ = sum(1 for v in d if v)
    (n, x0, y0, x1, y1), over = regions(w, h, d, level)
    ok = n < region
    box = f" at x {x0}-{x1}, y {y0}-{y1}" if n else ""
    print(f"{name}: {100.0 * differ / (w * h):.2f} % px differ, {over} px > {level} levels, "
          f"largest region {n} px{box}: {'PASS' if ok else 'FAIL'}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("files", nargs="+", help="REF NEW pairs, or with --flicker REF_N REF_N1 NEW_N NEW_N1 groups")
    ap.add_argument("--level", type=int, default=32, help="a pixel differs visibly above this many levels (default 32)")
    ap.add_argument("--region", type=int, default=64, help="a visible region has this many pixels or more (default 64)")
    ap.add_argument("--flicker", action="store_true", help="compare frame-to-frame changes (groups of four files)")
    a = ap.parse_args()
    group = 4 if a.flicker else 2
    if len(a.files) % group:
        print(f"expected groups of {group} files", file=sys.stderr)
        return 2
    ok = True
    try:
        for k in range(0, len(a.files), group):
            f = a.files[k:k + group]
            imgs = [read_ppm(p) for p in f]
            if a.flicker:
                w, h, d = change_diff(*imgs)
                name = f"flicker {f[0]}->{f[1]} vs {f[2]}->{f[3]}"
            else:
                w, h, d = channel_diff(*imgs)
                name = f"{f[0]} vs {f[1]}"
            ok = report(name, w, h, d, a.level, a.region) and ok
    except (OSError, ValueError) as e:
        print(e, file=sys.stderr)
        return 2
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
