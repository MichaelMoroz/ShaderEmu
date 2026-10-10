# Builds the room's models and writes them as FBX for Unity:
#   blender -b --python world/build.py -- [Room Shell Computer Furniture Annex City Gamepad]
# In a Blender that is open, run it from the text editor to look at the result.
import json
import os
import sys
import time

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
for name in ("lib", "materials", "assets", "bookdesigns", "room", "computer", "pc", "furniture", "annex", "shell", "city", "gamepad"):
    sys.modules.pop(name, None)   # so a second run in an open Blender picks up edits

import lib          # noqa: E402
import materials    # noqa: E402, F401
import room         # noqa: E402
import computer     # noqa: E402
import furniture    # noqa: E402
import annex        # noqa: E402
import shell        # noqa: E402
import city         # noqa: E402
import gamepad      # noqa: E402

PARTS = {"Room": room, "Shell": shell, "Computer": computer, "Furniture": furniture, "Annex": annex, "City": city, "Gamepad": gamepad}
OUT = os.path.join(HERE, "..", "unity", "ShaderEmu", "Models")


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    wanted = args or list(PARTS)
    if bpy.app.background:
        bpy.ops.wm.read_factory_settings(use_empty=True)
    os.makedirs(OUT, exist_ok=True)
    report = {}
    for name in wanted:
        start = time.time()
        old = bpy.data.collections.get(name)
        if old is not None:
            for ob in list(old.objects):
                bpy.data.objects.remove(ob, do_unlink=True)
            bpy.data.collections.remove(old)
        builder = lib.Builder(seed=7)
        PARTS[name].build(builder)
        objects = builder.finish(name)
        lib.export(objects, os.path.join(OUT, name + ".fbx"))
        report[name] = lib.triangles(objects)
        print("BUILT %s: %d objects, %d triangles, %.1f s" % (name, len(objects), sum(report[name].values()), time.time() - start))
    if not args:
        json.dump(report, open(os.path.join(HERE, "..", "build", "world_triangles.json"), "w"), indent=1)
    if bpy.app.background:
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(os.path.join(HERE, "..", "build", "world.blend")))


main()
