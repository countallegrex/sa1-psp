"""A flat little-endian image of guest memory (the executable, or a test
snippet) addressed by guest address.  All addresses are normalised to the
physical 29-bit space so that P0 (0x0C...), P1 (0x8C...) and P2 (0xAC...)
spellings of the same RAM word are the same key."""

import struct

PHYS_MASK = 0x1FFFFFFF


def phys(addr):
    return addr & PHYS_MASK


class Image:
    def __init__(self, data, base):
        self.data = bytes(data)
        self.base = phys(base)
        self.end = self.base + len(self.data)

    @classmethod
    def from_file(cls, path, base):
        with open(path, "rb") as f:
            return cls(f.read(), base)

    def contains(self, addr, size=2):
        a = phys(addr)
        return self.base <= a and a + size <= self.end

    def off(self, addr):
        return phys(addr) - self.base

    def u16(self, addr):
        return struct.unpack_from("<H", self.data, self.off(addr))[0]

    def u32(self, addr):
        return struct.unpack_from("<I", self.data, self.off(addr))[0]

    def s16(self, addr):
        return struct.unpack_from("<h", self.data, self.off(addr))[0]

    def s8(self, addr):
        return struct.unpack_from("<b", self.data, self.off(addr))[0]
