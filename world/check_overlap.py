# Faces that fight and parts that go through each other, in the whole built world:
#   blender -b build/world.blend --python world/check_overlap.py -- [--all] [--list N] [WORD ...]
# (world/build.py runs it after a whole build, and says what it found.)
# Two kinds of fault, between two parts (a part is a piece of one of our meshes that hangs
# together, or the whole of one of Poly Haven's models, whose own pieces are its makers'):
#   FIGHT  two triangles in one plane (within PLANE), facing one way, overlapping by more than
#          FIGHT_DEPTH inside it: they flicker (z-fighting).
#   CROSS  two triangles that pass through each other, the line where they cross longer than
#          CROSS_LONG, each with corners more than CROSS_DEEP on both sides of the other's plane:
#          what touches (a box on a table, two bars end to end) is not a crossing.
# The broad phase is a grid of cells in levels (a triangle goes into the level whose cells are as
# large as it, where it lies in 8 cells at most) and sorted keys, so the work grows as N log N
# with the triangles; the tests are numpy's over all candidate pairs at once.
# WORDS limit it to objects whose names have one of them. Crossings that are made so on purpose
# (a handle into its mug, a lead into its plug) are in world/overlap_allowed.txt: object-name
# words of both parts and why. It prints every fault, the two objects, their parts' places and
# the place where they meet (Unity's coordinates), and exits 1 if any is left.
import math
import os
import sys
import time

import bpy
import numpy as np

PLANE = 0.0002          # metres off one plane that still fight (a decal stands 0.3 to 0.4 mm proud)
FACING = 0.9995         # cosine between two normals that face one way
FIGHT_DEPTH = 0.0005    # how far two fighting faces must overlap within their plane
CROSS_DEEP = 0.0002     # how far a corner must be through the other's plane
CROSS_LONG = 0.001      # how long a crossing must be
ROW = 3                 # a long triangle lies in a row of 2^ROW cells or fewer
BASE = 0.01             # the finest cells' size
SKIP = ("Rain", "Holodeck mask")   # streaks that overlap on purpose, drawn without depth; the mask, which Unity moves to the room in use
HERE = os.path.dirname(os.path.abspath(__file__))


def allowed_pairs():
    path = os.path.join(HERE, "overlap_allowed.txt")
    rules = []
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if line:
                kind, a, b = [part.strip() for part in line.split("|")[:3]]
                rules.append((kind, a, b))
    return rules


def gather(words, everything):
    """All triangles in world coordinates, each with its part, its object and its own normal."""
    graph = bpy.context.evaluated_depsgraph_get()
    points, owners, part_object, objects, part_kind = [], [], [], [], []
    for ob in sorted(bpy.data.objects, key=lambda o: o.name):
        if ob.type != 'MESH' or (not everything and ob.name.startswith(SKIP)):
            continue
        if words and not any(word in ob.name for word in words):
            continue
        evaluated = ob.evaluated_get(graph)
        mesh = evaluated.to_mesh()
        mesh.calc_loop_triangles()
        nv, nt = len(mesh.vertices), len(mesh.loop_triangles)
        if nt == 0:
            evaluated.to_mesh_clear()
            continue
        co = np.empty(nv * 3, np.float64)
        mesh.vertices.foreach_get("co", co)
        tri = np.empty(nt * 3, np.int64)
        mesh.loop_triangles.foreach_get("vertices", tri)
        names = [m.name if m else "" for m in mesh.materials]
        evaluated.to_mesh_clear()
        co, tri = co.reshape(-1, 3), tri.reshape(-1, 3)
        world = np.array(ob.matrix_world)
        co = co @ world[:3, :3].T + world[:3, 3]
        co = np.stack([-co[:, 0], co[:, 2], -co[:, 1]], 1)   # Blender's (x, y, z) is Unity's (-x, z, -y)
        index = len(objects)
        objects.append(ob.name)
        whole = bool(names) and all(name.startswith("PH_") for name in names)   # one of Poly Haven's: one part
        if whole:
            label = np.zeros(nv, np.int64)
        else:   # the pieces that hang together, by their shared vertices
            label = np.arange(nv)
            while True:
                low = label[tri].min(1)
                before = label.copy()
                np.minimum.at(label, tri.ravel(), np.repeat(low, 3))
                label = label[label]
                label = label[label]
                if np.array_equal(label, before):
                    break
            _, label = np.unique(label, return_inverse=True)
        first = len(part_object)
        count = int(label.max()) + 1
        part_object.extend([index] * count)
        part_kind.extend([whole] * count)
        points.append(co[tri])
        owners.append(first + label[tri[:, 0]])
    return np.concatenate(points), np.concatenate(owners), np.array(part_object), objects


