"""prs.py - clean-room decoder for Sega's PRS compression (the .PRS files of
Sonic Adventure), written from the format's description: a bit stream of
flags, LSB first, taken a byte at a time; 1 = copy one literal byte;
0 then 0 = short copy, two more flag bits (MSB first) give the length minus
2, the next byte minus 256 the offset; 0 then 1 = long copy, two bytes a, b:
offset = ((b << 8 | a) >> 3) - 8192, length = (a & 7) + 2, or if a & 7 is 0
the following byte plus 1; a == b == 0 ends the stream.

    python tools/prs.py IN.PRS OUT.bin        decompress one file
    python tools/prs.py --find NAME IN.PRS    print the offsets of a byte string in the output

Used by tools/pvm_ram_check.py (docs/m2-render-report.md section 0) to
compare an archive as the game's own, recompiled, decompressor left it in
guest RAM with an independent decompression of the same file.  Part of
sa1-psp (MIT); writes nothing into the repo and contains no game data.
"""
import sys


def prs_decompress(src):
    out = bytearray()
    pos = 0
    flags = 0
    nbits = 0

    def bit():
        nonlocal flags, nbits, pos
        if nbits == 0:
            flags = src[pos]
            pos += 1
            nbits = 8
        b = flags & 1
        flags >>= 1
        nbits -= 1
        return b

    while True:
        if bit():
            out.append(src[pos])
            pos += 1
            continue
        if bit():
            a = src[pos]
            b = src[pos + 1]
            pos += 2
            if a == 0 and b == 0:
                break
            off = (((b << 8) | a) >> 3) - 0x2000
            size = a & 7
            if size == 0:
                size = src[pos] + 1
                pos += 1
            else:
                size += 2
        else:
            size = bit() << 1
            size |= bit()
            size += 2
            off = src[pos] - 0x100
            pos += 1
        base = len(out) + off
        if base < 0:
            raise ValueError("PRS: back-reference before the start at input offset %d" % pos)
        for i in range(size):
            out.append(out[base + i])
    return bytes(out)


def main(argv):
    if len(argv) >= 3 and argv[0] == "--find":
        data = prs_decompress(open(argv[2], "rb").read())
        needle = argv[1].encode("latin-1")
        i = data.find(needle)
        while i >= 0:
            print(i)
            i = data.find(needle, i + 1)
        return 0
    if len(argv) != 2:
        print(__doc__)
        return 2
    data = prs_decompress(open(argv[0], "rb").read())
    open(argv[1], "wb").write(data)
    print("%s: %d bytes" % (argv[1], len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
