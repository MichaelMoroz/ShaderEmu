"""Tiberian Dawn's vehicles made by script (mesh.py). Pixels: a cell is 24; x right, y ahead, z up.

Each is a few boxes, wedges and tubes; a turret is a part of its own that turns about its pivot.
The names are the game's ini names (its `UnitTypeClass` table), which is how the game finds them.
"""
from mesh import (Model, HOUSE, HOUSE_DARK, METAL, DARK, BLACK, GLASS, LIGHT, RUST)


def tracks(p, half_w, half_l, width=3.0, height=4.0):
    """Two tracks, each a box with its ends cut (a wedge at the front and the back)."""
    for side in (-1, 1):
        x0 = side * half_w - (width if side > 0 else 0)
        x1 = x0 + width
        p.prism([(x0, -half_l + 1.5), (x1, -half_l + 1.5), (x1, half_l - 1.5), (x0, half_l - 1.5)], 0, height, DARK)
        p.wedge(x0, half_l - 1.5, 0.8, x1, half_l, height, DARK, 2.0)


def wheels(p, half_w, positions, radius=2.2, width=1.6):
    for y in positions:
        for side in (-1, 1):
            x = side * half_w
            p.cylinder((x - width / 2, y, radius), (x + width / 2, y, radius), radius, 6, BLACK)


def mtnk():
    """Medium tank (GDI): hull on tracks, a turret with one gun."""
    m = Model("MTNK")
    hull = m.part("hull")
    tracks(hull, 7.0, 9.5)
    hull.prism(hull.octagon(0, 0, 4.2, 9.0, 1.5), 2.5, 5.5, HOUSE_DARK, top=HOUSE, shrink=0.9)
    hull.wedge(-4.0, 6.5, 2.5, 4.0, 9.0, 5.3, HOUSE, 2.0)
    turret = m.part("turret", pivot=(0.0, -1.0, 5.5), turns=True)
    turret.prism(turret.octagon(0, 0, 3.6, 4.2, 1.4), 0, 3.0, HOUSE_DARK, top=HOUSE, shrink=0.85)
    turret.cylinder((0, 3.5, 1.5), (0, 12.0, 1.5), 0.8, 5, METAL)
    turret.box(-1.0, -3.5, 3.0, 1.0, -1.5, 3.8, DARK)  # (a hatch)
    return m


def ltnk():
    """Light tank (Nod): lower and narrower, a small turret."""
    m = Model("LTNK")
    hull = m.part("hull")
    tracks(hull, 6.0, 8.0, width=2.6, height=3.5)
    hull.prism(hull.octagon(0, 0, 3.6, 7.6, 1.2), 2.2, 4.6, HOUSE_DARK, top=HOUSE, shrink=0.88)
    turret = m.part("turret", pivot=(0.0, -0.5, 4.6), turns=True)
    turret.prism(turret.octagon(0, 0, 2.8, 3.2, 1.1), 0, 2.4, HOUSE_DARK, top=HOUSE, shrink=0.8)
    turret.cylinder((0, 2.6, 1.2), (0, 9.5, 1.2), 0.6, 5, METAL)
    return m


def htnk():
    """Mammoth tank (GDI): wide, two guns, missile pods on the turret's sides."""
    m = Model("HTNK")
    hull = m.part("hull")
    tracks(hull, 9.5, 11.5, width=4.0, height=5.0)
    hull.prism(hull.octagon(0, 0, 5.5, 11.0, 2.0), 3.0, 6.5, HOUSE_DARK, top=HOUSE, shrink=0.9)
    turret = m.part("turret", pivot=(0.0, -1.5, 6.5), turns=True)
    turret.prism(turret.octagon(0, 0, 5.0, 5.0, 1.8), 0, 3.5, HOUSE_DARK, top=HOUSE, shrink=0.85)
    for x in (-1.5, 1.5):
        turret.cylinder((x, 4.0, 1.7), (x, 14.0, 1.7), 0.8, 5, METAL)
    for side in (-1, 1):
        turret.box(side * 5.0 - (2.5 if side > 0 else 0), -3.0, 0.5, side * 5.0 + (2.5 if side < 0 else 0), 3.0, 3.0, DARK, top=METAL)
    return m


