# Things standing the wrong way round or in the wrong place: a television facing the wall, a desk's
# drawers to the wall, a clamp lamp in the middle of a desk, a chair turned from its desk, a sofa in
# a doorway. Every placed thing has a pose (world/poses.py): Poly Haven's models by their name,
# ours where lib.Builder.pose made them, each its front, its top and how it is mounted (a kind of
# poses.KINDS). Each pose is checked by rays in Unity's coordinates against the other objects of
# the last build: its top up (but what lies or leans), what is behind its back (a wall, a shelf's
# back), what is before its front (nothing large, or what it faces), a clamp's jaw round an edge,
# what hangs on a wall or a ceiling; and every doorway kept free. A thing found wrong is listed:
#   blender -b build/world.blend --python world/check_placement.py -- [WORD ...] [--all] [--shots] [--unposed]
# WORD: only poses whose name has one of them. --all: every pose, right ones too, with what the
# rays found. --shots: a picture of each listed pose from its front and from above, into
# build/placement/ (to look at where the rays cannot tell: which side of a model is its front).
# --unposed: the large pieces of no pose (by object): every thing should have one.
import math
import os
import sys

import bpy
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_intersect import SKIP, pieces   # noqa: E402
from poses import KINDS, POSES               # noqa: E402

C = Matrix(((-1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))   # Unity's space to Blender's (lib.C)
SHELL = ("Room", "Shell")   # the parts that are walls, floors and ceilings
LARGE = 0.25                # a piece this long across stands in the way (a magazine on a desk does not)
UPRIGHT = 10.0              # degrees a standing thing's top may be off up
LEANING = 35.0
UNPOSED = 0.5               # a piece this long across should be in a pose's box (--unposed)
FLAT = 0.04                 # a piece this low may lie in a doorway (a rug's edge, a sill)
BUILDING = ("trim", "Holo grid", "Room doors", "Corridor doors", "window", "Lift", "Cables", "Network", " rest", "rugs")   # the building and its
# fittings, not things placed in it (--unposed leaves them out): mouldings, a holodeck's bars, doors' leaves, windows, the lift, leads, rugs
UP = Vector((0, 1, 0))


def matrix(values):
    return Matrix([values[k:k + 4] for k in range(0, 16, 4)])


class World:
    """Every object's triangles in one tree, in Unity's coordinates; each triangle's piece and object."""

    def __init__(self):
        points, faces, self.owner, self.boxes = [], [], [], []
        self.objects = [o for o in bpy.data.objects if o.type == 'MESH' and not o.name.startswith(SKIP) and len(o.data.polygons)]
        self.pieces = []
        for k, ob in enumerate(self.objects):
            shell = any(c.name in SHELL for c in ob.users_collection)
            for tris, lo, hi in pieces(ob):
                n = len(self.pieces)
                self.pieces.append((k, lo, hi, shell or (hi - lo).length >= 2.5))
                for t in tris:
                    faces.append((len(points), len(points) + 1, len(points) + 2))
                    points.extend(t)
                    self.owner.append(n)
        self.tree = BVHTree.FromPolygons(points, faces)

    def ray(self, origin, direction, far, skip):
        """The first hit along a ray that skip(piece, point) does not leave out: (distance, piece, point) or None."""
        origin, direction = Vector(origin), Vector(direction).normalized()
        gone = 0.0
        while gone < far:
            hit = self.tree.ray_cast(origin + direction * gone, direction, far - gone)
            if hit[0] is None:
                return None
            piece = self.owner[hit[2]]
            d = gone + hit[3]
            if not skip(piece, hit[0]):
                return d, piece, hit[0]
            gone = d + 0.0005
        return None


class Pose:
    def __init__(self, name, kind, frame, lo, hi, checks, ob=None):
        self.name, self.kind, self.frame, self.lo, self.hi, self.ob = name, kind, frame, lo, hi, ob
        self.checks = dict(KINDS[kind])
        self.checks.update({k: v for k, v in checks.items() if v is not None})
        axis = self.checks.pop("axis", None)
        if axis is not None and tuple(axis) != (0, 0, 1):   # a model whose front is another way of its own (-z, +x, -x): its frame turned to it
            turn = Matrix.Rotation(math.atan2(axis[0], axis[2]), 4, 'Y')
            self.frame = frame = frame @ turn
            back = turn.inverted()
            corners = [back @ Vector((x, y, z)) for x in (lo.x, hi.x) for y in (lo.y, hi.y) for z in (lo.z, hi.z)]
            self.lo = Vector([min(c[i] for c in corners) for i in range(3)])
            self.hi = Vector([max(c[i] for c in corners) for i in range(3)])
        self.inverse = frame.inverted()
        rot = frame.to_3x3()
        self.front = (rot @ Vector((0, 0, 1))).normalized()
        self.top = (rot @ Vector((0, 1, 0))).normalized()
        self.side = (rot @ Vector((1, 0, 0))).normalized()

    def at(self, x, y, z):   # a point of its box (each -1 to 1) in the world
        return self.frame @ Vector([(self.lo[i] + self.hi[i]) / 2 + (self.hi[i] - self.lo[i]) / 2 * c for i, c in enumerate((x, y, z))])

    def inside(self, point, grow=0.01):
        p = self.inverse @ Vector(point)
        return all(self.lo[i] - grow <= p[i] <= self.hi[i] + grow for i in range(3))

    def corners(self):
        return [self.at(x, y, z) for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)]


