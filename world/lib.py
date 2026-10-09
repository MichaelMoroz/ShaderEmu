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


class Builder:
    def __init__(self, seed=1):
        self.objects = {}      # name -> (bmesh, [material names])
        self.current = None
        self.stack = [Matrix.Identity(4)]
        self.assets = []       # (asset, matrix, size, triangle limit, parts)
        self.random = random.Random(seed)
        atlas = os.path.join(HERE, "atlas.json")
        self.atlas = json.load(open(atlas)) if os.path.exists(atlas) else {}

    # ---- context
    def obj(self, name):
        self.current = name
        if name not in self.objects:
            self.objects[name] = (bmesh.new(), [])
        return self

    def at(self, at=(0, 0, 0), rot=(0, 0, 0)):
        builder = self

        class Frame:
            def __enter__(self):
                builder.stack.append(builder.stack[-1] @ place(at, rot))

            def __exit__(self, *args):
                builder.stack.pop()
        return Frame()

    def rand(self, a, b):
        return a + self.random.random() * (b - a)

    # ---- putting a finished fragment into the current object
    def _emit(self, bm, mat, at, rot, normal=None):
        world = self.stack[-1] @ place(at, rot)
        bmesh.ops.transform(bm, matrix=C @ world, verts=bm.verts)
        bm.normal_update()
        turn = (C @ world).to_3x3()
        if normal is None:
            bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
            flip = []
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
        target, names = self.objects[self.current]
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
        for f in bm.faces:
            n = f.normal
            ax = max(range(3), key=lambda i: abs(n[i]))
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

    def asset(self, asset, at, rot=(0, 0, 0), size=(None, None, None), limit=None, parts=None, scale=None):
        """One of Poly Haven's models (assets.py), standing on `at` and facing local +z."""
        self.assets.append((asset, self.stack[-1] @ place(at, rot), size, limit, parts, scale))

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

    def lathe(self, at, profile, mat, segs=24, rot=(0, 0, 0), cap=True, uvrect=None):
        """Revolve (radius, height) points round the local y axis."""
        bm = bmesh.new()
        uv = bm.loops.layers.uv.verify()
        rings = []
        for r, y in profile:
            rings.append([bm.verts.new((r * math.cos(2 * math.pi * s / segs), y, r * math.sin(2 * math.pi * s / segs)))
                          for s in range(segs)])
        tu, tv = self._tile(mat)
        along = [0.0]
        for (r0, y0), (r1, y1) in zip(profile, profile[1:]):
            along.append(along[-1] + math.hypot(r1 - r0, y1 - y0))
        girth = 2 * math.pi * max(r for r, _ in profile)
        for i in range(len(profile) - 1):
            for s in range(segs):
                t = (s + 1) % segs
                face = bm.faces.new((rings[i][s], rings[i][t], rings[i + 1][t], rings[i + 1][s]))
                us = (s / segs, (s + 1) / segs, (s + 1) / segs, s / segs)
                vs = (along[i], along[i], along[i + 1], along[i + 1])
                rise, run = abs(profile[i + 1][1] - profile[i][1]), abs(profile[i + 1][0] - profile[i][0])
                flat = rise < 0.35 * run   # a table's top: no rings of grain round its middle
                for loop, u, v in zip(face.loops, us, vs):
                    if uvrect:
                        loop[uv].uv = (uvrect[0] + u * (uvrect[2] - uvrect[0]),
                                       uvrect[1] + v / along[-1] * (uvrect[3] - uvrect[1]))
                    elif flat:
                        loop[uv].uv = (loop.vert.co.x / tu, loop.vert.co.z / tv)
                    else:
                        loop[uv].uv = (v / tu, u * girth / tv)
        if cap:
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
        rings, along = [], [0.0]
        for i in range(n):
            before = dirs[i - 1] if (closed or i > 0) else dirs[0]
            after = dirs[i] if (closed or i < n - 1) else dirs[-1]
            s0, s1 = up.cross(before).normalized(), up.cross(after).normalized()
            side = (s0 + s1) / (1.0 + s0.dot(s1))
            rings.append([bm.verts.new(points[i] + side * a + up * b) for a, b in profile])
            if i > 0:
                along.append(along[-1] + (points[i] - points[i - 1]).length)
        if closed:
            along.append(along[-1] + (points[0] - points[-1]).length)
        tu, tv = self._tile(mat)
        girth = [0.0]
        m = len(profile)
        for j in range(m):
            a0, b0 = profile[j]
            a1, b1 = profile[(j + 1) % m]
            girth.append(girth[-1] + math.hypot(a1 - a0, b1 - b0))
        for i in range(n if closed else n - 1):
            k = (i + 1) % n
            for j in range(m):
                l = (j + 1) % m
                face = bm.faces.new((rings[i][j], rings[i][l], rings[k][l], rings[k][j]))
                us = (along[i], along[i], along[i + 1], along[i + 1])
                vs = (girth[j], girth[j + 1], girth[j + 1], girth[j])
                for loop, u, v in zip(face.loops, us, vs):
                    loop[uv].uv = (u / tu, v / tv)
        if not closed:
            for ring in (rings[0], rings[-1]):
                face = bm.faces.new(ring)
                for loop in face.loops:
                    loop[uv].uv = (0.0, 0.0)
        self._emit(bm, mat, at, rot)

    def tube(self, path, radius, mat, segs=8, at=(0, 0, 0), rot=(0, 0, 0), smooth=0):
        """A round tube through the points of `path`; `smooth` steps a span make a curve of it."""
        bm = bmesh.new()
        uv = bm.loops.layers.uv.verify()
        points = [Vector(p) for p in path]
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
            if i > 0:
                along.append(along[-1] + (points[i] - points[i - 1]).length)
        tu, tv = self._tile(mat)
        for i in range(n - 1):
            for s in range(segs):
                t = (s + 1) % segs
                face = bm.faces.new((rings[i][s], rings[i][t], rings[i + 1][t], rings[i + 1][s]))
                us = (along[i], along[i], along[i + 1], along[i + 1])
                vs = (s / segs, (s + 1) / segs, (s + 1) / segs, s / segs)
                for loop, u, v in zip(face.loops, us, vs):
                    loop[uv].uv = (u / tu, v * 2 * math.pi * radius / tv)
        for ring in (rings[0], rings[-1]):
            bm.faces.new(ring)
        self._emit(bm, mat, at, rot)

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

    def mesh(self, vertices, faces, uvs, mat, at=(0, 0, 0), rot=(0, 0, 0), normal=None, colours=None):
        """Raw geometry: faces index `vertices`; `uvs` (and `colours`) are per face corner."""
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
        self._emit(bm, mat, at, rot, normal)

    # ---- the scene
    def finish(self, collection_name):
        """Make Blender objects of everything built so far, in one collection."""
        collection = bpy.data.collections.new(collection_name)
        bpy.context.scene.collection.children.link(collection)
        made = []
        for name, (bm, names) in self.objects.items():
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
        if self.assets:
            import assets
            for index, (asset, world, size, limit, parts, scale) in enumerate(self.assets):
                made.append(assets.spawn("%s %d" % (asset, index), asset, world, size, limit, parts, scale, collection))
        self.objects, self.assets = {}, []
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
