# The games and the software of the decade in their boxes, and its magazines, from scans of the
# real things, fetched when the world is made and never kept in the repository (their pictures
# are the publishers'; the sites that scanned them only show them):
#   python world/gameboxes.py
# Downloads go to build/gameboxes. Written: Textures/Fetched/GameBoxes.png and GameBoxes2.png (every box's six faces),
# Textures/Fetched/Magazines.png (every magazine's), their gloss (_p), and world/gameboxes.json
# (each thing's size, atlas and faces), which world/furniture.py, computer.py and annex.py place.
import concurrent.futures
import json
import os
import re
import struct
import subprocess
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "..", "build", "gameboxes")
OUT = os.path.join(HERE, "..", "unity", "ShaderEmu", "Textures", "Fetched")   # out of git: .gitignore
FACES = ("front", "back", "left", "right", "top", "bottom")
ATLAS = 4096
GUTTER = 8   # pixels between faces, filled with each face's own edge

# A face's picture as it is seen from outside: the top's top edge is at the back, the bottom's at
# the front, a side's left edge at the front (right) or the back (left). Sizes are millimetres.
BBC = "https://bigboxcollection.com/images/"
BOXES = {
    # bigboxcollection.com: six pictures and a gloss map a face; three.js's box takes them as they are
    "doom": dict(bbc="DoomShareware", gloss="_default/14", mm=(167, 211, 19)),
    "quake": dict(bbc="Quake1", gloss="Quake1", mm=(182, 232, 43)),
    "steel_sky": dict(bbc="BeneathASteelSky", gloss="BeneathASteelSky", mm=(184, 232, 47)),
    # bigboxdb.com: a box's model, its faces in one KTX2 picture
    "red_alert": dict(bbdb="command-and-conquer-red-alert", variant=47, gloss=0.85),
    # gamebox.museum: photographs of the six faces on grass, in one picture. Each face's corners
    # there, upper left, lower left, lower right, upper right of the face as it is seen
    "command_conquer": dict(museum="GRSjvfkvzUKCsGVbIuFrOA", gloss=0.85, corners={
        "front": ((34, 48), (34, 2100), (1500, 2100), (1500, 48)),
        "back": ((1557, 30), (1557, 2102), (3057, 2102), (3057, 30)),
        "left": ((35, 2603), (1655, 2603), (1655, 2150), (35, 2150)),
        "right": ((35, 3062), (1655, 3062), (1655, 2615), (35, 2615)),
        "top": ((3042, 2603), (3042, 2155), (1718, 2155), (1718, 2603)),
        "bottom": ((1718, 2618), (1718, 3060), (3044, 3060), (3044, 2618))}),
    # the software of the decade, bigboxdb.com's variants of it
    "msdos6": dict(bbdb="ms-dos-6-upgrade", variant=625, gloss=0.8),
    "windows31": dict(bbdb="windows-3-1", variant=628, gloss=0.8),
    "windows95": dict(bbdb="windows-95-upgrade", variant=629, gloss=0.85),
    "netscape": dict(bbdb="netscape-navigator", variant=626, gloss=0.8),
    "flight_sim5": dict(bbdb="microsoft-flight-simulator-5-0", variant=434, gloss=0.8),
    "entertainment_pack": dict(bbdb="microsoft-entertainment-pack-the-best-of", variant=564, gloss=0.8),
}
# More games of the decade from bigboxcollection.com, by its name for each: a size is read from the
# box's page there, and a box without gloss maps of its own takes the site's default.
for _name, _bbc in (("doom2", "Doom2"), ("ultimate_doom", "DoomUltimate"), ("duke3d", "DukeNukem3D"), ("heretic", "Heretic"), ("hexen", "Hexen"),
                    ("quake2", "Quake2"), ("half_life", "HalfLife"), ("descent", "Descent1"), ("dark_forces", "DarkForces"),
                    ("system_shock", "SystemShock"), ("fallout", "Fallout"), ("fallout2", "Fallout2"), ("baldurs_gate", "BaldursGate"),
                    ("diablo", "DiabloUS"), ("warcraft2", "WarCraft2"), ("civ2", "Civilization2"), ("simcity", "SimCityClassic"),
                    ("simcity2000", "SimCity2000DE"), ("monkey2", "MonkeyIsland2"), ("tentacle", "DayOfTheTentacle"), ("sam_max", "SamNMax"),
                    ("full_throttle", "Vollgas"), ("grim_fandango", "GrimFandango"), ("the_dig", "TheDigDE"), ("syndicate", "Syndicate"),
                    ("underworld", "UltimaUnderworldEU"), ("ultima7", "Ultima7"), ("wing_commander3", "WingCommanderIII"), ("xwing", "SWXWing"),
                    ("lost_vikings", "LostVikings"), ("lemmings2", "Lemmings2"), ("worms", "WormsEU"), ("rayman", "Rayman"),
                    ("earthworm_jim", "EarthwormJim"), ("arena", "TESArena"), ("daggerfall", "DaggerfallDE"), ("settlers2", "Settlers2"),
                    ("age_of_empires", "AgeOfEmpires"), ("unreal", "UnrealUK"), ("id_anthology", "idAnthology"), ("broken_sword", "BrokenSword"),
                    ("flashback", "Flashback"), ("alone_dark", "AitD1"), ("jedi_knight", "JediKnight"), ("thief", "DarkProject")):
    BOXES[_name] = dict(bbc=_bbc)

