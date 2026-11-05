# Stretched textures: a piece whose texture coordinates do not lay the picture evenly on its surface.
# For each face, the area it has in the room over the area it has in the picture (the square of the
# texture's scale there: how many square metres one square of the picture covers) should be about
# the same all over one piece (the faces joined by edges, of one material): where it is not, the
# picture is drawn larger on some faces than on others (a lathe's rings pinched round its axis, a
# box face mapped with another's scale). Each piece's faces are weighed by area against the piece's
# median; a face more than STRETCH times off in scale (STRETCH squared in area), stretched more than
# STRETCH times one way than the other (sheared or squeezed: the same area, a weave drawn out; in the
# picture's pixels, so a picture that is not square counts as it is drawn), or whose picture has
# no area (collapsed: a smear), counts, and a piece with more than SHARE of its area so is listed.
# And a seam across a smooth surface (two faces within SMOOTH of flat) where the picture turns more than TURNED
# or changes its size shows as a line: an object with more than SEAMS of them is listed too.
# A material with no picture (a plain colour: Rubber, the cables) shows no stretch and is not looked at,
# nor is a triangle narrower than SLIVER (no texel's width):
#   blender -b build/world.blend --python world/check_uv.py -- [WORD ...] [--all] [--assets]
# WORD: only objects whose name has one of them. --all: every piece, not the first 60. Poly Haven's
# models are their makers' (--assets shows them). The second coordinates (the lightmap's) are
# world/check_shell.py's and check_charts.py's.
import math
import sys

import bmesh
import bpy

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from check_intersect import SKIP   # noqa: E402

STRETCH = 1.5        # times off the piece's own scale, along a side, that shows as a stretched picture
SHARE = 0.05         # of a piece's area so stretched
SMALL = 4e-6         # square metres: a piece smaller than this (2 x 2 mm) is not looked at
SEAM = 1e-5         # texture coordinates nearer than this are one
COLLAPSED = 1e-12    # a face's picture area under this is a smear
SLIVER = 0.0002      # metres: a triangle no higher than this (a texel of a 0.3 m picture of 1024) shows no stretch
SMOOTH = 25.0        # degrees: two faces meeting within this of flat are one smooth surface, where a seam shows
TURNED = 10.0        # degrees the picture turns across such a seam (or 1.15 times its scale) that shows as a seam
SEAMS = 0.02         # metres of such seams an object may have (a texel's slip here and there)


def aspect_of(mat):
    """Its picture's width over its height (1 with none): a square of the coordinates is so many
    pixels across for one up."""
    if mat and mat.node_tree:
        for node in mat.node_tree.nodes:
            if node.type == 'TEX_IMAGE' and node.image and node.image.size[1]:
                return node.image.size[0] / node.image.size[1]
    return 1.0


def has_picture(mat):
    """Whether the material draws a picture (any image in it): a plain colour cannot be stretched."""
    return bool(mat and mat.node_tree and any(node.type == 'TEX_IMAGE' and node.image for node in mat.node_tree.nodes))


