# Faces that lie in one plane, face the same way and overlap flicker in the room (z-fighting).
# This lists them from the last build:
#   blender -b build/world.blend --python world/check_overlap.py
# It compares faces' boxes within their plane, so a slanted pair may be listed without touching.
import bpy
from mathutils import Vector

SKIP = ("City", "Rain", "pachira", "potted_plant", "book_encyclopedia", "boombox", "classic_laptop", "sofa", "bar_chair",
        "round_wooden", "modern_coffee", "television", "cardboard", "desk_lamp", "alarm_clock", "wall_clock")
groups = {}
for ob in bpy.data.objects:
    if ob.type != 'MESH' or ob.name.startswith(SKIP):
        continue
    mesh, world = ob.data, ob.matrix_world
    names = [m.name if m else "?" for m in mesh.materials]
    for poly in mesh.polygons:
        if poly.area < 0.0004:
            continue
        normal = (world.to_3x3() @ poly.normal).normalized()
        points = [world @ mesh.vertices[i].co for i in poly.vertices]
        axis = max(range(3), key=lambda i: abs(normal[i]))
        if abs(normal[axis]) < 0.999:
            continue   # only faces square to the room: the rest are too few to matter here
        depth = round(points[0][axis] / 0.0004)
        a, b = [i for i in range(3) if i != axis]
        box = (min(p[a] for p in points), max(p[a] for p in points), min(p[b] for p in points), max(p[b] for p in points))
        key = (axis, 1 if normal[axis] > 0 else -1, depth)
        groups.setdefault(key, []).append((box, ob.name, names[poly.material_index] if names else "?", poly.index))
found = {}
for key, faces in groups.items():
    if len(faces) < 2 or len(faces) > 4000:
        continue
    for i in range(len(faces)):
        for j in range(i + 1, len(faces)):
            p, q = faces[i], faces[j]
            w = min(p[0][1], q[0][1]) - max(p[0][0], q[0][0]) - 0.002
            h = min(p[0][3], q[0][3]) - max(p[0][2], q[0][2]) - 0.002
            if w > 0 and h > 0 and w * h > 0.002:
                name = (p[1], p[2], q[1], q[2], "xyz"[key[0]], round(key[2] * 0.0004, 3))
                found[name] = max(found.get(name, 0), w * h)
for name, area in sorted(found.items(), key=lambda item: -item[1])[:40]:
    print("OVERLAP %6.0f cm2  %s [%s]  with  %s [%s]  in the plane %s = %.3f (Blender's axes)" % ((area * 10000,) + name))
print("OVERLAP pairs listed: %d" % len(found))