# Magazines of the decade, as archive.org has their scans (its collections computergamingworld,
# byte-magazine, pc-computing-magazine, linuxjournalmagazine): the front is the cover, the back the
# last page, the spine the cover's own edge colour, the pages' edges drawn, 0.05 mm a leaf.
MAGAZINES = ["Computer_Gaming_World_Issue_101", "Computer_Gaming_World_Issue_116", "Computer_Gaming_World_Issue_124",
             "Computer_Gaming_World_Issue_132", "Computer_Gaming_World_Issue_140", "Computer_Gaming_World_Issue_148",
             "Computer_Gaming_World_Issue_156", "Computer_Gaming_World_Issue_164", "Computer_Gaming_World_Issue_180",
             "BYTEVolume20Number12", "BYTE-1993-03", "ByteVol21No11", "BYTEVol1808199307PentiumPCs", "BYTEVolume22NUmber01", "ByteV19N9",
             "pc-computing-magazine-v7i9", "pc-computing-magazine-v8i11", "pc-computing-magazine-v7i3",
             "pc-computing-magazine-v10i6", "PC_Computing_1998_05", "LinuxJournalIssue58"]
MAGAZINE_WIDE = 1600   # the scans are fetched this wide


def get(url, name, data=None, missing_ok=False):
    path = os.path.join(CACHE, name)
    if os.path.exists(path) and (os.path.getsize(path) > 0 or missing_ok):   # (what may be missing may be an empty answer too)
        return path
    os.makedirs(os.path.dirname(path), exist_ok=True)
    request = urllib.request.Request(url, data=data, headers={"User-Agent": "Mozilla/5.0 ShaderEmu-world-build"})
    try:
        with urllib.request.urlopen(request, timeout=120) as response, open(path + ".part", "wb") as out:
            out.write(response.read())
    except urllib.error.HTTPError as e:
        if e.code == 404 and missing_ok:
            return None
        raise
    os.replace(path + ".part", path)
    return path


def quad(img, corners, size):
    """The face whose corners in `img` are `corners`, drawn upright at `size` pixels."""
    (a, b, c, d) = corners
    return img.transform(size, Image.QUAD, (*a, *b, *c, *d), Image.BICUBIC)


def bbc_size(name):
    """A box's size in millimetres, as its page at bigboxcollection.com says it."""
    page = open(get("https://bigboxcollection.com/generateBoxView.php?box=" + name, name + "/page.html"), encoding="utf-8", errors="replace").read()
    found = re.search(r"(\d+(?:\.\d+)?) &times; (\d+(?:\.\d+)?) &times; (\d+(?:\.\d+)?) mm", page)
    return tuple(float(v) for v in found.groups())


