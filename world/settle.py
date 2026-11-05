# Loose things fall into place (lib.Builder.loose, asset(loose=True)): a toy, a box lying, a
# magazine, a ball. When a part is finished each is a rigid body of its own (its convex hull) let
# go 1.5 mm over where it was put; what else the part has, and the walls it names (Builder.wall)
# and the floor at y = 0, stand still under it (their meshes). Blender's Bullet runs 2 s of it, and
# where each came to rest is kept. A thing that moved far or turned much is said (SETTLE far):
# it was put where it could not lie. The rests are kept in build/settle/<part>.json under a sum of
# everything the fall saw, so a build that changed nothing near them does not fall again
# (SETTLE_AGAIN=1: it falls anyway).
import hashlib
import json
import math
import os

import bmesh
import bpy
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "..", "build", "settle")
FRAMES, LIFT, NEAR = 120, 0.0015, 0.4    # 2 s at 60 a second; metres let go over the place; colliders this near a thing
FAR, TURNED = 0.05, 20.0                 # a fall this long or a turn this large is said
C = Matrix(((-1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))   # Unity -> Blender (lib.C)


def box_of(points):
    lo = Vector((min(p.x for p in points), min(p.y for p in points), min(p.z for p in points)))
    hi = Vector((max(p.x for p in points), max(p.y for p in points), max(p.z for p in points)))
    return lo, hi


def object_from(name, bm, collection):
    mesh = bpy.data.meshes.new(name)
    copy = bm.copy()
    copy.to_mesh(mesh)
    copy.free()
    ob = bpy.data.objects.new(name, mesh)
    collection.objects.link(ob)
    return ob


def box_object(name, matrix, size, collection):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    bmesh.ops.transform(bm, matrix=C @ matrix, verts=bm.verts)
    ob = object_from(name, bm, collection)
    bm.free()
    return ob


def centre(mesh):
    """The middle of a mesh's surface, each face by its area."""
    total, at = 0.0, Vector()
    for p in mesh.polygons:
        total += p.area
        at += p.center * p.area
    return at / total if total > 0 else Vector()


def world_box(ob):
    points = [ob.matrix_world @ Vector(c) for c in ob.bound_box]
    return box_of(points)


def summed(obs):
    """A sum of what the fall sees of these objects: their places, sizes and shapes, to a tenth of a millimetre."""
    h = hashlib.sha1()
    for ob in obs:
        lo, hi = world_box(ob)
        h.update(("%s|%d|%d|" % (ob.data.name.split(".")[0], len(ob.data.vertices), len(ob.data.polygons))).encode())
        h.update(("%.4f %.4f %.4f %.4f %.4f %.4f|" % (*lo, *hi)).encode())
        h.update(("%.4f|" % sum(v.co.x * 3 + v.co.y * 5 + v.co.z * 7 for v in ob.data.vertices)).encode())
        h.update(" ".join("%.4f" % x for row in ob.matrix_world for x in row).encode())
    return h.hexdigest()


def settle(part, objects, items, standing, falling, walls):
    """objects: the part's (bmesh, names) by name; items: lib.Builder.items; standing: its assets
    that do not fall; falling: those that do (moved here in place); walls: (matrix, size) boxes in
    Unity's space. Gives each item's move (a Blender matrix to put to its bmesh)."""
    scratch = bpy.data.collections.new("Settle " + part)
    bpy.context.scene.collection.children.link(scratch)
    things = [object_from("Settle item %d" % i, bm, scratch) for i, (_, bm, _) in enumerate(items)]
    for ob in falling:   # (an asset falls as a copy of its mesh where it stands)
        mesh = ob.data.copy()
        mesh.transform(ob.matrix_world)
        copy = bpy.data.objects.new("Settle " + ob.name, mesh)
        scratch.objects.link(copy)
        things.append(copy)
    for ob in things:   # (a body turns about its origin, its mass's middle: of its surface, as the meshes are not all closed)
        middle = centre(ob.data)
        ob.data.transform(Matrix.Translation(-middle))
        ob.matrix_world = Matrix.Translation(middle)
    boxes = [box_of([ob.matrix_world @ Vector(c) for c in ob.bound_box]) for ob in things]
    lo = Vector((min(b[0].x for b in boxes), min(b[0].y for b in boxes), min(b[0].z for b in boxes))) - Vector((NEAR,) * 3)
    hi = Vector((max(b[1].x for b in boxes), max(b[1].y for b in boxes), max(b[1].z for b in boxes))) + Vector((NEAR,) * 3)

    def near(ob):
        a, b = world_box(ob)
        if any(a[i] > hi[i] or b[i] < lo[i] for i in range(3)):
            return False
        return any(all(a[i] <= bb[1][i] + NEAR and b[i] >= bb[0][i] - NEAR for i in range(3)) for bb in boxes)

    colliders = [object_from("Settle " + name, bm, scratch) for name, (bm, _) in objects.items() if len(bm.faces)]
    colliders = [ob for ob in colliders if near(ob) or bpy.data.objects.remove(ob)] + [ob for ob in standing if near(ob)]
    floor = (Matrix.Translation((0, -0.25, 0)), (60.0, 0.5, 60.0))
    colliders += [box_object("Settle wall %d" % i, m, s, scratch) for i, (m, s) in enumerate(walls + [floor])]
    key = summed(things) + summed(colliders) + hashlib.sha1(open(__file__, "rb").read()).hexdigest()[:12]   # (and how it falls)
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, part + ".json")
    kept = json.load(open(path)) if os.path.exists(path) else {}
    before = [ob.matrix_world.copy() for ob in things]
    if kept.get("key") == key and len(kept.get("rests", [])) == len(things) and not os.environ.get("SETTLE_AGAIN"):
        rests = [Matrix([kept["rests"][i][r * 4:r * 4 + 4] for r in range(4)]) for i in range(len(things))]
        print("SETTLE %s: %d things as kept" % (part, len(things)))
    else:
        rests = let_down(things, colliders, fall(things, colliders, before), before)
        json.dump({"key": key, "rests": [[x for row in m for x in row] for m in rests]}, open(path, "w"))
        print("SETTLE %s: %d things fell on %d" % (part, len(things), len(colliders)))
    moves = []
    for i, (ob, start, rest) in enumerate(zip(things, before, rests)):
        moved = (rest.to_translation() - start.to_translation()).length
        turned = math.degrees(start.to_quaternion().rotation_difference(rest.to_quaternion()).angle)
        if moved > FAR or turned > TURNED:
            at = C.inverted() @ (start @ (sum((Vector(c) for c in ob.bound_box), Vector()) / 8))   # (its middle, in Unity's space)
            name = falling[i - len(items)].name if i >= len(items) else items[i][0]
            print("SETTLE far: %s %d moved %.0f mm, turned %.0f degrees, from (%.2f, %.2f, %.2f)"
                  % (name, i, moved * 1000, turned, at.x, at.y, at.z))
        if i < len(items):
            moves.append(rest @ start.inverted())
        else:
            asset = falling[i - len(items)]
            asset.matrix_world = rest @ start.inverted() @ asset.matrix_world
    for ob in list(scratch.objects):
        mesh = ob.data
        bpy.data.objects.remove(ob)
        bpy.data.meshes.remove(mesh)
    bpy.data.collections.remove(scratch)
    return moves


