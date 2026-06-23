#!/usr/bin/env python3
# bake_skyvis.py -- CSOZ renderer: offline geometric sky-visibility bake.
#
# Copyright (c) 2026 CSOZ project contributors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Clean-room tool: reads a GoldSrc BSP v30 file directly and computes, per world
# face-vertex, a geometric sky-visibility scalar skyVis in [0,1] (1 = fully open
# to the sky hemisphere, 0 = fully occluded by solid world geometry). Emits a
# "<mapname>.skyvis" sidecar keyed by GLOBAL surface index, with per-vertex
# values stored in the SAME edge-iteration order the runtime loader uses
# (csz_world.cpp EmitFaceVerts), so the loader can drop each value straight into
# the 5th VBO attribute (a_skyVis, location 4) without any remapping.
#
# This implements the AO-SKYACCESS-RESEARCH.md design (geometric sky visibility,
# 4th-going-on-5th VBO attribute, offline sidecar -- NOT the rejected runtime
# EV_PlayerTrace / lightmap-atlas-alpha paths).
#
# Occlusion is evaluated with the BSP's own rendering tree (NODES + PLANES +
# LEAFS, lumps 5/1/10): a hemisphere ray is "sky-visible" iff, walking the tree
# front-to-back from the surface point, the first non-empty leaf it enters is
# CONTENTS_SKY (or it escapes the world without hitting CONTENTS_SOLID). This is
# the physically-correct "does this point see sky", immune to artificial indoor
# light -- exactly the signal a lightmap-luminance proxy throws away.

import argparse
import json
import math
import os
import struct
import sys

# GoldSrc BSP v30 lump indices.
LUMP_PLANES = 1
LUMP_TEXTURES = 2
LUMP_VERTEXES = 3
LUMP_TEXINFO = 6
LUMP_FACES = 7
LUMP_NODES = 5
LUMP_LEAFS = 10
LUMP_EDGES = 12
LUMP_SURFEDGES = 13
LUMP_MODELS = 14

# GoldSrc leaf contents.
CONTENTS_SOLID = -2
CONTENTS_SKY = -6


def read_lumps(data):
    version = struct.unpack_from("<i", data, 0)[0]
    if version != 30:
        raise ValueError("not a GoldSrc BSP v30 (version=%d)" % version)
    lumps = []
    off = 4
    for _ in range(15):
        o, l = struct.unpack_from("<ii", data, off)
        lumps.append((o, l))
        off += 8
    return version, lumps


def slice_lump(data, lumps, idx):
    o, l = lumps[idx]
    return data[o:o + l]


def parse_planes(buf):
    # mplane_t: float normal[3], float dist, int type  (20 bytes)
    n = len(buf) // 20
    out = []
    for i in range(n):
        nx, ny, nz, d, _t = struct.unpack_from("<ffffi", buf, i * 20)
        out.append((nx, ny, nz, d))
    return out


def parse_vertexes(buf):
    n = len(buf) // 12
    out = []
    for i in range(n):
        out.append(struct.unpack_from("<fff", buf, i * 12))
    return out


def parse_edges(buf):
    # dedge_t (v30): unsigned short v[2]  (4 bytes)
    n = len(buf) // 4
    out = []
    for i in range(n):
        out.append(struct.unpack_from("<HH", buf, i * 4))
    return out


def parse_surfedges(buf):
    n = len(buf) // 4
    return list(struct.unpack_from("<%di" % n, buf, 0))


def parse_texinfo(buf):
    # texinfo_t: float vecs[2][4] (32), int miptex, int flags  (40 bytes)
    n = len(buf) // 40
    out = []
    for i in range(n):
        miptex, flags = struct.unpack_from("<ii", buf, i * 40 + 32)
        out.append((miptex, flags))
    return out


def parse_faces(buf):
    # dface_t: ushort planenum, ushort side, int firstedge, ushort numedges,
    #          ushort texinfo, byte styles[4], int lightofs  (20 bytes)
    n = len(buf) // 20
    out = []
    for i in range(n):
        planenum, side, firstedge, numedges, texinfo = struct.unpack_from(
            "<HHiHH", buf, i * 20)
        out.append({
            "planenum": planenum,
            "side": side,
            "firstedge": firstedge,
            "numedges": numedges,
            "texinfo": texinfo,
        })
    return out


def parse_nodes(buf):
    # dnode_t: int planenum, short children[2], short mins[3], short maxs[3],
    #          ushort firstface, ushort numfaces  (24 bytes)
    n = len(buf) // 24
    out = []
    for i in range(n):
        planenum, c0, c1 = struct.unpack_from("<ihh", buf, i * 24)
        out.append((planenum, c0, c1))
    return out


