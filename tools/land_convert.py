#!/usr/bin/env python3
"""Convert a level's land tables to the native renderer's meshes
(docs/native-renderer.md), from the user's own disc data.

    land_convert.py --data DUMP/data --out DATA_DIR [--level 26] [--cell 32]

Reads 1ST_READ.BIN (the per-level land table pointers at 0x8C19E9FC, the UV
scale at 0x8C038F10) and the level's code overlay (SONICADV/ADV00.PRS for
Station Square, STG01.PRS for Emerald Coast, level 1; both loaded at
0x8C900000), and writes DATA_DIR/native/
LAND_<level>_<act>.BIN for each act's land table: every COL with the visible
bit, each mesh set of its model as indexed triangles in model space, keyed
by the model's address and the set's index.  The runtime draws a set from
inside the game's own mesh-set loop (hle/ninja/ta_mesh.c), with the matrix
and the polygon header the game computed for it, so materials, matrices and
the visible list are the game's (docs/native-renderer.md).  --world also
writes world-space positions (the COL object's njTranslate and njRotateXYZ:
Z, then Y, then X; no scale), for a one-matrix draw.  The output is game
data: it goes to the user's data folder, never into the repository.

Mesh sets of every type are converted: triangle and quad lists (types 0
and 1: fixed lengths 3 and 4, no length words) are drawn by the game's strip
writer as strips of that length, N-gons (type 2) as strips (0x8C037980).

Cells (version 2, docs/native-renderer.md s.6.2): each model's triangles in
its opaque sets (neither USE_ALPHA nor the environment map) are binned by
their centroid into cubes of --cell units in model space; within each set
the triangles are ordered by cell, so a cell is one index range per set.
Each cell has a sphere over its triangles' vertices and its longest edge:
the runtime draws a cell the GE may take whole from the persistent copy and
the others through the CPU path.

File layout (little-endian):
  header   'SA1N', version 2, land table address, COL count, batch count,
           material count, vertex count, index count, UV scale (float),
           model count, cell count, span count
  material texture id word (+12), flags (+16), diffuse (+0), specular (+4)
  batch    model address, mesh set index u16, COL index u16, material u16,
           pad u16, first vertex, vertex count, first index, index count
  vertex   x, y, z, normal x, y, z (model space, or world with --world),
           u, v (floats), point index u32 (the model's vertex it came from:
           the game's per-point lighting is looked up by it)
  index    u16, relative to the batch's first vertex; three per triangle
           (padded to 4 bytes)
  model    model address, first cell, cell count, first span, span count
  cell     sphere x, y, z, radius, longest edge (floats, model space)
  span     mesh set index u16, cell u16 (relative to the model's first),
           first index (relative to the set's batch), index count

Part of sa1-psp (MIT)."""
import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prs import prs_decompress  # noqa: E402

BIN_BASE = 0x8C010000
LEVEL_TABLE = 0x8C19E9FC          # land table pointers [level * 8 + act]
UV_SCALE = 0x8C038F10
OVERLAYS = {1: ("STG01.PRS", 0x8C900000), 26: ("ADV00.PRS", 0x8C900000), 27: ("ADV00.PRS", 0x8C900000), 28: ("ADV00.PRS", 0x8C900000)}
TURN = 2.0 * math.pi / 65536.0
VISIBLE = 0x80000000
USE_ALPHA = 0x00100000
ENV_MAP = 0x00400000
FIXED_LEN = {0: 3, 1: 4}            # mesh set types without length words (the lengths at 0x8C037980)


class Image:
    def __init__(self, data, base):
        self.data, self.base = data, base & 0x1FFFFFFF

    def has(self, addr, n=4):
        o = (addr & 0x1FFFFFFF) - self.base
        return 0 <= o and o + n <= len(self.data)

    def off(self, addr):
        o = (addr & 0x1FFFFFFF) - self.base
        if not (0 <= o < len(self.data)):
            raise ValueError("address 0x%08x outside the image" % addr)
        return o

    def u32(self, a): return struct.unpack_from("<I", self.data, self.off(a))[0]
    def s32(self, a): return struct.unpack_from("<i", self.data, self.off(a))[0]
    def u16(self, a): return struct.unpack_from("<H", self.data, self.off(a))[0]
    def s16(self, a): return struct.unpack_from("<h", self.data, self.off(a))[0]
    def f32(self, a): return struct.unpack_from("<f", self.data, self.off(a))[0]


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def object_matrix(img, obj):
    """The COL object's model-to-world matrix as the land draw builds it on the
    identity: njTranslate(pos), njRotateXYZ(ang); column-major, as math.c."""
    m = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]
    pos = [img.f32(obj + 8 + 4 * i) for i in range(3)]
    ang = [img.s32(obj + 20 + 4 * i) & 0xFFFF for i in range(3)]
    m[12], m[13], m[14] = pos

    def sc(a):
        return math.sin(a * TURN), math.cos(a * TURN)
    if ang[2]:                                     # Z
        s, c = sc(ang[2])
        for i in range(4):
            c0, c1 = m[i], m[4 + i]
            m[i], m[4 + i] = c * c0 + s * c1, c * c1 - s * c0
    if ang[1]:                                     # Y
        s, c = sc(ang[1])
        for i in range(4):
            c0, c2 = m[i], m[8 + i]
            m[i], m[8 + i] = c * c0 - s * c2, s * c0 + c * c2
    if ang[0]:                                     # X
        s, c = sc(ang[0])
        for i in range(4):
            c1, c2 = m[4 + i], m[8 + i]
            m[4 + i], m[8 + i] = c * c1 + s * c2, c * c2 - s * c1
    return m


