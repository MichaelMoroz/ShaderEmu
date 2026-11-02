"""What each of Tiberian Dawn's terrain templates is, read from the game's own source at build time.

The game's `TemplateType` enumeration (`tiberiandawn/defines.h`) gives a template's number in a
map's `.BIN`; its table (`tiberiandawn/cdata.cpp`) gives each its name, its ground (`LAND_*`), its
size and the icons whose ground is another (`AltIcons`, `AltLand`), as `CellClass::Recalc_Attributes`
reads them. Nothing of that table is copied into this repository: the tree the build fetched is
read where it lies (`linux/tdawn/build.sh`'s `vanilla-conquer-td`), so the heights follow the game.
"""
import os
import re

LANDS = ["CLEAR", "ROAD", "WATER", "ROCK", "WALL", "TIBERIUM", "BEACH"]
NONE = 255


class Template:
    __slots__ = ("number", "enum", "name", "text", "land", "width", "height", "alt_land", "alt_icons")

    def land_of(self, icon):
        """The ground of one of its icons, as Recalc_Attributes has it."""
        return self.alt_land if icon in self.alt_icons else self.land

    @property
    def kind(self):
        """CLEAR, ROAD (debris), WATER (water and shores), SLOPE (cliffs), RIVER, PATCH, BRIDGE..."""
        return self.text[4:] if self.text.startswith("TXT_") else self.text


def _enum(defines):
    body = re.search(r"enum\s+TemplateType\b[^{]*\{(.*?)\}", defines, re.S)
    if not body:
        raise ValueError("no TemplateType in defines.h")
    names = []
    for line in body.group(1).splitlines():
        line = re.sub(r"//.*", "", line).strip()
        for item in line.split(","):
            item = item.strip()
            if not item:
                continue
            name = item.split("=")[0].strip()
            if name in ("TEMPLATE_COUNT", "TEMPLATE_NONE", "TEMPLATE_FIRST"):
                continue
            if "=" in item:
                raise ValueError("a template numbered by hand: " + item)
            names.append(name)
    return {name: number for number, name in enumerate(names)}


_ARRAY = re.compile(r"static\s+char\s+const\s+(\w+)\s*\[\s*\]\s*=\s*\{([^}]*)\}\s*;")
_TABLE = re.compile(
    r"TemplateTypeClass\s+const\s+\w+\s*\(\s*(TEMPLATE_\w+)\s*,\s*[^,]+,\s*\"([^\"]*)\"\s*,\s*(\w+)\s*,\s*"
    r"(LAND_\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(LAND_\w+)\s*,\s*([^;]*?)\)\s*;"
)


def load(source):
    """{number: Template} from a tree's tiberiandawn folder."""
    with open(os.path.join(source, "defines.h"), encoding="latin-1") as f:
        numbers = _enum(f.read())
    with open(os.path.join(source, "cdata.cpp"), encoding="latin-1") as f:
        cdata = re.sub(r"//[^\n]*", "", f.read())
    arrays = {}
    for name, values in _ARRAY.findall(cdata):
        icons = [int(v) for v in values.replace("\n", " ").split(",") if v.strip()]
        arrays[name] = frozenset(i for i in icons if i >= 0)
    templates = {}
    for enum, ini, text, land, w, h, alt_land, alt in _TABLE.findall(cdata):
        t = Template()
        t.number = numbers[enum]
        t.enum, t.name, t.text = enum, ini, text
        t.land, t.alt_land = land[5:], alt_land[5:]
        t.width, t.height = int(w), int(h)
        alt = alt.strip()
        array = re.search(r"(_\w+)\s*$", alt)
        t.alt_icons = arrays[array.group(1)] if alt != "NULL" and array else frozenset()
        templates[t.number] = t
    if len(templates) != len(numbers):
        missing = sorted(set(numbers) - {t.enum for t in templates.values()})
        raise ValueError("templates without a table entry: %s" % missing[:8])
    return templates