def jeep():
    """Humvee (GDI): a body on four wheels, a glass cab, a gun on top that turns."""
    m = Model("JEEP")
    body = m.part("body")
    wheels(body, 4.6, (-5.0, 5.0))
    body.box(-4.0, -7.5, 1.6, 4.0, 7.5, 4.6, HOUSE_DARK, top=HOUSE)
    body.wedge(-3.8, 1.0, 4.6, 3.8, 7.0, 6.0, HOUSE, 1.4)
    body.prism([(-3.5, -2.5), (3.5, -2.5), (3.5, 1.5), (-3.5, 1.5)], 4.6, 7.2, GLASS, top=HOUSE, shrink=0.85)
    gun = m.part("gun", pivot=(0.0, -4.5, 4.6), turns=True)
    gun.box(-1.0, -1.0, 0, 1.0, 1.0, 2.2, DARK)
    gun.cylinder((0, 0.8, 1.6), (0, 5.0, 1.6), 0.4, 4, METAL)
    return m


def bggy():
    """Nod buggy: an open frame on four wheels, a gun that turns."""
    m = Model("BGGY")
    body = m.part("body")
    wheels(body, 4.4, (-4.5, 4.5), radius=2.4)
    body.wedge(-3.2, -6.5, 1.8, 3.2, 6.5, 4.0, HOUSE_DARK, 1.0, top=HOUSE)
    for x in (-3.0, 3.0):  # (the roll bar)
        body.cylinder((x, -2.0, 4.0), (x, -2.0, 7.0), 0.35, 4, METAL)
    body.cylinder((-3.0, -2.0, 7.0), (3.0, -2.0, 7.0), 0.35, 4, METAL)
    gun = m.part("gun", pivot=(0.0, -4.0, 4.0), turns=True)
    gun.box(-0.8, -0.8, 0, 0.8, 0.8, 1.8, DARK)
    gun.cylinder((0, 0.6, 1.3), (0, 4.5, 1.3), 0.35, 4, METAL)
    return m


def apc():
    """Armoured carrier (GDI): a sloped box on tracks."""
    m = Model("APC")
    hull = m.part("hull")
    tracks(hull, 6.5, 9.0, width=2.8, height=3.8)
    hull.wedge(-4.0, -9.0, 2.0, 4.0, 9.0, 7.0, HOUSE_DARK, 3.0, top=HOUSE)
    hull.box(-1.2, -5.0, 7.0, 1.2, -2.5, 8.2, DARK)
    hull.cylinder((0, -3.7, 7.6), (0, 0.5, 7.6), 0.35, 4, METAL)
    return m


def harv():
    """Harvester: a long body, a cab ahead, the scoop at its front."""
    m = Model("HARV")
    body = m.part("body")
    tracks(body, 8.0, 11.0, width=3.4, height=4.5)
    body.box(-5.0, -11.0, 2.5, 5.0, 4.0, 9.0, HOUSE_DARK, top=HOUSE)
    body.wedge(-4.5, 4.0, 2.5, 4.5, 9.0, 8.0, HOUSE, 2.0)
    body.prism([(-3.5, 5.0), (3.5, 5.0), (3.5, 8.5), (-3.5, 8.5)], 8.0 - 2.0 * 0.2, 10.0, GLASS, top=HOUSE, shrink=0.8)
    body.wedge(-6.0, 9.0, 0.2, 6.0, 12.5, 3.5, RUST, 2.8)  # the scoop
    body.cylinder((-3.0, -9.0, 9.0), (-3.0, -9.0, 12.0), 0.7, 5, DARK)  # (its exhaust)
    return m


