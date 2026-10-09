# Models and surfaces from Poly Haven (polyhaven.com, all CC0), for what a script models badly:
#   python world/fetch.py
# Downloads go to build/polyhaven (not kept in the repository). Written out for Unity:
# Textures/World/<Surface>.png (+ _n.png) and, a material of each model, Textures/Assets/<name>_d.png
# (alpha: smoothness, or opacity for leaves) and <name>_n.jpg, listed in Models/materials.txt.
import json
import os
import urllib.parse
import urllib.request

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "..", "build", "polyhaven")
UNITY = os.path.join(HERE, "..", "unity", "ShaderEmu")

# our surface: (their texture, resolution, turned a quarter, smoothness kept).
# world/materials.py has how many metres each repeats.
SURFACES = {
    "Parquet": ("herringbone_parquet", "2k", False, 0.75),
    "Plaster": ("beige_wall_001", "2k", False, 0.5),
    "Ceiling": ("white_stucco", "1k"),
    "Wood": ("oak_veneer_01", "2k", True),      # turned: our boards' grain runs along u
    "Walnut": ("walnut_veneer", "2k", True),
    "Velvet": ("velour_velvet", "1k"),
    "Linen": ("rough_linen", "1k"),
}

MODELS = ["sofa_02", "modern_coffee_table_01", "round_wooden_table_01", "bar_chair_round_01", "desk_lamp_arm_01", "classic_laptop",
          "television_02", "cardboard_box_01", "wall_clock", "boombox", "potted_plant_02", "potted_plant_04",
          "book_encyclopedia_set_01", "alarm_clock_01", "pachira_aquatica_01",
          # the corridor's
          "korean_fire_extinguisher_01", "painted_wooden_bench", "rubber_boots", "industrial_wall_sconce", "modern_ceiling_lamp_01",
          "korean_public_payphone_01", "vintage_suitcase", "metal_trash_can", "hanging_picture_frame_01", "hanging_picture_frame_02",
          "fire_alarm", "side_table_tall_01", "wicker_basket_01", "ornate_mirror_01"]


def get(url, path):
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path
    os.makedirs(os.path.dirname(path), exist_ok=True)
    request = urllib.request.Request(url, headers={"User-Agent": "ShaderEmu-world-build"})
    with urllib.request.urlopen(request, timeout=120) as response, open(path + ".part", "wb") as out:
        out.write(response.read())
    os.replace(path + ".part", path)
    return path


def files(asset):
    path = get("https://api.polyhaven.com/files/" + asset, os.path.join(CACHE, asset, "files.json"))
    return json.load(open(path, encoding="utf-8"))


def channel(path, index=None):
    image = Image.open(path)
    if index is None:
        return np.asarray(image.convert("RGB")).astype(np.float32) / 255
    return np.asarray(image.convert("RGB")).astype(np.float32)[..., index] / 255


def surface(name, asset, res, turn=False, keep=1.0):
    listing = files(asset)
    maps = {}
    for kind in ("Diffuse", "nor_gl", "Rough", "AO"):
        entry = listing[kind][res]["jpg"]
        maps[kind] = get(entry["url"], os.path.join(CACHE, asset, os.path.basename(entry["url"])))
    colour = channel(maps["Diffuse"])
    smooth = (1.0 - channel(maps["Rough"], 0)) * keep
    out = os.path.join(UNITY, "Textures", "World")
    rgba = np.dstack([colour, smooth])
    packed = np.dstack([channel(maps["AO"], 0), 1.0 - smooth, np.ones_like(smooth)])   # blue: a material's own metallic scales it
    normal = channel(maps["nor_gl"])
    if turn:   # a quarter turn anticlockwise: what pointed right now points up
        rgba, normal, packed = np.rot90(rgba), np.rot90(normal), np.rot90(packed)
        normal = np.dstack([1.0 - normal[..., 1], normal[..., 0], normal[..., 2]])
    Image.fromarray((rgba * 255 + 0.5).astype(np.uint8)).save(os.path.join(out, name + ".png"))
    Image.fromarray((packed * 255 + 0.5).astype(np.uint8)).save(os.path.join(out, name + "_p.png"))
    Image.fromarray((normal * 255 + 0.5).astype(np.uint8)).save(os.path.join(out, name + "_n.png"))
    print("surface %-8s %s %s  mean colour %s  smoothness %.2f" % (name, asset, res, np.round(colour.mean((0, 1)), 2), smooth.mean()))


def model(asset, lines):
    entry = files(asset)["gltf"]["1k"]["gltf"]
    folder = os.path.join(CACHE, asset)
    gltf = get(entry["url"], os.path.join(folder, asset + ".gltf"))
    for relative, item in entry["include"].items():
        get(item["url"], os.path.join(folder, relative))
    document = json.load(open(gltf, encoding="utf-8"))
    out = os.path.join(UNITY, "Textures", "Assets")
    os.makedirs(out, exist_ok=True)

    def image_of(texture):
        if texture is None:
            return None
        source = document["textures"][texture["index"]]["source"]
        return os.path.join(folder, document["images"][source]["uri"])
    for material in document["materials"]:
        name = "PH_" + material["name"]
        pbr = material.get("pbrMetallicRoughness", {})
        colour_path = image_of(pbr.get("baseColorTexture"))
        arm_path = image_of(pbr.get("metallicRoughnessTexture"))
        normal_path = image_of(material.get("normalTexture"))
        cutout = material.get("alphaMode", "OPAQUE") != "OPAQUE"
        if colour_path is None:   # kind 2: glass if it lets light through, else a plain colour
            r, g, b, a = pbr.get("baseColorFactor", [0.5, 0.5, 0.5, 1.0])
            lines.append("%s\t2\t%.3f\t%.3f\t%.3f\t%.3f" % (name, r, g, b, 0.15 if cutout or "lass" in name else a))
            continue
        image = Image.open(colour_path)
        colour = np.asarray(image.convert("RGB")).astype(np.float32) / 255
        metallic, smooth = 0.0, 0.3
        if arm_path is not None:   # red occlusion, green roughness, blue metal
            Image.open(arm_path).convert("RGB").save(os.path.join(out, name + "_p.jpg"), quality=92)
            arm = np.asarray(Image.open(arm_path).convert("RGB").resize(image.size)).astype(np.float32) / 255
            metallic, smooth = float(arm[..., 2].mean()), float(1 - arm[..., 1].mean())
            metallic = 1.0 if metallic > 0.8 else 0.0   # one value a material: mostly metal, or not
            alpha = 1.0 - arm[..., 1]
        else:
            alpha = np.full(colour.shape[:2], smooth, np.float32)
        if cutout:
            alpha = np.asarray(image.convert("RGBA")).astype(np.float32)[..., 3] / 255
        Image.fromarray((np.dstack([colour, alpha]) * 255 + 0.5).astype(np.uint8)).save(os.path.join(out, name + "_d.png"))
        if normal_path is not None:
            Image.open(normal_path).convert("RGB").save(os.path.join(out, name + "_n.jpg"), quality=92)
        lines.append("%s\t%d\t%.3f\t%d\t0\t0" % (name, cutout, metallic, normal_path is not None))
        print("model %-26s material %-34s cutout %d metallic %.2f smoothness %.2f" % (asset, name, cutout, metallic, smooth))


def main():
    for name, spec in SURFACES.items():
        surface(name, *spec)
    lines = []
    for asset in MODELS:
        model(asset, lines)
    os.makedirs(os.path.join(UNITY, "Models"), exist_ok=True)
    open(os.path.join(UNITY, "Models", "materials.txt"), "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
