"""Tiberian Dawn's buildings made by script (mesh.py). Pixels: a cell is 24; x right, y north, z up.

A building's 0, 0 is the middle of its cells (`cells`: across, down), as the game's
`Center_Coord` is; it stands on z 0. A gun that turns (the guard tower's) is a part of its own.
"""
from mesh import (Model, HOUSE, HOUSE_DARK, METAL, DARK, BLACK, GLASS, CONCRETE, CONCRETE_DARK, LIGHT, ROOF, WHITE, RUST)


def pad(p, cells, margin=1.0, height=1.5):
    """The concrete a building stands on, its cells less a margin."""
    hw, hl = cells[0] * 12 - margin, cells[1] * 12 - margin
    p.box(-hw, -hl, 0, hw, hl, height, CONCRETE_DARK, top=CONCRETE)


def nuke():
    """Power plant, two cells square: a hall and two cooling towers."""
    m = Model("NUKE", "building", (2, 2))
    p = m.part("body")
    pad(p, m.cells)
    p.box(-20.0, -6.0, 1.5, 4.0, 18.0, 14.0, HOUSE_DARK, top=ROOF)
    for x, y in ((12.0, 10.0), (12.0, -10.0)):
        p.cylinder((x, y, 1.5), (x, y, 22.0), 7.0, 10, CONCRETE, caps=False, end_radius=5.0)
        p.cylinder((x, y, 22.0), (x, y, 22.2), 5.0, 10, BLACK)
    p.box(-18.0, -18.0, 1.5, -6.0, -8.0, 8.0, METAL, top=DARK)
    return m


def silo():
    """Tiberium silo, two cells across one: two round tanks."""
    m = Model("SILO", "building", (2, 1))
    p = m.part("body")
    pad(p, m.cells)
    for x in (-11.0, 11.0):
        p.cylinder((x, 0, 1.5), (x, 0, 14.0), 9.0, 10, HOUSE_DARK)
        p.cylinder((x, 0, 14.0), (x, 0, 17.0), 9.0, 10, METAL, end_radius=4.0)
    return m


def pyle():
    """Barracks, two cells square: a long hut and a yard."""
    m = Model("PYLE", "building", (2, 2))
    p = m.part("body")
    pad(p, m.cells)
    p.box(-20.0, 0.0, 1.5, 20.0, 18.0, 10.0, HOUSE_DARK)
    # the roof: a prism along x, its ridge in the middle
    p.quad((-21.0, -1.0, 10.0), (21.0, -1.0, 10.0), (21.0, 9.0, 16.0), (-21.0, 9.0, 16.0), ROOF)
    p.quad((21.0, 19.0, 10.0), (-21.0, 19.0, 10.0), (-21.0, 9.0, 16.0), (21.0, 9.0, 16.0), ROOF)
    p.triangle((-21.0, 19.0, 10.0), (-21.0, -1.0, 10.0), (-21.0, 9.0, 16.0), HOUSE)
    p.triangle((21.0, -1.0, 10.0), (21.0, 19.0, 10.0), (21.0, 9.0, 16.0), HOUSE)
    p.box(-4.0, -1.0, 1.5, 4.0, 0.0, 7.0, DARK)  # (its door, to the south)
    for x in (-18.0, 18.0):
        p.box(x - 1.0, -20.0, 1.5, x + 1.0, -18.0, 6.0, METAL)
    return m


def fact():
    """Construction yard, three cells across two: a works with a crane."""
    m = Model("FACT", "building", (3, 2))
    p = m.part("body")
    pad(p, m.cells)
    p.box(-32.0, -18.0, 1.5, 8.0, 18.0, 16.0, HOUSE_DARK, top=ROOF)
    p.box(-28.0, -20.0, 1.5, -10.0, -18.0, 12.0, DARK)
    p.cylinder((22.0, 0.0, 1.5), (22.0, 0.0, 30.0), 1.6, 6, METAL)
    p.box(4.0, -1.5, 28.0, 30.0, 1.5, 31.0, LIGHT, top=METAL)
    p.box(22.0, 10.0, 1.5, 32.0, 18.0, 6.0, METAL, top=DARK)
    return m


def proc():
    """Refinery, three cells square: the plant, a tank and the dock a harvester backs into."""
    m = Model("PROC", "building", (3, 3))
    p = m.part("body")
    pad(p, m.cells)
    p.box(-32.0, -2.0, 1.5, 12.0, 32.0, 18.0, HOUSE_DARK, top=ROOF)
    p.cylinder((24.0, 22.0, 1.5), (24.0, 22.0, 20.0), 8.0, 10, METAL)
    p.box(-14.0, -32.0, 1.5, 6.0, -2.0, 3.0, DARK)  # the dock
    p.box(-16.0, -6.0, 18.0, -6.0, 6.0, 24.0, RUST)
    p.cylinder((0.0, 20.0, 18.0), (0.0, 20.0, 28.0), 1.6, 6, METAL)
    return m


def hq():
    """Communications centre, two cells square: a block and its dish."""
    m = Model("HQ", "building", (2, 2))
    p = m.part("body")
    pad(p, m.cells)
    p.box(-18.0, -18.0, 1.5, 18.0, 10.0, 14.0, HOUSE_DARK, top=ROOF)
    p.cylinder((8.0, 4.0, 14.0), (8.0, 4.0, 19.0), 1.2, 6, METAL)
    p.cylinder((8.0, 4.0, 19.0), (8.0, 0.0, 24.0), 9.0, 10, WHITE, end_radius=1.0)
    p.box(-14.0, 10.0, 1.5, -2.0, 18.0, 8.0, GLASS)
    return m


def gtwr():
    """Guard tower, one cell: a post, its gun turns."""
    m = Model("GTWR", "building", (1, 1))
    p = m.part("body")
    pad(p, m.cells, margin=2.0)
    p.prism(p.octagon(0, 0, 6.0, 6.0, 2.0), 1.5, 12.0, CONCRETE, top=CONCRETE_DARK, shrink=0.8)
    gun = m.part("gun", pivot=(0.0, 0.0, 12.0), turns=True)
    gun.box(-2.0, -2.0, 0, 2.0, 2.0, 2.5, DARK)
    gun.cylinder((0, 1.5, 1.4), (0, 8.0, 1.4), 0.5, 4, METAL)
    return m


def gun():
    """Gun turret, one cell: a low dome, its gun turns."""
    m = Model("GUN", "building", (1, 1))
    p = m.part("body")
    pad(p, m.cells, margin=2.0)
    gun = m.part("turret", pivot=(0.0, 0.0, 1.5), turns=True)
    gun.prism(gun.octagon(0, 0, 7.0, 7.0, 2.5), 0, 5.0, HOUSE_DARK, top=HOUSE, shrink=0.7)
    gun.cylinder((0, 3.0, 3.0), (0, 13.0, 3.0), 0.9, 5, METAL)
    return m


ALL = [nuke, silo, pyle, fact, proc, hq, gtwr, gun]
