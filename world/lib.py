# Modelling helpers for the VRChat room (run inside Blender). Everything is authored in
# Unity's coordinates (x right, y up, z forward, metres) and converted on the way in.
import json
import math
import os
import random

import bmesh
import bpy
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
TEXTURES = os.path.join(HERE, "..", "unity", "ShaderEmu", "Textures", "World")

# Unity (x, y, z) -> Blender (-x, -z, y): what Blender's FBX export and Unity's import undo.
SHELL = 0.0006   # metres: the thickness a surface is given to be a closed solid (Builder.lathe without a cap)
C = Matrix(((-1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))

# name: (metres a texture repeat, viewport colour, texture file or None)
MATERIALS = {}


def material(name, tile=1.0, colour=(0.8, 0.8, 0.8), texture=None, emit=0.0):
    MATERIALS[name] = (tile if isinstance(tile, tuple) else (tile, tile), colour, texture, emit)


def euler(rx=0.0, ry=0.0, rz=0.0):
    """Unity's Euler angles in degrees: z, then x, then y."""
    return (Matrix.Rotation(math.radians(ry), 4, 'Y') @ Matrix.Rotation(math.radians(rx), 4, 'X')
            @ Matrix.Rotation(math.radians(rz), 4, 'Z'))


def place(at=(0, 0, 0), rot=(0, 0, 0)):
    return Matrix.Translation(Vector(at)) @ euler(*rot)


OBLIQUE = 0.7   # a face whose normal is no nearer an axis than this (46 degrees off, 1.43 times stretched along it) has its picture laid
# in its own plane: nearer, an axis keeps it joined to its neighbours (at 0.8 a bevel of 5 steps came apart in every face, and
# the holodecks' seats were more vertices than Unity's unwrapper takes)


def plane_uv(points, normal):
    """Points of a face laid flat in its own plane, in metres, upright along y (along z on a face that looks up or down)."""
    n = Vector(normal).normalized()
    up = Vector((0, 1, 0)) if abs(n.y) < 0.9 else Vector((0, 0, -1 if n.y > 0 else 1))
    e2 = (up - n * up.dot(n)).normalized()
    e1 = e2.cross(n)
    return [(Vector(p).dot(e1), Vector(p).dot(e2)) for p in points]


class Builder:
    def __init__(self, seed=1):
        self.objects = {}      # name -> (bmesh, [material names])
        self.current = None
        self.stack = [Matrix.Identity(4)]
        self.assets = []       # (asset, matrix, size, triangle limit, parts, scale, hands, on, loose, back_at)
        # what falls into place when the part is finished (world/settle.py): each a loose thing made
        # apart, (object it joins, bmesh, [material names]), and boxes only the fall stands on (matrix, size)
        self.items, self.item, self.depth, self.walls = [], None, 0, []
        self.poses = []        # what things made here are and how they stand: see pose()
        self.random = random.Random(seed)
        # objects that bring their own place in the lightmap ("UV2"), a chart a surface: see chart()
        self.charted, self.chart_ids, self.chart_spec, self.fragment = set(), {}, None, 0
        atlas = os.path.join(HERE, "atlas.json")
        self.atlas = json.load(open(atlas)) if os.path.exists(atlas) else {}

    # ---- context
    def obj(self, name, charted=False):
        self.current = name
        if charted:
            self.charted.add(name)
        if name not in self.objects:
            self.objects[name] = (bmesh.new(), [])
        return self

    def loose(self):
        """What is made inside is one loose thing (a toy, a box lying, a magazine): when the part is
        finished it falls, a rigid body, onto what is under it and stays where it came to rest
        (world/settle.py), and then joins the current object. Inside another, it is part of that."""
        builder = self

        class Item:
            def __enter__(self):
                builder.depth += 1
                if builder.depth == 1:
                    builder.items.append((builder.current, bmesh.new(), []))
                    builder.item = len(builder.items) - 1

            def __exit__(self, *args):
                builder.depth -= 1
                if builder.depth == 0:
                    builder.item = None
        return Item()

    def wall(self, at, size, rot=(0, 0, 0)):
        """A box (in this frame) that loose things fall against and that is no model of this part:
        another part's floor or wall (a game leaning on the shell's wall)."""
        self.walls.append((self.stack[-1] @ place(at, rot), size))

    def pose(self, kind, size, at=(0, 0, 0), rot=(0, 0, 0), name=None, **checks):
        """Says what a thing modelled here is and how it stands, for world/check_placement.py: its box
        of `size` with its middle at `at` in this frame (its foot at at - size.y / 2), its front
        towards +z of that frame and its top towards +y; `kind` is how it is mounted (poses.KINDS) and
        `checks` change that kind's distances (back, clear, reach) or name what it faces (faces)."""
        self.poses.append(dict(kind=kind, size=tuple(size), matrix=self.stack[-1] @ place(at, rot), name=name or self.current, **checks))

    def at(self, at=(0, 0, 0), rot=(0, 0, 0)):
        builder = self

        class Frame:
            def __enter__(self):
                builder.stack.append(builder.stack[-1] @ place(at, rot))

            def __exit__(self, *args):
                builder.stack.pop()
        return Frame()

    def chart(self, name, uv_of):
        """In a charted object: what is made inside is one chart of the lightmap, laid out by
        uv_of(place, normal) in metres, the place and the face's normal in the frame this is
        entered in. It may give (part, u, v) for a part of the chart to lie by itself. Outside
        any chart a fragment is six charts, its faces seen along each axis either way."""
        frame, builder = self.stack[-1].copy(), self

        class Chart:
            def __enter__(self):
                builder.chart_spec = (name, uv_of, frame)

            def __exit__(self, *args):
                builder.chart_spec = None
        return Chart()

    def _chart(self, bm):
        uv2 = bm.loops.layers.uv.get("UV2") or bm.loops.layers.uv.new("UV2")
        tag = bm.faces.layers.int.get("chart") or bm.faces.layers.int.new("chart")
        self.fragment += 1
        spec = self.chart_spec
        back = (C @ spec[2]).inverted() if spec else None
        turn = back.to_3x3() if spec else None
        bm.normal_update()
        for f in bm.faces:
            if spec:
                normal, part, coords = turn @ f.normal, None, []
                for loop in f.loops:
                    r = spec[1](back @ loop.vert.co, normal)
                    if len(r) == 3:
                        part, r = r[0], r[1:]
                    coords.append(r)
                name = (spec[0], part)
            else:
                axis = max(range(3), key=lambda i: abs(f.normal[i]))
                name = (self.fragment, axis, f.normal[axis] > 0)
                coords = [(loop.vert.co[(axis + 1) % 3], loop.vert.co[(axis + 2) % 3]) for loop in f.loops]
            f[tag] = self.chart_ids.setdefault((self.current, name), len(self.chart_ids) + 1)
            for loop, c in zip(f.loops, coords):
                loop[uv2].uv = c

    def _pack(self, name, bm, margin=0.012):
        """The charts of an object side by side in a square, at one scale: rows, the tallest first."""
        uv2, tag = bm.loops.layers.uv.get("UV2"), bm.faces.layers.int.get("chart")
        boxes = {}
        for f in bm.faces:
            box = boxes.setdefault(f[tag], [1e9, 1e9, -1e9, -1e9])
            for loop in f.loops:
                u, v = loop[uv2].uv
                box[0], box[1], box[2], box[3] = min(box[0], u), min(box[1], v), max(box[2], u), max(box[3], v)
        sizes = {k: (b[2] - b[0] + margin, b[3] - b[1] + margin) for k, b in boxes.items()}
        side = max(math.sqrt(sum(w * h for w, h in sizes.values())) * 1.1, max(w for w, h in sizes.values()))
        while True:
            place, x, y, row = {}, 0.0, 0.0, 0.0
            for k in sorted(sizes, key=lambda k: -sizes[k][1]):
                w, h = sizes[k]
                if x + w > side:
                    x, y, row = 0.0, y + row, 0.0
                place[k] = (x, y)
                x, row = x + w, max(row, h)
            if y + row <= side:
                break
            side *= 1.06
        for f in bm.faces:
            box, (ox, oy) = boxes[f[tag]], place[f[tag]]
            for loop in f.loops:
                u, v = loop[uv2].uv
                loop[uv2].uv = ((ox + margin / 2 + u - box[0]) / side, (oy + margin / 2 + v - box[1]) / side)
        used = sum((b[2] - b[0]) * (b[3] - b[1]) for b in boxes.values()) / (side * side)
        print("CHARTS %-18s %4d charts in %.2f m square, %2.0f%% of it used" % (name, len(boxes), side, used * 100))

    def rand(self, a, b):
        return a + self.random.random() * (b - a)

    # ---- putting a finished fragment into the current object
    def _emit(self, bm, mat, at, rot, normal=None, charted=False):
        # a fragment's points that lie on each other are one: its seams joined, so it is watertight (world/check_closed.py)
        bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-6)
        if bm.edges and all(len(e.link_faces) == 2 for e in bm.edges):
            normal = None   # a closed solid looks out, by its own topology, whatever normals were asked
        world = self.stack[-1] @ place(at, rot)
        bmesh.ops.transform(bm, matrix=C @ world, verts=bm.verts)
        bm.normal_update()
        turn = (C @ world).to_3x3()
        if normal is None:
            bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
            flip = []
            if bm.edges and all(len(e.link_faces) == 2 for e in bm.edges):   # closed: its volume by its faces must come out positive
                volume, middle = 0.0, sum((v.co for v in bm.verts), Vector()) / len(bm.verts)   # (about its middle: a sticker's
                for f in bm.faces:                                                         # volume is under float32's error far out)
                    vs = [v.co - middle for v in f.verts]
                    for k in range(1, len(vs) - 1):
                        volume += vs[0].dot(vs[k].cross(vs[k + 1]))
                if volume < 0:   # (a slab too thin for recalc_face_normals to tell its outside)
                    flip = bm.faces[:]
        elif normal == 'out':   # away from the fragment's middle
            middle = sum((v.co for v in bm.verts), Vector()) / len(bm.verts)
            flip = [f for f in bm.faces if f.normal.dot(f.calc_center_median() - middle) < 0]
        elif isinstance(normal, list):   # one wanted normal a face
            flip = [f for f, n in zip(bm.faces, normal) if f.normal.dot(turn @ Vector(n)) < 0]
        else:
            want = turn @ Vector(normal)
            flip = [f for f in bm.faces if f.normal.dot(want) < 0]
        if flip:
            bmesh.ops.reverse_faces(bm, faces=flip)
        if self.current in self.charted and not charted:
            self._chart(bm)
        target, names = self.objects[self.current] if self.item is None else self.items[self.item][1:]
        if mat not in names:
            names.append(mat)
        index = names.index(mat)
        for f in bm.faces:
            f.material_index = index
            f.smooth = True
        mesh = bpy.data.meshes.new("fragment")
        bm.to_mesh(mesh)
        bm.free()
        target.from_mesh(mesh)
        bpy.data.meshes.remove(mesh)

    def _tile(self, mat):
        return MATERIALS[mat][0]

    def _box_uv(self, bm, mat, grain=None, world=None, scatter=True):
        tu, tv = self._tile(mat)
        ou, ov = (self.random.random(), self.random.random()) if scatter and world is None else (0.0, 0.0)
        uv = bm.loops.layers.uv.verify()
        bm.normal_update()
        warped = []   # (a quad twisted out of its plane, as a bevel's long thin strip can be, is two triangles each laid by its own normal:
        for f in bm.faces:   # one projection for both had one of them stretched 1.7 times)
            if len(f.verts) == 4:
                a, b, c, d = (v.co for v in f.verts)
                n = (b - a).cross(c - a)
                side = min((b - a).length, (c - b).length, (d - c).length, (a - d).length)
                if n.length > 1e-12 and side > 0 and abs((d - a).dot(n.normalized())) > 0.05 * side:
                    warped.append(f)
        if warped:
            bmesh.ops.triangulate(bm, faces=warped)
            bm.normal_update()
        for f in bm.faces:
            n = f.normal
            ax = max(range(3), key=lambda i: abs(n[i]))
            if abs(n[ax]) < OBLIQUE:   # a bevel's corner, between the axes: laid in its own plane (along an axis it was stretched up to 1.7 times)
                cos = [world @ loop.vert.co if world is not None else loop.vert.co for loop in f.loops]
                nn = (world.to_3x3() @ n).normalized() if world is not None else n
                for loop, (a, b) in zip(f.loops, plane_uv(cos, nn)):
                    loop[uv].uv = (a / tu + ou, b / tv + ov)
                continue
            ua, va = ((2, 1), (0, 2), (0, 1))[ax]
            if grain is not None and grain != ax:
                ua, va = grain, 3 - ax - grain
            for loop in f.loops:
                co = world @ loop.vert.co if world is not None else loop.vert.co
                loop[uv].uv = (co[ua] / tu + ou, co[va] / tv + ov)

    def _bevel(self, bm, bevel, segs, edges='all'):
        if bevel <= 0:
            return
        pick = bm.edges[:]
        if edges != 'all':
            axes = ['xyz'.index(a) for a in edges]
            pick = [e for e in pick
                    if max(range(3), key=lambda i: abs((e.verts[0].co - e.verts[1].co)[i])) in axes]
        bmesh.ops.bevel(bm, geom=pick, offset=bevel, offset_type='OFFSET', segments=segs, profile=0.5,
                        affect='EDGES', clamp_overlap=True)

    def asset(self, asset, at, rot=(0, 0, 0), size=(None, None, None), limit=None, parts=None, scale=None, hands=None, on=None, loose=False, back_at=None):
        """One of Poly Haven's models (assets.py), standing on `at` and facing local +z. `hands`: a
        clock's (assets.spawn_hands), which become objects of their own. on: what an earlier call
        gave back, if this stands on that model: it is let down on it (assets.rest). loose: a thing
        that falls into place (see loose()); inside loose() it is one too. back_at: metres behind
        `at` (along its -z) where its back is to be, whatever the model's own depth: a picture's on its wall."""
        self.assets.append((asset, self.stack[-1] @ place(at, rot), size, limit, parts, scale, hands, on, loose or self.depth > 0, back_at))
        return len(self.assets) - 1

    # ---- primitives
    def box(self, at, size, mat, bevel=0.0, segs=2, rot=(0, 0, 0), grain='auto', edges='all', wuv=False):
        bm = bmesh.new()
        bmesh.ops.create_cube(bm, size=1.0)
        bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
        self._bevel(bm, min(bevel, min(size) * 0.49), segs, edges)
        if grain == 'auto':
            grain = max(range(3), key=lambda i: size[i])
        world = self.stack[-1] @ place(at, rot) if wuv else None
        self._box_uv(bm, mat, grain, world)
        self._emit(bm, mat, at, rot)

    def lathe(self, at, profile, mat, segs=24, rot=(0, 0, 0), cap=True, uvrect=None, print_y=None):
        """Revolve (radius, height) points round the local y axis. Its ends are closed by flat caps;
        without `cap` the profile is a surface given a thickness of SHELL (back along its other side),
        so a rim, a dimple or a ring is still a closed solid (world/check_closed.py)."""
        bm = bmesh.new()
        uv = bm.loops.layers.uv.verify()
        closed = False
        if len(profile) > 2 and profile[0] == profile[-1]:   # a profile that goes round: a ring of its own, closed
            profile, closed = list(profile[:-1]), True
        elif not cap and len(profile) >= 2:
            inner, n = [], len(profile)
            for i, (r, y) in enumerate(profile):
                a, c = profile[max(i - 1, 0)], profile[min(i + 1, n - 1)]
                length = math.hypot(c[0] - a[0], c[1] - a[1]) or 1.0
                nr, ny = (c[1] - a[1]) / length, -(c[0] - a[0]) / length
                inner.append((max(0.0, r - nr * SHELL), y - ny * SHELL))
            profile, closed = list(profile) + inner[::-1], True
        rings = []
        for r, y in profile:
            rings.append([bm.verts.new((r * math.cos(2 * math.pi * s / segs), y, r * math.sin(2 * math.pi * s / segs)))
                          for s in range(segs)])
        tu, tv = self._tile(mat)
        pairs = [(i, i + 1) for i in range(len(profile) - 1)] + ([(len(profile) - 1, 0)] if closed else [])
        along = [0.0]
        for i, j in pairs:
            along.append(along[-1] + math.hypot(profile[j][0] - profile[i][0], profile[j][1] - profile[i][1]))
        def band(p, q, start, vert, u):
            """A band between two rings unrolled flat, unstretched: a cone's band is a piece of a ring of
            the plane round its apex (a slant s from it, an angle of the turn times its sine), a cylinder's
            a rectangle. (Each ring by its own girth, one coordinate along and one round, had the round one
            slide sideways from ring to ring of a slanted band: a lamp's shade sheared twice.)"""
            r = math.hypot(vert.co.x, vert.co.z)
            slant = math.hypot(q[0] - p[0], q[1] - p[1]) or 1e-9
            fall = abs(q[0] - p[0]) / slant   # the sine of the cone's half angle
            turn = (u - 0.5) * 2 * math.pi
            if fall < 1e-9 or fall * 3.0 < max(p[0], q[0]):   # (no nearer apex than 3 m: as good as a cylinder, its girth the band's middle)
                on_q = abs(vert.co.y - q[1]) + abs(r - q[0]) < abs(vert.co.y - p[1]) + abs(r - p[0])
                return ((start + (slant if on_q else 0.0)) / tu, turn * (p[0] + q[0]) / 2 / tv)
            s = r / fall
            return (s * math.cos(turn * fall) / tu, s * math.sin(turn * fall) / tv)

        for k, (i, j) in enumerate(pairs):
            for s in range(segs):
                t = (s + 1) % segs
                face = bm.faces.new((rings[i][s], rings[i][t], rings[j][t], rings[j][s]))
                us = (s / segs, (s + 1) / segs, (s + 1) / segs, s / segs)
                vs = (along[k], along[k], along[k + 1], along[k + 1])
                rise, run = abs(profile[j][1] - profile[i][1]), abs(profile[j][0] - profile[i][0])
                flat = rise < 0.35 * run   # a table's top: no rings of grain round its middle
                bare = print_y is not None and (max(profile[i][1], profile[j][1]) <= print_y[0] + 1e-9 or min(profile[i][1], profile[j][1]) >= print_y[1] - 1e-9)
                for loop, u, v in zip(face.loops, us, vs):
                    if uvrect and bare:   # (outside the print, which is bare metal there: unrolled flat onto a spot of it, small;
                        # laid by its share of the outline a lid's middle had a whole turn of the picture in a point)
                        x, y = (loop.vert.co.x, loop.vert.co.z) if flat else band(profile[i], profile[j], 0.0, loop.vert, u)
                        loop[uv].uv = (uvrect[0] + (0.5 + 0.01 * x) * (uvrect[2] - uvrect[0]), uvrect[1] + (0.02 + 0.01 * y) * (uvrect[3] - uvrect[1]))
                    elif uvrect:
                        loop[uv].uv = (uvrect[0] + u * (uvrect[2] - uvrect[0]),
                                       uvrect[1] + v / along[-1] * (uvrect[3] - uvrect[1]))
                    elif flat:
                        loop[uv].uv = (loop.vert.co.x / tu, loop.vert.co.z / tv)
                    else:
                        loop[uv].uv = band(profile[i], profile[j], along[k], loop.vert, u)
        if cap and not closed:
            for ring, (r, _) in ((rings[0], profile[0]), (rings[-1], profile[-1])):
                if r > 1e-6:
                    face = bm.faces.new(ring)
                    for loop in face.loops:
                        loop[uv].uv = (loop.vert.co.x / tu, loop.vert.co.z / tv)
        bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-6)
        self._emit(bm, mat, at, rot)

    def cyl(self, at, radius, height, mat, segs=24, bevel=0.0, rot=(0, 0, 0)):
        """A cylinder centred on `at`, along the local y axis."""
        h, b = height / 2, min(bevel, radius * 0.5, height * 0.5)
        if b > 0:
            k = b * 0.2929
            profile = [(radius - b, -h), (radius - k, -h + k), (radius, -h + b), (radius, h - b), (radius - k, h - k), (radius - b, h)]
        else:
            profile = [(radius, -h), (radius, h)]
        self.lathe(at, profile, mat, segs, rot)

    def prism(self, at, points, length, mat, plane='zy', bevel=0.0, segs=2, rot=(0, 0, 0), grain=None):
        """A polygon in a local plane ('zy', 'xy' or 'xz'), extruded `length` along the third axis."""
        bm = bmesh.new()
        half = length / 2
        put = {'zy': lambda a, b, e: (e, b, a), 'xy': lambda a, b, e: (a, b, e), 'xz': lambda a, b, e: (a, e, b)}[plane]
        near = [bm.verts.new(put(a, b, -half)) for a, b in points]
        far = [bm.verts.new(put(a, b, half)) for a, b in points]
        bm.faces.new(near)
        bm.faces.new(far)
        n = len(points)
        for i in range(n):
            bm.faces.new((near[i], near[(i + 1) % n], far[(i + 1) % n], far[i]))
        self._bevel(bm, bevel, segs)
        self._box_uv(bm, mat, grain)
        self._emit(bm, mat, at, rot)

    def sweep(self, profile, path, mat, up=(0, 1, 0), closed=False, at=(0, 0, 0), rot=(0, 0, 0)):
        """A closed profile of (out, up) points carried along a flat path, mitred at its corners."""
        bm = bmesh.new()
        uv = bm.loops.layers.uv.verify()
        up = Vector(up).normalized()
        points = [Vector(p) for p in path]
        n = len(points)
        dirs = []
        for i in range(n if closed else n - 1):
            dirs.append((points[(i + 1) % n] - points[i]).normalized())
        rings = []
        for i in range(n):
            before = dirs[i - 1] if (closed or i > 0) else dirs[0]
            after = dirs[i] if (closed or i < n - 1) else dirs[-1]
            s0, s1 = up.cross(before).normalized(), up.cross(after).normalized()
            side = (s0 + s1) / (1.0 + s0.dot(s1))
            rings.append([bm.verts.new(points[i] + side * a + up * b) for a, b in profile])
        # (a run's picture lies along the run itself, each point by how far along the run's direction it is: at a mitre the
        # outer edge is longer than the inner, and one u for both sheared the picture; the joint is a seam, as in wood)
        m = len(profile)
        along = [0.0]
        for i in range(1, n + (1 if closed else 0)):
            along.append(along[-1] + (points[i % n] - points[i - 1]).length)
        tu, tv = self._tile(mat)
        girth = [0.0]
        for j in range(m):
            a0, b0 = profile[j]
            a1, b1 = profile[(j + 1) % m]
            girth.append(girth[-1] + math.hypot(a1 - a0, b1 - b0))
        for i in range(n if closed else n - 1):
            k = (i + 1) % n
            for j in range(m):
                l = (j + 1) % m
                face = bm.faces.new((rings[i][j], rings[i][l], rings[k][l], rings[k][j]))
                us = [along[i] + (v.co - points[i]).dot(dirs[i]) for v in (rings[i][j], rings[i][l], rings[k][l], rings[k][j])]
                vs = (girth[j], girth[j + 1], girth[j + 1], girth[j])
                for loop, u, v in zip(face.loops, us, vs):
                    loop[uv].uv = (u / tu, v / tv)
        if not closed:
            for ring in (rings[0], rings[-1]):   # (an end is the profile itself: its picture laid flat by the profile's own coordinates)
                face = bm.faces.new(ring)
                for loop in face.loops:
                    a, b = profile[ring.index(loop.vert)]
                    loop[uv].uv = (a / tu, b / tv)
        self._emit(bm, mat, at, rot)

    def tube(self, path, radius, mat, segs=8, at=(0, 0, 0), rot=(0, 0, 0), smooth=0, bend=0.0):
        """A round tube through the points of `path`; `smooth` steps a span make a curve of it, or `bend`
        rounds each corner in an arc of that radius (round its middle), as a tube is bent round a former."""
        bm = bmesh.new()
        uv = bm.loops.layers.uv.verify()
        points = [Vector(p) for p in path]
        if bend > 0 and len(points) > 2:
            arcs = [points[0]]
            for i in range(1, len(points) - 1):
                into, out = (points[i] - points[i - 1]).normalized(), (points[i + 1] - points[i]).normalized()
                turn = into.angle(out, 0.0)
                if turn < 1e-3:
                    arcs.append(points[i])
                    continue
                d = min(bend * math.tan(turn / 2), (points[i] - points[i - 1]).length / 2, (points[i + 1] - points[i]).length / 2)
                r = d / math.tan(turn / 2)
                centre = points[i] + (out - into).normalized() * (r / math.cos(turn / 2))
                a, c = points[i] - into * d - centre, points[i] + out * d - centre
                steps = max(2, int(math.ceil(turn / 0.2)))
                arcs += [centre + a.normalized().slerp(c.normalized(), k / steps).normalized() * r for k in range(steps + 1)]
            points = arcs + [points[-1]]
        if smooth and len(points) > 2:   # Catmull-Rom through every point
            ends = [points[0] * 2 - points[1]] + points + [points[-1] * 2 - points[-2]]
            curve = []
            for i in range(1, len(ends) - 2):
                p0, p1, p2, p3 = ends[i - 1], ends[i], ends[i + 1], ends[i + 2]
                for k in range(smooth):
                    t = k / smooth
                    curve.append(0.5 * ((2 * p1) + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t
                                        + (3 * p1 - p0 - 3 * p2 + p3) * t * t * t))
            points = curve + [points[-1]]
        kept = [points[0]]   # (points nearer each other than half the radius: rings that close tilt into each other, a shear)
        for q in points[1:-1]:
            if (q - kept[-1]).length >= 0.5 * radius and (points[-1] - q).length >= 0.5 * radius:
                kept.append(q)
        points = kept + [points[-1]]
        rounded = [points[0]]   # (a kink, a turn at one point tighter than the tube is thick, rounded: its inside had folded
        for i in range(1, len(points) - 1):   # through itself, and its picture there was stretched up to 11 times)
            into, out = points[i] - points[i - 1], points[i + 1] - points[i]
            turn = into.angle(out, 0.0) if into.length > 1e-9 and out.length > 1e-9 else 0.0
            d = min(into.length, out.length) / 2
            if turn < 0.05 or d / math.tan(turn / 2) >= 2.5 * radius:
                rounded.append(points[i])
                continue
            start, end = points[i] - into.normalized() * d, points[i] + out.normalized() * d
            steps = max(1, min(4, int(2 * d / (0.5 * radius))))   # (its points no nearer than half the radius either)
            for k in range(steps + 1):
                t = k / steps
                q = start * (1 - t) ** 2 + points[i] * (2 * t * (1 - t)) + end * t * t
                if (q - rounded[-1]).length > 1e-7:   # (two kinks side by side meet at their span's middle)
                    rounded.append(q)
        points = rounded + [points[-1]]
        n = len(points)
        tangents = []
        for i in range(n):
            a, b = points[max(i - 1, 0)], points[min(i + 1, n - 1)]
            tangents.append((b - a).normalized())
        side = tangents[0].orthogonal().normalized()
        rings, along = [], [0.0]
        for i in range(n):
            side = (side - tangents[i] * side.dot(tangents[i])).normalized()
            other = tangents[i].cross(side)
            rings.append([bm.verts.new(points[i] + (side * math.cos(2 * math.pi * s / segs) + other * math.sin(2 * math.pi * s / segs)) * radius)
                          for s in range(segs)])
        # each line along its own track (a bend tighter than the tube is thick had its inside a third of its
        # middle's length), but the differences of a bend add up as a shear: where the tube has turned 0.25
        # radians since, every line starts again from the middle line's length, a seam round it
        along = [0.0]
        for i in range(1, n):
            along.append(along[-1] + (points[i] - points[i - 1]).length)
        spans, now, turned = [], [0.0] * segs, 0.0
        for i in range(n - 1):
            turn = tangents[i].angle(tangents[i + 1], 0.0)
            if turned + turn > 0.25:   # (before the span that would take it over: a tight span begins level, a keystone on a keystone)
                now, turned = [along[i]] * segs, 0.0
            turned += turn
            then = [now[s] + (rings[i + 1][s].co - rings[i][s].co).length for s in range(segs)]
            spans.append((now, then))
            now = then
        tu, tv = self._tile(mat)
        for i in range(n - 1):
            for s in range(segs):
                t = (s + 1) % segs
                face = bm.faces.new((rings[i][s], rings[i][t], rings[i + 1][t], rings[i + 1][s]))
                us = (spans[i][0][s], spans[i][0][t], spans[i][1][t], spans[i][1][s])
                vs = (s / segs, (s + 1) / segs, (s + 1) / segs, s / segs)
                for loop, u, v in zip(face.loops, us, vs):
                    loop[uv].uv = (u / tu, v * 2 * math.pi * radius / tv)
        ends = [bm.faces.new(ring) for ring in (rings[0], rings[-1])]
        for f in ends:   # (an end is a round of the tube's radius: its picture laid flat on it)
            for k, loop in enumerate(f.loops):
                s = (rings[0] if f is ends[0] else rings[-1]).index(loop.vert)
                loop[uv].uv = (radius * math.cos(2 * math.pi * s / segs) / tu, radius * math.sin(2 * math.pi * s / segs) / tv)
        own = self.current in self.charted and self.chart_spec is None
        if own:
            # unrolled: its length across the chart, its girth up it, in lengths of 0.3 m one over another
            uv2, tag = bm.loops.layers.uv.new("UV2"), bm.faces.layers.int.new("chart")
            uv = bm.loops.layers.uv[0]   # (a new layer leaves the handles to the old ones stale)
            self.fragment += 1
            piece = 0.3
            for f in bm.faces:
                if f in ends:
                    f[tag] = self.chart_ids.setdefault((self.current, (self.fragment, 'end', ends.index(f))), len(self.chart_ids) + 1)
                    for k, loop in enumerate(f.loops):
                        loop[uv2].uv = (radius * math.cos(2 * math.pi * k / segs), radius * math.sin(2 * math.pi * k / segs))
                    continue
                us = [loop[uv].uv[0] * tu for loop in f.loops]
                part = int(min(us) / piece + 1e-6)
                f[tag] = self.chart_ids.setdefault((self.current, (self.fragment, 'run', part)), len(self.chart_ids) + 1)
                for loop, u in zip(f.loops, us):
                    loop[uv2].uv = (u - part * piece, loop[uv].uv[1] * tv)
        self._emit(bm, mat, at, rot, charted=own)

    def quad(self, at, size, mat, rot=(0, 0, 0), uvrect=None, decal=None):
        """A rectangle in the local xy plane, seen from -z, its picture upright along +y."""
        if decal is not None:
            uvrect = self.atlas[decal]
        bm = bmesh.new()
        uv = bm.loops.layers.uv.verify()
        w, h = size[0] / 2, size[1] / 2
        corners = ((-w, -h), (w, -h), (w, h), (-w, h))
        face = bm.faces.new([bm.verts.new((x, y, 0)) for x, y in corners])
        tu, tv = self._tile(mat)
        for loop, (x, y) in zip(face.loops, corners):
            if uvrect:
                loop[uv].uv = (uvrect[0] + (x / size[0] + 0.5) * (uvrect[2] - uvrect[0]),
                               uvrect[1] + (y / size[1] + 0.5) * (uvrect[3] - uvrect[1]))
            else:
                loop[uv].uv = (x / tu, y / tv)
        self._emit(bm, mat, at, rot, normal=(0, 0, -1))

    def sheet(self, rect, mat, holes=(), light=(0.0, 0.0), extent=1.0, uv_from=(0.0, 0.0), flip=False):
        """A rectangle (x0, y0, x1, y1) in the local xy plane, seen from -z, with rectangular
        openings: one surface, cut along every opening's edges from side to side so that all its
        pieces share whole edges. Its place in the lightmap is written here ("UV2"): its lower
        left corner at `light` metres of a layout `extent` metres square. flip: seen from +z."""
        x0, y0, x1, y1 = rect
        xs = sorted({x0, x1} | {min(max(x, x0), x1) for h in holes for x in (h[0], h[2])})
        ys = sorted({y0, y1} | {min(max(y, y0), y1) for h in holes for y in (h[1], h[3])})
        bm = bmesh.new()
        uv = bm.loops.layers.uv.verify()
        uv2 = bm.loops.layers.uv.new("UV2")
        tu, tv = self._tile(mat)
        corner = {}
        for j in range(len(ys) - 1):
            for i in range(len(xs) - 1):
                cx, cy = (xs[i] + xs[i + 1]) / 2, (ys[j] + ys[j + 1]) / 2
                if any(h[0] < cx < h[2] and h[1] < cy < h[3] for h in holes):
                    continue
                face = []
                for a, c in ((i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)):
                    if (a, c) not in corner:
                        corner[(a, c)] = bm.verts.new((xs[a], ys[c], 0))
                    face.append(corner[(a, c)])
                for loop in bm.faces.new(face).loops:
                    x, y = loop.vert.co.x, loop.vert.co.y
                    loop[uv].uv = ((x - uv_from[0]) / tu, (y - uv_from[1]) / tv)
                    loop[uv2].uv = ((light[0] + x - x0) / extent, (light[1] + y - y0) / extent)
        self._emit(bm, mat, (0, 0, 0), (0, 0, 0), normal=(0, 0, 1) if flip else (0, 0, -1))

    def mesh(self, vertices, faces, uvs, mat, at=(0, 0, 0), rot=(0, 0, 0), normal=None, colours=None, sheet=False):
        """Raw geometry: faces index `vertices`; `uvs` (and `colours`) are per face corner. sheet: a
        surface laid on another (a seam's line, a label), open by nature: world/check_closed.py lets it be."""
        bm = bmesh.new()
        uv = bm.loops.layers.uv.verify()
        col = bm.loops.layers.float_color.new("Col") if colours is not None else None
        verts = [bm.verts.new(v) for v in vertices]
        for index, face in enumerate(faces):
            f = bm.faces.new([verts[i] for i in face])
            for corner, loop in enumerate(f.loops):
                loop[uv].uv = uvs[index][corner]
                if col is not None:
                    loop[col] = colours[index][corner]
        if sheet:
            mark = bm.faces.layers.int.new("sheet")
            for f in bm.faces:
                f[mark] = 1
        self._emit(bm, mat, at, rot, normal)

    # ---- the scene
    def finish(self, collection_name):
        """Make Blender objects of everything built so far, in one collection."""
        collection = bpy.data.collections.new(collection_name)
        bpy.context.scene.collection.children.link(collection)
        made, spawned, falling = [], [], []
        if self.assets:
            import assets
            for index, (asset, world, size, limit, parts, scale, hands, on, loose, back_at) in enumerate(self.assets):
                spawned.append(assets.spawn("%s %d" % (asset, index), asset, world, size, limit, parts, scale, collection))
                spawned[-1]["asset"] = asset   # (what world/check_closed.py leaves to its makers)
                if back_at is not None:   # its back put on the plane back_at behind its place
                    ob = spawned[-1]
                    here = (C @ world).inverted() @ ob.matrix_world
                    back = min((here @ v.co).z for v in ob.data.vertices)
                    ob.matrix_world = C @ world @ Matrix.Translation((0, 0, -back_at - back)) @ world.inverted() @ C.inverted() @ ob.matrix_world
                if on is not None:
                    assets.rest(spawned[-1], spawned[on])
                spawned[-1]["placed"] = [x for row in world for x in row]   # (its pose: its place and turn, as asked)
                spawned[-1]["spawned"] = [x for row in spawned[-1].matrix_world for x in row]
                if loose:
                    falling.append(spawned[-1])
        if self.items or falling:
            import settle
            for target, _, _ in self.items:
                self.obj(target)
            moves = settle.settle(collection_name, self.objects, self.items, [ob for ob in spawned if ob not in falling], falling, self.walls)
            for (target, bm, names), move in zip(self.items, moves):
                bmesh.ops.transform(bm, matrix=move, verts=bm.verts)
                into, into_names = self.objects[target]
                for f in bm.faces:
                    mat = names[f.material_index]
                    if mat not in into_names:
                        into_names.append(mat)
                    f.material_index = into_names.index(mat)
                mesh = bpy.data.meshes.new("fragment")
                bm.to_mesh(mesh)
                bm.free()
                into.from_mesh(mesh)
                bpy.data.meshes.remove(mesh)
        for name, (bm, names) in self.objects.items():
            if name in self.charted:
                self._pack(name, bm)
            for edge in bm.edges:
                edge.smooth = len(edge.link_faces) != 2 or edge.calc_face_angle(0.0) < math.radians(38)
            mesh = bpy.data.meshes.new(name)
            bm.to_mesh(mesh)
            bm.free()
            for mat in names:
                mesh.materials.append(blender_material(mat))
            ob = bpy.data.objects.new(name, mesh)
            collection.objects.link(ob)
            if "Col" in mesh.attributes:
                mesh.color_attributes.active_color = mesh.color_attributes["Col"]
                mesh.color_attributes.render_color_index = mesh.color_attributes.find("Col")
            else:
                weighted = ob.modifiers.new("Weighted normals", 'WEIGHTED_NORMAL')
                weighted.keep_sharp = True
                weighted.weight = 100
            made.append(ob)
        for ob in spawned:   # its pose where it came to rest (world/check_placement.py): the fall's move on the place asked
            asked = Matrix([ob["placed"][k:k + 4] for k in range(0, 16, 4)])
            first = Matrix([ob["spawned"][k:k + 4] for k in range(0, 16, 4)])
            ob["pose"] = [x for row in C.inverted() @ ob.matrix_world @ first.inverted() @ C @ asked for x in row]
        for k, p in enumerate(self.poses):   # an empty for each pose, which no export takes (meshes only)
            empty = bpy.data.objects.new("Pose %s %d" % (p["name"], k), None)
            empty["pose"] = [x for row in p["matrix"] for x in row]
            for key, value in p.items():
                if key != "matrix":
                    empty["pose_" + key] = list(value) if isinstance(value, tuple) else value
            collection.objects.link(empty)
        for ob, (asset, _, _, _, _, _, hands, _, _, _) in zip(spawned, self.assets):
            made.append(ob)
            if hands:
                made.extend(assets.spawn_hands(ob, asset, hands, collection))
        self.objects, self.assets, self.items, self.walls, self.poses = {}, [], [], [], []
        self.was_charted, self.charted = self.charted, set()
        return made


