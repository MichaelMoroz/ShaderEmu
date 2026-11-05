# Things that hang in the air: a knob off its drawer, a shade off its lamp, a box over a shelf. Every
# object of the last build is cut into its pieces (world/check_intersect.py's), pieces within TOUCH of
# each other are joined, and each group of joined pieces must reach something that holds it: the
# floor (a piece down to FLOOR), the shell (the parts Room and Shell, which are the walls, floors and
# ceilings) or a piece as large as a wall. A group that reaches none is listed, its objects and where:
#   blender -b build/world.blend --python world/check_float.py -- [WORD ...] [--all] [--near]
# WORD: only groups with an object whose name has one of them. --all: every group, not the first 40.
# --near: what each group is nearest to and how far (slow: with a WORD).
# FLOATING pieces are meant to hang (the city's lights, the rain, a holodeck's mask).
import os
import sys

import bpy
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_intersect import SKIP, pieces, tree   # noqa: E402

TOUCH = 0.002            # metres apart that still touch: a decal on its wall, a thing let down on another
FLOOR = 0.01             # a piece whose foot is this near y = 0 stands on the floor (a rug's pile, a mat)
LARGE = 2.5              # a piece whose box is this long across is a wall, a floor or a ceiling
SHELL = ("Room", "Shell")   # the parts that hold everything
FLOATING = ()            # words of objects' names that may hang free


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    every, near = "--all" in args, "--near" in args
    words = [a for a in args if not a.startswith("--")]
    objects = [o for o in bpy.data.objects if o.type == 'MESH' and not o.name.startswith(SKIP) and len(o.data.polygons)
               and not any(w in o.name for w in FLOATING)]
    parts, held = [], []
    for ob in objects:
        shell = any(c.name in SHELL for c in ob.users_collection)
        for piece in pieces(ob):
            parts.append((ob.name, piece))
            lo, hi = piece[1], piece[2]
            held.append(shell or lo.y <= FLOOR or (hi - lo).length >= LARGE)
    print("FLOAT %d objects, %d pieces" % (len(objects), len(parts)))
    group = list(range(len(parts)))

    def find(i):
        while group[i] != i:
            group[i] = group[group[i]]
            i = group[i]
        return i
    order = sorted(range(len(parts)), key=lambda i: parts[i][1][1].x)
    trees = {}

    def tree_of(i):
        if i not in trees:
            trees[i] = tree(parts[i][1][0])
        return trees[i]

    def touch(i, j):
        if tree_of(i).overlap(tree_of(j)):
            return True
        for near, far in ((i, j), (j, i)):   # each one's corners against the other's faces (a poster's against its wall's)
            lo, hi = parts[far][1][1] - Vector((TOUCH,) * 3), parts[far][1][2] + Vector((TOUCH,) * 3)
            other = tree_of(far)
            for t in parts[near][1][0]:
                for p in (*t, (t[0] + t[1] + t[2]) / 3):   # (its corners and its middle: two faces flat on each other may have no corner near the other)
                    if lo.x <= p.x <= hi.x and lo.y <= p.y <= hi.y and lo.z <= p.z <= hi.z and other.find_nearest(p, TOUCH)[0] is not None:
                        return True
        return False
    for k, i in enumerate(order):
        _, (_, lo_i, hi_i) = parts[i]
        for j in order[k + 1:]:
            _, (_, lo_j, hi_j) = parts[j]
            if lo_j.x > hi_i.x + TOUCH:
                break
            if lo_j.y > hi_i.y + TOUCH or lo_i.y > hi_j.y + TOUCH or lo_j.z > hi_i.z + TOUCH or lo_i.z > hi_j.z + TOUCH:
                continue
            a, b = find(i), find(j)
            if a == b or (held[a] and held[b]):
                continue
            if touch(i, j):
                group[a] = b
                held[b] = held[b] or held[a]
    groups = {}
    for i in range(len(parts)):
        groups.setdefault(find(i), []).append(i)
    free = []
    for root, members in groups.items():
        if held[root]:
            continue
        names = sorted({parts[i][0] for i in members})
        if words and not any(w in n for n in names for w in words):
            continue
        lo = Vector((min(parts[i][1][1][k] for i in members) for k in range(3)))
        hi = Vector((max(parts[i][1][2][k] for i in members) for k in range(3)))
        free.append((lo, hi, names, len(members), members))
    free.sort(key=lambda f: -(f[1] - f[0]).length)
    for lo, hi, names, n, members in free if every else free[:40]:
        middle = (lo + hi) / 2
        print("FLOAT %s: %d pieces, %.0f mm across, its foot at %.3f m, at (%.2f, %.2f, %.2f)"
              % (", ".join(names[:4]) + (" ..." if len(names) > 4 else ""), n, (hi - lo).length * 1000, lo.y, middle.x, middle.y, middle.z))
        if near:   # what the group is nearest to, outside it, and how far
            best = None
            mine = set(members)
            for j in range(len(parts)):
                if j in mine:
                    continue
                a, b = parts[j][1][1], parts[j][1][2]
                if any(a[k] > hi[k] + 0.1 or b[k] < lo[k] - 0.1 for k in range(3)):
                    continue
                other = tree_of(j)
                for i in members:
                    for t in parts[i][1][0]:
                        for p in t:
                            hit = other.find_nearest(p, 0.1)
                            if hit[0] is not None and (best is None or hit[3] < best[0]):
                                best = (hit[3], parts[j][0], p, held[find(j)])
            if best:
                print("      nearest: %s (%s) %.1f mm away, from (%.3f, %.3f, %.3f)" % (best[1], "held" if best[3] else "not held", best[0] * 1000, *best[2]))
    print("FLOAT groups: %d" % len(free))


main()