def poses():
    found = []
    for ob in bpy.data.objects:
        if "pose_kind" in ob:   # ours: a box in its frame
            frame = matrix(list(ob["pose"]))
            size = Vector(list(ob["pose_size"]))
            checks = {k[5:]: (list(v) if hasattr(v, "__len__") and not isinstance(v, str) else v) for k, v in ob.items()
                      if k.startswith("pose_") and k not in ("pose_kind", "pose_size", "pose_name")}
            found.append(Pose(ob["pose_name"], ob["pose_kind"], frame, -size / 2, size / 2, checks))
        elif ob.type == 'MESH' and "pose" in ob and "asset" in ob:   # Poly Haven's: its own box in its pose's frame
            frame = matrix(list(ob["pose"]))
            to_own = frame.inverted() @ C.inverted() @ ob.matrix_world
            points = [to_own @ v.co for v in ob.data.vertices]
            lo = Vector([min(p[i] for p in points) for i in range(3)])
            hi = Vector([max(p[i] for p in points) for i in range(3)])
            pose = POSES.get(ob["asset"])
            if pose is None:
                found.append(Pose(ob.name, "any", frame, lo, hi, {}, ob))
                found[-1].unclassified = True
                continue
            found.append(Pose(ob.name, pose["kind"], frame, lo, hi, {k: v for k, v in pose.items() if k != "kind"}, ob))
    return found


