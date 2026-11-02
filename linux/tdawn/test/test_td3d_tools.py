"""Host tests of the 3D view's tools: mix files, the templates, the heights and the models' file.

    python linux/tdawn/test/test_td3d_tools.py [-v]

With TDAWN_SOURCE (the fetched tree's tiberiandawn folder) set, the heights are also made from the
demo's own maps (linux/prebuilt/root/usr/share/tdawn-DEMO*.MIX); otherwise that case is skipped.
"""
import math
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TDAWN = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(TDAWN, "td3d"))
sys.path.insert(0, os.path.join(TDAWN, "models"))

import build  # noqa: E402
import heights  # noqa: E402
import mesh  # noqa: E402
import mix  # noqa: E402
import pack  # noqa: E402
import templates  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(TDAWN))
SHARE = os.path.join(ROOT, "linux", "prebuilt", "root", "usr", "share")


def make_mix(files):
    """A mix of the 1995 game's kind from {name: bytes}."""
    entries = sorted((mix.name_id(n), d) for n, d in files.items())
    body, records = b"", b""
    for fid, data in entries:
        records += struct.pack("<III", fid, len(body), len(data))
        body += data
    return struct.pack("<HI", len(entries), len(body)) + records + body


SOURCE_DEFINES = """
typedef enum TemplateType : unsigned char
{
    TEMPLATE_CLEAR1,
    TEMPLATE_WATER, // This must be the first non-clear template.
    TEMPLATE_SHORE1,
    TEMPLATE_SLOPE1,
    TEMPLATE_COUNT,
    TEMPLATE_NONE = 255,
    TEMPLATE_FIRST = 0
} TemplateType;
"""
SOURCE_CDATA = """
static char const _slope111111[] = {0, 1, 2, 3, 4, 5, -1};
static char const _slope001[] = {2, -1};
static TemplateTypeClass const Clear(TEMPLATE_CLEAR1, THEATERF_TEMPERATE, "CLEAR1", TXT_CLEAR, LAND_CLEAR, 5, 4, LAND_CLEAR, NULL);
static TemplateTypeClass const
    Water(TEMPLATE_WATER, THEATERF_TEMPERATE, "W1", TXT_WATER, LAND_WATER, 1, 1, LAND_WATER, NULL);
static TemplateTypeClass const Shore1(TEMPLATE_SHORE1,
                                      THEATERF_WINTER | THEATERF_TEMPERATE,
                                      "SH1",
                                      TXT_WATER,
                                      LAND_WATER,
                                      3,
                                      3,
                                      LAND_BEACH,
                                      (char const*)_slope111111);
static TemplateTypeClass const Slope1(TEMPLATE_SLOPE1, THEATERF_TEMPERATE, "S01", TXT_SLOPE, LAND_ROCK, 2, 2, LAND_CLEAR,
                                      (char const*)_slope001); // a comment
"""


def mini_source(folder):
    with open(os.path.join(folder, "defines.h"), "w") as f:
        f.write(SOURCE_DEFINES)
    with open(os.path.join(folder, "cdata.cpp"), "w") as f:
        f.write(SOURCE_CDATA)


class Mixes(unittest.TestCase):
    def test_ids(self):
        # (the game's sum: four bytes at a time, the sum turned left one and added; padded with zeros)
        self.assertEqual(mix.name_id("A"), 0x41)
        self.assertEqual(mix.name_id("ABCDE"), ((0x44434241 << 1) & 0xFFFFFFFF | 0x44434241 >> 31) + 0x45)
        self.assertEqual(mix.name_id("scg01ea.bin"), mix.name_id("SCG01EA.BIN"))

    def test_read(self):
        m = mix.Mix(make_mix({"ONE.BIN": b"1234", "TWO.INI": b"[MAP]"}))
        self.assertEqual(m.get("ONE.BIN"), b"1234")
        self.assertEqual(m.get("two.ini"), b"[MAP]")
        self.assertIsNone(m.get("THREE"))
        self.assertIn("ONE.BIN", m)

    @unittest.skipUnless(os.path.exists(os.path.join(SHARE, "tdawn-DEMO.MIX")), "no demo data")
    def test_demo(self):
        demo = mix.Mix.open(os.path.join(SHARE, "tdawn-DEMO.MIX"))
        self.assertEqual(len(demo.get("SCG03EA.BIN")), 8192)
        self.assertEqual(len(demo.get("TEMPERAT.PAL")), 768)


class Templates(unittest.TestCase):
    def test_parse(self):
        with tempfile.TemporaryDirectory() as d:
            mini_source(d)
            t = templates.load(d)
        self.assertEqual(sorted(t), [0, 1, 2, 3])
        self.assertEqual((t[1].name, t[1].land, t[1].kind), ("W1", "WATER", "WATER"))
        self.assertEqual(t[2].land_of(4), "BEACH")
        self.assertEqual(t[2].land_of(7), "WATER")
        self.assertEqual((t[3].land_of(0), t[3].land_of(2), t[3].kind), ("ROCK", "CLEAR", "SLOPE"))