def from_bbc(box):
    faces, gloss = {}, {}
    for face in FACES:
        path = get(BBC + "textures/%s/%s.webp" % (face, box["bbc"]), "%s/%s.webp" % (box["bbc"], face), missing_ok=face in ("top", "bottom"))
        if path is None:   # a box scanned without its top or bottom: plain card the colour of the front's edge there
            front = np.asarray(faces["front"])
            edge = front[: max(2, front.shape[0] // 80)] if face == "top" else front[-max(2, front.shape[0] // 80):]
            faces[face] = Image.new("RGB", (256, 64), tuple(int(v) for v in np.median(edge.reshape(-1, 3), 0)))
        else:
            faces[face] = Image.open(path).convert("RGB")
        g = box.get("gloss") or box["bbc"]
        g = g if g.startswith("_") else "%s/%s" % (face, g)
        path = get(BBC + "specularmaps/%s.webp" % g, "%s/gloss_%s.webp" % (box["bbc"], face), missing_ok=True)
        if path is None or os.path.getsize(path) == 0:   # no gloss maps of its own (or an empty answer)
            path = get(BBC + "specularmaps/_default/14.webp", "_default/gloss_14.webp")
        gloss[face] = Image.open(path).convert("L")
    return faces, gloss, box.get("mm") or bbc_size(box["bbc"])


def from_archive(ident):
    """A magazine: its cover and its last page as archive.org's reader has them, its spine and its pages' edges drawn."""
    meta = json.load(open(get("https://archive.org/metadata/%s/metadata" % ident, "mag/%s/meta.json" % ident)))["result"]
    pages = int(meta.get("imagecount", 100))
    page = lambda n: Image.open(get("https://archive.org/download/%s/page/n%d_w%d.jpg" % (ident, n, MAGAZINE_WIDE),
                                    "mag/%s/n%d.jpg" % (ident, n))).convert("RGB")
    w, h = 213.0, 276.0   # a magazine's trim; a scan with more round it is cut to that shape about its middle

    def trim(img):
        x, y = img.size
        if y / x > h / w:
            cut = int((y - x * h / w) / 2)
            return img.crop((0, cut, x, y - cut))
        cut = int((x - y * w / h) / 2)
        return img.crop((cut, 0, x - cut, y))
    front = trim(page(0))
    look = lambda img: np.asarray(img.resize((32, 40), Image.BILINEAR), np.float32)
    for n in range(pages - 1, pages - 4, -1):   # the back: the last page that is not the cover again (some scans end with it)
        back = trim(page(n))
        if np.abs(look(back) - look(front)).mean() > 20:
            break
    d = max(1.5, round(pages / 2 * 0.05, 1))
    edge = np.asarray(front)[:, : max(2, front.size[0] // 60)].reshape(-1, 3)
    spine = np.median(edge, 0) * 0.92
    rng = np.random.default_rng(len(ident))

    def paper(across):
        """The pages' edges: off-white, a faint line a leaf along the face."""
        n = max(4, pages // 2)
        shade = np.repeat(0.94 + 0.05 * rng.random(n), int(np.ceil(256 / n)))[:256]
        img = np.outer(shade, np.ones(256)) if across else np.outer(np.ones(256), shade)
        return Image.fromarray((img[..., None] * np.array([232, 228, 216])).clip(0, 255).astype(np.uint8))
    side = Image.fromarray((spine * (0.97 + 0.03 * rng.random((256, 64, 1)))).clip(0, 255).astype(np.uint8))
    faces = {"front": front, "back": back, "left": side, "right": paper(False), "top": paper(True), "bottom": paper(True)}
    gloss = {f: Image.new("L", (8, 8), 230 if f in ("front", "back", "left") else 40) for f in FACES}
    return faces, gloss, (w, h, d)


def from_bbdb(box):
    """The model's six faces, from where its corners lie in its picture."""
    slug = box["bbdb"]
    meta = json.load(open(get("https://www.bigboxdb.com/api/variants/%d" % box["variant"], slug + "/variant.json")))
    glb = open(get("https://www.bigboxdb.com" + meta["textureFileName"], slug + "/box.glb"), "rb").read()
    n = struct.unpack("<I", glb[12:16])[0]
    gltf, blob = json.loads(glb[20:20 + n]), glb[28 + n:]

    def view(i):
        v = gltf["bufferViews"][i]
        return blob[v.get("byteOffset", 0):v.get("byteOffset", 0) + v["byteLength"]]
    ktx2 = os.path.join(CACHE, slug, "box.ktx2")
    open(ktx2, "wb").write(view(gltf["images"][0]["bufferView"]))
    basis = os.path.join(CACHE, "basis")
    for name in ("basis_transcoder.js", "basis_transcoder.wasm"):
        get("https://unpkg.com/three@0.160.0/examples/jsm/libs/basis/" + name, "basis/" + name)
    w, h = map(int, subprocess.check_output(["node", os.path.join(HERE, "gamebox_ktx2.cjs"), basis, ktx2, ktx2 + ".rgba"], text=True).split())
    img = Image.frombytes("RGBA", (w, h), open(ktx2 + ".rgba", "rb").read()).convert("RGB")
    prim = gltf["meshes"][0]["primitives"][0]["attributes"]
    count = gltf["accessors"][prim["POSITION"]]["count"]
    pos = np.frombuffer(view(gltf["accessors"][prim["POSITION"]]["bufferView"]), "<f4").reshape(count, 3) * (1, 1, -1)   # glTF's front is +z, ours -z
    nrm = np.frombuffer(view(gltf["accessors"][prim["NORMAL"]]["bufferView"]), "<f4").reshape(count, 3) * (1, 1, -1)
    uv = np.frombuffer(view(gltf["accessors"][prim["TEXCOORD_0"]]["bufferView"]), "<f4").reshape(count, 2)
    half = pos.max(0)
    faces = {}
    for face, (normal, right, up) in AXES.items():
        mine = [i for i in range(count) if np.dot(nrm[i], normal) > 0.9]
        def corner(su, sv):
            want = np.array(normal) * half + su * np.array(right) * half + sv * np.array(up) * half
            i = min(mine, key=lambda i: np.abs(pos[i] - want).sum())
            return tuple(uv[i] * (w, h))
        faces[face] = (img, (corner(-1, 1), corner(-1, -1), corner(1, -1), corner(1, 1)))
    mm = tuple(float(x) * 25.4 for x in (meta["w"], meta["h"], meta["d"]))   # inches
    return faces, mm


def from_museum(box):
    xml = json.load(open(get("https://www.gamebox.museum/api/scans.php", box["museum"] + "/scan.json",
                             ("action=getScan&UID=" + box["museum"]).encode())))["message"]
    scan = ET.fromstring(xml)
    base = next(f for f in scan.iter("Feature") if f.findtext("UVTemplate") == "defaultBaseBox")
    img = Image.open(get("https://www.gamebox.museum/files/" + base.findtext("TextureFile"), box["museum"] + "/base.jpg")).convert("RGB")
    mm = tuple(float(scan.findtext(k)) * 10 for k in "XYZ")   # centimetres
    return {face: (img, box["corners"][face]) for face in FACES}, mm


# a face's outward normal, and the box's axes along its picture's right and up (x right, y up, the front at -z)
AXES = {"front": ((0, 0, -1), (1, 0, 0), (0, 1, 0)), "back": ((0, 0, 1), (-1, 0, 0), (0, 1, 0)),
        "left": ((-1, 0, 0), (0, 0, -1), (0, 1, 0)), "right": ((1, 0, 0), (0, 0, 1), (0, 1, 0)),
        "top": ((0, 1, 0), (1, 0, 0), (0, 0, 1)), "bottom": ((0, -1, 0), (1, 0, 0), (0, 0, -1))}


def face_mm(face, mm):
    w, h, d = mm
    return {"front": (w, h), "back": (w, h), "left": (d, h), "right": (d, h), "top": (w, d), "bottom": (w, d)}[face]


def layout(sizes, scale):
    """Each box a block: its sides, front and back in a row, its top and bottom under the front and
    back; blocks in shelves. The faces' pixel rectangles, or None if they do not fit."""
    px = lambda v: int(round(v * scale))
    rects, x, y, row = {}, 0, 0, 0
    for name, (w, h, d) in sorted(sizes.items(), key=lambda kv: -kv[1][1]):
        cols = [("left", px(d)), ("front", px(w)), ("right", px(d)), ("back", px(w))]
        wide = sum(c for _, c in cols) + GUTTER * 4
        high = px(h) + px(d) + GUTTER * 2
        if x + wide > ATLAS:
            x, y, row = 0, y + row, 0
        if y + high > ATLAS or wide > ATLAS:
            return None
        cx = x + GUTTER // 2
        for face, c in cols:
            rects[name, face] = (cx, y + GUTTER // 2, c, px(h))
            if face in ("front", "back"):
                rects[name, "top" if face == "front" else "bottom"] = (cx, y + GUTTER // 2 + px(h) + GUTTER, c, px(d))
            cx += c + GUTTER
        x += wide
        row = max(row, high)
    return rects


def bleed(atlas, x, y, w, h):
    """Spread a face's edge pixels into the gutter round it, so mip maps do not mix in a neighbour."""
    g = GUTTER // 2
    a = atlas
    a[y:y + h, max(x - g, 0):x] = a[y:y + h, x:x + 1]
    a[y:y + h, x + w:x + w + g] = a[y:y + h, x + w - 1:x + w]
    a[max(y - g, 0):y, max(x - g, 0):x + w + g] = a[y:y + 1, max(x - g, 0):x + w + g]
    a[y + h:y + h + g, max(x - g, 0):x + w + g] = a[y + h - 1:y + h, max(x - g, 0):x + w + g]


def atlas(name, sources, sizes, out):
    """Every thing's six faces in one picture, Textures/Fetched/NAME.png, and its gloss."""
    scale = 8.0
    while layout(sizes, scale) is None:
        scale -= 0.05
    rects = layout(sizes, scale)
    colour = np.full((ATLAS, ATLAS, 3), 128, np.uint8)
    shine = np.zeros((ATLAS, ATLAS), np.uint8)
    for thing, (faces, gloss) in sources.items():
        out[thing] = {"atlas": name, "size": [v / 1000 for v in sizes[thing]], "faces": {}}
        for face in FACES:
            x, y, w, h = rects[thing, face]
            img, corners = faces[face]
            pic = img.resize((w, h), Image.LANCZOS) if corners is None else quad(img, corners, (w, h))
            colour[y:y + h, x:x + w] = np.asarray(pic)
            g = gloss[face].resize((w, h), Image.BILINEAR) if isinstance(gloss, dict) else Image.new("L", (w, h), int(gloss * 255))
            shine[y:y + h, x:x + w] = np.asarray(g)
            bleed(colour, x, y, w, h)
            bleed(shine, x, y, w, h)
            # (v up from the atlas's bottom, as atlas.json's; half a texel in from the edges)
            out[thing]["faces"][face] = [(x + 0.5) / ATLAS, 1 - (y + h - 0.5) / ATLAS, (x + w - 0.5) / ATLAS, 1 - (y + 0.5) / ATLAS]
    os.makedirs(OUT, exist_ok=True)
    Image.fromarray(colour).save(os.path.join(OUT, name + ".png"))
    # gloss: a laminated print where the map is white, a matt card where it is black
    smooth = 0.30 + 0.35 * (shine.astype(np.float32) / 255)
    packed = np.dstack([np.ones_like(smooth), 1 - smooth, np.ones_like(smooth)])   # occlusion, roughness, metal (as textures.save)
    Image.fromarray((packed * 255 + 0.5).astype(np.uint8)).resize((ATLAS // 4, ATLAS // 4), Image.BILINEAR).save(os.path.join(OUT, name + "_p.png"))
    print("%s: %d things, %.2f pixels a millimetre" % (name, len(sources), scale))


def fetch_box(name):
    box = BOXES[name]
    if "bbc" in box:
        faces, gloss, mm = from_bbc(box)
        return name, ({f: (faces[f], None) for f in FACES}, gloss), mm
    faces, mm = (from_bbdb if "bbdb" in box else from_museum)(box)
    return name, (faces, box["gloss"]), mm


def fetch_magazine(ident):
    faces, gloss, mm = from_archive(ident)
    return ident.lower().replace("-", "_"), ({f: (faces[f], None) for f in FACES}, gloss), mm


def main():
    out = {}
    for name in ("basis_transcoder.js", "basis_transcoder.wasm"):   # (once, before the threads: they share it)
        get("https://unpkg.com/three@0.160.0/examples/jsm/libs/basis/" + name, "basis/" + name)
    with concurrent.futures.ThreadPoolExecutor(6) as pool:   # (a few at a time: the sites are small)
        boxes = list(pool.map(fetch_box, BOXES))
        magazines = list(pool.map(fetch_magazine, MAGAZINES))
    half = (len(boxes) + 1) // 2   # the boxes in two pictures: in one they were 1.3 pixels a millimetre
    for name, things in (("GameBoxes", boxes[:half]), ("GameBoxes2", boxes[half:]), ("Magazines", magazines)):
        atlas(name, {n: src for n, src, _ in things}, {n: mm for n, _, mm in things}, out)
    json.dump(out, open(os.path.join(HERE, "gameboxes.json"), "w"), indent=1)
    print(", ".join("%s %.0f x %.0f x %.0f mm" % ((n,) + tuple(v * 1000 for v in t["size"])) for n, t in out.items()))


if __name__ == "__main__":
    main()