def check(world, pose, everything):
    """What is wrong with a pose: a list of sentences, and what the rays found."""
    wrong, seen = [], []
    own = {k for k, o in enumerate(world.objects) if pose.ob is not None and o == pose.ob}

    def mine(piece, point):   # (a model is its own object, or two: a station and its rest; ours is its box)
        if pose.ob is not None:
            name = world.objects[world.pieces[piece][0]].name
            return world.pieces[piece][0] in own or any(name.startswith(w) for w in pose.checks.get("also", ()))
        return pose.inside(point, -0.001)   # (what touches its box's face, a wall behind a bookcase, is not its own)

    def blocking(piece, point):   # what does not stand in the way: its own, small pieces, what it faces
        if mine(piece, point):
            return True
        k, lo, hi, large = world.pieces[piece]
        if not large and (hi - lo).length < LARGE:
            return True
        faces = pose.checks.get("faces")
        return bool(faces) and any(o.kind == faces and o.inside(point) for o in everything)
    kind, c = pose.kind, pose.checks
    tilt = math.degrees(math.acos(max(-1.0, min(1.0, pose.top.dot(UP)))))
    if kind == "faceup":
        up = math.degrees(math.acos(max(-1.0, min(1.0, pose.front.dot(UP)))))
        if up > 25:
            wrong.append("its front is %.0f degrees off up (it should lie on its back)" % up)
    if kind not in ("lying", "any", "leaning", "faceup") and tilt > UPRIGHT:
        wrong.append("its top is %.0f degrees off up" % tilt)
    if kind == "leaning" and tilt > LEANING:
        wrong.append("it leans %.0f degrees" % tilt)
    if kind == "reads":   # read from the room: where a wall is near, its print's top points to it
        walled = lambda piece, point: not any(w in world.objects[world.pieces[piece][0]].name.lower() for w in ("wall", "trim", "wainscot"))
        start, best = pose.at(0, 1, 0) + UP * 0.03, None
        for k in range(16):
            way = Vector((math.cos(k * math.pi / 8), 0, math.sin(k * math.pi / 8)))
            hit = world.ray(start, way, c["near"], walled)
            if hit and (best is None or hit[0] < best[0]):
                best = (hit[0], way, world.objects[world.pieces[hit[1]][0]].name)
        top = Vector((pose.front.x, 0, pose.front.z)).normalized()
        if best is None:
            seen.append("no wall within %.2f m: any way round" % c["near"])
        else:
            off = math.degrees(math.acos(max(-1.0, min(1.0, top.dot(best[1])))))
            seen.append("%s %.0f mm away; its print's top %.0f degrees off it" % (best[2], best[0] * 1000, off))
            if off > 120:   # (sideways, as magazines stacked across a shelf with their spines out, is how they lie)
                wrong.append("its print is upside down from the room: its top is %.0f degrees off %s, %.0f mm away" % (off, best[2], best[0] * 1000))
    depth = (pose.hi.z - pose.lo.z)
    flat_front = Vector((pose.front.x, 0, pose.front.z))
    if "back" in c:   # its back against something within back
        best = None
        for x, y in ((0, 0), (-0.6, 0), (0.6, 0), (0, 0.5), (0, -0.5)):
            start = pose.at(x, y, -1) + pose.front * 0.002
            hit = world.ray(start, -pose.front, c["back"] + 0.002, mine)
            if hit and (best is None or hit[0] < best[0]):
                best = hit
        seen.append("behind: %s" % ("nothing within %.2f m" % c["back"] if best is None else "%s at %.0f mm" % (world.objects[world.pieces[best[1]][0]].name, best[0] * 1000)))
        if best is None:
            wrong.append("nothing behind its back within %.2f m (it should stand against something)" % c["back"])
    if "clear" in c and kind != "door":   # nothing large before its front
        best = None
        for x, y in ((0, 0), (-0.6, 0), (0.6, 0), (0, 0.5), (0, -0.4)):
            start = pose.at(x, y, 1) - pose.front * 0.002
            hit = world.ray(start, pose.front, c["clear"], blocking)
            if hit and (best is None or hit[0] < best[0]):
                best = hit
        seen.append("before: %s" % ("clear for %.2f m" % c["clear"] if best is None else "%s at %.0f mm, at (%.2f, %.2f, %.2f)" % (
            world.objects[world.pieces[best[1]][0]].name, best[0] * 1000, *best[2])))
        if best is not None:
            wrong.append("%s stands %.0f mm before its front (it should face out, free for %.2f m)"
                         % (world.objects[world.pieces[best[1]][0]].name, best[0] * 1000, c["clear"]))
    if c.get("faces"):   # what it faces is before its front
        targets = [o for o in everything if o.kind == c["faces"] and o is not pose]
        middle = pose.at(0, 0, 0)
        near = None
        for o in targets:
            centre = o.at(0, 0, 0)
            to = Vector((centre.x - middle.x, 0, centre.z - middle.z))
            if to.length < 1e-6 or flat_front.length < 1e-6:
                continue
            ahead = to.dot(flat_front.normalized())
            # whether a line out of its front, from across its width, goes into the other's box within reach
            # of its front's face (a table's sitters are at its places, not at its middle)
            hits = any(o.inside(pose.at(x, 0, 1) + flat_front.normalized() * d + Vector((0, y, 0)), 0.05)
                       for d in [k * 0.05 for k in range(int(c["reach"] / 0.05) + 1)] for y in (-0.2, 0.0, 0.2)
                       for x in (-0.75, -0.5, -0.25, 0.0, 0.25, 0.5, 0.75))
            if hits and (near is None or ahead < near[0]):
                near = (ahead, o)
        seen.append("faces: %s" % ("no %s within %.1f m" % (c["faces"], c["reach"]) if near is None else near[1].name))
        if near is None:
            wrong.append("no %s before its front within %.1f m (it should face one)" % (c["faces"], c["reach"]))
    if kind in ("floor", "desk", "seat"):   # on the floor
        foot = min(p.y for p in pose.corners())
        if foot > 0.03:
            hit = world.ray(pose.at(0, -1, 0) + UP * 0.005, -UP, 0.05, mine)
            if hit is None:
                wrong.append("its foot is %.0f mm over the floor and on nothing" % (foot * 1000))
    if kind == "hung":
        if abs(pose.front.y) > 0.2:
            wrong.append("it hangs facing %s" % ("up" if pose.front.y > 0 else "down"))
    if kind == "ceiling":
        hits = [world.ray(pose.at(x, 1, z) - UP * 0.005, UP, 0.5, mine) for x, z in ((0, 0), (-0.3, 0), (0.3, 0), (0, -0.3), (0, 0.3))]
        near = min((h[0] - 0.005 for h in hits if h), default=None)
        seen.append("over: %s" % ("nothing within 0.5 m" if near is None else "%s at %.0f mm" % (world.objects[world.pieces[min((h for h in hits if h), key=lambda h: h[0])[1]][0]].name, near * 1000)))
        if near is None or near > 0.03:
            wrong.append("its top is %s under the ceiling (it should hang from it)" % ("more than 0.5 m" if near is None else "%.0f mm" % (near * 1000)))
    if kind == "clamp":   # its jaw round an edge: a top under its origin's front, none behind its spine
        origin = pose.frame.to_translation()
        inner = world.ray(origin + flat_front.normalized() * 0.03 + UP * 0.01, -UP, 0.05, mine)
        outer = world.ray(origin - flat_front.normalized() * 0.03 + UP * 0.01, -UP, 0.05, mine)
        seen.append("under its front: %s, under its spine: %s" % ("nothing" if inner is None else "%.0f mm" % ((inner[0] - 0.01) * 1000),
                                                                  "nothing" if outer is None else "%.0f mm" % ((outer[0] - 0.01) * 1000)))
        if inner is None or abs(inner[0] - 0.01) > 0.004:
            wrong.append("no top under its jaw's front (it should grip a top)")
        if outer is not None:
            wrong.append("a top goes on behind its spine (a clamp grips an edge, not the middle of a top)")
    return wrong, seen


