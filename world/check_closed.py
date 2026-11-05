# Meshes with holes (and faces of more than four corners, which Unity's importer cuts itself and
# can leave with no triangles at all: lib.export cuts them first, so there must be none): a solid whose surface does not close (an end left open, a seam not joined, a
# point at a lathe's axis made many times: nothing is welded first). Every object of the last build is cut into its pieces
# (the faces joined by edges), and a piece must be watertight: each edge between exactly two faces.
# A sheet is let be open: a piece all of whose faces look one way (within SHEET degrees of their
# mean), as a decal, a poster's paper, a wall; one lib.Builder.mesh made with sheet=True (a seam's
# line laid on a body); and a room's inside (ROOMS). Each object with open pieces is listed: how many,
# their open and their overfull edges, and where the first is. A closed piece must also be wound
# one way (each edge gone along in turn by its two faces) and look out (a positive volume): a face
# turned in is a hole in the picture, culled from the front:
#   blender -b build/world.blend --python world/check_closed.py -- [WORD ...] [--all] [--assets] [--pieces]
# --pieces: the first open pieces of each object, their size and place.
# WORD: only objects whose name has one of them. --all: every object, not the first 60. Poly Haven's
# models (assets.py's, named "<asset> <n>") are their makers' and are left out but with --assets.
import math
import os
import sys

import bmesh
import bpy
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_intersect import SKIP   # noqa: E402

ROOMS = ("Holo room",)   # a room's inside, one-sided as a wall is (a holodeck's shell, its doorway a hole in it)
SHEET = 15.0   # (a dimple or a bowl is no sheet: within this a face of a pinned poster bows)


def is_asset(ob):
    """One of Poly Haven's models (lib.Builder.finish marks them)."""
    return "asset" in ob


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    every, assets, each = "--all" in args, "--assets" in args, "--pieces" in args
    words = [a for a in args if not a.startswith("--")]
    found, total, ngons = [], 0, {}
    for ob in bpy.data.objects:
        if ob.type != 'MESH' or ob.name.startswith(SKIP) or not len(ob.data.polygons):
            continue
        many = sum(1 for p in ob.data.polygons if len(p.vertices) > 4)
        if many:   # (left for Unity's importer to cut: it cut a die's rounded top into nothing; lib.export cuts them)
            ngons[ob.name] = many
        if words and not any(w in ob.name for w in words):
            continue
        if (is_asset(ob) and not assets) or ob.name.startswith(ROOMS):
            continue
        bm = bmesh.new()
        bm.from_mesh(ob.data)
        bm.transform(ob.matrix_world)
        bm.normal_update()
        marked = bm.faces.layers.int.get("sheet")   # (lib.Builder.mesh's sheets: a seam laid on a body)
        seen, open_pieces, holes, over, first, bad_winding = set(), 0, 0, 0, None, 0
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
            edges = {e for f in island for e in f.edges}
            bare = [e for e in edges if len(e.link_faces) == 1]
            many = [e for e in edges if len(e.link_faces) > 2]
            # each edge between two faces is gone along in turn by each (their windings agree), and the
            # closed whole has its faces looking out (its volume, summed by them, comes out positive)
            turned = [e for e in edges if len(e.link_faces) == 2 and
                      (all(l.vert == e.verts[0] for l in e.link_loops) or all(l.vert == e.verts[1] for l in e.link_loops))]
            inside_out = False
            if not bare and not many and not turned:
                volume, middle = 0.0, sum((v.co for f in island for v in f.verts), Vector()) / sum(len(f.verts) for f in island)
                for f in island:   # (about the piece's middle: far out a sticker's volume is under float32's error)
                    vs = [v.co - middle for v in f.verts]
                    for k in range(1, len(vs) - 1):
                        volume += vs[0].dot(vs[k].cross(vs[k + 1])) / 6
                inside_out = volume < 0
            if turned or inside_out:
                bad_winding += 1
                if first is None:
                    p = (turned[0].verts[0] if turned else island[0].verts[0]).co
                    first = Vector((-p.x, p.z, -p.y))
            if not bare and not many:
                continue
            if marked is not None and all(f[marked] for f in island):
                continue
            mean = sum((f.normal * f.calc_area() for f in island), Vector())
            if mean.length > 0 and not many:
                mean.normalize()
                if all(f.normal.dot(mean) >= math.cos(math.radians(SHEET)) for f in island if f.calc_area() > 0):
                    continue   # a sheet
            if each and open_pieces < 8:
                points = [v.co for f in island for v in f.verts]
                lo = Vector((min(p[k] for p in points) for k in range(3)))
                hi = Vector((max(p[k] for p in points) for k in range(3)))
                mid = (lo + hi) / 2
                print("CLOSED   %s: a piece of %d faces, %.0f mm, %d open edges, at (%.3f, %.3f, %.3f)"
                      % (ob.name, len(island), (hi - lo).length * 1000, len(bare), -mid.x, mid.z, -mid.y))
            open_pieces += 1
            holes += len(bare)
            over += len(many)
            if first is None:
                p = (bare or many)[0].verts[0].co
                first = Vector((-p.x, p.z, -p.y))   # (Blender's axes to Unity's)
        bm.free()
        if open_pieces or bad_winding:
            found.append((open_pieces, holes, over, ob.name, first, bad_winding))
            total += open_pieces + bad_winding
    found.sort(key=lambda f: -f[0])
    for n, holes, over, name, at, wound in found if every else found[:60]:
        print("CLOSED %s: %d open pieces, %d open edges, %d edges of three faces or more, %d pieces wound wrong or inside out, one at (%.3f, %.3f, %.3f)"
              % (name, n, holes, over, wound, at.x, at.y, at.z))
    for name, many in sorted(ngons.items(), key=lambda kv: -kv[1])[:60]:
        print("CLOSED %s: %d faces of more than four corners" % (name, many))
    print("CLOSED objects with holes: %d, open or wrongly wound pieces: %d, faces of more than four corners: %d" % (len(found), total, sum(ngons.values())))


main()