class Heights(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        mini_source(self.dir.name)
        self.t = templates.load(self.dir.name)

    def tearDown(self):
        self.dir.cleanup()

    def binary(self, cells):
        out = bytearray()
        for template, icon in cells:
            out += bytes((template, icon))
        return bytes(out)

    def test_water_and_ridge(self):
        cells = [(255, 0)] * (64 * 64)
        for y in range(64):
            for x in range(40, 64):
                cells[y * 64 + x] = (1, 0)          # water to the east
            cells[y * 64 + 20] = (3, 0)             # a line of cliff rock (a ridge: one region both sides)
        cells[5 * 64 + 20] = (255, 0)               # (with a gap: the line does not cut the map)
        h, classes = heights.make(self.binary(cells), self.t)
        self.assertEqual(len(h), 65 * 65)
        self.assertEqual(classes.count(heights.WATER), 64 * 24)
        water = [h[y * 65 + x] for y in range(65) for x in range(42, 64)]
        land = h[30 * 65 + 5]
        ridge = h[30 * 65 + 20]
        self.assertEqual(len(set(water)), 1, "water is flat")
        self.assertLess(water[0], land)
        self.assertGreater(ridge, land)

    def test_terraces(self):
        cells = [(255, 0)] * (64 * 64)
        for x in range(64):
            cells[20 * 64 + x] = (3, 0)             # a cliff right across: two regions
        h, _ = heights.make(self.binary(cells), self.t)
        north, south = h[5 * 65 + 30], h[50 * 65 + 30]
        self.assertEqual(south, heights.BASE + heights.RUNG)                   # the larger: the main ground
        self.assertEqual(north, heights.BASE + 2 * heights.RUNG)               # the smaller: a rung up

    def test_file(self):
        maps = [("SCG01EA", bytes(range(256)) * 16 + bytes(65 * 65 - 4096)), ("SCB10EB", bytes(65 * 65))]
        back = heights.unpack(heights.pack(maps))
        self.assertEqual(back, dict(maps))

    @unittest.skipUnless(os.environ.get("TDAWN_SOURCE") and os.path.exists(os.path.join(SHARE, "tdawn-DEMO.MIX")),
                         "TDAWN_SOURCE not set, or no demo data")
    def test_demo_maps(self):
        t = templates.load(os.environ["TDAWN_SOURCE"])
        mixes = [mix.Mix.open(os.path.join(SHARE, n)) for n in ("tdawn-DEMOL.MIX", "tdawn-DEMO.MIX")]
        names = heights.scenario_names(mixes)
        self.assertEqual(names, ["SCG01EA", "SCG03EA", "SCG05EA", "SCG06EA", "SCG10EA"])
        for n in names:
            h, classes = heights.make(mix.find(mixes, n + ".BIN"), t)
            self.assertGreater(classes.count(heights.OPEN), 2000, n)


class Models(unittest.TestCase):
    def test_round_trip(self):
        made = build.models()
        models, normals, materials, coords_at = pack.unpack(pack.pack(made))
        self.assertEqual([m["name"] for m in models], [m.name for m in made])
        self.assertEqual(coords_at % 16, 0)
        self.assertEqual(materials[mesh.HOUSE], 0xFFFFFFFF)
        self.assertEqual(materials[mesh.METAL], 150 << 16 | 150 << 8 | 140)
        for m, mm in zip(made, models):
            self.assertEqual(len(m.parts), len(mm["parts"]))
            for p, pp in zip(m.parts, mm["parts"]):
                self.assertEqual(pp["count"], 3 * len(p.triangles))
                self.assertEqual(pp["count"] % 3, 0)
                s = pp["step"]
                for ((a, b, c), material), k in zip(p.triangles, range(0, pp["count"], 3)):
                    for q, (x, y, z, tag) in zip((a, b, c), pp["vertices"][k:k + 3]):
                        back = (pp["origin"][0] + x * s, pp["origin"][1] + y * s, pp["origin"][2] + z * s)
                        for i in range(3):
                            self.assertLessEqual(abs(back[i] - q[i]), s / 2 + 256 / 65536.0)  # (origin and step are 16.16)
                        self.assertEqual(tag >> 4, material)

    def test_budget(self):
        for m in build.models():
            low, high = build.BUDGET[m.kind]
            self.assertLessEqual(m.triangles(), high, m.name)

    def test_nothing_faces_down(self):
        for m in build.models():
            for p in m.parts:
                for (a, b, c), _ in p.triangles:
                    self.assertGreaterEqual(mesh.normal(a, b, c)[2], -0.7, m.name)

    def test_normals(self):
        self.assertEqual(pack.normal_index((0, 0, 1)), 0)
        self.assertEqual(pack.normal_index((0, 1, 0)), 1)          # ahead
        self.assertEqual(pack.normal_index((1, 0, 0)), 3)          # to the right: a quarter clockwise
        self.assertEqual(pack.normal_index((0, -0.7, 0.7)), 11)    # behind and up
        for n in pack.NORMALS[:14]:
            self.assertAlmostEqual(math.sqrt(sum(v * v for v in n)), 1.0, places=3)


if __name__ == "__main__":
    unittest.main()