def xform(m, v, w):
    return [m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12] * w,
            m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13] * w,
            m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] * w]


def convert(img, lt, level, act, world=False):
    ncol = img.s16(lt)
    cols = img.u32(lt + 12)
    mats, mat_index, batches, verts, idx, seen = [], {}, [], [], [], set()
    stats = {"cols": ncol, "visible": 0, "tris": 0, "skipped": 0}
    for ci in range(ncol):
        col = cols + 0x24 * ci
        flags = img.u32(col + 32)
        obj = img.u32(col + 24)
        if not (flags & VISIBLE) or not obj:
            continue
        model = img.u32(obj + 4)
        if not model or model == 0x195 or not img.has(model, 40):
            stats["skipped"] += 1
            continue
        stats["visible"] += 1
        if not world and model in seen:
            continue                        # another COL with the same model: model space, the same batches
        seen.add(model)
        m = object_matrix(img, obj) if world else [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]
        points, normals, npoint = img.u32(model), img.u32(model + 4), img.s32(model + 8)
        msets, mtab, nmset = img.u32(model + 12), img.u32(model + 16), img.u16(model + 20)
        wp = [xform(m, [img.f32(points + 12 * i + 4 * k) for k in range(3)], 1.0) for i in range(npoint)]
        wn = [xform(m, [img.f32(normals + 12 * i + 4 * k) for k in range(3)], 0.0) for i in range(npoint)] if normals else [[0.0, 1.0, 0.0]] * npoint
        for si in range(nmset):
            ms = msets + 24 * si
            matid, stype = img.u16(ms) & 0x3FFF, img.u16(ms) >> 14
            nstrip, strips, uvs = img.u16(ms + 2), img.u32(ms + 4), img.u32(ms + 20)
            mat = mtab + 20 * matid
            key = (img.u32(mat + 12), img.u32(mat + 16), img.u32(mat), img.u32(mat + 4))
            if key not in mat_index:
                mat_index[key] = len(mats)
                mats.append(key)
            vfirst, ifirst, local = len(verts), len(idx), {}
            p, q = strips, uvs
            for _ in range(nstrip):
                if stype in FIXED_LEN:
                    n = FIXED_LEN[stype]
                else:
                    n = img.s16(p) & 0x3FFF
                    p += 2
                corner = []
                for k in range(n):
                    pi = img.u16(p)
                    p += 2
                    u, v = (img.s16(q), img.s16(q + 2)) if uvs else (0, 0)
                    q += 4 if uvs else 0
                    vk = (pi, u, v)
                    if vk not in local:
                        local[vk] = len(verts) - vfirst
                        verts.append(wp[pi] + wn[pi] + [u, v, pi])
                    corner.append(local[vk])
                for k in range(n - 2):
                    a, b, c = corner[k], corner[k + 1], corner[k + 2]
                    if a == b or b == c or a == c:
                        continue
                    idx.extend((a, b, c) if k % 2 == 0 else (b, a, c))
                    stats["tris"] += 1
            if len(verts) - vfirst > 65535:
                raise ValueError("COL %d set %d: more than 65535 vertices" % (ci, si))
            batches.append((model, si, ci, mat_index[key], vfirst, len(verts) - vfirst, ifirst, len(idx) - ifirst))
    return mats, batches, verts, idx, stats


