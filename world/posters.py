# Posters of the decade for the den's, the corridor's and the side rooms' walls, from scans of the
# real things, fetched when the world is made and never kept in the repository (as world/gameboxes.py's
# boxes: the pictures are their makers'); and the side rooms' number plates, which are ours.
#   python world/posters.py [fetch | signs]
# Downloads go to build/posters. Written: Textures/Fetched/Posters.png (every poster in one atlas, out
# of git) and its _p, world/posters.json (each poster's place in the atlas and its shape), which
# world/rooms.py and world/furniture.py hang; and Textures/World/RoomSigns.png (+ _p), the plates.
import json
import os
import sys
import urllib.error
import urllib.request

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "..", "build", "posters")
FETCHED = os.path.join(HERE, "..", "unity", "ShaderEmu", "Textures", "Fetched")   # out of git: .gitignore
WORLD = os.path.join(HERE, "..", "unity", "ShaderEmu", "Textures", "World")
FONTS = "C:/Windows/Fonts/"
ATLAS = 4096
GUTTER = 8

# Found on archive.org (scans of posters, flyers and magazine pages; IIIF scales the largest down)
# and IMP Awards (films' one-sheets), each fetched with a plain GET. The order is the order they are
# hung in: world/rooms.py names the den's and the corridor's, and the rooms take the rest in order.
POSTERS = {
    # the side rooms' (world/rooms.py hangs them in this order: four a room, N13 to N15)
    "doom_ad": dict(title='Doom magazine advertisement (id Software)', year=1994, url='https://iiif.archive.org/image/iiif/3/doom-ad%2FDoomAd.png/full/!1200,1800/0/default.jpg'),
    "hexen_poster": dict(title='Hexen: Beyond Heretic poster', year=1995, url='https://iiif.archive.org/image/iiif/3/hexen-poster-600dpi%2FHexen%20Poster%20600dpi.png/full/!1100,1600/0/default.jpg'),
    "matrix_1999": dict(title='The Matrix', year=1999, url='http://www.impawards.com/1999/posters/matrix_ver1_xlg.jpg'),
    "system_shock_poster": dict(title='System Shock poster (Origin, 1994 CES)', year=1994, url='https://iiif.archive.org/image/iiif/3/1994-ces-origin-system-shock-poster%2F1994_CES_Origin_SystemShock_Poster.png/full/!1200,1800/0/default.jpg'),
    "xwing_poster": dict(title='Star Wars: X-Wing poster (LucasArts)', year=1993, url='https://iiif.archive.org/image/iiif/3/star-wars-x-wing-poster-600dpi%2FStar%20Wars%20X-Wing%20Poster%20300dpi.png/full/!1200,1800/0/default.jpg'),
    "lemmings_poster": dict(title='Lemmings and Oh No! More Lemmings promotional poster (Psygnosis)', year=1991, url='https://iiif.archive.org/image/iiif/3/lemmings-and-oh-no-more-lemmings-poster%2FLemmings%20and%20oh%20no%20more%20lemmings%20page%201.png/full/!1200,1800/0/default.jpg'),
    "tron_1982": dict(title='Tron', year=1982, url='http://www.impawards.com/1982/posters/tron_xlg.jpg'),
    "kyrandia_poster": dict(title='The Legend of Kyrandia store poster (Westwood)', year=1992, url='https://archive.org/download/the-legend-of-kyrandia-store-poster-ad/0%20-%20poster_front_descreened_thumbnail.jpg'),
    "hackers_1995": dict(title='Hackers', year=1995, url='http://www.impawards.com/1995/posters/hackers_ver1_xlg.jpg'),
    "terminator2_1991": dict(title='Terminator 2: Judgment Day', year=1991, url='http://www.impawards.com/1991/posters/terminator_two_judgement_day_xlg.jpg'),
    "windows95_ad_tgim": dict(title="Windows 95 magazine advertisement 'T.G.I. Monday' (Microsoft)", year=1995, url='https://archive.org/download/05_20250411_20250411_1826/05.jpg'),
    "diablo_flyer": dict(title='Diablo flyer (Blizzard / Bomico, German)', year=1997, url='https://archive.org/download/diablo-flyer/Diablo_Flyer_1.jpg'),
    # the den's and the corridor's (world/rooms.py's DEN_POSTERS and CORRIDOR_POSTERS name them; the rest but the holodecks' are the rooms')
    "intel_pentium_pro_poster": dict(title='Intel Pentium Pro Processor poster', year=1995, url='https://archive.org/download/intel-pentium-pro-utp-poster/Intel_PentiumPro_1_Poster.jpg'),
    "wargames_1983": dict(title='WarGames', year=1983, url='http://www.impawards.com/1983/posters/wargames_ver1_xlg.jpg'),
    "amiga500_ad": dict(title='Commodore Amiga 500 advertisement (Swedish)', year=1988, url='https://archive.org/download/amiga-500-sa-nara-verkligheten-du-kan-komma-91an-24-1988/amiga%20500%20-%20s%C3%A5%20n%C3%A4ra%20verkligheten%20du%20kan%20komma%20%2891an%2024-1988%29%20600dpi.jpg'),
    "sneakers_1992": dict(title='Sneakers', year=1992, url='http://www.impawards.com/1992/posters/sneakers_xlg.jpg'),
    "jurassic_park_1993": dict(title='Jurassic Park', year=1993, url='http://www.impawards.com/1993/posters/jurassic_park_ver2_xlg.jpg'),
    "ultima8_poster": dict(title='Ultima VIII: Pagan poster (Origin, 1994 CES)', year=1994, url='https://iiif.archive.org/image/iiif/3/1994-ces-origin-ultima-viiipagan-poster%2F1994_CES_Origin_UltimaVIIIPagan_Poster.png/full/!1200,1800/0/default.jpg'),
    "blood_money_poster": dict(title='Blood Money poster (Psygnosis, Amiga)', year=1989, url='https://iiif.archive.org/image/iiif/3/blood-money-poster%2FBlood%20Money%20Poster.png/full/!1200,1800/0/default.jpg'),
    "halflife_ad": dict(title='Half-Life magazine advertisement (Dreamcast version)', year=2000, url='https://iiif.archive.org/image/iiif/3/Half-LifeForDreamcast-MagazineAds%2Fimg208.jpg/full/!1200,1800/0/default.jpg'),
    "windows95_ad_smarter": dict(title='Windows 95 magazine advertisement (Microsoft)', year=1995, url='https://archive.org/download/02_20250411/02.jpg'),
    "tomb_raider_poster": dict(title='Tomb Raider poster (Hobby Consolas pull-out, landscape)', year=1996, url='https://archive.org/download/hcptr/page/n0.jpg'),
    "lawnmower_man_1992": dict(title='The Lawnmower Man', year=1992, url='http://www.impawards.com/1992/posters/lawnmower_man_xlg.jpg'),
    "intel_manufacturing_poster": dict(title='Intel manufacturing poster', year=1987, url='https://archive.org/download/intel-1/Intel1.JPG'),
    # the holodecks' corridor's (world/rooms.py's HOLO_POSTERS): films of worlds made by machines
    "star_trek_first_contact_1996": dict(title='Star Trek: First Contact', year=1996, url='http://www.impawards.com/1996/posters/star_trek_first_contact_ver1_xlg.jpg'),
    "virtuosity_1995": dict(title='Virtuosity', year=1995, url='http://www.impawards.com/1995/posters/virtuosity_ver1_xlg.jpg'),
    "existenz_1999": dict(title='eXistenZ', year=1999, url='http://www.impawards.com/1999/posters/existenz_ver2_xlg.jpg'),
    "last_starfighter_1984": dict(title='The Last Starfighter', year=1984, url='http://www.impawards.com/1984/posters/last_starfighter_xlg.jpg'),
    "total_recall_1990": dict(title='Total Recall', year=1990, url='http://www.impawards.com/1990/posters/total_recall_xlg.jpg'),
    "thirteenth_floor_1999": dict(title='The Thirteenth Floor', year=1999, url='http://www.impawards.com/1999/posters/thirteenth_floor_xlg.jpg'),
}