def parse_leafs(buf):
    # dleaf_t: int contents, int visofs, short mins[3], short maxs[3],
    #          ushort firstmarksurface, ushort nummarksurfaces, byte ambient[4] (28)
    n = len(buf) // 28
    out = []
    for i in range(n):
        contents = struct.unpack_from("<i", buf, i * 28)[0]
        out.append(contents)
    return out


def parse_models(buf):
    # dmodel_t: float mins[3], maxs[3], origin[3], int headnode[4], int visleafs,
    #           int firstface, int numfaces  (64 bytes)
    n = len(buf) // 64
    out = []
    for i in range(n):
        vals = struct.unpack_from("<9f7i", buf, i * 64)
        out.append({
            "headnode0": vals[9],
            "firstface": vals[14],
            "numfaces": vals[15],
        })
    return out


def parse_texture_names(buf):
    # miptex lump: int nummiptex, int offsets[nummiptex], then miptex_t each
    # (char name[16], uint w,h, uint offsets[4]).
    if len(buf) < 4:
        return []
    nummiptex = struct.unpack_from("<i", buf, 0)[0]
    offs = struct.unpack_from("<%di" % nummiptex, buf, 4)
    names = []
    for o in offs:
        if o < 0 or o + 16 > len(buf):
            names.append("")
            continue
        raw = buf[o:o + 16]
        name = raw.split(b"\x00", 1)[0].decode("ascii", "replace")
        names.append(name)
    return names


class Bsp:
    def __init__(self, data):
        _, lumps = read_lumps(data)
        self.planes = parse_planes(slice_lump(data, lumps, LUMP_PLANES))
        self.vertexes = parse_vertexes(slice_lump(data, lumps, LUMP_VERTEXES))
        self.edges = parse_edges(slice_lump(data, lumps, LUMP_EDGES))
        self.surfedges = parse_surfedges(slice_lump(data, lumps, LUMP_SURFEDGES))
        self.texinfo = parse_texinfo(slice_lump(data, lumps, LUMP_TEXINFO))
        self.faces = parse_faces(slice_lump(data, lumps, LUMP_FACES))
        self.nodes = parse_nodes(slice_lump(data, lumps, LUMP_NODES))
        self.leaf_contents = parse_leafs(slice_lump(data, lumps, LUMP_LEAFS))
        self.models = parse_models(slice_lump(data, lumps, LUMP_MODELS))
        self.texnames = parse_texture_names(slice_lump(data, lumps, LUMP_TEXTURES))

    def face_texname(self, face):
        ti = face["texinfo"]
        if ti < 0 or ti >= len(self.texinfo):
            return ""
        miptex = self.texinfo[ti][0]
        if miptex < 0 or miptex >= len(self.texnames):
            return ""
        return self.texnames[miptex]

    def face_is_sky(self, face):
        return self.face_texname(face).lower() == "sky"

    def face_vertex_indices(self, face):
        # Reproduce the runtime edge walk (csz_world.cpp FetchEdgeVertex):
        # surfedge >= 0 -> edge.v[0]; < 0 -> edge.v[1] of edge |surfedge|.
        out = []
        fe = face["firstedge"]
        for e in range(face["numedges"]):
            se = self.surfedges[fe + e]
            if se >= 0:
                vi = self.edges[se][0]
            else:
                vi = self.edges[-se][1]
            out.append(vi)
        return out

    def face_normal(self, face):
        pn = self.planes[face["planenum"]]
        nx, ny, nz = pn[0], pn[1], pn[2]
        if face["side"]:
            nx, ny, nz = -nx, -ny, -nz
        return (nx, ny, nz)


