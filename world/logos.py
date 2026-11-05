# The fetched logos (world/fetch.py's LOGOS, SVG) made pictures for world/textures.py: each imported by
# Blender's SVG reader as curves with their own fills, drawn from straight above in their own colours
# on nothing (alpha), 2048 pixels across:
#   blender -b --python world/logos.py
import os

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
DIR = os.path.join(HERE, "..", "build", "fetched", "logos")
WIDE = 2048


def render(name):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    bpy.ops.import_curve.svg(filepath=os.path.join(DIR, name))
    curves = [o for o in scene.objects if o.type == 'CURVE']
    for ob in curves:
        ob.data.resolution_u = 24
    xs, ys = [], []
    for ob in curves:
        for corner in ob.bound_box:
            p = ob.matrix_world @ __import__("mathutils").Vector(corner)
            xs.append(p.x)
            ys.append(p.y)
    w, h = max(xs) - min(xs), max(ys) - min(ys)
    cam_data = bpy.data.cameras.new("Logo")
    cam_data.type = 'ORTHO'
    cam_data.ortho_scale = max(w, h)
    cam = bpy.data.objects.new("Logo", cam_data)
    scene.collection.objects.link(cam)
    cam.location = ((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, 1.0)
    scene.camera = cam
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.light = 'FLAT'
    scene.display.shading.color_type = 'MATERIAL'
    scene.render.film_transparent = True
    scene.view_settings.view_transform = 'Standard'
    scene.render.resolution_x = WIDE
    scene.render.resolution_y = max(8, int(round(WIDE * h / w)))
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.render.filepath = os.path.join(DIR, name.replace(".svg", ".png"))
    bpy.ops.render.render(write_still=True)
    print("LOGO", name, "%.3f x %.3f" % (w, h), scene.render.filepath)


for name in ("Coca-Cola_logo.svg", "Diet_Coke_logo.svg"):
    render(name)
