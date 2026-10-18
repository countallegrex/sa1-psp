"""texsheet.py - contact sheets of a texture dump directory.

    python tools/texsheet.py <dump dir> <out dir> [--tile 96] [--cols 8] [--rows 8]

<dump dir> holds the PAMs (RGB + alpha) the desktop build writes with
--texdump DIR (named <index>_<gbix>_<fmt>_<flags>_<w>x<h>_<mips>.pam; plain
PPMs are accepted too).  Each sheet tiles up to cols x rows of them in index
order, scaled to fit the tile (nearest neighbour, aspect kept, alpha
composited over a grey checkerboard), with a label of index, pixel format
and data format under each; sheets are written as sheet_NNN.png to <out dir>
and an index.txt lists which files went on which sheet.  To review: look at
the sheets, note the index of every tile that looks wrong, and group the
indices by the format and flags in their labels, which name the decode path
(hle/ninja/pvr_decode.c).  No dependencies beyond the standard library.

Part of sa1-psp (MIT); the dumps stay outside the repo.
"""
import os
import re
import struct
import sys
import zlib

# 5x7 glyphs for the labels: digits, upper-case letters, a few symbols.
FONT = {
    '0': "01110 10001 10011 10101 11001 10001 01110", '1': "00100 01100 00100 00100 00100 00100 01110",
    '2': "01110 10001 00001 00010 00100 01000 11111", '3': "11111 00010 00100 00010 00001 10001 01110",
    '4': "00010 00110 01010 10010 11111 00010 00010", '5': "11111 10000 11110 00001 00001 10001 01110",
    '6': "00110 01000 10000 11110 10001 10001 01110", '7': "11111 00001 00010 00100 01000 01000 01000",
    '8': "01110 10001 10001 01110 10001 10001 01110", '9': "01110 10001 10001 01111 00001 00010 01100",
    'A': "01110 10001 10001 11111 10001 10001 10001", 'B': "11110 10001 10001 11110 10001 10001 11110",
    'C': "01110 10001 10000 10000 10000 10001 01110", 'D': "11100 10010 10001 10001 10001 10010 11100",
    'E': "11111 10000 10000 11110 10000 10000 11111", 'F': "11111 10000 10000 11110 10000 10000 10000",
    'G': "01110 10001 10000 10111 10001 10001 01111", 'H': "10001 10001 10001 11111 10001 10001 10001",
    'I': "01110 00100 00100 00100 00100 00100 01110", 'J': "00111 00010 00010 00010 00010 10010 01100",
    'K': "10001 10010 10100 11000 10100 10010 10001", 'L': "10000 10000 10000 10000 10000 10000 11111",
    'M': "10001 11011 10101 10101 10001 10001 10001", 'N': "10001 10001 11001 10101 10011 10001 10001",
    'O': "01110 10001 10001 10001 10001 10001 01110", 'P': "11110 10001 10001 11110 10000 10000 10000",
    'Q': "01110 10001 10001 10001 10101 10010 01101", 'R': "11110 10001 10001 11110 10100 10010 10001",
    'S': "01111 10000 10000 01110 00001 00001 11110", 'T': "11111 00100 00100 00100 00100 00100 00100",
    'U': "10001 10001 10001 10001 10001 10001 01110", 'V': "10001 10001 10001 10001 10001 01010 00100",
    'W': "10001 10001 10001 10101 10101 10101 01010", 'X': "10001 10001 01010 00100 01010 10001 10001",
    'Y': "10001 10001 10001 01010 00100 00100 00100", 'Z': "11111 00001 00010 00100 01000 10000 11111",
    ' ': "00000 00000 00000 00000 00000 00000 00000", '?': "01110 10001 00001 00010 00100 00000 00100",
    '_': "00000 00000 00000 00000 00000 00000 11111", '-': "00000 00000 00000 11111 00000 00000 00000",
}


