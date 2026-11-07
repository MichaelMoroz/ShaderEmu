# Every model's place in the lightmap ("UV2", Unity's second coordinates), laid out here by
# Blender and not by Unity's importer (whose charts of a mug were slivers): cut along every sharp
# edge and wherever the picture's own coordinates are cut (a lathe's bands and its one meridian, a
# tube's line along its run, a box's faces, a downloaded model's islands as its artist drew them),
# each piece unwrapped by angle (developable pieces unroll with no stretch), any piece that came out
# folded or stretched laid again by Smart UV Project, all at one density and packed. The shell keeps
# its own (world/shell.py: one chart a surface, so no line shows where two pieces of a wall met).
import hashlib
import math
import os

import bmesh
import bpy
import numpy as np

CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "unwrap")   # a layout a mesh, by its key

SHARP = 50.0       # degrees between two faces' normals at which an edge is a seam: a lightmap's light breaks there anyway (a 45-degree bevel is no seam: its pieces were slivers)
FOLD = 4.0         # a piece whose faces' share of the picture differ by more than this times from its middle one is laid again
MARGIN_TEXELS = 2.5   # between pieces, in the lightmap's texels (Bakery's "Adjust UV padding" works only on Unity's own charts)
BAKE_TEXELS = 20.0    # Bakery's texels a metre (its settings' Texels per unit), before a renderer's scale (ShaderEmuModels.cs: BakeTexels)


def lightmap_scale(name):
    """A renderer's scale in the lightmap, as ShaderEmuModels.cs's LightmapScale gives it (the least: Unity may give more)."""
    if name in ("Books", "Shelf", "Rack") or name.startswith("book_"):
        return 2.0
    if name in ("Screen housings", "Desk", "Consoles", "Tower", "Radiator"):
        return 1.5
    if name in ("Desk things", "Table things", "Room desk things"):
        return 8.0
    if name.endswith(" things"):
        return 4.0
    if name in ("Cables", "Network", "Hubs"):
        return 4.0
    if name == "Panelling":
        return 0.7
    if name.startswith("Station ") and name.endswith(" rest"):
        return 6.0
    if name.startswith("pachira") or name.startswith("potted_plant"):
        return 0.35
    return 1.0


def _side(ob):
    """How many metres of the surface one side of the mesh's square holds."""
    me, scale = ob.data, ob.matrix_world.to_3x3().determinant()
    surface = sum(p.area for p in me.polygons) * abs(scale) ** (2.0 / 3.0)
    uv = me.uv_layers["UV2"].data
    laid = 0.0
    for p in me.polygons:
        pts = [uv[i].uv for i in p.loop_indices]
        laid += abs(sum(pts[i].x * pts[i - 1].y - pts[i - 1].x * pts[i].y for i in range(len(pts)))) / 2
    return math.sqrt(surface / laid) if laid > 0 else 0.0


def _seams(bm, uv):
    """Marks the edges where the lightmap may be cut: sharp ones, open ones, and those the picture is cut along
    but for the ones a primitive marked joinable (lib._joinable: round a lathe's or a tube's rings)."""
    join = bm.edges.layers.int.get("lm_join")
    for e in bm.edges:
        e.seam = False
        if len(e.link_faces) != 2:
            e.seam = True
            continue
        if e.calc_face_angle(0.0) > math.radians(SHARP):
            e.seam = True
            continue
        if uv is None or (join is not None and e[join]):
            continue
        a, b = e.link_faces
        for v in e.verts:
            ua = next(l[uv].uv for l in a.loops if l.vert is v)
            ub = next(l[uv].uv for l in b.loops if l.vert is v)
            if (ua - ub).length > 1e-5:
                e.seam = True
                break


def _islands(bm):
    """The faces in groups joined across edges that are no seams."""
    seen, out = set(), []
    for f in bm.faces:
        if f.index in seen:
            continue
        group, todo = [], [f]
        seen.add(f.index)
        while todo:
            g = todo.pop()
            group.append(g)
            for e in g.edges:
                if e.seam:
                    continue
                for h in e.link_faces:
                    if h.index not in seen:
                        seen.add(h.index)
                        todo.append(h)
        out.append(group)
    return out


def _uv_area(face, uv):
    pts = [l[uv].uv for l in face.loops]
    return sum(pts[i].x * pts[i - 1].y - pts[i - 1].x * pts[i].y for i in range(len(pts))) / 2


