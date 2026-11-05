# Faces that flicker (z-fighting): two faces in one plane (within DEPTH), facing the same way, that
# overlap by more than AREA, at any slope and of any size: a keycap's top on its well, a decal on a
# face it was not lifted off, a box doubled. (world/check_overlap.py looked only at large faces
# square to the room's axes: a keyboard's slope was never seen.) Every object of the last build:
#   blender -b build/world.blend --python world/check_zfight.py -- [WORD ...] [--all]
# WORD: only pairs with an object whose name has one of them. --all: every pair, not the first 60.
# --why: a few of each pair's overlaps, with the way they face and where.
# Within one object too: a model's own faces doubled flicker as much. Faces of Poly Haven's models
# against each other are their makers' (left out); against ours they count. Only what an eye can see
# counts: a point of the overlap whose way out along the faces' normal meets something within COVER
# (a foot's underside on the floor, a back against a wall), or that lies inside a solid, is hidden,
# and a pair is the area of its overlap left seen; a pair under SHOWS in all is not listed.
import math
import sys

import bmesh
import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree

DEPTH = 0.0003      # metres apart that a depth buffer cannot tell at a few metres (24 bits, near 0.01)
AREA = 2e-7         # square metres of one overlap that is looked at (half a millimetre square)
SHOWS = 1e-5        # square metres of a pair's overlaps, seen, that show (a spot of 3 x 3 mm)
THIN = 1e-4         # metres: an overlap narrower than this is a sliver along a shared edge
FACING = 0.9995     # their normals' cosine: within 1.8 degrees
COVER = 0.004       # metres in front of an overlap within which something hides it from every eye
SKIP = ("City", "Rain", "Holodeck mask", "Window glass")


def clip(subject, a, b):
    """The part of a convex polygon (2D points) on the left of the line a -> b."""
    out = []
    for i in range(len(subject)):
        p, q = subject[i - 1], subject[i]
        sp = (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])
        sq = (b[0] - a[0]) * (q[1] - a[1]) - (b[1] - a[1]) * (q[0] - a[0])
        if sq >= 0:
            if sp < 0:
                t = sp / (sp - sq)
                out.append((p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t))
            out.append(q)
        elif sp >= 0:
            t = sp / (sp - sq)
            out.append((p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t))
    return out


def area(poly):
    return 0.5 * sum(poly[i - 1][0] * poly[i][1] - poly[i][0] * poly[i - 1][1] for i in range(len(poly)))


def overlap(t, u, normal):
    """The area two triangles share, both laid in the plane square to normal, and the shared
    polygon's points back in the room."""
    x = normal.orthogonal().normalized()
    y = normal.cross(x)
    p = [(v.dot(x), v.dot(y)) for v in t]
    q = [(v.dot(x), v.dot(y)) for v in u]
    if area(p) < 0:
        p.reverse()
    if area(q) < 0:
        q.reverse()
    shared = p
    for i in range(3):
        shared = clip(shared, q[i - 1], q[i])
        if len(shared) < 3:
            return 0.0, []
    d = normal.dot(t[0])
    return area(shared), [x * a + y * b + normal * d for a, b in shared]


def seen(world, points, normal):
    """The share of an overlap an eye can see: its middle and points between it and its corners,
    each looking out along the normal for anything within COVER."""
    middle = sum(points, Vector()) / len(points)
    samples = [middle] + [middle + (p - middle) * 0.8 for p in points]
    return sum(1 for p in samples if not covered(world, p, normal)) / len(samples)


