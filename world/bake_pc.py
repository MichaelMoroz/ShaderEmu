# The classroom's computer as the room uses it: the monitor's and the tower's bare shells, with
# everything that stands on them (slots, keys, drives, sockets, grilles, lettering, lamps) baked
# into the shells' own pictures, and the glass, keyboard, mouse and leads as they are.
#   blender -b --python world/bake_pc.py
# It writes build/polyhaven/pc_station/pc_station.gltf for world/assets.py, the shells' pictures
# as Textures/Assets/PH_pc_station_{d.png,n.jpg,p.jpg,e.png}, and Models/materials_extra.txt.
import os
import sys
import time

import bpy
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lib          # noqa: E402
import materials    # noqa: E402, F401
import pc           # noqa: E402

SIZE = 4096
RELIEF = 2.2  # how steeply the rim of what is dark leans in the normal map
FACE = 2.6    # how much larger a front's or a back's place in the picture is: the lettering and sockets are there
UNITY = os.path.join(HERE, "..", "unity", "ShaderEmu")
OUT = os.path.join(HERE, "..", "build", "polyhaven", "pc_station")


def built(mode, which, name):
    pc.MODE = mode
    b = lib.Builder(seed=7)
    b.obj(name)
    pc.station(b, which)
    ob = b.finish(name)[0]
    pc.MODE = "full"
    return ob


def only(*objects):
    bpy.ops.object.select_all(action='DESELECT')
    for ob in objects:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = objects[-1]


def pixels(image):
    a = np.empty(SIZE * SIZE * 4, dtype=np.float32)
    image.pixels.foreach_get(a)
    return a.reshape(SIZE, SIZE, 4)


def saved(array, path, colour):
    image = bpy.data.images.new("out", SIZE, SIZE, alpha=False)
    if colour:   # the bake is linear light: a colour picture is kept in sRGB
        rgb = array[..., :3]
        array = array.copy()
        array[..., :3] = np.where(rgb <= 0.0031308, rgb * 12.92, 1.055 * np.power(np.maximum(rgb, 0), 1 / 2.4) - 0.055)
    else:
        image.colorspace_settings.name = 'Non-Color'
    array[..., 3] = 1
    image.pixels.foreach_set(np.clip(array, 0, 1).astype(np.float32).ravel())
    image.filepath_raw = path
    image.file_format = 'PNG' if path.endswith(".png") else 'JPEG'
    image.save()
    bpy.data.images.remove(image)


def enlarge_faces(ob):
    """Each island of the picture that looks to the front or the back (Blender's y) is made FACE
    times larger about its middle. An island is the faces joined by an edge whose ends have one
    place in the picture."""
    import bmesh
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    uv = bm.loops.layers.uv.verify()
    seen = set()
    for start in bm.faces:
        if start in seen:
            continue
        island, todo = [], [start]
        seen.add(start)
        while todo:
            face = todo.pop()
            island.append(face)
            for loop in face.loops:
                for other in loop.edge.link_loops:
                    f = other.face
                    if f in seen:
                        continue
                    # the edge is shared in the picture if both its ends are at one place for both faces
                    a, b = loop[uv].uv, loop.link_loop_next[uv].uv
                    c, d = other.link_loop_next[uv].uv, other[uv].uv
                    if (a - c).length < 1e-5 and (b - d).length < 1e-5:
                        seen.add(f)
                        todo.append(f)
        area = sum(f.calc_area() for f in island)
        facing = sum(abs(f.normal.y) * f.calc_area() for f in island) / max(area, 1e-9)
        if facing < 0.8:
            continue
        loops = [loop for f in island for loop in f.loops]
        middle = sum((loop[uv].uv for loop in loops), type(loops[0][uv].uv)((0, 0))) / len(loops)
        for loop in loops:
            loop[uv].uv = middle + (loop[uv].uv - middle) * FACE
    bm.to_mesh(ob.data)
    bm.free()


