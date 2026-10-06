# Glyph atlas for the terminal shader: ASCII 32..127, 16 columns x 6 rows, top row first.
import sys
from PIL import Image, ImageDraw, ImageFont
CW, CH = 32, 64
font = ImageFont.truetype("C:/Windows/Fonts/CascadiaMono.ttf", 50)
img = Image.new("L", (16 * CW, 6 * CH), 0)
d = ImageDraw.Draw(img)
asc, desc = font.getmetrics()
adv = font.getlength("M")
ox = (CW - adv) / 2
oy = (CH - (asc + desc)) / 2
for c in range(32, 127):
    i = c - 32
    d.text((i % 16 * CW + ox, i // 16 * CH + oy), chr(c), font=font, fill=255)
img.save(sys.argv[1])
print("advance", adv, "ascent", asc, "descent", desc)
