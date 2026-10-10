# The shell's lightmap layout, checked: blender -b build/world.blend --python world/check_shell.py
# Every object of the Shell collection must have a second set of coordinates inside the unit
# square, with no two faces overlapping there, and one scale from metres to the layout.
import bpy


def main():
    bad = 0
    for ob in bpy.data.collections["Shell"].objects:
        mesh = ob.data
        if len(mesh.uv_layers) < 2:
            print("SHELL %s: no second coordinates" % ob.name)
            bad += 1
            continue
        uv = mesh.uv_layers[1].data
        rects, scales, area_uv = [], [], 0.0
        for poly in mesh.polygons:
            us = [uv[i].uv[0] for i in poly.loop_indices]
            vs = [uv[i].uv[1] for i in poly.loop_indices]
            rect = (min(us), min(vs), max(us), max(vs))
            rects.append(rect)
            here = (rect[2] - rect[0]) * (rect[3] - rect[1])
            area_uv += here
            if poly.area > 1e-9:
                scales.append((here / poly.area) ** 0.5)
        outside = [r for r in rects if r[0] < -1e-6 or r[1] < -1e-6 or r[2] > 1 + 1e-6 or r[3] > 1 + 1e-6]
        overlaps = 0
        for i in range(len(rects)):
            for j in range(i + 1, len(rects)):
                a, b = rects[i], rects[j]
                if min(a[2], b[2]) - max(a[0], b[0]) > 1e-6 and min(a[3], b[3]) - max(a[1], b[1]) > 1e-6:
                    overlaps += 1
        even = max(scales) - min(scales) < 1e-4 * max(scales)
        ok = not outside and not overlaps and even
        bad += not ok
        print("SHELL %-22s %3d faces, %4.1f%% of the square used, a metre is %.4f of it: %s" % (
            ob.name, len(rects), 100 * area_uv, sum(scales) / len(scales),
            "ok" if ok else "%d outside, %d overlap, scale %s" % (len(outside), overlaps, "even" if even else "uneven")))
    print("SHELL %s" % ("ok" if not bad else "%d objects wrong" % bad))


main()