def main():
    start = time.time()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = 16
    try:
        preferences = bpy.context.preferences.addons['cycles'].preferences
        preferences.compute_device_type = 'OPTIX'
        preferences.get_devices()
        for device in preferences.devices:
            device.use = True
        scene.cycles.device = 'GPU'
    except Exception as problem:   # no card it can use: the processor bakes
        print("BAKE on the processor:", problem)

    high = built("high", "shell", "high")
    shell = built("low", "shell", "pc_station")
    rest = built("low", "rest", "pc_station rest")
    for ob in (high, shell):
        only(ob)
        for modifier in list(ob.modifiers):
            bpy.ops.object.modifier_apply(modifier=modifier.name)

    # What on the shell is not of the baked skin goes to the rest: a material left on the shell
    # makes its own texture a target of the bake, which is cleared, and the plastic baked black.
    only(shell)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='DESELECT')
    bpy.ops.object.mode_set(mode='OBJECT')
    skin = shell.data.materials.find("PCBaked")
    other = [p for p in shell.data.polygons if p.material_index != skin]
    for p in other:
        p.select = True
    if other:
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.separate(type='SELECTED')
        bpy.ops.object.mode_set(mode='OBJECT')
        parted = [ob for ob in bpy.context.selected_objects if ob is not shell]
        only(*parted, rest)
        bpy.ops.object.join()
        only(shell)
        for i in reversed(range(len(shell.data.materials))):
            if i != skin:
                shell.data.materials.pop(index=i)

    # the shell's own picture: every face its own place in it, as large as its area
    only(shell)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=1.15, island_margin=0.004, area_weight=0.0, correct_aspect=True)
    bpy.ops.object.mode_set(mode='OBJECT')
    enlarge_faces(shell)
    only(shell)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.select_all(action='SELECT')
    bpy.ops.uv.pack_islands(rotate=True, scale=True, margin=0.003)   # packed again, their sizes to one another kept
    bpy.ops.object.mode_set(mode='OBJECT')
    material = bpy.data.materials["PCBaked"]
    material.name = "pc_station"
    nodes = material.node_tree.nodes
    target = nodes.new("ShaderNodeTexImage")
    nodes.active = target

    def bake(kind, colour, samples=16, **more):
        image = bpy.data.images.new(kind, SIZE, SIZE, alpha=False, float_buffer=True)
        if not colour:
            image.colorspace_settings.name = 'Non-Color'
        target.image = image
        scene.cycles.samples = samples
        only(high, shell)
        bpy.ops.object.bake(type=kind, use_selected_to_active=True, cage_extrusion=0.016, max_ray_distance=0.03, margin=6, **more)
        result = pixels(image)
        bpy.data.images.remove(image)
        print("BAKED %s at %.0f s" % (kind, time.time() - start))
        return result

    normal = bake('NORMAL', False, 4, normal_space='TANGENT')
    rough = bake('ROUGHNESS', False, 4)
    # the lamps are dark in the picture: each place's are lit by its own machine (ShaderEmuStations.cs)
    for used in high.data.materials:
        if used.name.split('.')[0] in ("Led", "Amber"):
            used.node_tree.nodes["Principled BSDF"].inputs["Emission Strength"].default_value = 0.0
    light = bake('EMIT', True, 4)
    shade = bake('AO', False, 48)
    # the colour last, as light: each material gives out its own colour and nothing else, so the
    # picture is the colour itself and not what a lamp makes of it
    for used in high.data.materials:
        tree = used.node_tree
        shader = tree.nodes.get("Principled BSDF")
        base = shader.inputs["Base Color"]
        if base.links:
            tree.links.new(base.links[0].from_socket, shader.inputs["Emission Color"])
        else:
            shader.inputs["Emission Color"].default_value = base.default_value
        if used.name.split('.')[0] in ("Led", "Amber"):   # an unlit lamp's lens is dull
            shader.inputs["Emission Color"].default_value = [0.22 * c for c in base.default_value[:3]] + [1.0]
        shader.inputs["Emission Strength"].default_value = 1.0
    colour = bake('EMIT', True, 4)
    print("BAKE colour: %.0f%% of the picture is the shell's" % (100 * (colour[..., :3].sum(2) > 0).mean()))

    # Slots, grilles and holes are dark boxes a hair proud of the shell: there is no hollow for the
    # bake to find. So what is dark in the colour is let into the surface here: its edges lean
    # in the normal map as a hollow's do, and its floor is in shade.
    seen = colour[..., :3] @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    covered = seen > 0.004   # nothing of the shell is as black as the picture's empty parts
    dark = (seen < 0.03) & covered

    def soft(a, times):
        for _ in range(times):   # a binomial blur, each way
            a = (np.roll(a, 1, 0) + 2 * a + np.roll(a, -1, 0)) / 4
            a = (np.roll(a, 1, 1) + 2 * a + np.roll(a, -1, 1)) / 4
        return a
    hollow = soft(dark.astype(np.float32), 3)
    inside = soft(covered.astype(np.float32), 4) > 0.999   # not at an island's rim, where "dark" is only the picture's end
    lean_u = (np.roll(hollow, -1, 1) - np.roll(hollow, 1, 1)) * RELIEF
    lean_v = (np.roll(hollow, -1, 0) - np.roll(hollow, 1, 0)) * RELIEF
    n = normal[..., :3] * 2 - 1
    n[..., 0] += np.where(inside, lean_u, 0)
    n[..., 1] += np.where(inside, lean_v, 0)
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
    normal = normal.copy()
    normal[..., :3] = np.where(covered[..., None], n * 0.5 + 0.5, normal[..., :3])
    shade = shade.copy()
    shade[..., 0] *= 1 - 0.55 * np.where(inside, hollow, 0)
    print("BAKE relief: %.1f%% of the picture is let in" % (100 * (dark & inside).mean()))

    textures = os.path.join(UNITY, "Textures", "Assets")
    stem = os.path.join(textures, "PH_pc_station")
    saved(colour, stem + "_d.png", True)
    saved(normal, stem + "_n.jpg", False)
    packed = np.ones((SIZE, SIZE, 4), dtype=np.float32)
    covered = colour[..., :3].sum(2) > 0.05
    open_face = np.percentile(shade[..., 0][covered], 90)   # what a face with nothing near it gets
    packed[..., 0] = np.clip(0.4 + 0.6 * shade[..., 0] / open_face, 0, 1)   # occlusion: the slots and seams keep their shade
    packed[..., 1] = np.maximum(rough[..., 0], np.where(inside, 0.5 + 0.45 * hollow, 0))   # a hollow mirrors nothing
    packed[..., 2] = 0
    saved(packed, stem + "_p.jpg", False)
    saved(np.clip(light, 0, 1), stem + "_e.png", True)
    open(os.path.join(UNITY, "Models", "materials_extra.txt"), "w").write("PH_pc_station\t0\t0.00\t1\t0\t0\n")

    # the set, for world/assets.py: the shell with its picture's place on it, and the rest
    bpy.data.objects.remove(high, do_unlink=True)
    target.image = None
    os.makedirs(OUT, exist_ok=True)
    only(shell, rest)
    bpy.ops.export_scene.gltf(filepath=os.path.join(OUT, "pc_station.gltf"), export_format='GLTF_SEPARATE', use_selection=True, export_apply=True)
    # the tower alone, for the den's own computer (world/computer.py): the same faces and picture,
    # its foot's middle at the file's origin
    import bmesh
    alone = shell.copy()
    alone.data = shell.data.copy()
    alone.name = "pc_tower"
    bpy.context.scene.collection.objects.link(alone)
    bm = bmesh.new()
    bm.from_mesh(alone.data)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.calc_center_median().x > -(pc.TOWER_X - 0.15)], context='FACES')   # Blender's x is Unity's, turned
    bmesh.ops.translate(bm, verts=bm.verts, vec=(pc.TOWER_X, pc.TOWER_Z, 0))
    bm.to_mesh(alone.data)
    bm.free()
    out = os.path.join(OUT, "..", "pc_tower")
    os.makedirs(out, exist_ok=True)
    only(alone)
    bpy.ops.export_scene.gltf(filepath=os.path.join(out, "pc_tower.gltf"), export_format='GLTF_SEPARATE', use_selection=True, export_apply=True)
    print("BAKE the tower alone: %d triangles" % sum(len(p.vertices) - 2 for p in alone.data.polygons))
    count = sum(len(p.vertices) - 2 for ob in (shell, rest) for p in ob.data.polygons)
    print("BAKE done: %d triangles in the set, %.0f s" % (count, time.time() - start))


main()
