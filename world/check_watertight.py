# Where skins end: blender -b build/world.blend --python world/check_watertight.py -- [WORD ...]
# A closed thing has two faces at every edge. For each object whose name has one of the words
# (the classroom's first place, without any) this lists the loops of edges with one face, in
# Unity's coordinates: each is a hole unless something else stands in it.
import sys

import bmesh
import bpy
from mathutils import Vector

LISTED = 0.2   # loops shorter than this (metres) are only counted


def main():
    words = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not words:   # the shells are one mesh: the first of them, and the first place's rest
        words = [min(o.name for o in bpy.data.objects if o.name.startswith("pc_station")), "Station 0 rest"]
    graph = bpy.context.evaluated_depsgraph_get()
    total = 0
    for ob in sorted((o for o in bpy.data.objects if o.type == 'MESH' and any(word in o.name for word in words)), key=lambda o: o.name):
        bm = bmesh.new()
        bm.from_object(ob, graph)
        bm.transform(ob.matrix_world)
        bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-5)
        names = [slot.material.name for slot in ob.material_slots]
        open_edges = {e for e in bm.edges if e.is_boundary}
        crowded = sum(1 for e in bm.edges if len(e.link_faces) > 2)
        loops = []
        while open_edges:
            loop, todo = [], [open_edges.pop()]
            while todo:
                edge = todo.pop()
                loop.append(edge)
                for vert in edge.verts:
                    for other in vert.link_edges:
                        if other in open_edges:
                            open_edges.remove(other)
                            todo.append(other)
            loops.append(loop)
        print("OPEN %-22s %d loops, %.2f m; %d edges with more than two faces" % (
            ob.name, len(loops), sum(e.calc_length() for loop in loops for e in loop), crowded))
        small = 0
        for loop in sorted(loops, key=lambda l: -sum(e.calc_length() for e in l)):
            length = sum(e.calc_length() for e in loop)
            if length < LISTED:
                small += 1
                continue
            points = [Vector((-v.co.x, v.co.z, -v.co.y)) for e in loop for v in e.verts]
            low = Vector([min(p[i] for p in points) for i in range(3)])
            high = Vector([max(p[i] for p in points) for i in range(3)])
            mid, size = (low + high) / 2, high - low
            print("OPEN   %5.2f m of %-12s middle (%.3f, %.3f, %.3f), %.3f x %.3f x %.3f" % (
                length, names[loop[0].link_faces[0].material_index], mid.x, mid.y, mid.z, size.x, size.y, size.z))
        if small:
            print("OPEN   and %d loops under %.1f m" % (small, LISTED))
        total += len(loops)
        bm.free()
    print("OPEN %d loops in all" % total)


main()