def tree_of(ob):
    from mathutils.bvhtree import BVHTree
    points = [ob.matrix_world @ v.co for v in ob.data.vertices]
    return BVHTree.FromPolygons(points, [tuple(p.vertices) for p in ob.data.polygons]), box_of(points)


def starts_in(things, colliders):
    """A thing let go inside another (or inside a thing let go before it) is lifted till it is
    clear, 5 mm at a time and 0.3 m at most, and falls from there; one still inside is said
    (it is thrown out when the fall begins: put it somewhere else)."""
    trees = {}

    def inside(ob, others):
        mine, (lo, hi) = tree_of(ob)
        for other in others:
            if other not in trees:
                trees[other] = tree_of(other)
            tree, (a, b) = trees[other]
            if not any(a[k] > hi[k] or b[k] < lo[k] for k in range(3)) and mine.overlap(tree):
                return other
        return None
    for i, ob in enumerate(things):
        others = colliders + things[:i]
        first = inside(ob, others)
        if first is None:
            continue
        lift = 0
        while lift < 60 and inside(ob, others) is not None:
            ob.matrix_world = Matrix.Translation((0, 0, 0.005)) @ ob.matrix_world
            lift += 1
        trees.pop(ob, None)
        at = C.inverted() @ ob.matrix_world.to_translation()
        still = inside(ob, others)
        if still is None:
            print("SETTLE lifted %d mm out of %s: %s at (%.2f, %.2f, %.2f)" % (lift * 5, first.name, ob.name, at.x, at.y, at.z))
        else:
            print("SETTLE starts in: %s inside %s at (%.2f, %.2f, %.2f)" % (ob.name, still.name, at.x, at.y, at.z))


