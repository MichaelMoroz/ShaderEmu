# Poly Haven's models (world/fetch.py downloads them) placed among ours. A model keeps its own
# material, renamed PH_<name>; Unity makes that material from the textures fetch.py wrote.
import os

import bmesh
import bpy
from mathutils import Matrix, Vector

from lib import C, MATERIALS, blender_material

CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "polyhaven")
_loaded = {}


def two_sided():
    """Materials seen from both sides: pictures with holes (kind 1 in fetch.py's list), and leaves."""
    listing = os.path.join(CACHE, "..", "..", "unity", "ShaderEmu", "Models", "materials.txt")
    names = set()
    for line in open(listing, encoding="utf-8"):
        fields = line.split("\t")
        if fields[1:2] == ["1"] or "leaves" in fields[0]:
            names.add(fields[0])
    return names


OURS = {"pc_station"}   # models we make (world/bake_pc.py): their parts are laid out against world/pc.py's, whose leads plug into them


GAP = 0.003   # metres: a gap in a model this small is its maker's slip, larger it is a thing apart


def seat_parts(mesh, asset, touch=0.002, far=0.05):
    """A model's parts that hang in it (a panel before its body, a button off its face) put against
    what holds them, as world/check_float.py wants every part held: the parts that touch (within
    `touch`) are a group, the one with the model's foot holds, and each group that touches none is
    lowered onto the nearest held part under it (`far` at most), till none moves. A group with
    nothing of the model under it is left where it is: it is a thing of its own (a cassette player's
    tape beside it), to be loaded apart with `parts` and placed loose, never pushed onto another; but
    a part within GAP of the held ones (a button a hair off its face: the maker's gap) is moved to them."""
    import bmesh
    from mathutils.bvhtree import BVHTree
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bm.verts.ensure_lookup_table()
    seen, islands = set(), []
    for v in bm.verts:
        if v.index in seen:
            continue
        todo, island = [v], []
        seen.add(v.index)
        while todo:
            u = todo.pop()
            island.append(u)
            for e in u.link_edges:
                w = e.other_vert(u)
                if w.index not in seen:
                    seen.add(w.index)
                    todo.append(w)
        if any(u.link_faces for u in island):
            islands.append(island)

    def tree(island):
        faces = {f for u in island for f in u.link_faces}
        index = {u.index: k for k, u in enumerate(island)}
        return BVHTree.FromPolygons([u.co.copy() for u in island], [[index[u.index] for u in f.verts] for f in faces])
    trees = [tree(i) for i in islands]
    boxes = [(Vector((min(u.co[k] for u in i) for k in range(3))), Vector((max(u.co[k] for u in i) for k in range(3)))) for i in islands]
    group = list(range(len(islands)))

    def find(i):
        while group[i] != i:
            group[i] = group[group[i]]
            i = group[i]
        return i

    def near(a, b):
        (alo, ahi), (blo, bhi) = boxes[a], boxes[b]
        if any(alo[k] > bhi[k] + touch or blo[k] > ahi[k] + touch for k in range(3)):
            return False
        if trees[a].overlap(trees[b]):
            return True
        return any(trees[b].find_nearest(u.co, touch)[0] is not None for u in islands[a]) or             any(trees[a].find_nearest(u.co, touch)[0] is not None for u in islands[b])
    order = sorted(range(len(islands)), key=lambda i: boxes[i][0].x)
    for k, i in enumerate(order):
        for j in order[k + 1:]:
            if boxes[j][0].x > boxes[i][1].x + touch:
                break
            if find(i) != find(j) and near(i, j):
                group[find(i)] = find(j)
    foot = min(boxes[i][0].z for i in range(len(islands)))
    held = {find(i) for i in range(len(islands)) if boxes[i][0].z <= foot + touch}
    down, lowered = Vector((0, 0, -1)), 0
    if os.environ.get("SEAT_TRACE"):
        roots = {find(i) for i in range(len(islands))}
        print("SEAT %s: %d parts in %d groups, %d held" % (asset, len(islands), len(roots), len(held)))
        for root in roots - held:
            members = [i for i in range(len(islands)) if find(i) == root]
            lo = Vector((min(boxes[i][0][k] for i in members) for k in range(3)))
            hi = Vector((max(boxes[i][1][k] for i in members) for k in range(3)))
            print("SEAT   free: %d parts, %s .. %s" % (len(members), tuple(round(x, 4) for x in lo), tuple(round(x, 4) for x in hi)))
    for _ in range(8):
        moved = False
        for root in {find(i) for i in range(len(islands))} - held:
            members = [i for i in range(len(islands)) if find(i) == root]
            others = [i for i in range(len(islands)) if find(i) in held]
            gap = None
            for i in members:
                for u in islands[i][::max(1, len(islands[i]) // 200)]:
                    for j in others:
                        lo, hi = boxes[j]
                        if not (lo.x - 0.001 <= u.co.x <= hi.x + 0.001 and lo.y - 0.001 <= u.co.y <= hi.y + 0.001) or lo.z > u.co.z:
                            continue
                        hit = trees[j].ray_cast(u.co + Vector((0, 0, 0.0005)), down, far)
                        if hit[0] is not None:
                            gap = hit[3] - 0.0005 if gap is None else min(gap, hit[3] - 0.0005)
            shift = None
            if gap is not None and gap > touch * 0.5:
                shift = Vector((0, 0, -(gap - 0.0002)))
            elif gap is None:   # nothing under it: a modelling gap (a button 2 mm off its face) is closed, never more
                best = None
                for i in members:
                    for u in islands[i][::max(1, len(islands[i]) // 200)]:
                        for j in others:
                            hit = trees[j].find_nearest(u.co, GAP)
                            if hit[0] is not None and (best is None or hit[3] < best[0]):
                                best = (hit[3], hit[0] - u.co)
                if best is not None:
                    shift = best[1] * (1 - 0.0002 / best[0]) if best[0] > touch * 0.5 else Vector()
            if shift is not None:
                if shift.length > 0:
                    for i in members:
                        for u in islands[i]:
                            u.co += shift
                        trees[i] = tree(islands[i])
                        boxes[i] = (boxes[i][0] + shift, boxes[i][1] + shift)
                    lowered += 1
                held.add(root)
                moved = True
            elif gap is not None:
                held.add(root)
                moved = True
        if not moved:
            break
    if lowered:
        bm.to_mesh(mesh)
        print("ASSET %s: %d parts put against what holds them" % (asset, lowered))
    bm.free()


def load(asset, limit=None, parts=None):
    """The asset as one mesh, with its bounds in Unity's axes. `parts` keeps only the objects
    whose names contain one of its words (a file may hold several plants side by side). `limit`
    cuts triangles, which only boxy things survive: it tore chairs and leaves apart."""
    key = (asset, parts)
    if key in _loaded:
        return _loaded[key]
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=os.path.join(CACHE, asset, asset + ".gltf"), merge_vertices=True)
    new = [o for o in bpy.data.objects if o not in before]
    # a word of `parts` is part of an object's name, or with = before it the whole name
    meshes = [o for o in new if o.type == 'MESH' and (parts is None or any(o.name == word[1:] if word[:1] == '=' else word in o.name for word in parts))]
    bpy.ops.object.select_all(action='DESELECT')
    for ob in meshes:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.object.parent_clear(type='CLEAR_KEEP_TRANSFORM')
    if len(meshes) > 1:
        bpy.ops.object.join()
    ob = bpy.context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    count = sum(len(p.vertices) - 2 for p in ob.data.polygons)
    if limit is not None and count > limit:
        modifier = ob.modifiers.new("Decimate", 'DECIMATE')
        modifier.ratio = limit / count
        bpy.ops.object.modifier_apply(modifier=modifier.name)
    mesh = ob.data
    if asset not in OURS:
        seat_parts(mesh, asset)
    for slot, material in enumerate(mesh.materials):
        if material.name.startswith("PH_"):
            continue
        own = material.name.rsplit(".", 1)[0] if material.name[-4:-3] == "." and material.name[-3:].isdigit() else material.name
        if own in MATERIALS:   # one of the room's own (a model world/bake_pc.py made): the room's material of that name
            mesh.materials[slot] = blender_material(own)
            continue
        name = "PH_" + material.name.rsplit(".", 1)[0] if material.name[-4:-3] == "." and material.name[-3:].isdigit() else "PH_" + material.name
        known = bpy.data.materials.get(name)   # a file imported twice brings its materials twice
        if known is not None:
            mesh.materials[slot] = known
        else:
            material.name = name
    both = two_sided()
    cards = [i for i, m in enumerate(mesh.materials) if m.name in both]
    if cards:   # Unity's Standard shader draws one side: a second face, turned over
        bm = bmesh.new()
        bm.from_mesh(mesh)
        faces = [f for f in bm.faces if f.material_index in cards]
        copies = bmesh.ops.duplicate(bm, geom=faces)["geom"]
        bmesh.ops.reverse_faces(bm, faces=[g for g in copies if isinstance(g, bmesh.types.BMFace)])
        bm.to_mesh(mesh)
        bm.free()
    mesh.name = asset if parts is None else asset + " " + parts[0]
    points = [C.inverted() @ v.co for v in mesh.vertices]   # Unity's axes
    low = Vector((min(p.x for p in points), min(p.y for p in points), min(p.z for p in points)))
    high = Vector((max(p.x for p in points), max(p.y for p in points), max(p.z for p in points)))
    print("ASSET %-34s %6d -> %6d triangles, %.2f x %.2f x %.2f m, materials %s"
          % (mesh.name, count, sum(len(p.vertices) - 2 for p in mesh.polygons), high.x - low.x, high.y - low.y, high.z - low.z,
             [m.name for m in mesh.materials]))
    for other in [o for o in bpy.data.objects if o not in before]:
        bpy.data.objects.remove(other, do_unlink=True)
    _loaded[key] = (mesh, low, high)
    return _loaded[key]


def spawn(name, asset, world, size, limit, parts, scale, collection):
    """An instance standing on `world` (a matrix in Unity's space). `size` is (x, y, z) in metres;
    None for a side keeps the model's shape, and all None its own size. With `scale` the file's
    own origin is kept instead (two parts of one file then meet as they did in it)."""
    mesh, low, high = load(asset, limit, parts)
    if scale is not None:
        ob = bpy.data.objects.new(name, mesh)
        ob.matrix_world = C @ world @ Matrix.Diagonal((scale, scale, scale, 1.0)) @ C.inverted()
        collection.objects.link(ob)
        return ob
    own = high - low
    given = [size[i] / own[i] for i in range(3) if size[i] is not None]
    even = sum(given) / len(given) if given else 1.0
    scale = [size[i] / own[i] if size[i] is not None and len(given) == 3 else even for i in range(3)]
    base = Vector(((low.x + high.x) / 2, low.y, (low.z + high.z) / 2))
    fit = Matrix.Diagonal((scale[0], scale[1], scale[2], 1.0)) @ Matrix.Translation(-base)
    ob = bpy.data.objects.new(name, mesh)
    ob.matrix_world = C @ world @ fit @ C.inverted()
    collection.objects.link(ob)
    return ob


def rest(thing, support):
    """Lets `thing` down on `support` (or lifts it out of it) till they touch: under every place of
    its footprint, 4 mm apart, its underside against the support's top. Neither is flat."""
    from mathutils.bvhtree import BVHTree
    trees = []
    for ob in (thing, support):
        mesh = ob.data
        points = [ob.matrix_world @ v.co for v in mesh.vertices]
        trees.append((BVHTree.FromPolygons(points, [tuple(p.vertices) for p in mesh.polygons]), points))
    (above, points), (below, _) = trees
    x0, x1 = min(p.x for p in points), max(p.x for p in points)
    y0, y1 = min(p.y for p in points), max(p.y for p in points)
    low, high = min(p.z for p in points) - 1.0, max(p.z for p in points) + 1.0
    gap, step = None, 0.004
    for i in range(int((x1 - x0) / step) + 1):
        for j in range(int((y1 - y0) / step) + 1):
            x, y = x0 + i * step, y0 + j * step
            under = above.ray_cast(Vector((x, y, low)), Vector((0, 0, 1)))[0]
            top = below.ray_cast(Vector((x, y, high)), Vector((0, 0, -1)))[0]
            if under is not None and top is not None and (gap is None or under.z - top.z < gap):
                gap = under.z - top.z
    if gap is not None:
        thing.matrix_world = Matrix.Translation((0, 0, -gap)) @ thing.matrix_world
        print("RESTS %s on %s: let down %.1f mm, its lowest point now at %.3f" % (thing.name, support.name, gap * 1000, min(p.z for p in points) - gap))


def spawn_hands(body, asset, hands, collection):
    """A clock's hands as objects of their own, for ClockHand.shader to turn: `hands` is the dial's
    middle in the file (Blender's axes, the dial facing -y) and then (name, part) pairs. A hand's
    origin is that middle and its mesh points at twelve, whatever time the file showed."""
    import math
    pivot = Vector(hands[0])
    made = []
    for label, part in hands[1:]:
        mesh = load(asset, None, ("=" + part,))[0].copy()
        tip = max((v.co - pivot for v in mesh.vertices), key=lambda p: p.x * p.x + p.z * p.z)
        turn = Matrix.Rotation(-math.atan2(tip.x, tip.z), 4, 'Y') @ Matrix.Translation(-pivot)
        mesh.transform(turn)
        up = max((v.co for v in mesh.vertices), key=lambda p: p.x * p.x + p.z * p.z)
        print("HAND %s %s points to (%.3f, %.3f) of the dial" % (asset, label, up.x, up.z))
        ob = bpy.data.objects.new("Clock hand %s %s" % (label, body.name), mesh)
        ob.matrix_world = body.matrix_world @ Matrix.Translation(pivot)
        collection.objects.link(ob)
        made.append(ob)
    return made