joined = [0]


def candidates(lo, hi, owner, chunk=4_000_000):
    """Pairs of triangles of different parts whose boxes meet, from a grid in levels. A triangle
    goes into the level whose cells are as large as its box's middle side, and into every cell
    of that level its box meets (a long thin one into a row of them, not into a cell as long as
    it); it asks the cells of every coarser level it meets for what is there."""
    sides = np.sort(hi - lo, 1)

    def level_of(extent):
        return np.maximum(0, np.ceil(np.log2(np.maximum(extent, 1e-9) / BASE))).astype(np.int64)
    level = np.maximum(level_of(sides[:, 1]), level_of(sides[:, 2]) - ROW)
    pairs = []
    joined[0] = 0

    def cells(which, size):
        a = np.floor(lo[which] / size).astype(np.int64)
        n = np.floor(hi[which] / size).astype(np.int64) - a + 1
        count = n.prod(1)
        rows = np.repeat(np.arange(len(which)), count)
        k = np.arange(count.sum()) - np.repeat(np.cumsum(count) - count, count)
        nx, ny = n[rows, 0], n[rows, 1]
        c = a[rows] + np.stack([k % nx, (k // nx) % ny, k // (nx * ny)], 1) + (1 << 20)
        return (c[:, 0] << 42) | (c[:, 1] << 21) | c[:, 2], which[rows]
    clock = time.time()
    for top in np.unique(level):
        size = BASE * 2.0 ** top
        key_in, tri_in = cells(np.nonzero(level == top)[0], size)
        key_ask, tri_ask = cells(np.nonzero(level <= top)[0], size)
        # a cell that holds one part alone has nothing to pair: most cells along a long bar's own triangles
        keys = np.concatenate([key_in, key_ask])
        owners = owner[np.concatenate([tri_in, tri_ask])]
        order = np.lexsort((owners, keys))
        keys, owners = keys[order], owners[order]
        group = np.r_[True, keys[1:] != keys[:-1]]
        first = np.maximum.accumulate(np.where(group, np.arange(len(keys)), 0))
        mixed = keys[owners != owners[first]]
        key_ask, tri_ask = key_ask[np.isin(key_ask, mixed)], tri_ask[np.isin(key_ask, mixed)]
        keep = np.isin(key_in, mixed)
        key_in, tri_in = key_in[keep], tri_in[keep]
        order = np.argsort(key_in, kind="stable")
        key_in, tri_in = key_in[order], tri_in[order]
        start = np.searchsorted(key_in, key_ask, "left")
        many = np.searchsorted(key_in, key_ask, "right") - start
        has = many > 0
        tri_ask, start, many = tri_ask[has], start[has], many[has]
        total = np.cumsum(many)
        if os.environ.get("OVERLAP_DEBUG"):
            print("level", top, "%.2f s" % (time.time() - clock), "in", len(key_in), "ask", len(key_ask), "joined", int(many.sum()))
        begin = 0
        while begin < len(many):   # in pieces, so that a crowded cell does not take all memory
            end = max(int(np.searchsorted(total, (total[begin - 1] if begin else 0) + chunk, "right")), begin + 1)
            m = many[begin:end]
            joined[0] += int(m.sum())
            a = np.repeat(tri_ask[begin:end], m)
            step = np.arange(m.sum()) - np.repeat(np.cumsum(m) - m, m)
            b = tri_in[np.repeat(start[begin:end], m) + step]
            # a pair is found by the finer of its two (or by either at one level: then once, a < b)
            keep = (owner[a] != owner[b]) & ((level[a] < top) | (a < b))
            a, b = a[keep], b[keep]
            keep = np.all(lo[a] <= hi[b], 1) & np.all(lo[b] <= hi[a], 1)
            pairs.append(np.stack([a[keep], b[keep]], 1))
            begin = end
    pairs = np.concatenate(pairs) if pairs else np.zeros((0, 2), np.int64)
    code = np.unique(pairs.min(1) * len(lo) + pairs.max(1))
    return np.stack([code // len(lo), code % len(lo)], 1)


def in_plane(tri, normal, a, b):
    """How far a and b overlap within a's plane (the least overlap along the six edges' normals
    there: the separating axes of two triangles in one plane)."""
    n = normal[a]
    depth = np.full(len(a), np.inf)
    if len(a) == 0:
        return depth
    for t in (tri[a], tri[b]):
        for e in range(3):
            edge = t[:, (e + 1) % 3] - t[:, e]
            axis = np.cross(n, edge)
            axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-12)
            pa = np.einsum("ijk,ik->ij", tri[a], axis)
            pb = np.einsum("ijk,ik->ij", tri[b], axis)
            depth = np.minimum(depth, np.minimum(pa.max(1), pb.max(1)) - np.maximum(pa.min(1), pb.min(1)))
    return depth


def fights(tri, normal, a, b):
    """Of pairs a, b: those in one plane facing one way, and how far they overlap in it. A pair
    whose two faces both lie against a third part's face that looks the other way (two feet on
    the floor, two backs on a wall) is pressed out of sight and is left out."""
    facing = np.einsum("ij,ij->i", normal[a], normal[b])
    off = np.abs(np.einsum("ijk,ik->ij", tri[b] - tri[a][:, :1], normal[a])).max(1)
    flat = off < PLANE
    same, back = flat & (facing > FACING), flat & (facing < -FACING)
    backed = np.zeros(len(tri), bool)
    ba, bb = a[back], b[back]
    against = in_plane(tri, normal, ba, bb) > FIGHT_DEPTH
    backed[ba[against]] = True
    backed[bb[against]] = True
    a, b = a[same], b[same]
    if len(a) == 0:
        return a, b, np.zeros(0)
    depth = in_plane(tri, normal, a, b)
    keep = (depth > FIGHT_DEPTH) & ~(backed[a] & backed[b])
    return a[keep], b[keep], depth[keep]


def crossings(tri, normal, a, b):
    """Of pairs a, b: those that pass through each other, and how long the line of it is."""
    da = np.einsum("ijk,ik->ij", tri[a] - tri[b][:, :1], normal[b])   # a's corners off b's plane
    db = np.einsum("ijk,ik->ij", tri[b] - tri[a][:, :1], normal[a])
    through = (da.min(1) < -CROSS_DEEP) & (da.max(1) > CROSS_DEEP) & (db.min(1) < -CROSS_DEEP) & (db.max(1) > CROSS_DEEP)
    a, b, da, db = a[through], b[through], da[through], db[through]
    if len(a) == 0:
        return a, b, np.zeros(0), np.zeros((0, 3))
    line = np.cross(normal[a], normal[b])
    line /= np.maximum(np.linalg.norm(line, axis=1, keepdims=True), 1e-12)

    def span(t, d):
        """Where triangle t (its corners d off the other's plane) meets that plane, along the line."""
        p = np.einsum("ijk,ik->ij", t, line)
        low, high = np.full(len(t), np.inf), np.full(len(t), -np.inf)
        for i in range(3):
            j = (i + 1) % 3
            cut = d[:, i] * d[:, j] < 0
            s = d[:, i] / np.where(cut, d[:, i] - d[:, j], 1)
            q = np.where(cut, p[:, i] + s * (p[:, j] - p[:, i]), np.nan)
            on = d[:, i] == 0
            q = np.where(on, p[:, i], q)
            low = np.fmin(low, q)
            high = np.fmax(high, q)
        return low, high
    la, ha = span(tri[a], da)
    lb, hb = span(tri[b], db)
    long = np.minimum(ha, hb) - np.maximum(la, lb)
    keep = long > CROSS_LONG
    centre = (tri[a].mean(1) + tri[b].mean(1)) / 2
    return a[keep], b[keep], long[keep], centre[keep]


def main(args=None, leave=True):
    """The check; `leave`: in Blender without a window, exit 1 if anything is left, 0 if not."""
    if args is None:
        args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    everything = "--all" in args
    listed = 60
    if "--list" in args:
        listed = int(args[args.index("--list") + 1])
        args = args[:args.index("--list")] + args[args.index("--list") + 2:]
    words = [w for w in args if not w.startswith("--")]
    start = time.time()
    tri, owner, part_object, objects = gather(words, everything)
    edge1, edge2 = tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]
    normal = np.cross(edge1, edge2)
    area = np.linalg.norm(normal, axis=1)
    good = area > 1e-10    # (twice the area: a millimetre's sliver of 0.1 mm is 2e-7)
    normal = normal / np.maximum(area, 1e-30)[:, None]
    lo, hi = tri.min(1) - PLANE, tri.max(1) + PLANE
    keep = np.nonzero(good)[0]
    gathered = time.time()
    pairs = candidates(lo[keep], hi[keep], owner[keep])
    pairs = keep[pairs]
    found = time.time()
    a, b = pairs[:, 0], pairs[:, 1]
    fa, fb, depth = fights(tri, normal, a, b)
    ca, cb, long, centre = crossings(tri, normal, a, b)
    tested = time.time()
    rules = allowed_pairs()

    def name(part):
        return objects[part_object[part]]

    def allowed(kind, pa, pb):
        na, nb = name(pa), name(pb)
        return any(k == kind and ((x in na and y in nb) or (x in nb and y in na)) for k, x, y in rules)

    def where(t):
        return tri[t].mean(0)
    faults = []
    for kind, xs, ys, sizes in (("FIGHT", fa, fb, depth), ("CROSS", ca, cb, long)):
        worst = {}
        for x, y, size in zip(xs, ys, sizes):
            key = (min(owner[x], owner[y]), max(owner[x], owner[y]))
            if key not in worst or size > worst[key][0]:
                worst[key] = (size, x, y, worst.get(key, (0, 0, 0, 0))[3] + 1)
            else:
                worst[key] = worst[key][:3] + (worst[key][3] + 1,)
        for (pa, pb), (size, x, y, count) in worst.items():
            if not allowed(kind, pa, pb):
                faults.append((kind, size, pa, pb, x, y, count))
    faults.sort(key=lambda f: (f[0], -f[1]))
    # each part's box, to say which piece of an object it is
    box_lo = np.full((len(part_object), 3), np.inf)
    box_hi = np.full((len(part_object), 3), -np.inf)
    np.minimum.at(box_lo, owner, tri.min(1))
    np.maximum.at(box_hi, owner, tri.max(1))
    for i, (kind, size, pa, pb, x, y, count) in enumerate(faults):
        if i == listed:
            print("OVERLAP ... and %d more" % (len(faults) - listed))
            break
        place = (where(x) + where(y)) / 2

        def part(p):
            mid, ext = (box_lo[p] + box_hi[p]) / 2, box_hi[p] - box_lo[p]
            return "%s [part at (%.3f, %.3f, %.3f), %.0f x %.0f x %.0f mm]" % (name(p), *mid, *(ext * 1000))
        print("OVERLAP %s %5.1f mm at (%.3f, %.3f, %.3f): %s  with  %s  (%d triangle pairs)" % (
            kind, size * 1000, *place, part(pa), part(pb), count))
    fights_left = sum(1 for f in faults if f[0] == "FIGHT")
    print("OVERLAP %d triangles of %d objects in %d parts, %d pairs of boxes; %.1f s to read, %.1f s to pair, %.1f s to test" % (
        len(tri), len(objects), len(part_object), len(pairs), gathered - start, found - gathered, tested - found))
    print("OVERLAP %d box pairs joined in the grid" % joined[0])
    print("OVERLAP %d faults: %d fighting, %d crossing" % (len(faults), fights_left, len(faults) - fights_left))
    sys.stdout.flush()
    if leave and bpy.app.background:
        os._exit(1 if faults else 0)
    return faults


if __name__ == "__main__":
    main()
