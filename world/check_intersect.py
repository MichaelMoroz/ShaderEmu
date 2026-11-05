# Things that go into each other: a chair through a desk, a box through a shelf, a bat through
# the ceiling. Every object of the last build is cut into its pieces (the faces joined by edges:
# a cushion, a knob, a lead), and a piece of one object that reaches more than TOLERANCE into a
# piece of another is listed, deepest first, with where it is in Unity's coordinates:
#   blender -b build/world.blend --python world/check_intersect.py -- [WORD ...] [--within] [--all]
# WORD: only objects whose name has one of them (all of them without). --within: pieces of one
# object too (a model's own parts often go into each other on purpose: a knob into its drawer).
# --all: list every pair, not the first 60. What rests on something touches it and is not listed;
# KNOWN pairs are meant (a plug's lead in its plug, a picture on its hook).
import sys

import bmesh
import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree

TOLERANCE = 0.006    # metres a piece may be in another: a thing let down on another, a moulding on its wall
SKIP = ("City", "Rain", "Window glass", "Holodeck mask", "Clock hand")
KNOWN = [   # (a word of one object's name, a word of the other's): meant to be in each other
    ("Network", "Station "), ("Network", "Hubs"),   # the classroom's network leads end in plugs on the places' cards and the hubs
    ("Cables", "Desk"), ("Cables", "Computer"),     # the den's leads go into the wall and its units
    ("Holo grid", "Holo room"),                     # a holodeck's bars stand in its walls
    ("pachira", "Pots"), ("potted_plant_02", "potted_plant_02"),   # a plant in its pot, its leaves in its soil (two parts of one file)
    ("pc_station", "Station "),                     # a place's computer and its rest (the leads' plugs, the keys) are one model in two objects
    ("Cables", "Rack"), ("Cables", "Screen wall"),  # the den's leads into the rack and through the wall
    ("Panelling", "Window"), ("Beams", "Mouldings"),   # joinery: the dado rail into the window's casing, the beams into the cornice
]


def pieces(ob):
    """The object's pieces in Unity's coordinates: each a list of triangles (three points) and its box."""
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    bm.transform(ob.matrix_world)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    seen, out = set(), []
    for face in bm.faces:
        if face.index in seen:
            continue
        todo, island = [face], []
        seen.add(face.index)
        while todo:
            f = todo.pop()
            island.append(f)
            for e in f.edges:
                for g in e.link_faces:
                    if g.index not in seen:
                        seen.add(g.index)
                        todo.append(g)
        tris = [tuple(Vector((v.co.x, v.co.z, -v.co.y)) for v in f.verts) for f in island]   # (Blender's axes to Unity's, flipped in x below)
        tris = [tuple(Vector((-p.x, p.y, p.z)) for p in t) for t in tris]
        lo = Vector((min(p[i] for t in tris for p in t) for i in range(3)))
        hi = Vector((max(p[i] for t in tris for p in t) for i in range(3)))
        out.append((tris, lo, hi))
    bm.free()
    return out


def tree(tris):
    points, faces = [], []
    for t in tris:
        faces.append((len(points), len(points) + 2, len(points) + 1))   # (turned: the change of axes mirrored them)
        points += list(t)
    return BVHTree.FromPolygons(points, faces, epsilon=0.0)


def depth(a, b, tree_b):
    """How far the points of piece a go in behind piece b's faces (b's normals out), where a and b overlap at all."""
    lo, hi = b[1] - Vector((0.001,) * 3), b[2] + Vector((0.001,) * 3)
    most, where = 0.0, None
    for t in a[0]:
        for p in t:
            if not (lo.x <= p.x <= hi.x and lo.y <= p.y <= hi.y and lo.z <= p.z <= hi.z):
                continue
            hit = tree_b.find_nearest(p)
            if hit[0] is None:
                continue
            nearest, normal, _, distance = hit
            # behind the nearest face, and inside: a ray from the point upwards meets b an odd number of times
            if (p - nearest).dot(normal) < 0 and distance > most and inside(p, tree_b):
                most, where = distance, p
    return most, where


def inside(p, tree_b):
    crossings, start = 0, p.copy()
    for _ in range(64):
        hit = tree_b.ray_cast(start, Vector((0.0123, 1.0, 0.0071)).normalized())
        if hit[0] is None:
            break
        crossings += 1
        start = hit[0] + Vector((0.0123, 1.0, 0.0071)).normalized() * 1e-5
    return crossings % 2 == 1


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    within, every = "--within" in args, "--all" in args
    words = [a for a in args if not a.startswith("--")]
    objects = [o for o in bpy.data.objects if o.type == 'MESH' and not o.name.startswith(SKIP) and len(o.data.polygons)
               and (not words or any(w in o.name for w in words))]
    print("INTERSECT %d objects" % len(objects))
    parts = []   # (object's name, piece)
    for ob in objects:
        parts += [(ob.name, piece) for piece in pieces(ob)]
    print("INTERSECT %d pieces" % len(parts))
    # pairs whose boxes overlap, by sweeping along x
    order = sorted(range(len(parts)), key=lambda i: parts[i][1][1].x)
    trees, found = {}, []
    for k, i in enumerate(order):
        name_i, (tris_i, lo_i, hi_i) = parts[i]
        for j in order[k + 1:]:
            name_j, (tris_j, lo_j, hi_j) = parts[j]
            if lo_j.x > hi_i.x - TOLERANCE:
                break
            if (name_i == name_j and not within) or lo_j.y > hi_i.y - TOLERANCE or lo_i.y > hi_j.y - TOLERANCE \
                    or lo_j.z > hi_i.z - TOLERANCE or lo_i.z > hi_j.z - TOLERANCE:
                continue
            if any((x in name_i and y in name_j) or (y in name_i and x in name_j) for x, y in KNOWN):
                continue
            for key, tris in ((i, tris_i), (j, tris_j)):
                if key not in trees:
                    trees[key] = tree(tris)
            if not trees[i].overlap(trees[j]):
                continue
            d1, w1 = depth(parts[i][1], parts[j][1], trees[j])
            d2, w2 = depth(parts[j][1], parts[i][1], trees[i])
            d, w = (d1, w1) if d1 >= d2 else (d2, w2)
            if d > TOLERANCE:
                found.append((d, name_i, name_j, w))
    found.sort(key=lambda f: -f[0])
    for d, a, b, w in found if every else found[:60]:
        print("INTERSECT %4.0f mm  %s  into  %s  at (%.2f, %.2f, %.2f)" % (d * 1000, a, b, w.x, w.y, w.z))
    print("INTERSECT pairs: %d" % len(found))


if __name__ == "__main__":   # (world/check_float.py takes its pieces)
    main()
