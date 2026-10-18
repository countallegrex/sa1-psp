#!/usr/bin/env python3
"""Compare two save states chunk by chunk, or list one.

  python tools/state_diff.py A.state [B.state]

Prints the header (frame, flags, build hash) and every chunk with its size;
with two files, which chunks differ and where the first difference is.  The
file format is docs/savestate.md section 3 (backend/desktop/savestate.c).

Part of sa1-psp (MIT).
"""
import struct
import sys

MAGIC = b"SA1STATE"
HEADER = struct.Struct("<32sQQIIqQQ")


def varint(buf, pos):
    r, shift = 0, 0
    while True:
        b = buf[pos]
        pos += 1
        r |= (b & 0x7F) << shift
        if not b & 0x80:
            return r, pos
        shift += 7


def unpack(buf, raw_len):
    out = bytearray()
    pos = 0
    while pos < len(buf):
        op = buf[pos]
        pos += 1
        n, pos = varint(buf, pos)
        if op == 0:
            out += bytes(n)
        elif op == 1:
            out += buf[pos:pos + n]
            pos += n
        elif op == 2:
            out += bytes([buf[pos]]) * n
            pos += 1
        else:
            raise ValueError("bad op %d at %d" % (op, pos - 1))
    if len(out) != raw_len:
        raise ValueError("unpacked %d bytes, header says %d" % (len(out), raw_len))
    return bytes(out)


def read_state(path):
    data = open(path, "rb").read()
    if data[:8] != MAGIC:
        raise ValueError("%s is not a state file" % path)
    version, hlen = struct.unpack_from("<II", data, 8)
    sha, anchor, base, frame, flags, epoch, raw_len, packed_len = HEADER.unpack_from(data, 16)
    body = unpack(data[16 + hlen:16 + hlen + packed_len], raw_len)
    chunks = []
    pos = 0
    while pos < len(body):
        name = body[pos:pos + 16].split(b"\0", 1)[0].decode()
        (n,) = struct.unpack_from("<I", body, pos + 16)
        chunks.append((name, body[pos + 20:pos + 20 + n]))
        pos += 20 + n
    header = dict(version=version, frame=frame, flags=flags, epoch=epoch, sha=sha.hex(), anchor=anchor, base=base,
                  raw_len=raw_len, packed_len=packed_len)
    return header, chunks


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    a_hdr, a = read_state(argv[0])
    print("%s: frame %d flags 0x%x epoch %d build %s... raw %d packed %d, %d chunks" % (
        argv[0], a_hdr["frame"], a_hdr["flags"], a_hdr["epoch"], a_hdr["sha"][:16], a_hdr["raw_len"], a_hdr["packed_len"], len(a)))
    if len(argv) == 1:
        for name, payload in a:
            print("  %-16s %10d" % (name, len(payload)))
        return 0
    b_hdr, b = read_state(argv[1])
    print("%s: frame %d flags 0x%x epoch %d build %s... raw %d packed %d, %d chunks" % (
        argv[1], b_hdr["frame"], b_hdr["flags"], b_hdr["epoch"], b_hdr["sha"][:16], b_hdr["raw_len"], b_hdr["packed_len"], len(b)))
    differ = 0
    for i in range(max(len(a), len(b))):
        if i >= len(a) or i >= len(b):
            print("  chunk %d only in %s" % (i, argv[0] if i < len(a) else argv[1]))
            differ += 1
            continue
        (na, pa), (nb, pb) = a[i], b[i]
        if na != nb or len(pa) != len(pb):
            print("  %-16s %10d  vs  %-16s %10d" % (na, len(pa), nb, len(pb)))
            differ += 1
            continue
        if pa != pb:
            first = next(k for k in range(len(pa)) if pa[k] != pb[k])
            count = sum(1 for k in range(len(pa)) if pa[k] != pb[k])
            print("  %-16s %10d  differs: %d byte(s), first at offset 0x%x" % (na, len(pa), count, first))
            differ += 1
    print("%d chunk(s) differ" % differ)
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