def mcv():
    """Mobile construction vehicle: a large truck with its folded yard on the back."""
    m = Model("MCV")
    body = m.part("body")
    tracks(body, 9.0, 12.0, width=3.6, height=4.8)
    body.box(-5.8, -12.0, 2.5, 5.8, 12.0, 5.5, DARK, top=METAL)
    body.wedge(-5.0, 5.5, 5.5, 5.0, 11.5, 10.0, HOUSE, 2.5)
    body.box(-5.5, -11.5, 5.5, 5.5, 4.5, 11.0, HOUSE_DARK, top=HOUSE)
    body.box(-4.0, -9.0, 11.0, 0.0, -2.0, 13.0, METAL)
    body.prism([(-4.0, 7.0), (4.0, 7.0), (4.0, 10.0), (-4.0, 10.0)], 10.0 - 2.5 * 0.5, 11.5, GLASS, top=HOUSE, shrink=0.8)
    return m


def arty():
    """Artillery (Nod): a low carriage, a long gun fixed ahead."""
    m = Model("ARTY")
    hull = m.part("hull")
    tracks(hull, 6.0, 8.5, width=2.6, height=3.4)
    hull.box(-3.8, -8.0, 2.0, 3.8, 3.0, 4.6, HOUSE_DARK, top=HOUSE)
    hull.wedge(-3.0, -4.0, 4.6, 3.0, 2.0, 7.0, HOUSE, 0.5)
    hull.cylinder((0, 0.0, 6.0), (0, 15.0, 8.0), 0.9, 5, METAL)
    return m


def ftnk():
    """Flame tank (Nod): a rounded hull, two flame tubes ahead."""
    m = Model("FTNK")
    hull = m.part("hull")
    tracks(hull, 7.0, 9.0, width=3.0, height=4.0)
    hull.prism(hull.octagon(0, -0.5, 4.2, 8.5, 2.5), 2.5, 7.0, HOUSE_DARK, top=HOUSE, shrink=0.7)
    for x in (-2.0, 2.0):
        hull.cylinder((x, 5.0, 4.5), (x, 11.5, 4.5), 0.9, 5, DARK)
        hull.cylinder((x, 11.5, 4.5), (x, 12.3, 4.5), 0.6, 5, LIGHT)
    return m


def stnk():
    """Stealth tank (Nod): flat and angular, missile pods on its back."""
    m = Model("STNK")
    hull = m.part("hull")
    tracks(hull, 6.5, 9.0, width=2.6, height=3.4)
    hull.prism([(-4.5, -8.5), (4.5, -8.5), (4.5, 4.0), (0.0, 10.0), (-4.5, 4.0)], 2.2, 5.0, HOUSE_DARK, top=HOUSE, shrink=0.85)
    for side in (-1, 1):
        hull.box(side * 2.6 - 1.2, -7.0, 5.0, side * 2.6 + 1.2, -1.0, 6.8, DARK, top=METAL)
    return m


def msam():
    """Rocket launcher (GDI's MLRS here: MSAM): a truck whose launcher turns."""
    m = Model("MSAM")
    body = m.part("body")
    wheels(body, 4.8, (-6.0, 0.0, 6.0), radius=2.0)
    body.box(-4.2, -8.5, 1.6, 4.2, 8.5, 4.4, HOUSE_DARK, top=HOUSE)
    body.wedge(-4.0, 4.0, 4.4, 4.0, 8.5, 8.0, HOUSE, 2.0)
    launcher = m.part("launcher", pivot=(0.0, -3.0, 4.4), turns=True)
    launcher.box(-1.5, -1.5, 0, 1.5, 1.5, 1.5, DARK)
    launcher.wedge(-3.2, -4.0, 1.5, 3.2, 4.0, 5.5, METAL, -2.0, top=DARK)
    return m


ALL = [mtnk, ltnk, htnk, jeep, bggy, apc, harv, mcv, arty, ftnk, stnk, msam]