def make_cells(mats, batches, verts, idx, size):
    """Bins each model's opaque triangles into cubes of size units (by the
    centroid), reorders every opaque batch's triangles by cell (in place in
    idx), and returns the model directory, the cells and the spans."""
    by_model = {}
    for bi, b in enumerate(batches):
        by_model.setdefault(b[0], []).append(bi)
    models, cells, spans = [], [], []
    for model in sorted(by_model):
        tris = []                           # (cell key, batch, triangle's indices)
        for bi in by_model[model]:
            b = batches[bi]
            if mats[b[3]][1] & (USE_ALPHA | ENV_MAP):
                continue
            v0, i0, ni = b[4], b[6], b[7]
            for t in range(i0, i0 + ni, 3):
                tri = idx[t:t + 3]
                c = [sum(verts[v0 + k][a] for k in tri) / 3.0 for a in range(3)]
                key = (math.floor(c[0] / size), math.floor(c[2] / size), math.floor(c[1] / size))
                tris.append((key, bi, tri))
        keys = sorted({t[0] for t in tris})
        cell_of = {k: n for n, k in enumerate(keys)}
        if len(keys) > 65535:
            raise ValueError("model 0x%08x: more than 65535 cells" % model)
        pts_of = {k: [] for k in keys}
        edge_of = {k: 0.0 for k in keys}
        for key, bi, tri in tris:
            v0 = batches[bi][4]
            p = [verts[v0 + q][:3] for q in tri]
            pts_of[key] += p
            for a, b2 in ((0, 1), (1, 2), (2, 0)):
                edge_of[key] = max(edge_of[key], math.sqrt(sum((p[a][x] - p[b2][x]) ** 2 for x in range(3))))
        c0, s0 = len(cells), len(spans)
        for k in keys:
            pts = pts_of[k]
            lo = [min(q[a] for q in pts) for a in range(3)]
            hi = [max(q[a] for q in pts) for a in range(3)]
            ctr = [f32(0.5 * (lo[a] + hi[a])) for a in range(3)]
            r = max(math.sqrt(sum((q[a] - ctr[a]) ** 2 for a in range(3))) for q in pts)
            cells.append(ctr + [r * 1.0001 + 1e-3, edge_of[k]])
        for bi in by_model[model]:
            b = batches[bi]
            mine = [(cell_of[key], tri) for key, x, tri in tris if x == bi]
            if not mine:
                continue
            mine.sort(key=lambda t: t[0])   # stable: a cell's triangles keep their order
            out, at = [], 0
            for n, (cell, tri) in enumerate(mine):
                if n == 0 or cell != mine[n - 1][0]:
                    spans.append([b[1], cell, at, 0])
                out += tri
                at += 3
                spans[-1][3] += 3
            idx[b[6]:b[6] + len(out)] = out
        models.append((model, c0, len(cells) - c0, s0, len(spans) - s0))
    return models, cells, spans


def write(path, lt, ncol, uvscale, mats, batches, verts, idx, models, cells, spans):
    out = [b"SA1N", struct.pack("<7If3I", 2, lt, ncol, len(batches), len(mats), len(verts), len(idx), uvscale,
                                len(models), len(cells), len(spans))]
    out += [struct.pack("<4I", *m) for m in mats]
    out += [struct.pack("<IHHHHIIII", b[0], b[1], b[2], b[3], 0, *b[4:]) for b in batches]
    out += [struct.pack("<8fI", *(v[:6] + [v[6] * uvscale, v[7] * uvscale]), v[8]) for v in verts]
    out.append(struct.pack("<%dH" % len(idx), *idx))
    if len(idx) & 1:
        out.append(bytes(2))
    out += [struct.pack("<5I", *m) for m in models]
    out += [struct.pack("<5f", *c) for c in cells]
    out += [struct.pack("<HHII", *sp) for sp in spans]
    data = b"".join(out)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return len(data)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--data", required=True, help="the dump's data folder (1ST_READ.BIN, SONICADV/)")
    ap.add_argument("--out", required=True, help="the game's data folder to write native/ into")
    ap.add_argument("--level", type=int, default=26)
    ap.add_argument("--world", action="store_true", help="world-space positions (one matrix for the whole land; no cells)")
    ap.add_argument("--cell", type=float, default=32.0, help="cell size in model units (0: no cells)")
    a = ap.parse_args(argv)
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_abs = os.path.abspath(a.out)
    if out_abs == repo or out_abs.startswith(repo + os.sep):
        sys.exit("land_convert: the output must be outside the repository (it is game data)")
    exe = Image(open(os.path.join(a.data, "1ST_READ.BIN"), "rb").read(), BIN_BASE)
    if a.level not in OVERLAYS:
        sys.exit("land_convert: level %d is not mapped to its overlay yet" % a.level)
    name, base = OVERLAYS[a.level]
    ovl = Image(prs_decompress(open(os.path.join(a.data, "SONICADV", name), "rb").read()), base)
    uvscale = exe.f32(UV_SCALE)
    done = set()
    for act in range(8):
        lt = exe.u32(LEVEL_TABLE + 4 * (a.level * 8 + act))
        if not lt or lt in done or not ovl.has(lt, 32):
            continue
        done.add(lt)
        if ovl.u16(lt + 4) & 0x10:
            print("land_convert: act %d (0x%08x): models in a separate file, not handled yet" % (act, lt))
            continue
        mats, batches, verts, idx, st = convert(ovl, lt, a.level, act, a.world)
        models, cells, spans = make_cells(mats, batches, verts, idx, a.cell) if a.cell > 0 and not a.world else ([], [], [])
        path = os.path.join(a.out, "native", "LAND_%d_%d.BIN" % (a.level, act))
        size = write(path, lt, st["cols"], uvscale, mats, batches, verts, idx, models, cells, spans)
        print("land_convert: act %d land table 0x%08x: %d COLs, %d drawn, %d batches, %d materials, %d vertices, "
              "%d triangles, %d cells -> %s (%d KB)" % (act, lt, st["cols"], st["visible"], len(batches), len(mats), len(verts),
                                                        st["tris"], len(cells), path, size // 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
