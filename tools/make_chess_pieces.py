"""The chess pieces of nxchess as bitmaps: linux/chess/pieces.h, committed (docs/games/chess.md).
From DejaVu Sans's chess glyphs (its licence allows it). Each piece of each colour has a hull (the glyph and all it encloses) and the glyph itself: the
window system draws the hull in the piece's ground and the glyph in black.
   python tools/make_chess_pieces.py /path/to/DejaVuSans.ttf"""
import sys
from PIL import Image, ImageDraw, ImageFont

SIZE = 44
FONT = sys.argv[1]
# white's glyphs are open, black's solid: king queen rook bishop knight pawn at U+2654 and U+265A
ORDER = {'PAWN': 5, 'KNIGHT': 4, 'BISHOP': 3, 'ROOK': 2, 'QUEEN': 1, 'KING': 0}
TYPES = ['PAWN', 'KNIGHT', 'BISHOP', 'ROOK', 'QUEEN', 'KING']

font = ImageFont.truetype(FONT, 41)


def glyph(code, level=120):
    big = Image.new('L', (SIZE * 2, SIZE * 2), 0)
    ImageDraw.Draw(big).text((SIZE // 2, SIZE // 2), chr(code), 255, font=font)
    box = big.point(lambda v: 255 if v > 120 else 0).getbbox()
    out = Image.new('L', (SIZE, SIZE), 0)
    out.paste(big.crop(box), ((SIZE - (box[2] - box[0])) // 2, (SIZE - (box[3] - box[1])) // 2))
    return [[out.getpixel((x, y)) > level for x in range(SIZE)] for y in range(SIZE)]


def hull(g):
    outside = [[False] * SIZE for _ in range(SIZE)]
    todo = [(x, y) for x in range(SIZE) for y in (0, SIZE - 1)] + [(x, y) for y in range(SIZE) for x in (0, SIZE - 1)]
    while todo:
        x, y = todo.pop()
        if 0 <= x < SIZE and 0 <= y < SIZE and not outside[y][x] and not g[y][x]:
            outside[y][x] = True
            todo += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    return [[not outside[y][x] for x in range(SIZE)] for y in range(SIZE)]


def outlined(g):
    # the open glyphs' fine lines go at this size: all they enclose gets an edge of its own, and the lines are taken at a lower level
    h = hull(g)
    def inside(x, y):
        return 0 <= x < SIZE and 0 <= y < SIZE and h[y][x]
    return [[g[y][x] or (h[y][x] and not (inside(x + 1, y) and inside(x - 1, y) and inside(x, y + 1) and inside(x, y - 1))) for x in range(SIZE)] for y in range(SIZE)]


def words(mask):
    out = []
    for row in mask:
        for w in range(0, SIZE, 16):
            v = 0
            for b in range(16):
                if w + b < SIZE and row[w + b]:
                    v |= 0x8000 >> b
            out.append(v)
    return out


lines = ['/* Made by tools/make_chess_pieces.py from DejaVu Sans. By colour, type less one, then hull and glyph. */',
         f'#define PIECE_SIZE {SIZE}', f'#define PIECE_WORDS {(SIZE + 15) // 16 * SIZE}',
         'static const unsigned short piece_art[2][6][2][PIECE_WORDS] = {']
for colour in (0, 1):
    lines.append('{')
    for t in TYPES:
        g = glyph((0x2654 if colour == 0 else 0x265a) + ORDER[t], 50 if colour == 0 else 120)
        g = outlined(g) if colour == 0 else g
        lines.append('{')
        for m in (hull(g), g):
            w = words(m)
            lines.append('{' + ','.join('0x%x' % v for v in w) + '},')
        lines.append('},')
    lines.append('},')
lines.append('};')
with open('linux/chess/pieces.h', 'wb') as f:
    f.write(('\n'.join(lines) + '\n').encode())
# its icon: a knight on two squares of the board
icon = Image.new('RGBA', (64, 64), (238, 216, 180, 255))
ImageDraw.Draw(icon).polygon([(0, 0), (64, 0), (0, 64)], fill=(170, 124, 84, 255))
g = glyph(0x265e)
h = hull(g)
for y in range(SIZE):
    for x in range(SIZE):
        if h[y][x]:
            icon.putpixel((x + 10, y + 10), (20, 20, 20, 255) if g[y][x] else (240, 240, 240, 255))
icon.save('linux/apps/icons/nxchess.png')