class Tracer:
    """Front-to-back ray walk over the BSP rendering tree (hull 0).

    Returns the kind of the first decisive leaf the ray enters:
      'solid' (CONTENTS_SOLID), 'sky' (CONTENTS_SKY), or 'escape'
    (ray reached tmax through empty/water space without hitting either).
    """

    def __init__(self, bsp, headnode):
        self.planes = bsp.planes
        self.nodes = bsp.nodes
        self.leaf_contents = bsp.leaf_contents
        self.headnode = headnode

    def trace(self, ox, oy, oz, dx, dy, dz, tmax):
        planes = self.planes
        nodes = self.nodes
        leaf_contents = self.leaf_contents
        stack = []
        node = self.headnode
        t0 = 0.0
        t1 = tmax
        while True:
            # Descend interior nodes, splitting the [t0,t1] segment at planes.
            while node >= 0:
                pl = planes[nodes[node][0]]
                pnx, pny, pnz, pd = pl
                do = ox * pnx + oy * pny + oz * pnz - pd
                nd = dx * pnx + dy * pny + dz * pnz
                s0 = do + t0 * nd
                s1 = do + t1 * nd
                c0 = nodes[node][1]  # front child (normal side)
                c1 = nodes[node][2]  # back child
                if s0 >= 0.0 and s1 >= 0.0:
                    node = c0
                    continue
                if s0 < 0.0 and s1 < 0.0:
                    node = c1
                    continue
                tmid = -do / nd
                if s0 >= 0.0:
                    near, far = c0, c1
                else:
                    near, far = c1, c0
                stack.append((far, tmid, t1))
                node = near
                t1 = tmid
            # Leaf reached.
            c = leaf_contents[-1 - node]
            if c == CONTENTS_SOLID:
                return "solid"
            if c == CONTENTS_SKY:
                return "sky"
            if not stack:
                return "escape"
            node, t0, t1 = stack.pop()


def radical_inverse_base2(i):
    # van der Corput sequence in base 2.
    rev = 0
    f = 0.5
    r = 0.0
    while i > 0:
        r += f * (i & 1)
        i >>= 1
        f *= 0.5
    return r


def hemisphere_dirs(nx, ny, nz, count):
    # Cosine-weighted hemisphere about (nx,ny,nz) via Malley's method over a
    # deterministic Hammersley sequence (reproducible -> stable stats).
    # Build an orthonormal basis (t,b,n).
    if abs(nz) < 0.999:
        ux, uy, uz = 0.0, 0.0, 1.0
    else:
        ux, uy, uz = 1.0, 0.0, 0.0
    # t = normalize(cross(u, n))
    tx = uy * nz - uz * ny
    ty = uz * nx - ux * nz
    tz = ux * ny - uy * nx
    tl = math.sqrt(tx * tx + ty * ty + tz * tz)
    if tl < 1e-6:
        tx, ty, tz = 1.0, 0.0, 0.0
        tl = 1.0
    tx, ty, tz = tx / tl, ty / tl, tz / tl
    # b = cross(n, t)
    bx = ny * tz - nz * ty
    by = nz * tx - nx * tz
    bz = nx * ty - ny * tx
    dirs = []
    for i in range(count):
        u1 = (i + 0.5) / count
        u2 = radical_inverse_base2(i + 1)
        r = math.sqrt(u1)          # sin(theta), cosine-weighted
        cz = math.sqrt(max(0.0, 1.0 - u1))  # cos(theta)
        phi = 2.0 * math.pi * u2
        lx = r * math.cos(phi)
        ly = r * math.sin(phi)
        dx = lx * tx + ly * bx + cz * nx
        dy = lx * ty + ly * by + cz * ny
        dz = lx * tz + ly * bz + cz * nz
        dirs.append((dx, dy, dz))
    return dirs


