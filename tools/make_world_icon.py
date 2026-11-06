"""The VRChat world's picture: a capture of the room with its name and what it has lettered on.

    python tools/make_world_icon.py CAPTURE.png [OUT.png]

CAPTURE is 4:3 (a camera rendered in play mode with the machine running: docs/world/world.md, "The
board's words, and the world's listing"). The lettering is the room's own typeface (unity/ShaderEmu/Fonts),
in a column beside the capture's screen (the screen on the left), well inside the picture's edges: VRChat
crops it to 16:9 in its lists and to a square in places, which cut the old lettering at the top, the bottom
and the ends, and lettering over the screen hid what the world is.
"""
import os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'unity', 'ShaderEmu', 'Fonts')
TOP = ['ShaderEmu v2', 'RISC-V computer']
UNDER = ['Emulated on your GPU', 'in shaders']
BOTTOM = [['Linux', 'Old games', 'NES'], ['Browser', 'Multimedia', 'IDE']]
W, H = 1200, 900
SIDE = 170          # the column's margin from the right edge: a square crop of 4:3 loses 150
LEFT = 610          # and its left edge, past the screen
MIDDLE = (LEFT + W - SIDE) / 2
AMBER = (250, 192, 51)


def band(picture, x0):
    """Darkens the picture rightwards from x0, so that letters read on any room."""
    shade = Image.new('L', (W - x0, 1))
    for x in range(W - x0):
        t = min(1.0, x / 220)
        shade.putpixel((x, 0), int(190 * t ** 1.5))
    black = Image.new('RGB', (W - x0, H), (6, 8, 14))
    picture.paste(black, (x0, 0), shade.resize((W - x0, H)))


def lettered(picture, text, font, centre_y, colour, parts=None):
    draw = ImageDraw.Draw(picture)
    width = draw.textlength(text, font=font)
    x = MIDDLE - width / 2
    # a soft shadow under the letters, then the letters
    shadow = Image.new('RGBA', picture.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((x + 3, centre_y + 4), text, font=font, fill=(0, 0, 0, 230), anchor='lm')
    picture.paste(shadow.filter(ImageFilter.GaussianBlur(5)), (0, 0), shadow.filter(ImageFilter.GaussianBlur(5)))
    draw = ImageDraw.Draw(picture)
    if not parts:
        draw.text((x, centre_y), text, font=font, fill=colour, anchor='lm')
        return
    for part, part_colour in parts:
        draw.text((x, centre_y), part, font=font, fill=part_colour, anchor='lm')
        x += draw.textlength(part, font=font)


def fitted(path, text, size):
    """The typeface at the largest size, up to `size`, at which the text leaves a margin each side."""
    while ImageFont.truetype(path, size).getlength(text) > W - SIDE - LEFT:
        size -= 2
    return ImageFont.truetype(path, size)


def main():
    source = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(source), 'world_icon.png')
    picture = Image.open(source).convert('RGB').resize((W, H), Image.LANCZOS)
    band(picture, LEFT - 140)
    bold = os.path.join(FONTS, 'IBMPlexSansCondensed-Bold.ttf')
    medium = os.path.join(FONTS, 'IBMPlexSansCondensed-Medium.ttf')
    title = fitted(bold, TOP[0], 104)
    second = fitted(bold, TOP[1], 72)
    under = fitted(medium, max(UNDER, key=len), 46)
    words = []
    for group in BOTTOM:
        parts = []
        for i, word in enumerate(group):
            if i:
                parts.append(('  •  ', AMBER))
            parts.append((word, (255, 255, 255)))
        words.append(parts)
    small = fitted(bold, max((''.join(p for p, _ in parts) for parts in words), key=len), 44)
    y = 250
    lettered(picture, TOP[0], title, y, 'white', [('ShaderEmu', AMBER), (TOP[0][len('ShaderEmu'):], (255, 255, 255))]); y += 95
    lettered(picture, TOP[1], second, y, 'white'); y += 90
    for line in UNDER:
        lettered(picture, line, under, y, (232, 236, 244)); y += 56
    y += 40
    for parts in words:
        lettered(picture, ''.join(p for p, _ in parts), small, y, 'white', parts); y += 58
    picture.save(out)
    print(out)


if __name__ == '__main__':
    main()