def let_down(things, colliders, rests, before):
    """Bullet leaves a body resting a little over what holds it (its margins and its contacts' reach):
    each thing is let down the last of that, to 0.2 mm over the nearest under any corner of it, the
    lowest first, and again till none moves (a pile's top waits for the one under it). Gives the
    rests so lowered."""
    from mathutils.bvhtree import BVHTree
    fixed = [tree_of(ob)[0] for ob in colliders]
    down = Vector((0, 0, -1))

    def tree_at(i):
        ob = things[i]
        return BVHTree.FromPolygons([rests[i] @ v.co for v in ob.data.vertices], [tuple(p.vertices) for p in ob.data.polygons])
    mine = [tree_at(i) for i in range(len(things))]
    for _ in range(6):
        moved = False
        for i in sorted(range(len(things)), key=lambda i: rests[i].to_translation().z):
            ob = things[i]
            # its corners, and its faces' and edges' middles: a box turned on another has every corner outside
            # the one under it, and fell through it to what was under both (two magazines on a tower)
            mesh = ob.data
            points = [rests[i] @ v.co for v in mesh.vertices] + [rests[i] @ p.center for p in mesh.polygons]                 + [rests[i] @ ((mesh.vertices[e.vertices[0]].co + mesh.vertices[e.vertices[1]].co) / 2) for e in mesh.edges]
            step = max(1, len(points) // 1800)
            gap = None
            for p in points[::step]:
                for k, tree in enumerate(fixed + mine):
                    if k == len(fixed) + i:
                        continue
                    hit = tree.ray_cast(p + Vector((0, 0, 0.001)), down, 0.06)
                    if hit[0] is not None:
                        d = hit[3] - 0.001
                        if d < 0 and k >= len(fixed) and hit[1].z < 0:
                            continue   # (a corner under another thing that slid over it, its underside: that one is let down after;
                            #            its top there is a thing beside at this height, which this one must not go through)
                        if hit[1].z < 0:
                            d = 0.0    # (an underside met from above: the corner is in that thing, as Bullet left two thin ones; down is through it)
                        gap = d if gap is None else min(gap, d)
            if os.environ.get("SETTLE_TRACE_DOWN"):
                at = C.inverted() @ rests[i].to_translation()
                print("SETTLE down %s: gap %s at (%.2f, %.2f, %.2f)" % (ob.name, "none" if gap is None else "%.1f mm" % (gap * 1000), at.x, at.y, at.z))
            if gap is not None and 0.0004 < gap < 0.05:
                rests[i] = Matrix.Translation((0, 0, -(gap - 0.0002))) @ rests[i]
                mine[i] = tree_at(i)
                moved = True
        if not moved:
            break
    for ob, start in zip(things, before):
        ob.matrix_world = start
    return rests


def fall(things, colliders, before):
    scene = bpy.context.scene
    if scene.rigidbody_world is None:
        bpy.ops.rigidbody.world_add()
    world = scene.rigidbody_world
    world.enabled = True
    world.substeps_per_frame = 20
    world.solver_iterations = 30
    world.use_split_impulse = True
    world.point_cache.frame_start, world.point_cache.frame_end = 1, FRAMES
    scene.render.fps, scene.frame_start, scene.frame_end = 60, 1, FRAMES
    if world.collection is None:
        world.collection = bpy.data.collections.new("RigidBodyWorld")
    for ob in things:
        ob.matrix_world = Matrix.Translation((0, 0, LIFT)) @ ob.matrix_world
    starts_in(things, colliders)
    for kind, obs in (('ACTIVE', things), ('PASSIVE', colliders)):
        with bpy.context.temp_override(selected_objects=obs, active_object=obs[0], object=obs[0]):
            bpy.ops.rigidbody.objects_add(type=kind)
        for ob in obs:
            body = ob.rigid_body
            body.collision_shape = 'CONVEX_HULL' if kind == 'ACTIVE' else 'MESH'
            body.use_margin, body.collision_margin = True, 0.0002
            body.friction, body.restitution = (0.8, 0.0)
            if kind == 'ACTIVE':
                body.mass = 0.3
                body.linear_damping, body.angular_damping = 0.4, 0.7
                body.use_deactivation, body.deactivate_linear_velocity, body.deactivate_angular_velocity = True, 0.004, 0.02
    world.point_cache.frame_end = FRAMES
    bpy.ops.ptcache.free_bake_all()
    trace = [int(k) for k in os.environ.get("SETTLE_TRACE", "").split(",") if k]   # things whose fall is printed
    for frame in range(1, FRAMES + 1):
        scene.frame_set(frame)
        for k in trace:
            if frame % 6 == 1 and k < len(things):
                m = things[k].evaluated_get(bpy.context.evaluated_depsgraph_get()).matrix_world
                at, up = C.inverted() @ m.to_translation(), C.inverted().to_3x3() @ (m.to_3x3() @ Vector((0, 0, 1)))
                print("SETTLE trace %d frame %d at (%.3f, %.3f, %.3f) up (%.2f, %.2f, %.2f)" % (k, frame, *at, *up))
    graph = bpy.context.evaluated_depsgraph_get()
    rests = [ob.evaluated_get(graph).matrix_world.copy() for ob in things]
    for ob in things + colliders:
        with bpy.context.temp_override(selected_objects=[ob], active_object=ob, object=ob):
            bpy.ops.rigidbody.object_remove()
    scene.frame_set(1)
    bpy.ops.rigidbody.world_remove()
    for ob, start in zip(things, before):
        ob.matrix_world = start
    return rests