def doors(world, everything):
    """What stands in a doorway: pieces whose box comes within clear before a door, both sides."""
    wrong = []
    for door in [p for p in everything if p.kind == "door"]:
        clear = door.checks["clear"]
        width = door.hi.x - door.lo.x
        zone = [door.frame @ Vector((x * width / 2, y, z)) for x in (-1, 1) for y in (FLAT, min(2.0, door.hi.y - door.lo.y)) for z in (0.01, clear)]
        lo = Vector([min(p[i] for p in zone) for i in range(3)])
        hi = Vector([max(p[i] for p in zone) for i in range(3)])
        for k, plo, phi, large in world.pieces:
            name = world.objects[k].name
            if large or "door" in name.lower() or "Door" in name or phi.y < FLAT:
                continue
            if all(plo[i] < hi[i] and phi[i] > lo[i] for i in range(3)):
                out = max((door.inverse @ Vector((x, y, z))).z for x in (plo.x, phi.x) for y in (plo.y, phi.y) for z in (plo.z, phi.z))
                if out < 0.06:   # (on the wall's face: the casing, a skirting)
                    continue
                wrong.append((door.name, name, (plo + phi) / 2))
    return wrong


def shot(world, pose, folder):
    """Two pictures of a pose (Workbench): from before its front and from above, each from inside
    the room (short of the wall before it, under the ceiling over it)."""
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.render.resolution_x, scene.render.resolution_y = 640, 480
    scene.display.shading.light = 'STUDIO'
    scene.display.shading.color_type = 'MATERIAL'
    cam_data = bpy.data.cameras.get("Placement") or bpy.data.cameras.new("Placement")
    cam_data.lens = 14
    cam = bpy.data.objects.get("Placement") or bpy.data.objects.new("Placement", cam_data)
    if cam.name not in scene.collection.objects:
        scene.collection.objects.link(cam)
    scene.camera = cam
    middle = pose.at(0, 0, 0)
    size = max((pose.hi - pose.lo).length, 0.3)
    others = [o for o in world.objects if o != pose.ob]

    def free(origin, direction, far):   # how far one can go before a large piece (its own left out)
        hit = world.ray(origin, direction, far, lambda piece, point: world.objects[world.pieces[piece][0]] not in others or pose.inside(point)
                        or (not world.pieces[piece][3] and (world.pieces[piece][2] - world.pieces[piece][1]).length < LARGE))
        return far if hit is None else hit[0] - 0.05
    out = (pose.front + UP * 0.3).normalized()
    up_to = free(middle, UP, size * 2.2)
    for view, eye in (("front", middle + out * max(0.15, free(middle, out, size * 1.8))), ("above", middle + UP * max(0.15, up_to) + pose.front * 0.01)):
        b_eye, b_at = C @ eye, C @ middle
        look = (b_at - b_eye).normalized()
        cam.matrix_world = Matrix.Translation(b_eye) @ look.to_track_quat('-Z', 'Z' if view == "front" else 'Y').to_matrix().to_4x4()
        cam_data.clip_start = 0.02
        path = os.path.join(folder, "%s %s.png" % (pose.name, view))
        scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        print("PLACE   picture %s" % path)


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    every, shots, unposed = "--all" in args, "--shots" in args, "--unposed" in args
    words = [a for a in args if not a.startswith("--")]
    world = World()
    everything = poses()
    print("PLACE %d objects, %d pieces, %d poses" % (len(world.objects), len(world.pieces), len(everything)))
    folder = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "placement")
    if shots:
        os.makedirs(folder, exist_ok=True)
    bad = 0
    for pose in everything:
        if words and not any(w in pose.name for w in words):
            continue
        if getattr(pose, "unclassified", False):
            print("PLACE %s: no pose for %s in world/poses.py (say what it is and how it stands)" % (pose.name, pose.ob["asset"]))
            bad += 1
            continue
        if pose.kind == "door":
            continue
        wrong, seen = check(world, pose, everything)
        middle = pose.at(0, 0, 0)
        if wrong or every:
            print("PLACE %s (%s) at (%.2f, %.2f, %.2f), its front (%.2f, %.2f, %.2f): %s" % (
                pose.name, pose.kind, middle.x, middle.y, middle.z, pose.front.x, pose.front.y, pose.front.z, "; ".join(wrong) or "right"))
            if every:
                print("      %s" % "; ".join(seen))
        if wrong:
            bad += 1
            if shots:
                shot(world, pose, folder)
    for door, name, at in doors(world, everything):
        if not words or any(w in door or w in name for w in words):
            print("PLACE %s stands in the doorway of %s, at (%.2f, %.2f, %.2f)" % (name, door, at.x, at.y, at.z))
            bad += 1
    if unposed:   # large pieces in no pose's box
        counts = {}
        for k, lo, hi, large in world.pieces:
            ob = world.objects[k]
            if large or "asset" in ob or (hi - lo).length < UNPOSED or any(w in ob.name for w in BUILDING):
                continue
            middle = (lo + hi) / 2
            if not any(p.inside(middle, 0.05) for p in everything):
                counts.setdefault(ob.name, []).append(middle)
        for name, places in sorted(counts.items(), key=lambda kv: -len(kv[1])):
            print("PLACE unposed: %s, %d large pieces, one at (%.2f, %.2f, %.2f)" % (name, len(places), *places[0]))
    print("PLACE wrong: %d" % bad)


if __name__ == "__main__":
    main()
