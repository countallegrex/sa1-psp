"""verify_dump.py - check an extracted GD-ROM dump against its GDI image.

    python tools/verify_dump.py <dump dir>

<dump dir> holds the .gdi, its track files and the extracted tree in data/
(what gdi_extract.py wrote).  The high-density data track is read as an
ISO9660 volume (2352-byte raw sectors, mode 1: 16-byte header, 2048 bytes of
user data, 288 of EDC/ECC; track LBAs are absolute, the high-density area
starts at 45000); every file in the volume is hashed from the track and
compared with the SHA-256 of the extracted file.  Prints the files that are
missing or differ and a one-line summary; exit status 1 if anything is
wrong.  Prints no hashes of game files (the repo keeps only 1ST_READ.BIN's,
recomp/sa1_us_1005/README.md).

Part of sa1-psp (MIT).  Clean-room ISO9660 reader; no game data in the repo.
"""
import hashlib
import os
import re
import struct
import sys

SECTOR = 2048


def parse_gdi(path):
    base = os.path.dirname(os.path.abspath(path))
    pat = re.compile(r'^(\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+(?:"([^"]+)"|(\S+))\s+(-?\d+)')
    tracks = []
    with open(path, "r", errors="replace") as f:
        lines = [l.strip() for l in f if l.strip()]
    for line in lines[1:]:
        m = pat.match(line)
        if m:
            tracks.append({"num": int(m[1]), "lba": int(m[2]), "type": int(m[3]), "ss": int(m[4]),
                           "file": os.path.join(base, m[5] or m[6])})
    return tracks


class Track:
    def __init__(self, info):
        self.lba0 = info["lba"]
        self.ss = info["ss"]
        self.f = open(info["file"], "rb")
        self.off = 0
        if self.ss == 2352:
            self.f.seek(0)
            head = self.f.read(16)
            self.off = 24 if head[15] == 2 else 16          # mode 2 form 1 / mode 1

    def user(self, lba, count):
        """User data of `count` sectors from absolute LBA `lba`."""
        self.f.seek((lba - self.lba0) * self.ss)
        raw = self.f.read(count * self.ss)
        if self.ss == 2048:
            return raw
        return b"".join(raw[i * self.ss + self.off:i * self.ss + self.off + SECTOR] for i in range(count))

    def hash_extent(self, lba, size):
        h = hashlib.sha256()
        left = size
        while left > 0:
            n = min(left, 256 * SECTOR)
            secs = (n + SECTOR - 1) // SECTOR
            h.update(self.user(lba, secs)[:n])
            lba += secs
            left -= n
        return h.hexdigest()


def walk(track, dir_lba, dir_size, prefix, out):
    data = track.user(dir_lba, (dir_size + SECTOR - 1) // SECTOR)[:dir_size]
    pos = 0
    while pos < len(data):
        ln = data[pos]
        if ln == 0:                                  # padding to the next sector
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + ln]
        pos += ln
        extent = struct.unpack_from("<I", rec, 2)[0]
        size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("latin-1").split(";")[0]
        path = prefix + name
        if flags & 2:
            walk(track, extent, size, path + "/", out)
        else:
            out.append((path, extent, size))


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    root = argv[0]
    gdis = [f for f in os.listdir(root) if f.lower().endswith(".gdi")]
    if not gdis:
        print("no .gdi in", root)
        return 2
    tracks = parse_gdi(os.path.join(root, gdis[0]))
    hd = [t for t in tracks if t["type"] == 4 and t["lba"] >= 45000]
    if not hd:
        print("no high-density data track")
        return 2
    track = Track(hd[-1])
    pvd = track.user(track.lba0 + 16, 1)
    if pvd[1:6] != b"CD001":
        print("no ISO9660 volume descriptor at LBA %d" % (track.lba0 + 16))
        return 2
    root_rec = pvd[156:190]
    root_lba = struct.unpack_from("<I", root_rec, 2)[0]
    root_size = struct.unpack_from("<I", root_rec, 10)[0]
    files = []
    walk(track, root_lba, root_size, "", files)
    data_dir = os.path.join(root, "data")
    bad = missing = ok = 0
    total = 0
    for path, lba, size in files:
        local = os.path.join(data_dir, *path.split("/"))
        total += size
        if not os.path.isfile(local):
            print("missing  %s" % path)
            missing += 1
            continue
        h = hashlib.sha256()
        with open(local, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        if os.path.getsize(local) != size or h.hexdigest() != track.hash_extent(lba, size):
            print("differs  %s (%d bytes on disc, %d extracted)" % (path, size, os.path.getsize(local)))
            bad += 1
        else:
            ok += 1
    print("verify_dump: %d files in the volume (%.1f MB), %d identical, %d differ, %d missing" % (len(files), total / 1e6, ok, bad, missing))
    return 1 if bad or missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