def _bad(bm, uv2):
    """The faces of pieces that the unwrap folded (faces turned over) or stretched."""
    bad = []
    for group in _islands(bm):
        if _bad_group(group, uv2):
            bad.extend(group)
    return bad


def _bad_group(group, uv2):
    """Whether one piece is folded (faces turned over) or stretched in uv2."""
    shares = [(f, _uv_area(f, uv2), f.calc_area()) for f in group]
    live = [(a, s) for _, a, s in shares if s > 1e-12]
    back = sum(1 for a, _ in live if a < 0)
    turned = min(back, len(live) - back)   # (a piece mirrored whole is no harm: one with both windings is folded)
    sign = -1.0 if back * 2 > len(live) else 1.0
    ratios = sorted(sign * a / s for a, s in live if sign * a > 0)
    if not ratios:
        return True
    middle = ratios[len(ratios) // 2]
    return bool(turned or ratios[0] < middle / FOLD or ratios[-1] > middle * FOLD)


def unwrap(objects):
    """Lays out UV2 of every mesh of these objects (a mesh shared by instances once)."""
    done, counts = set(), [0, 0]
    meshes = [ob for ob in objects if ob.type == 'MESH' and ob.data.polygons and ob.data.name not in done and not done.add(ob.data.name)]
    if not meshes:
        return
    if bpy.context.object is not None and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    # a mesh laid out before with the same shape and picture takes its layout from the cache
    os.makedirs(CACHE, exist_ok=True)
    keys, fresh = {}, []
    for ob in meshes:
        keys[ob.data.name] = key = _key(ob.data)
        kept = os.path.join(CACHE, key + ".npy")
        if os.path.exists(kept):
            _layer(ob.data).data.foreach_set("uv", np.load(kept).ravel())
        else:
            fresh.append(ob)
    if fresh:
        undo = bpy.context.preferences.edit.use_global_undo
        bpy.context.preferences.edit.use_global_undo = False   # (each operator's undo step copied the whole file: 10 s an object)
        try:
            _unwrap(fresh, counts)
        finally:
            bpy.context.preferences.edit.use_global_undo = undo
        for ob in fresh:
            uv = np.empty(len(ob.data.loops) * 2, dtype=np.float32)
            ob.data.uv_layers["UV2"].data.foreach_get("uv", uv)
            np.save(os.path.join(CACHE, keys[ob.data.name] + ".npy"), uv)
    print("UNWRAP %d meshes (%d from the cache), %d faces laid out: %d of them again by Smart UV Project"
          % (len(meshes), len(meshes) - len(fresh), counts[0], counts[1]) + (": new %s" % ", ".join(ob.data.name for ob in fresh) if fresh else ""))


def _key(me):
    """What a mesh's layout depends on: its points, its faces' corners and its picture's coordinates."""
    h = hashlib.sha1(b"unwrap 5 %r %r %r %r %r %r" % (SHARP, FOLD, MARGIN_TEXELS, BAKE_TEXELS, lightmap_scale(me.name), bpy.app.version))
    co = np.empty(len(me.vertices) * 3, dtype=np.float32)
    me.vertices.foreach_get("co", co)
    corners = np.empty(len(me.loops), dtype=np.int32)
    me.loops.foreach_get("vertex_index", corners)
    sizes = np.empty(len(me.polygons), dtype=np.int32)
    me.polygons.foreach_get("loop_total", sizes)
    for a in (co, corners, sizes):
        h.update(a.tobytes())
    if "lm_join" in me.attributes:
        join = np.empty(len(me.edges), dtype=np.int32)
        me.attributes["lm_join"].data.foreach_get("value", join)
        h.update(join.tobytes())
    if me.uv_layers and me.uv_layers[0].name != "UV2":
        uv = np.empty(len(me.loops) * 2, dtype=np.float32)
        me.uv_layers[0].data.foreach_get("uv", uv)
        h.update(uv.tobytes())
    return h.hexdigest()


def _layer(me):
    """The mesh's second coordinates, named UV2 (Unity's lightmap takes the second: a third would be lost)."""
    while len(me.uv_layers) > 2:
        me.uv_layers.remove(me.uv_layers[-1])
    if len(me.uv_layers) < 2:
        me.uv_layers.new(name="UV2")
    me.uv_layers[1].name = "UV2"
    return me.uv_layers[1]


def _unwrap(meshes, counts):
    for ob in meshes:
        me = ob.data
        _layer(me)
        bm = bmesh.new()
        bm.from_mesh(me)
        picture = bm.loops.layers.uv[me.uv_layers[0].name] if me.uv_layers[0].name != "UV2" else None
        _seams(bm, picture)
        bm.to_mesh(me)
        bm.free()
        me.uv_layers.active_index = 1
    # one edit of them all: Blender unwraps and packs each mesh in its own square
    bpy.ops.object.select_all(action='DESELECT')
    for ob in meshes:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.unwrap(method='ANGLE_BASED', fill_holes=True, margin=0.0)
    bpy.ops.object.mode_set(mode='OBJECT')
    # what came out folded or stretched: laid again by Smart UV Project, its faces alone
    again = []
    for ob in meshes:
        bm = bmesh.new()
        bm.from_mesh(ob.data)
        bm.faces.ensure_lookup_table()
        bad = {f.index for f in _bad(bm, bm.loops.layers.uv["UV2"])}
        bm.free()
        # (the faces alone, by faces: the first edit's select_all left every vertex selected in the mesh, and in
        # vertex mode those made every face selected again, so Smart UV Project laid out the whole mesh anew in
        # pieces, not its bad faces: until 7 November 2026 the mouse's smooth shell was 13 charts by that alone)
        for v in ob.data.vertices:
            v.select = False
        for e in ob.data.edges:
            e.select = False
        for p in ob.data.polygons:
            p.select = p.index in bad
        counts[0] += len(ob.data.polygons)
        counts[1] += len(bad)
        if bad:
            again.append(ob)
    if again:
        bpy.ops.object.select_all(action='DESELECT')
        for ob in again:
            ob.select_set(True)
        bpy.context.view_layer.objects.active = again[0]
        bpy.context.tool_settings.mesh_select_mode = (False, False, True)
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=0.0, area_weight=0.0, correct_aspect=True, scale_to_bounds=False)
        bpy.ops.object.mode_set(mode='OBJECT')
    # one density over each mesh, and packed in a square of its own (in one edit together they would share one),
    # MARGIN_TEXELS apart at the density Bakery bakes it at: packed once to learn how many metres its square is, then again
    tight = []

    def _pack(margin):
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.select_all(action='SELECT')   # (out of the edit and in again, the UV selection is gone: a pack of nothing)
        bpy.ops.uv.select_all(action='SELECT')
        bpy.ops.uv.pack_islands(udim_source='CLOSEST_UDIM', shape_method='AABB', rotate=True, rotate_method='AXIS_ALIGNED',   # (its finer shapes took 30 s on one model)
                                margin_method='FRACTION', margin=margin)
        bpy.ops.object.mode_set(mode='OBJECT')

    for ob in meshes:
        bpy.ops.object.select_all(action='DESELECT')
        ob.select_set(True)
        bpy.context.view_layer.objects.active = ob
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.uv.select_all(action='SELECT')
        bpy.ops.uv.average_islands_scale()
        bpy.ops.object.mode_set(mode='OBJECT')
        _pack(0.001)
        first = _side(ob)
        layer = ob.data.uv_layers["UV2"].data
        packed = np.empty(len(layer) * 2, dtype=np.float32)
        layer.foreach_get("uv", packed)
        want = min(0.05, MARGIN_TEXELS / (BAKE_TEXELS * lightmap_scale(ob.name)) / first) if first > 0 else 0.001
        margin = want
        while margin > 0.001:   # (a mesh of thousands of pieces has no room for the margin it wants: the packer shrank them to points)
            _pack(margin)
            if _side(ob) < first * 1.5:
                break
            ob.data.uv_layers["UV2"].data.foreach_set("uv", packed)   # (and what it shrank stays shrunk: the layout of before again)
            margin /= 2
        else:
            margin = 0.001
        if margin < want * 0.99:
            tight.append("%s (%.1f texels)" % (ob.name, MARGIN_TEXELS * margin / want))
        bpy.ops.object.mode_set(mode='OBJECT')
    if tight:
        print("UNWRAP margins under %.1f texels at the least scale (Unity gives these more lightmap): %s" % (MARGIN_TEXELS, ", ".join(tight)))
    for ob in meshes:
        ob.data.uv_layers.active_index = 0
        for p in ob.data.polygons:
            p.select = False