def get(url, path):
    """A file, downloaded once. archive.org's IIIF service makes a new size slowly the first time,
    and may answer 500 meanwhile: it is asked again."""
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path
    os.makedirs(os.path.dirname(path), exist_ok=True)
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (ShaderEmu world build)"})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(request, timeout=50) as response, open(path + ".part", "wb") as out:
                out.write(response.read())
            break
        except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as error:
            print("posters: %s: %s, again" % (os.path.basename(path), error))
            if attempt == 2:
                raise
    os.replace(path + ".part", path)
    return path


def fetch_all():
    """Every poster downloaded, several at once (`posters.py fetch`, before the atlas)."""
    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(6) as pool:
        for name in pool.map(lambda kv: (get(kv[1]["url"], path_of(*kv)), kv[0])[1], POSTERS.items()):
            print("posters: have", name)


def path_of(name, item):
    return os.path.join(CACHE, name + os.path.splitext(item["url"].split("?")[0])[1].lower().replace(".png", ".jpg"))


def signs():
    """The side rooms' plates: brass, the room's number engraved and filled black, a rule under it
    and the words under that. Four rows (N13 to N15 and a spare), each the plate's shape (120 x 55 mm)."""
    w, h = 1024, 470
    rows = []
    for number in (13, 14, 15, None):
        y, x = np.mgrid[0:h, 0:w].astype(np.float32)
        rng = np.random.default_rng(number or 0)
        brush = np.asarray(Image.fromarray((rng.random((h // 4, w)) * 255).astype(np.uint8)).resize((w, h)).filter(ImageFilter.GaussianBlur((12, 0.6)))) / 255
        base = np.array((0.78, 0.6, 0.3))[None, None, :] * (0.88 + 0.16 * brush[..., None]) * (1 - 0.08 * ((x / w - 0.5) ** 2 + (y / h - 0.5) ** 2))[..., None]
        ink = Image.new("L", (w, h), 0)
        d = ImageDraw.Draw(ink)
        if number is not None:
            d.text((w / 2, h * 0.42), "N%d" % number, fill=255, anchor="mm", font=ImageFont.truetype(FONTS + "timesbd.ttf", 270))
            d.line((w * 0.2, h * 0.73, w * 0.8, h * 0.73), fill=255, width=6)
            d.text((w / 2, h * 0.85), "COMPUTER ROOM", fill=255, anchor="mm", font=ImageFont.truetype(FONTS + "times.ttf", 64))
        cut = np.asarray(ink.filter(ImageFilter.GaussianBlur(1.2))).astype(np.float32)[..., None] / 255
        colour = base * (1 - cut) + np.array((0.05, 0.04, 0.03))[None, None, :] * cut
        rows.append(np.asarray(Image.fromarray((np.clip(colour, 0, 1) * 255).astype(np.uint8)).resize((1024, 512), Image.LANCZOS)))
        rows[-1] = (rows[-1], 1 - 0.75 * np.asarray(Image.fromarray((cut[..., 0] * 255).astype(np.uint8)).resize((1024, 512))) / 255)
    rgb = np.concatenate([r for r, _ in rows])
    smooth = np.concatenate([s for _, s in rows]) * 0.75
    Image.fromarray(np.dstack([rgb, (smooth * 255 + 0.5).astype(np.uint8)])).save(os.path.join(WORLD, "RoomSigns.png"))
    packed = np.dstack([np.ones_like(smooth), 1 - smooth, 0.85 * (1 - (smooth < 0.3))]).astype(np.float32)   # brass is a metal; the black fill is not
    Image.fromarray((np.clip(packed, 0, 1) * 255 + 0.5).astype(np.uint8)).save(os.path.join(WORLD, "RoomSigns_p.png"))
    print("signs: RoomSigns.png")


def posters():
    """Every poster fetched, cut to its own picture, and packed in rows into one atlas."""
    pictures = {}
    for name, item in POSTERS.items():
        path = get(item["url"], path_of(name, item))
        image = Image.open(path).convert("RGB")
        if "crop" in item:   # (left, top, right, bottom) as fractions: a scan's margin or a page's edge cut away
            l, t, r, b = item["crop"]
            image = image.crop((int(l * image.width), int(t * image.height), int(r * image.width), int(b * image.height)))
        pictures[name] = image
    # rows of one height; a poster's width by its own shape
    row_high = 1000
    while True:
        x, y, placed = GUTTER, GUTTER, {}
        for name, image in pictures.items():
            wide = round(row_high * image.width / image.height)
            if x + wide + GUTTER > ATLAS:
                x, y = GUTTER, y + row_high + GUTTER
            placed[name] = (x, y, wide)
            x += wide + GUTTER
        if y + row_high + GUTTER <= ATLAS:
            break
        row_high -= 50
    atlas = Image.new("RGB", (ATLAS, ATLAS), (40, 40, 40))
    out = {}
    for name, (x, y, wide) in placed.items():
        image = pictures[name].resize((wide, row_high), Image.LANCZOS)
        g = GUTTER // 2   # the gutter round it is its own edge, stretched: mip levels do not bleed in the grey
        atlas.paste(image.resize((wide + 2 * g, row_high + 2 * g)), (x - g, y - g))
        atlas.paste(image, (x, y))
        u0, u1 = x / ATLAS, (x + wide) / ATLAS
        v1, v0 = 1 - y / ATLAS, 1 - (y + row_high) / ATLAS
        out[name] = {"uv": [round(u0, 6), round(v0, 6), round(u1, 6), round(v1, 6)], "aspect": round(wide / row_high, 4),
                     "title": POSTERS[name]["title"]}
    os.makedirs(FETCHED, exist_ok=True)
    atlas.save(os.path.join(FETCHED, "Posters.png"))
    gloss = np.full((ATLAS, ATLAS, 3), (255, 140, 0), np.uint8)   # occlusion 1, paper's roughness, no metal
    Image.fromarray(gloss).save(os.path.join(FETCHED, "Posters_p.png"))
    json.dump(out, open(os.path.join(HERE, "posters.json"), "w"), indent=1)
    print("posters: %d in the atlas, rows %d px high" % (len(out), row_high))


if __name__ == "__main__":
    if sys.argv[1:] == ["fetch"]:
        fetch_all()
    else:
        signs()
        if sys.argv[1:] != ["signs"]:
            posters()