def read_image(path):
    """PPM (P6, RGB) or PAM (P7, RGB_ALPHA): width, height, RGBA bytes."""
    d = open(path, 'rb').read()
    if d.startswith(b'P7'):
        hdr, body = d.split(b'ENDHDR\n', 1)
        fields = dict(l.split(None, 1) for l in hdr.decode('latin-1').splitlines()[1:] if ' ' in l)
        w, h, depth = int(fields['WIDTH']), int(fields['HEIGHT']), int(fields['DEPTH'])
        if depth == 4:
            return w, h, body[:w * h * 4]
        out = bytearray()
        for i in range(w * h):
            out += body[i * depth:i * depth + 3] + b'\xff'
        return w, h, bytes(out)
    parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    pix = parts[3]
    out = bytearray()
    for i in range(w * h):
        out += pix[i * 3:i * 3 + 3] + b'\xff'
    return w, h, bytes(out)


def write_png(path, w, h, rows):
    raw = b''.join(b'\x00' + r for r in rows)

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def draw_text(canvas, cw, x0, y0, text, scale=1):
    for ci, ch in enumerate(text.upper()):
        g = FONT.get(ch, FONT['?']).split()
        for gy, row in enumerate(g):
            for gx, bit in enumerate(row):
                if bit == '1':
                    for sy in range(scale):
                        for sx in range(scale):
                            x = x0 + (ci * 6 + gx) * scale + sx
                            y = y0 + gy * scale + sy
                            if 0 <= x < cw and 0 <= y < len(canvas):
                                canvas[y][x * 3:x * 3 + 3] = b'\xff\xff\xff'


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    src, out = argv[0], argv[1]
    tile, cols, rows = 96, 8, 8
    i = 2
    while i < len(argv):
        if argv[i] == '--tile': tile = int(argv[i + 1]); i += 2
        elif argv[i] == '--cols': cols = int(argv[i + 1]); i += 2
        elif argv[i] == '--rows': rows = int(argv[i + 1]); i += 2
        else: print('unknown option', argv[i]); return 2
    pat = re.compile(r'^(\d+)_(\d+)_([^_]+)_([^_]+)_(\d+)x(\d+)_(\d)\.(?:ppm|pam)$')
    files = []
    for f in os.listdir(src):
        m = pat.match(f)
        if m:
            files.append((int(m[1]), f, m[3], m[4], int(m[5]), int(m[6]), m[7]))
    files.sort()
    if not files:
        print('no texture dumps in', src)
        return 2
    os.makedirs(out, exist_ok=True)
    label_h = 20
    cell_w, cell_h = tile + 4, tile + label_h + 4
    W, H = cols * cell_w, rows * cell_h
    per = cols * rows
    index = open(os.path.join(out, 'index.txt'), 'w')
    for sheet in range((len(files) + per - 1) // per):
        canvas = [bytearray(b'\x30' * (W * 3)) for _ in range(H)]
        for k, (idx, f, fmt, flags, w, h, mips) in enumerate(files[sheet * per:(sheet + 1) * per]):
            cx, cy = (k % cols) * cell_w + 2, (k // cols) * cell_h + 2
            tw, th, pix = read_image(os.path.join(src, f))
            s = min(tile / tw, tile / th)
            dw, dh = max(1, int(tw * s)), max(1, int(th * s))
            ox, oy = cx + (tile - dw) // 2, cy + (tile - dh) // 2
            for y in range(dh):
                sy = min(th - 1, int(y / s))
                row = canvas[oy + y]
                for x in range(dw):
                    sx = min(tw - 1, int(x / s))
                    k = (sy * tw + sx) * 4
                    a = pix[k + 3]
                    # alpha composited over an 8-px grey checkerboard, so transparency is visible
                    bg = 0x90 if ((x // 8) + (y // 8)) & 1 else 0x60
                    row[(ox + x) * 3:(ox + x) * 3 + 3] = bytes((pix[k + c] * a + bg * (255 - a)) // 255 for c in range(3))
            draw_text(canvas, W, cx, cy + tile + 2, '%04d %s' % (idx, fmt))
            draw_text(canvas, W, cx, cy + tile + 11, '%s %dx%d%s' % (flags, w, h, ' M' if mips == '1' else ''))
            index.write('sheet_%03d tile %2d: %s\n' % (sheet, k, f))
        name = os.path.join(out, 'sheet_%03d.png' % sheet)
        write_png(name, W, H, [bytes(r) for r in canvas])
        print(name)
    index.close()
    print('%d textures on %d sheets' % (len(files), (len(files) + per - 1) // per))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