def main():
    ap = argparse.ArgumentParser(description="CSOZ offline sky-visibility bake")
    ap.add_argument("bsp", help="path to <map>.bsp")
    ap.add_argument("-o", "--out", help="output .skyvis path (default: <map>.skyvis next to bsp)")
    ap.add_argument("-s", "--samples", type=int, default=96, help="hemisphere rays per vertex")
    ap.add_argument("--stats", help="output stats json path")
    ap.add_argument("--epsilon", type=float, default=2.0, help="surface push-off along normal (units)")
    args = ap.parse_args()

    with open(args.bsp, "rb") as f:
        data = f.read()
    bsp = Bsp(data)

    world = bsp.models[0]
    headnode = world["headnode0"]
    tracer = Tracer(bsp, headnode)

    # World-diagonal as the ray length (a fully open ray must reach a sky leaf or
    # escape within the world bound).
    big = 1.0e9
    for m in bsp.models[:1]:
        pass
    # derive map extent from vertex AABB
    xs = [v[0] for v in bsp.vertexes]
    ys = [v[1] for v in bsp.vertexes]
    zs = [v[2] for v in bsp.vertexes]
    diag = math.sqrt((max(xs) - min(xs)) ** 2 + (max(ys) - min(ys)) ** 2 +
                     (max(zs) - min(zs)) ** 2) if xs else 8192.0
    tmax = diag * 1.5 + 64.0

    nfaces = len(bsp.faces)
    samples = args.samples
    eps = args.epsilon

    # Per-(quantized pos, quantized normal) cache: coincident face-vertices that
    # share a position AND normal (e.g. a wall corner) get one trace, not N.
    cache = {}

    per_face = []  # list of (numverts, [skyVis,...]) indexed by global face
    all_vals = []
    sky_faces = 0

    for fi, face in enumerate(bsp.faces):
        ne = face["numedges"]
        if bsp.face_is_sky(face):
            sky_faces += 1
            # Sky faces emit no runtime geometry; store zeros as placeholders so
            # the sidecar stays globally indexed. Loader never reads these.
            per_face.append((ne, [0.0] * ne))
            continue
        nrm = bsp.face_normal(face)
        vis = bsp.face_vertex_indices(face)
        vals = []
        for vi in vis:
            px, py, pz = bsp.vertexes[vi]
            ox = px + nrm[0] * eps
            oy = py + nrm[1] * eps
            oz = pz + nrm[2] * eps
            key = (round(ox, 1), round(oy, 1), round(oz, 1),
                   round(nrm[0], 3), round(nrm[1], 3), round(nrm[2], 3))
            sv = cache.get(key)
            if sv is None:
                hits = 0
                for (dx, dy, dz) in hemisphere_dirs(nrm[0], nrm[1], nrm[2], samples):
                    r = tracer.trace(ox, oy, oz, dx, dy, dz, tmax)
                    if r == "sky" or r == "escape":
                        hits += 1
                sv = hits / float(samples)
                cache[key] = sv
            vals.append(sv)
            all_vals.append(sv)
        per_face.append((ne, vals))
        if (fi % 500) == 0:
            sys.stderr.write("  faces %d/%d  (cache=%d)\r" % (fi, nfaces, len(cache)))
            sys.stderr.flush()
    sys.stderr.write("\n")

    # --- write sidecar ---
    out_path = args.out
    if not out_path:
        out_path = os.path.splitext(args.bsp)[0] + ".skyvis"
    with open(out_path, "wb") as f:
        f.write(b"CSZSKYV1")
        f.write(struct.pack("<II", 1, nfaces))
        for (ne, vals) in per_face:
            f.write(struct.pack("<I", ne))
            f.write(struct.pack("<%df" % ne, *vals))

    # --- stats ---
    n = len(all_vals)
    if n == 0:
        print("no drawable faces baked")
        return
    vmin = min(all_vals)
    vmax = max(all_vals)
    vmean = sum(all_vals) / n
    nbins = 10
    hist = [0] * nbins
    for v in all_vals:
        b = min(nbins - 1, int(v * nbins))
        hist[b] += 1

    # Independent indoor/outdoor labelling: a single straight-up ray per
    # face centroid (roofed -> not 'sky'/'escape' = indoor). This is a SEPARATE
    # signal from the hemisphere skyVis, so comparing the two cross-checks the bake.
    indoor_sv = []
    outdoor_sv = []
    for fi, face in enumerate(bsp.faces):
        if bsp.face_is_sky(face):
            continue
        ne, vals = per_face[fi]
        if not vals:
            continue
        vis = bsp.face_vertex_indices(face)
        cx = sum(bsp.vertexes[v][0] for v in vis) / ne
        cy = sum(bsp.vertexes[v][1] for v in vis) / ne
        cz = sum(bsp.vertexes[v][2] for v in vis) / ne
        nrm = bsp.face_normal(face)
        up = tracer.trace(cx + nrm[0] * 4.0, cy + nrm[1] * 4.0, cz + nrm[2] * 4.0 + 4.0,
                          0.0, 0.0, 1.0, tmax)
        fmean = sum(vals) / ne
        if up == "solid":
            indoor_sv.append(fmean)
        else:
            outdoor_sv.append(fmean)

    def avg(a):
        return (sum(a) / len(a)) if a else float("nan")

    stats = {
        "map": os.path.basename(args.bsp),
        "samples_per_vertex": samples,
        "total_faces": nfaces,
        "sky_faces": sky_faces,
        "baked_vertices": n,
        "unique_traced": len(cache),
        "skyVis_min": vmin,
        "skyVis_max": vmax,
        "skyVis_mean": vmean,
        "histogram_bins": ["%.1f-%.1f" % (i / nbins, (i + 1) / nbins) for i in range(nbins)],
        "histogram_counts": hist,
        "roofed_face_count": len(indoor_sv),
        "open_face_count": len(outdoor_sv),
        "roofed_mean_skyVis": avg(indoor_sv),
        "open_mean_skyVis": avg(outdoor_sv),
        "sidecar": out_path,
        "sidecar_bytes": os.path.getsize(out_path),
    }
    print(json.dumps(stats, indent=2))
    if args.stats:
        with open(args.stats, "w") as f:
            json.dump(stats, f, indent=2)


if __name__ == "__main__":
    main()