def blender_material(name):
    mat = bpy.data.materials.get(name)
    if mat is not None:
        return mat
    tile, colour, texture, emit = MATERIALS[name]
    # a tint is written as Unity shows it (sRGB); Blender's shader wants light
    light = tuple(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in colour)
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    shader = mat.node_tree.nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = (*light, 1.0)
    mat.diffuse_color = (*light, 1.0)
    if emit > 0:
        shader.inputs["Emission Color"].default_value = (*light, 1.0)
        shader.inputs["Emission Strength"].default_value = emit
    path = os.path.join(TEXTURES, texture + ".png") if texture else None
    if path and os.path.exists(path):
        image = mat.node_tree.nodes.new("ShaderNodeTexImage")
        image.image = bpy.data.images.load(os.path.abspath(path), check_existing=True)
        image.image.alpha_mode = 'CHANNEL_PACKED'   # a texture's alpha is its smoothness, not its cover
        mix = mat.node_tree.nodes.new("ShaderNodeMix")
        mix.data_type = 'RGBA'
        mix.blend_type = 'MULTIPLY'
        mix.inputs[0].default_value = 1.0
        # a colour mix's A, B and result are the node's seventh, eighth and third sockets: by
        # name it is the number ones that answer, and the tint was a grey of 0.5
        mix.inputs[7].default_value = (*light, 1.0)
        mat.node_tree.links.new(image.outputs["Color"], mix.inputs[6])
        mat.node_tree.links.new(mix.outputs[2], shader.inputs["Base Color"])
    return mat