def anisotropy(face, uv, aspect):
    """How much more the picture is stretched one way than the other on the face, at worst over its
    triangles (a fan): the larger singular value of the map from the picture's pixels to the
    surface over the smaller. 1 is even; a sheared or squeezed weave is more."""
    loops = face.loops
    worst = 1.0
    for k in range(1, len(loops) - 1):
        a, b, c = loops[0], loops[k], loops[k + 1]
        e1, e2 = b.vert.co - a.vert.co, c.vert.co - a.vert.co
        d1 = ((b[uv].uv.x - a[uv].uv.x) * aspect, b[uv].uv.y - a[uv].uv.y)
        d2 = ((c[uv].uv.x - a[uv].uv.x) * aspect, c[uv].uv.y - a[uv].uv.y)
        det = d1[0] * d2[1] - d1[1] * d2[0]
        twice = e1.cross(e2).length
        if abs(det) < 1e-14 or twice < 1e-12:
            continue
        if twice / max(e1.length, e2.length, (e2 - e1).length) < SLIVER:   # (narrower than a texel: no stretch shows on it)
            continue
        # J = [e1 e2] inverse([d1 d2]): its columns are the surface's change along the picture's u and v
        ju = (e1 * d2[1] - e2 * d1[1]) / det
        jv = (e2 * d1[0] - e1 * d2[0]) / det
        g11, g12, g22 = ju.dot(ju), ju.dot(jv), jv.dot(jv)
        mean, root = (g11 + g22) / 2, math.sqrt(max(((g11 - g22) / 2) ** 2 + g12 * g12, 0.0))
        low = mean - root
        if low <= 0:
            continue
        worst = max(worst, math.sqrt((mean + root) / low))
    return worst


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    every, assets = "--all" in args, "--assets" in args
    words = [a for a in args if not a.startswith("--")]
    found, seams = [], []
    for ob in bpy.data.objects:
        if ob.type != 'MESH' or ob.name.startswith(SKIP) or not len(ob.data.polygons):
            continue
        if ("asset" in ob and not assets) or (words and not any(w in ob.name for w in words)):
            continue
        if not ob.data.uv_layers:
            continue
        bm = bmesh.new()
        bm.from_mesh(ob.data)
        bm.transform(ob.matrix_world)
        uv = bm.loops.layers.uv[0]   # (the first coordinates: the picture's)
        names = [m.name if m else "?" for m in ob.data.materials]
        aspects = [aspect_of(m) for m in ob.data.materials]
        pictured = [has_picture(m) for m in ob.data.materials]
        turned = 0.0   # turned seams: the picture changes its way or its size across a smooth surface (a cylinder's own seam goes on as it was)
        for e in bm.edges:
            if len(e.link_loops) != 2 or e.calc_length() < 1e-5:
                continue
            l1, l2 = e.link_loops
            f1, f2 = l1.face, l2.face
            if f1.material_index != f2.material_index or f1.material_index >= len(pictured) or not pictured[f1.material_index]:
                continue
            if math.degrees(f1.normal.angle(f2.normal, 0.0)) > SMOOTH:
                continue
            a1, b1 = l1[uv].uv, l1.link_loop_next[uv].uv          # the edge from l1's vertex to the next, on each side
            if l2.vert == l1.vert:
                a2, b2 = l2[uv].uv, l2.link_loop_next[uv].uv
            else:
                a2, b2 = l2.link_loop_next[uv].uv, l2[uv].uv
            if (a1 - a2).length < SEAM and (b1 - b2).length < SEAM:
                continue   # (no seam)
            asp = aspects[f1.material_index]
            d1, d2 = ((b1 - a1).x * asp, (b1 - a1).y), ((b2 - a2).x * asp, (b2 - a2).y)
            n1, n2 = math.hypot(*d1), math.hypot(*d2)
            if n1 < 1e-9 or n2 < 1e-9:
                continue
            turn = math.degrees(math.acos(max(-1.0, min(1.0, (d1[0] * d2[0] + d1[1] * d2[1]) / (n1 * n2)))))
            if turn > TURNED or max(n1, n2) / min(n1, n2) > 1.15:
                turned += e.calc_length()
        if turned > SEAMS:
            seams.append((turned, ob.name))
        seen = set()
        for face in bm.faces:
            if face.index in seen:
                continue
            todo, island = [face], []
            seen.add(face.index)
            while todo:
                f = todo.pop()
                island.append(f)
                for l in f.loops:   # (across an edge only where the picture goes on: both its ends at the same coordinates)
                    o = l.link_loop_radial_next
                    g = o.face
                    if g is f or g.index in seen or g.material_index != f.material_index:
                        continue
                    if (l[uv].uv - o.link_loop_next[uv].uv).length < SEAM and (l.link_loop_next[uv].uv - o[uv].uv).length < SEAM:
                        seen.add(g.index)
                        todo.append(g)
            areas, scales, smeared, sheared, most = [], [], 0.0, 0.0, 1.0
            if island[0].material_index < len(pictured) and not pictured[island[0].material_index]:
                continue
            aspect = aspects[island[0].material_index] if island[0].material_index < len(aspects) else 1.0
            for f in island:
                a = f.calc_area()
                if a <= 0:
                    continue
                p = [l[uv].uv for l in f.loops]
                t = abs(sum(p[i - 1].x * p[i].y - p[i].x * p[i - 1].y for i in range(len(p)))) / 2
                if t < COLLAPSED:
                    smeared += a
                    continue
                areas.append(a)
                scales.append(a / t)
                skew = anisotropy(f, uv, aspect)
                most = max(most, skew)
                if skew > STRETCH:
                    sheared += a
            total = sum(areas) + smeared
            if total < SMALL:
                continue
            if not areas:
                found.append((1.0, ob.name, names[island[0].material_index] if names else "?", total, 0.0, island[0].calc_center_median()))
                continue
            order = sorted(range(len(areas)), key=lambda k: scales[k])
            half, run, median = sum(areas) / 2, 0.0, scales[order[-1]]
            for k in order:
                run += areas[k]
                if run >= half:
                    median = scales[k]
                    break
            off = smeared + sheared + sum(a for a, s in zip(areas, scales) if s > median * STRETCH ** 2 or s < median / STRETCH ** 2)
            if off / total > SHARE:
                worst = max(max(s / median, median / s) for s in scales)
                found.append((off / total, ob.name, names[island[0].material_index] if names else "?", total, max(math.sqrt(worst), most),
                              island[0].calc_center_median()))
        bm.free()
    found.sort(key=lambda f: -f[0] * f[3])
    for share, name, mat, total, worst, at in found if every else found[:60]:
        print("UV %s (%s): %.0f%% of its %.0f cm2 stretched, the worst %s, at (%.3f, %.3f, %.3f)"
              % (name, mat, share * 100, total * 1e4, "%.1f times (in scale, or one way against the other)" % worst if worst else "smeared", -at.x, at.z, -at.y))
    print("UV stretched pieces: %d" % len(found))
    seams.sort(reverse=True)
    for length, name in seams if every else seams[:30]:
        print("UV seam %s: %.0f cm of seams where the picture turns or changes size across a smooth surface" % (name, length * 100))
    print("UV objects with turned seams: %d" % len(seams))


if __name__ == "__main__":
    main()