def covered(world, p, normal):
    """Whether no eye can see p: something lies on it looking back at it (the floor under a foot,
    a wall behind a back), stands in front of it within COVER, or it is inside a solid: out along
    the normal, past the stack's own faces, more faces are left by than entered (a stile's back in
    the door's leaf, whatever else is buried with it)."""
    if any(n.dot(normal) < -0.5 for _, n, _, _ in world.find_nearest_range(p, DEPTH)):
        return True
    origin, depth, first = p + normal * 1e-5, 0, True
    for _ in range(256):
        loc, n, _, _ = world.ray_cast(origin, normal, 100.0)
        if loc is None:
            break
        t = (loc - p).dot(normal)
        origin = loc + normal * 1e-5
        if n.dot(normal) > 0.99 and t <= 2 * DEPTH:
            continue   # (a face of the stack itself)
        if first and t <= COVER:
            return True
        first = False
        depth += 1 if n.dot(normal) > 0 else -1
    return depth > 0


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    every = "--all" in args
    words = [a for a in args if not a.startswith("--")]
    buckets = {}
    tris = []
    for ob in bpy.data.objects:
        if ob.type != 'MESH' or ob.name.startswith(SKIP) or not len(ob.data.polygons):
            continue
        bm = bmesh.new()
        bm.from_mesh(ob.data)
        bm.transform(ob.matrix_world)
        bmesh.ops.triangulate(bm, faces=bm.faces[:])
        names = [m.name if m else "?" for m in ob.data.materials]
        for f in bm.faces:
            a, b, c = (v.co.copy() for v in f.verts)
            n = (b - a).cross(c - a)
            if n.length < 1e-12:
                continue
            twice = n.length
            n.normalize()
            k = len(tris)
            tris.append((ob.name, "asset" in ob, names[f.material_index] if names else "?", (a, b, c), n, twice / 2))
            d = n.dot(a)
            key = (round(n.x * 50), round(n.y * 50), round(n.z * 50), round(d / DEPTH))
            buckets.setdefault(key, []).append(k)
        bm.free()
    print("ZFIGHT %d triangles in %d planes" % (len(tris), len(buckets)))
    world = BVHTree.FromPolygons([v for t in tris for v in t[3]], [(3 * k, 3 * k + 1, 3 * k + 2) for k in range(len(tris))], epsilon=0.0)
    found, why = {}, {}
    for key, members in buckets.items():
        # this plane and its neighbours in depth and in each normal's step (rounding puts a pair apart)
        near = []
        for dn in ((0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)):
            for dd in (0, 1):
                if dn == (0, 0, 0) and dd == 0:
                    continue
                near += buckets.get((key[0] + dn[0], key[1] + dn[1], key[2] + dn[2], key[3] + dd), [])
        group = [(k, True) for k in members] + [(k, False) for k in near]
        if len(group) < 2:
            continue
        # along one axis of the plane: sweep the triangles' spans
        n0 = tris[members[0]][4]
        x = n0.orthogonal().normalized()
        spans = sorted(((min(v.dot(x) for v in tris[k][3]), max(v.dot(x) for v in tris[k][3]), k, own) for k, own in group))
        for i, (lo, hi, k, own) in enumerate(spans):
            for lo2, hi2, k2, own2 in spans[i + 1:]:
                if lo2 > hi:
                    break
                if not (own or own2) or k == k2:
                    continue
                A, B = tris[k], tris[k2]
                if A[1] and B[1]:
                    continue   # (two of a Poly Haven model's own)
                if A[4].dot(B[4]) < FACING:
                    continue
                if max(abs(A[4].dot(v - A[3][0])) for v in B[3]) > DEPTH:
                    continue
                if words and not any(w in A[0] or w in B[0] for w in words):
                    continue
                s, points = overlap(A[3], B[3], A[4])
                if s > AREA and s / max((p - q).length for p in points for q in points) < THIN:
                    s = 0.0   # (a sliver along an edge two faces share: rounding, not an overlap)
                if s > AREA:
                    s *= seen(world, points, A[4])
                if s > AREA:
                    pair = tuple(sorted(((A[0], A[2]), (B[0], B[2]))))
                    if "--why" in args and len(why.setdefault(pair, [])) < 4:
                        c = sum(points, Vector()) / len(points)
                        extents = ["x%.3f..%.3f y%.3f..%.3f z%.3f..%.3f" % (min(-v.x for v in T[3]), max(-v.x for v in T[3]), min(v.z for v in T[3]), max(v.z for v in T[3]),
                                                                          min(-v.y for v in T[3]), max(-v.y for v in T[3])) for T in (A, B)]
                        why[pair].append("%.0f mm2 facing (%.2f, %.2f, %.2f) at (%.3f, %.3f, %.3f)  [%s | %s]" % (s * 1e6, -A[4].x, A[4].z, -A[4].y, -c.x, c.z, -c.y, extents[0], extents[1]))
                    old = found.get(pair)
                    middle = sum(A[3], Vector()) / 3
                    if old is None or s > old[0]:
                        found[pair] = (s + (old[0] if old else 0), middle) if old is None else (old[0] + s, old[1])
                    else:
                        found[pair] = (old[0] + s, old[1])
    order = sorted(((pair, v) for pair, v in found.items() if v[0] > SHOWS), key=lambda kv: -kv[1][0])
    for ((o1, m1), (o2, m2)), (s, at) in order if every else order[:60]:
        print("ZFIGHT %8.1f mm2  %s (%s)  and  %s (%s)  at (%.3f, %.3f, %.3f)" % (s * 1e6, o1, m1, o2, m2, -at.x, at.z, -at.y))
        for line in why.get(((o1, m1), (o2, m2)), []):
            print("ZFIGHT      " + line)
    print("ZFIGHT pairs: %d" % len(order))


if __name__ == "__main__":
    main()