def export(objects, path):
    # every face of more than four corners cut into triangles here: Unity's importer cuts them itself and
    # left some with none (a die's rounded top, a 20-gon, was a hole; world/check_closed.py counts them)
    for ob in objects:
        if ob.type == 'MESH' and any(len(p.vertices) > 4 for p in ob.data.polygons):
            bm = bmesh.new()
            bm.from_mesh(ob.data)
            bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 4], quad_method='BEAUTY', ngon_method='EAR_CLIP')
            bm.to_mesh(ob.data)
            bm.free()
    bpy.ops.object.select_all(action='DESELECT')
    for ob in objects:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.export_scene.fbx(filepath=path, use_selection=True, axis_forward='-Z', axis_up='Y',
                             apply_scale_options='FBX_SCALE_UNITS', bake_space_transform=True,
                             object_types={'MESH'}, use_mesh_modifiers=True, mesh_smooth_type='OFF',
                             colors_type='LINEAR', add_leaf_bones=False, bake_anim=False)


def triangles(objects):
    graph = bpy.context.evaluated_depsgraph_get()
    total = {}
    for ob in objects:
        mesh = ob.evaluated_get(graph).data
        name = ob.name.rsplit(" ", 1)[0] if ob.name.rsplit(" ", 1)[-1].isdigit() else ob.name   # instances of one asset together
        total[name] = total.get(name, 0) + sum(len(p.vertices) - 2 for p in mesh.polygons)
    return total
