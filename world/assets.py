# Poly Haven's models (world/fetch.py downloads them) placed among ours. A model keeps its own
# material, renamed PH_<name>; Unity makes that material from the textures fetch.py wrote.
import os

import bmesh
import bpy
from mathutils import Matrix, Vector

from lib import C

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
    for slot, material in enumerate(mesh.materials):
        if material.name.startswith("PH_"):
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
