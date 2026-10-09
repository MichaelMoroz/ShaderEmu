# Sixteen books, each drawn at its own size in a cell of Books.png (64 by 512 pixels, 0.62 mm a
# pixel): what textures.py draws and furniture.py models must agree, or a spine's text stretches.
import random

CELLS, CELL_W, CELL_H, MM = 16, 64, 512, 0.62
_rng = random.Random(3)
DESIGNS = [(_rng.randint(34, 62), _rng.randint(340, 508)) for _ in range(CELLS)]   # a spine's pixels: wide, tall


def size(cell):
    """(thickness, height) in metres."""
    w, h = DESIGNS[cell]
    return w * MM / 1000.0, h * MM / 1000.0
