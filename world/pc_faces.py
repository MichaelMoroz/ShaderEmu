# What is printed beside each control and socket of the classroom's computers: a legend each, so
# world/pc.py can put it at its own control's place (world/textures.py draws them, one picture a
# legend). A name: its text and what it is printed on.
LEGEND_PX = 16          # a legend's letters in the atlas
LEGEND_M = 0.0002       # a texel of a legend on the model: letters 3.2 mm high

LEGENDS = {
    # the tower's front
    "power": ("POWER", "front"), "turbo": ("TURBO", "front"), "hdd": ("H.D.D.", "front"), "reset": ("RESET", "front"),
    "mips_all": ("MIPS ALL", "front"), "mips_cpu": ("MIPS CPU", "front"),
    # the tower's back, stamped on a strip of foil
    "keyboard": ("KEYBOARD", "steel"), "com1": ("COM 1", "steel"), "com2": ("COM 2", "steel"), "printer": ("PRINTER", "steel"),
    "volts": ("115/230V", "steel"), "lan": ("10BASE-T", "steel"),
    # the monitor's terminal board
    "rgb1": ("RGB 1", "board"), "rgb2": ("RGB 2", "board"), "r": ("R", "board"), "g": ("G", "board"), "b": ("B", "board"),
    "hcs": ("H/CS", "board"), "v": ("V", "board"), "term": ("75 OHM", "board"), "video": ("VIDEO", "board"),
    "audio_l": ("AUDIO L", "board"), "audio_r": ("AUDIO R", "board"), "speaker": ("EXT SPEAKER 8 OHM", "board"),
    "lp": ("L+", "board"), "lm": ("L-", "board"), "rp": ("R+", "board"), "rm": ("R-", "board"),
    "acin": ("AC IN", "back"),
}
# paper and ink of each ground: the front's plastic, the back's, bare steel, the dark board
TONES = {"front": ((212, 202, 172), (60, 58, 54)), "back": ((189, 181, 158), (50, 48, 44)),
         "steel": ((132, 134, 139), (24, 24, 28)), "board": ((52, 52, 54), (214, 214, 208))}

# A key's cap seen from above, in the layout's units (world/keyboard.json): how far in from its
# rectangle its shoulder and its top are at the sides, behind and before. Both keyboards' caps
# and Keyboard.png (world/textures.py) are of these.
CAP_SHOULDER, CAP_TOP = (4.0, 3.0, 6.0), (6.0, 4.5, 8.0)
CAP_STEP = 1.0 / 6.0    # the shoulder under the top, of the cap's height

# Keyboard.png is the layout seen from above, over two bands: the caps' sides, unrolled at their own
# size (from above a side 16 high showed a strip 2.5 to 4.5 across: the picture stretched 3 times up
# it). The band of the light caps lies under the layout, the named keys' under that; a side's top
# edge is MARGIN up from its band's foot, and the side goes on up the band as far as it runs down
# the cap. Unity's caps take the layout's rectangle as the canvas of their legends (Plate.shader's
# _Canvas, ShaderEmuRetro.cs's KeyboardLook): CANVAS_H and BAND here are the numbers there.
SIDE_BAND, SIDE_MARGIN = 26.0, 2.0
CANVAS_H = 437.0        # world/keyboard.json's height


def cap_named(legend):
    """A key with a name, not a character, has the darker cap (Keyboard.png)."""
    named = (sum(c.isalpha() for c in legend) >= 2 and " " not in legend.strip()) or (len(legend) > 1 and legend[0] == "F" and legend[1].isdigit())
    return named or legend.startswith("Use my")


def canvas_uv(px, py, wide, tall):
    """A place of the layout (from its top left) in Keyboard.png."""
    return (px / wide, (tall - py + 2 * SIDE_BAND) / (tall + 2 * SIDE_BAND))


def side_v(d, named, tall):
    """How far `d` down a cap's side from its top edge lies in Keyboard.png."""
    return ((0.0 if named else SIDE_BAND) + SIDE_MARGIN + d) / (tall + 2 * SIDE_BAND)


def side_shade(d):
    """The light down a side (Keyboard.png's bands): from its top edge into the well."""
    return 0.97 - 0.15 * min(d / 3.7, 1.0) - 0.22 * min(max((d - 3.7) / 16.3, 0.0), 1.0)


def cap_side_uvs(corners, k, rises, named, wide, tall):
    """A cap's side k unrolled into its band. corners: for each ring (foot, shoulder, top) its four
    (x, y) corners in the layout, in the rings' order ((x0, y1), (x1, y1), (x1, y0), (x0, y0));
    rises: each ring's height over the foot, in the layout's units. The coordinates of the side's
    two faces, (lower k, lower k + 1, upper k + 1, upper k): along the edge as the layout's, down it
    by the distance on the surface (the two faces join without a seam)."""
    import math
    a, b, across = k, (k + 1) % 4, 1 if k in (0, 2) else 0

    def run(r0, r1):
        return math.hypot(corners[r1][a][across] - corners[r0][a][across], rises[r1] - rises[r0])
    d = [run(0, 1) + run(1, 2), run(1, 2), 0.0]

    def uv(r, c):
        return (corners[r][c][1 - across] / wide, side_v(d[r], named, tall))
    return [[uv(0, a), uv(0, b), uv(1, b), uv(1, a)], [uv(1, a), uv(1, b), uv(2, b), uv(2, a)]]

# Stickers on the classroom's computers: wide and high in metres, and whether it is round.
STICKERS = {"st_qc": (0.021, 0.021, True), "st_smiley": (0.022, 0.022, True), "st_asset": (0.045, 0.018, False), "st_y2k": (0.036, 0.016, False),
            "st_inside": (0.024, 0.024, False), "st_label": (0.070, 0.054, False), "st_warranty": (0.050, 0.011, False),
            "st_rainbow": (0.060, 0.011, False), "st_property": (0.050, 0.020, False), "st_price": (0.028, 0.018, False),
            "note_0": (0.076, 0.076, False), "note_1": (0.076, 0.076, False), "note_2": (0.076, 0.076, False)}
