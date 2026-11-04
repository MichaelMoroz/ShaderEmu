# What stands in the room. The sofa, tables, stools, plants, laptop, television, boxes and
# the encyclopedia are Poly Haven's models (assets.py); the games, the software and the magazines
# are scans of the real things (world/gameboxes.py); the rest is modelled here.
import json
import math
import os

import bmesh

from room import HALF_D, HALF_W, frame, on_wall
import drape
import lib
import pc
from mathutils import Vector
from pc import office_chair


def mug(b, at, mat="Ceramic", turn=0):
    with b.at(at, (0, turn, 0)):
        b.lathe((0, 0, 0), [(0, 0.004), (0.036, 0.004), (0.04, 0), (0.041, 0.095), (0.037, 0.095), (0.036, 0.008), (0, 0.008)], mat, segs=24)
        b.tube([(0.04, 0.075, 0), (0.062, 0.07, 0), (0.066, 0.045, 0), (0.058, 0.022, 0), (0.04, 0.02, 0)], 0.006, mat, segs=8, smooth=4)


def cushion(b, at, side, thick, rot):
    """A stuffed square cushion standing in its own xy plane: fullest round a button sewn through
    its middle, creased towards its corners, piped along its seam, its corners drawn out into ears."""
    n, vertices, faces, uvs, normals, seam = 20, [], [], [], [], []

    def ear(u, v):   # sides drawn in, corners out
        return 1 + 0.07 * (u * v) ** 2 - 0.05 * (1 - (u * v) ** 2) * (u * u + v * v) / 2
    for face in (1, -1):
        first = len(vertices)
        for j in range(n + 1):
            for i in range(n + 1):
                u, v = 2 * i / n - 1, 2 * j / n - 1
                full = ((1 - u ** 4) * (1 - v ** 4)) ** 0.42
                full *= 1 - 0.5 * math.exp(-(u * u + v * v) / 0.012)                                  # the button pulls it in
                full *= 1 - 0.1 * (u * v) ** 2 * (0.5 + 0.5 * math.cos(9 * math.atan2(v, u)))          # creases run to the corners
                vertices.append((u * side / 2 * ear(u, v), v * side / 2 * ear(u, v), face * thick / 2 * full))
        for j in range(n):
            for i in range(n):
                a = first + j * (n + 1) + i
                faces.append((a, a + 1, a + n + 2, a + n + 1))
                uvs.append([(vertices[k][0] / 0.28, vertices[k][1] / 0.28) for k in faces[-1]])
                normals.append((0, 0, face))
    for u, v in [(2 * i / n - 1, -1) for i in range(n)] + [(1, 2 * i / n - 1) for i in range(n)] + [(1 - 2 * i / n, 1) for i in range(n)] + [(-1, 1 - 2 * i / n) for i in range(n + 1)]:
        seam.append((u * side / 2 * ear(u, v), v * side / 2 * ear(u, v), 0))
    with b.at(at, rot):
        b.mesh(vertices, faces, uvs, "FabricCushion", normal=normals)
        b.tube(seam, 0.0045, "FabricCushion", segs=6, smooth=0)
        for face in (1, -1):
            b.cyl((0, 0, face * thick * 0.24), 0.012, 0.004, "FabricCushion", segs=12, bevel=0.0015, rot=(90, 0, 0))


def sofa(b):
    b.asset("sofa_02", (-0.4, 0, -HALF_D + 0.63), size=(2.26, None, None))   # its back clear of the panelling's cap
    b.obj("Cushions")
    with b.at((-0.4, 0, -HALF_D + 0.63)):
        cushion(b, (-0.74, 0.60, 0.0), 0.44, 0.15, (-24, 16, 8))
        cushion(b, (0.76, 0.59, 0.0), 0.42, 0.15, (-22, -22, -6))


def low_table(b):
    b.obj("Rugs")
    b.quad((-0.4, 0.006, -HALF_D + 1.75), (3.8, 2.2), "Rug", rot=(90, 0, 0), uvrect=(0, 0, 1, 1))
    at = (-0.4, 0.003, -HALF_D + 1.75)   # (its feet in the rug's pile, which is 6 mm, not through it)
    b.asset("modern_coffee_table_01", at, rot=(0, 90, 0), size=(None, 0.40, None))
    b.obj("Table things")
    with b.at(at):
        top = 0.401
        mug(b, (0.32, top, 0.08), turn=40)
        # a game and the software it wants, and magazines spread where the papers were
        pile(b, ["windows95", "half_life"], (-0.17, top, -0.05), (-12, 7))
        pile(b, ["computer_gaming_world_issue_156", "pc_computing_magazine_v8i11"], (0.03, top, 0.15), (96, 81), drift=0.03)
        lying(b, "bytevol1808199307pentiumpcs", (0.25, top, -0.12), 64)


# The classroom in the middle of the den: three rows of two tables in the main desk's oak, an
# office chair and a computer of the decade (world/pc.py) at each of twelve places, one a visitor.
# Unity puts each owner's picture on its glass (ShaderEmuStations.cs reads Models/stations.json).
ROWS_Z = (2.45, 0.45, -1.55)
PLACES_X = (-1.5, -0.5, 0.5, 1.5)
TABLES_X = (-1.0, 1.0)
CHAIRS_BACK = (0.90, 0.90, 0.66)   # a row's chairs behind its places: clear of the next row's leads, the last row's pushed in clear of the rug
TABLE_TOP = 0.765   # as the main desk's
STATION_Z = 0.0     # a place's computer: the monitor is as deep as the table allows


def table_parts(x, z):
    """A table's boxes (centre, size, material, bevel, grain): its top, its panel ends and their feet, the board behind the knees."""
    parts = [((x, TABLE_TOP - 0.022, z), (2.0, 0.044, 0.9), "WoodOak", (0.01, 3), 0), ((x, 0.45, z + 0.38), (1.89, 0.4, 0.022), "WoodOak", (0, 2), 0)]
    for side in (-1, 1):
        parts.append(((x + side * 0.96, (TABLE_TOP - 0.044) / 2, z), (0.03, TABLE_TOP - 0.044, 0.8), "WoodOak", (0.004, 1), 1))
        parts.append(((x + side * 0.96, 0.012, z), (0.05, 0.024, 0.84), "PlasticDark", (0.004, 1), 'auto'))
    return parts


def table(b, x, z):
    """Two metres of the main desk's kind: an oak top on panel ends, a board behind the knees."""
    for centre, size, mat, (bevel, segs), grain in table_parts(x, z):
        b.box(centre, size, mat, bevel=bevel, segs=segs, grain=grain)
    for gx in (-0.5, 0.5):   # grommets for the cables
        b.lathe((x + gx, TABLE_TOP + 0.0005, z + 0.36), [(0, 0), (0.028, 0), (0.032, 0.002), (0.032, 0.004), (0.02, 0.005)], "PlasticDark", segs=16)


GAME_BOXES = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "gameboxes.json")))
GAME_AXES = {"front": ((0, 0, -1), (1, 0, 0), (0, 1, 0)), "back": ((0, 0, 1), (-1, 0, 0), (0, 1, 0)),   # as world/gameboxes.py's
             "left": ((-1, 0, 0), (0, 0, -1), (0, 1, 0)), "right": ((1, 0, 0), (0, 0, 1), (0, 1, 0)),
             "top": ((0, 1, 0), (1, 0, 0), (0, 0, 1)), "bottom": ((0, -1, 0), (1, 0, 0), (0, 0, -1))}


def game_box(b, game, at, rot=(0, 0, 0)):
    """A game's box from world/gameboxes.json, its middle at `at`, its front towards -z, its edges worn round a millimetre."""
    box = GAME_BOXES[game]
    half = Vector(box["size"]) / 2
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(box["size"]), verts=bm.verts)
    b._bevel(bm, 0.001, 1)
    uv = bm.loops.layers.uv.verify()
    bm.normal_update()
    for f in bm.faces:   # a bevel's face takes the line along the edge of the face it is most of
        face, (n, right, up) = max(GAME_AXES.items(), key=lambda kv: f.normal.dot(Vector(kv[1][0])))
        u0, v0, u1, v1 = box["faces"][face]
        r, t = Vector(right), Vector(up)
        hr, ht = abs(half.dot(r)), abs(half.dot(t))
        for loop in f.loops:
            s, q = (loop.vert.co.dot(r) / hr + 1) / 2, (loop.vert.co.dot(t) / ht + 1) / 2
            loop[uv].uv = (u0 + s * (u1 - u0), v0 + q * (v1 - v0))
    b.random.random(), b.random.random()   # what a plain box drew for its texture: the books after it stay where they were
    b._emit(bm, box.get("atlas", "GameBoxes"), at, rot)


def thing_size(name):
    """A box's or a magazine's width, height and thickness (world/gameboxes.json)."""
    return GAME_BOXES[name]["size"]


def lying(b, name, at, turn):
    """A box or a magazine lying face up, its underside's middle at `at`, turned about up."""
    w, h, t = thing_size(name)
    game_box(b, name, (at[0], at[1] + t / 2, at[2]), rot=(90, turn, 0))


def pile(b, names, at, turns, drift=0.006):
    """Things lying one on another from `at` up, the first lowest, each turned its own way and a
    little off the one under it. Gives the pile's top."""
    x, y, z = at
    for i, (name, turn) in enumerate(zip(names, turns)):
        lying(b, name, (x + drift * math.sin(i * 2.3), y, z + drift * math.cos(i * 1.7)), turn)
        y += thing_size(name)[2] + 0.0004
    return y


def standing(b, name, x, y, z, lean=0.0):
    """A box upright on y with its spine (its left side) towards -z, as on a shelf: its foot's
    edge towards -x at x, its middle at z. `lean` tips it that way about that edge, onto a
    neighbour. Gives where along x it ends."""
    w, h, t = thing_size(name)
    with b.at((x, y, z), (0, 0, lean)):
        game_box(b, name, (t / 2, h / 2, 0), rot=(0, -90, 0))
    return x + t * math.cos(math.radians(lean))


def shelf_row(b, names, left, right, y, front, rng, most=99):
    """Boxes standing spine out along a shelf from `left`, taken from `names` while they fit (and
    no more than `most`),
    their spines a little in or out from `front`; the last leans on the one before when there is
    room. Gives where the row ends."""
    x, stood = left, 0
    while names and stood < most:
        stood += 1
        w, h, t = thing_size(names[0])
        if x + t > right or w > 0.3:
            break
        name = names.pop(0)
        x = standing(b, name, x, y, front + w / 2 + rng.uniform(0, 0.02)) + 0.001
        if names and rng.random() < 0.25:   # now and then a little gap
            x += rng.uniform(0.01, 0.03)
    if names and stood < most:
        w, h, t = thing_size(names[0])
        lean = 14.0
        if x + h * math.sin(math.radians(lean)) + t + 0.01 < right and w <= 0.3:
            name = names.pop(0)
            x = standing(b, name, x + h * math.sin(math.radians(lean)), y, front + w / 2 + 0.01, lean)
    return x


SHELF_GAMES = ["doom2", "ultimate_doom", "heretic", "hexen", "duke3d", "descent", "dark_forces", "jedi_knight", "quake2", "half_life", "unreal",
               "thief", "system_shock", "underworld", "ultima7", "arena", "daggerfall", "baldurs_gate", "fallout", "fallout2", "diablo",
               "warcraft2", "civ2", "simcity", "simcity2000", "settlers2", "age_of_empires", "syndicate", "xwing", "wing_commander3",
               "monkey2", "tentacle", "sam_max", "full_throttle", "the_dig", "grim_fandango", "broken_sword", "flashback", "alone_dark",
               "lost_vikings", "lemmings2", "worms", "rayman", "earthworm_jim"]
SOFTWARE = ["msdos6", "windows31", "windows95", "netscape", "flight_sim5", "entertainment_pack"]
MAGAZINES = [name for name, thing in GAME_BOXES.items() if thing.get("atlas") == "Magazines"]


def leaning_box(b, x, y, wall, turn, lean, game):
    """A game in its box standing on y at x, turned, and leaning back till its top meets the face at z = wall."""
    w, h, t = GAME_BOXES[game]["size"]
    a, yaw = math.radians(lean), math.radians(abs(turn))
    back = h / 2 * math.sin(a) + t / 2 * math.cos(a)
    with b.at((x, y + h / 2 * math.cos(a) + t / 2 * math.sin(a), wall - 0.0005 - back * math.cos(yaw) - w / 2 * math.sin(yaw)), (lean, turn, 0)):
        game_box(b, game, (0, 0, 0))


# a place: the boxes and magazines lying on its tower, the lowest first, and their turns (the hubs' towers, 1 and 9, have none)
ON_TOWERS = {0: (["doom2"], (3,)), 2: (["computer_gaming_world_issue_132", "computer_gaming_world_issue_101"], (-4, 6)),
             4: (["msdos6", "windows31"], (2, -5)), 6: (["bytevolume22number01"], (-7,)), 7: (["warcraft2"], (5,)),
             10: (["pc_computing_magazine_v7i9", "simcity2000"], (4, -3)), 11: (["computer_gaming_world_issue_180"], (8,))}
HUB_HOSTS = {0: 1, 2: 9}   # a row: the place whose tower its hub stands on (the middle row's is on the floor)
LAN_R = 0.003


HUB_SUPPLY = {0: 1, 1: 5, 2: 9}   # a row: the place whose strip its hub's supply is plugged into
DC_R = 0.0016                     # the supply's lead


def network(b, places, laid, strips):
    """The room's network: a lead from every tower's card to its row's hub, the hubs to each other
    and the first to the den's rack, and each hub's supply. places: each place's frame; laid: its
    own leads as they fell (name, path, radius), in the room's frame; strips: where each place's
    strip lies, in its frame. Gives the hubs for Models/stations.json."""
    import random
    rng = random.Random(77)
    T, R = TABLE_TOP, LAN_R
    back = pc.TOWER_Z + pc.FRONT + pc.TOWER_DEEP                  # a tower's back, in its place
    card = pc.tower_back()["lan"]
    face = pc.TOWER_X + 0.09                                      # a tower's right side
    hub_at = (pc.TOWER_X + 0.018, pc.TOWER_HIGH, 0.17)            # a hub on a tower, in the place's frame: turned, its sockets to the right
    boxes = [((0, -0.05, 0), (12, 0.1, 14), 0), ((0.95, 0.375, HALF_D - 0.52), (0.48, 0.75, 0.9), 0), ((3.75, 0.04, HALF_D - 0.45), (0.7, 0.08, 0.8), 0)]
    for z in ROWS_Z:
        for x in TABLES_X:
            boxes += [(centre, size, 0) for centre, size, _, _, _ in table_parts(x, z)]
    lanes = (0.255, 0.30)   # the network's leads under a table: so far behind the row's middle
    bumps = []   # where a strip's own lead lies across a lane: from x to x, the height to clear, the row, the lane
    for index, world in enumerate(places):
        turn = math.degrees(math.atan2(world[0][2], world[0][0]))
        for centre, size in (((pc.TOWER_X, pc.TOWER_HIGH / 2, back - pc.TOWER_DEEP / 2), (0.184, pc.TOWER_HIGH, pc.TOWER_DEEP)),
                             ((pc.SET_X, pc.LOW + pc.OUT_H / 2, 0.12), (pc.OUT_W, pc.OUT_H, 0.52))):
            boxes.append((tuple(world @ Vector(centre)), size, turn))
        if index % 4 in (0, 2):   # its mouse's lead lies behind the tower: the network's goes over a shelf as high as a lead
            boxes.append((tuple(world @ Vector((pc.TOWER_X, 0.006, back + 0.05))), (0.3, 0.012, 0.09), turn))
        row_z = ROWS_Z[index // 4]
        for name, path, radius in laid[index]:
            every = [a.lerp(c, k / 8) for a, c in zip(path, path[1:]) for k in range(9)] if name == "strip" else []
            for lane in lanes:
                under = [p for p in every if abs(p.z - row_z - lane) < 0.03]
                if under:
                    x0, x1, high = min(p.x for p in under) - radius - 0.002, max(p.x for p in under) + radius + 0.002, max(p.y for p in under) + radius
                    boxes.append((((x0 + x1) / 2, high / 2, row_z + lane), (x1 - x0, high, 0.06), 0))
                    bumps.append((x0, x1, high, index // 4, lane))
    hubs, groups, ends = [], {0: [], 1: [], 2: []}, {}
    b.obj("Network")

    def lay(group, name, corners, mat, up=0.012, bend=0.04, radius=R, chance=None):
        chance = chance or rng
        del pc.LEADS[:]
        pc.lead(b, name, corners, radius, mat, bend=bend)
        groups[group].append((name, drape.ruffled(pc.LEADS[0][1], chance, up=up * chance.uniform(0.3, 1.0), wander=0.002), radius, mat))

    def table_edge(row, world, x):
        """The row's tables' back edge in a place's frame, at x of that frame."""
        return (world.inverted() @ Vector(((world @ Vector((x, 0, 0.45))).x, T, ROWS_Z[row] + 0.45))).z

    def supply(row, hub_frame, power, laid_out):
        """The hub's supply lead: from its plug in the hub's socket at `power`, through `laid_out`
        (the room's points), along the floor before its place's strip and up into the supply on it."""
        index = HUB_SUPPLY[row]
        world, lies = places[index], strips[index]
        frame = hub_frame @ lib.place(pc.hub_power(power))
        with b.at((0, 0, 0)):
            b.stack[-1] = frame
            pc.dc_plug(b, 0, 0, 0)
        start, out = frame @ Vector((0, 0, pc.DC_LONG - 0.002)), frame @ Vector((0, 0, pc.DC_LONG + 0.014))
        s = Vector(pc.strip_socket(pc.HUB_SOCKET, lies))
        low, top = -T + DC_R + 0.001, s.y + pc.ADAPTER_HIGH
        front = min(table_edge(row, world, s.x) + 0.022, lies[2] - 0.062)   # before the strip and the mains leads' fall, under the table's edge
        local = [(s.x + 0.035, low, front), (s.x + 0.01, -T + 0.105, lies[2] - 0.032), (s.x, top + 0.03, lies[2]), (s.x, top - 0.002, lies[2])]
        lay(row, "supply %d" % row, [tuple(start), tuple(out)] + [tuple(p) for p in laid_out(front)] + [tuple(world @ Vector(p)) for p in local],
            "Rubber", radius=DC_R, up=0.008, chance=random.Random(78 + row))   # (its own chance: the network's leads keep theirs)
        boxes.extend((tuple(world @ Vector(centre)), size, turn + math.degrees(math.atan2(world[0][2], world[0][0])))
                     for centre, size, turn in (pc.strip_box(lies), pc.adapter_box(lies, pc.HUB_SOCKET)))

    def plugged(world, x, y, z, mat, turn=(0, 0, 0)):
        """A plug in a socket at x, y, z of a frame whose +z, turned, the socket looks along; gives where its lead leaves it, and a point further out."""
        frame = world @ lib.place((x, y, z), turn)
        with b.at((0, 0, 0)):
            b.stack[-1] = frame
            pc.lan_plug(b, 0, 0, 0, mat)
        return tuple(frame @ Vector((0, 0.0006, pc.LAN_PLUG - 0.004))), tuple(frame @ Vector((0, 0.0006, pc.LAN_PLUG + 0.03)))

    def under_table(row, start, lane, stop):
        """Along the floor under a row's tables from x = start to x = stop, lane metres behind the row's middle, over each strip's lead on the way."""
        z, points = ROWS_Z[row] + lane, [(start, R + 0.001, ROWS_Z[row] + lane)]
        for x0, x1, high, r, crossed in sorted(bumps, key=lambda bump: abs(bump[0] - start)):
            if (r, crossed) == (row, lane) and min(start, stop) < x0 and x1 < max(start, stop):
                over = [(x0 - 0.03, R + 0.001, z), (x0 - 0.006, high + R + 0.005, z), (x1 + 0.006, high + R + 0.005, z), (x1 + 0.03, R + 0.001, z)]
                points += over if stop > start else over[::-1]
        return points + [(stop, R + 0.001, z)]

    def colour(index):
        return "CableYellow" if index in (2, 8) else "CableBlue"

    def from_card(index):
        """A place's lead from its card over the table's back edge to the floor behind: its points, and its x there."""
        world = places[index]
        start, out = plugged(world, pc.TOWER_X + card[0], card[1], back, colour(index))
        edge = ROWS_Z[index // 4] + 0.45
        x = (world @ Vector((pc.TOWER_X + 0.055, 0, 0.45))).x
        return [start, out, (x, T + 0.03, edge - 0.012), (x, T - 0.04, edge + 0.028), (x, 0.3, edge + 0.03), (x, R + 0.001, edge + 0.03)], x

    def riser(row, k, floor_x):
        """Up from the floor behind a row's middle into socket k of the hub on a tower: over the table's edge, along the table beside the tower, up its side."""
        world = places[HUB_HOSTS[row]]
        lane, z = face + 0.03 + 0.012 * k, hub_at[2] + 0.07 - pc.HUB_PITCH * k
        edge = (world.inverted() @ Vector(((world @ Vector((lane, 0, 0.45))).x, T, ROWS_Z[row] + 0.45))).z
        end, out = plugged(world @ lib.place(hub_at, (0, 90, 0)), pc.hub_port(k)[0], pc.hub_port(k)[1], pc.HUB[2] / 2, colours[(row, k)])
        local = [(lane, -0.25, edge + 0.03), (lane, -0.02, edge + 0.03), (lane, 0.014, edge + 0.01), (lane, R + 0.003, edge - 0.02), (lane, R + 0.001, z + 0.04), (face + 0.012, R + 0.001, z),
                 (face + 0.012, 0.08, z), (face + 0.03, pc.TOWER_HIGH - 0.06, z)]
        return [(floor_x, R + 0.001, ROWS_Z[row] + 0.47)] + [tuple(world @ Vector(p)) for p in local] + [out, end]

    colours = {}
    for row, z in enumerate(ROWS_Z):
        left, near_left, near_right, right = (row * 4 + c for c in range(4))
        if row in HUB_HOSTS:
            world = places[HUB_HOSTS[row]] @ lib.place(hub_at, (0, 90, 0))
            # the sockets' leads, from the rear one: the tower's own, the left table's, the hubs' two, the right table's far and near
            through = [None, -1, -1] if row == 0 else [None, None, -1]
            for k, index in enumerate([near_left, left] + through[1:] + [right, near_right]):
                colours[(row, k)] = "CableBlue" if index is None or index < 0 else colour(index)
            floor_x = {1: -0.05, 2: 0.0, 3: 0.035, 4: 0.07, 5: 0.11}
            # the tower's own card: round its back corner to the table beside it, and up its side
            start, out = plugged(places[near_left], pc.TOWER_X + card[0], card[1], back, colour(near_left))
            end, last = plugged(world, pc.hub_port(0)[0], pc.hub_port(0)[1], pc.HUB[2] / 2, colour(near_left))
            port_z = hub_at[2] + 0.07
            own = [(face - 0.01, 0.07, back + 0.085), (face + 0.03, R + 0.002, back + 0.06), (face + 0.03, R + 0.001, port_z + 0.04), (face + 0.012, R + 0.001, port_z),
                   (face + 0.012, 0.08, port_z), (face + 0.03, pc.TOWER_HIGH - 0.06, port_z)]
            lay(row, "lan %d" % near_left, [start, out] + [tuple(places[near_left] @ Vector(p)) for p in own] + [last, end], colour(near_left))
            down, x = from_card(left)
            lay(row, "lan %d" % left, down + under_table(row, x, lanes[1], -0.10) + [(-0.10, R + 0.001, z + 0.44)] + riser(row, 1, floor_x[1]), colour(left))
            down, x = from_card(near_right)
            lay(row, "lan %d" % near_right, down + under_table(row, x, lanes[1], 0.15) + [(0.15, R + 0.001, z + 0.44)] + riser(row, 5, floor_x[5]), colour(near_right))
            down, x = from_card(right)
            lay(row, "lan %d" % right, down[:-1] + [(min(x - 0.01, 1.92), R + 0.001, z + 0.48)] + under_table(row, min(x - 0.04, 1.89), lanes[0], 0.105)   # (clear of the table's end)
                + [(0.105, R + 0.001, z + 0.44)] + riser(row, 4, floor_x[4]), colour(right))
            ports = [(0, near_left), (1, left), (4, right), (5, near_right)]
            if row == 0:
                # to the middle row's hub: through between the two tables' feet; and to the den's rack along the desk's foot
                ends["A-B"] = (0.0, R + 0.001, ROWS_Z[1] + 1.25)
                lay(row, "hubs 0", list(reversed(riser(row, 2, floor_x[2]))) + [(0.0, R + 0.001, z + 0.40), (0.0, R + 0.001, z - 0.5), ends["A-B"]], "CableBlue", up=0.004)
                desk, rack = HALF_D - 1.03, RACK_SIDE
                lay(row, "den", list(reversed(riser(row, 3, floor_x[3]))) + [(0.03, R + 0.001, z + 0.9), (0.25, R + 0.001, desk), (3.0, R + 0.001, desk),
                                                                             (rack[0] - 0.2, R + 0.001, rack[2] - 0.12), (rack[0] - 0.05, rack[1], rack[2]), rack], "CableBlue")
                ports += [(2, -1), (3, -1)]
            else:
                lay(row, "hubs 1b", [ends["B-C"], (0.0, R + 0.001, z + 0.9)] + riser(row, 2, floor_x[2]), "CableBlue", up=0.004)
                ports += [(2, -1)]
            host = places[HUB_HOSTS[row]]
            boxes.append((tuple(host @ Vector((hub_at[0], hub_at[1] + pc.HUB[1] / 2, hub_at[2]))), (pc.HUB[2], pc.HUB[1], pc.HUB[0]), math.degrees(math.atan2(host[0][2], host[0][0]))))
            power = -pc.HUB_POWER   # (its socket towards the tower's back)

            def down_back(front, host=host, row=row):
                """The supply's lead round the tower's back corner, down its back clear of the plugs, over the table's edge."""
                x, hz, low = pc.TOWER_X + 0.04, hub_at[2] - power, DC_R + 0.001
                e = table_edge(row, host, x)
                local = [(face + 0.035, pc.TOWER_HIGH + 0.006, hz + 0.03), (face + 0.018, pc.TOWER_HIGH - 0.008, back + 0.018), (face - 0.015, pc.TOWER_HIGH - 0.03, back + 0.022),
                         (x + 0.01, 0.2, back + 0.012), (x + 0.005, 0.03, back + 0.012), (x, low, back + 0.035), (x, low, e - 0.04), (x, 0.012, e + 0.006),
                         (x, -0.03, e + 0.014), (x, -0.3, e + 0.02), (x - 0.02, -T + 0.03, front + 0.012), (x - 0.06, -T + low, front + 0.006)]
                return [host @ Vector(p) for p in local]
            supply(row, world, power, down_back)
        else:
            # on the floor behind the row's middle, its sockets to the tables: world x of socket k is -hub_port(k)
            world = lib.place((0.0, 0, z + 0.78), (0, 180 + rng.uniform(-2, 2), 0))
            boxes.append(((0.0, pc.HUB[1] / 2, z + 0.78), pc.HUB, 0))

            def into(k, mat):
                end, out = plugged(world, pc.hub_port(k)[0], pc.hub_port(k)[1], pc.HUB[2] / 2, mat)
                return [(out[0], R + 0.001, out[2] - 0.035), out, end]
            down, x = from_card(left)
            lay(row, "lan %d" % left, down + under_table(row, x, lanes[1], -0.10) + [(-0.10, R + 0.001, z + 0.50)] + into(7, colour(left)), colour(left))
            down, x = from_card(near_left)
            lay(row, "lan %d" % near_left, down + into(6, colour(near_left)), colour(near_left))
            down, x = from_card(near_right)
            lay(row, "lan %d" % near_right, down + under_table(row, x, lanes[1], 0.125) + [(0.125, R + 0.001, z + 0.49)] + into(1, colour(near_right)), colour(near_right))
            down, x = from_card(right)
            lay(row, "lan %d" % right, down[:-1] + [(min(x - 0.01, 1.92), R + 0.001, z + 0.48)] + under_table(row, min(x - 0.04, 1.89), lanes[0], 0.10)
                + [(0.10, R + 0.001, z + 0.47)] + into(2, colour(right)), colour(right))
            # from the front row's hub: round the hub's end and back into its last socket; on to the back row's through the feet
            lay(row, "hubs 0b", [ends["A-B"], (0.0, R + 0.001, z + 1.0), (0.17, R + 0.001, z + 0.89), (0.17, R + 0.001, z + 0.69), (0.125, R + 0.001, z + 0.62)]
                + into(0, "CableBlue"), "CableBlue", up=0.004)
            ends["B-C"] = (0.0, R + 0.001, ROWS_Z[2] + 1.25)
            lay(row, "hubs 1", list(reversed(into(4, "CableBlue"))) + [(0.0, R + 0.001, z + 0.50), (0.0, R + 0.001, z - 0.5), ends["B-C"]], "CableBlue", up=0.004)
            ports = [(7, left), (6, near_left), (1, near_right), (2, right), (0, -1), (4, -1)]
            power = pc.HUB_POWER

            def round_hub(front, hub=world):
                """The supply's lead from the hub's back along the floor, clear of the leads into it, to before the left table's tower."""
                low = DC_R + 0.001
                return [hub @ Vector(p) for p in ((power + 0.006, low, 0.135), (power + 0.04, low, 0.19))] + [places[HUB_SUPPLY[1]] @ Vector((pc.TOWER_X - 0.015, -T + low, front + 0.02))]
            supply(row, world, power, round_hub)
        b.obj("Hubs")
        with b.at((0, 0, 0)):
            b.stack[-1] = world
            pc.hub(b, power)
        b.obj("Network")
        at = world @ Vector((0, 0, 0))
        hubs.append({"x": at.x, "y": at.y, "z": at.z, "turn": math.degrees(math.atan2(world[0][2], world[0][0])),
                     "ports": [v for k, index in sorted(ports) for v in pc.hub_lamp(k) + (index,)], "lamp": list(pc.HUB_LAMP)})
    # each hub's leads fall together; a lead between two hubs is two that meet on the floor, made as one
    # (a lead drawn tight over a table's back edge is cut by it: they fall on an edge 2.5 mm further out)
    guards = [((x, T - 0.012, z + 0.45 + 0.00125), (2.0, 0.024, 0.0025), 0) for z in ROWS_Z for x in TABLES_X]
    fallen = {}
    for group, leads in groups.items():
        for wrong in drape.check([(name, path, radius) for name, path, radius, _ in leads], boxes, [], quiet=True):
            print("LAID", wrong)   # (a mistake in the layout above, before anything falls)
        paths = drape.settle([(name, path, radius) for name, path, radius, _ in leads], boxes, [], guards=guards)
        drape.check([(name, path, radius) for (name, _, radius, _), path in zip(leads, paths)], boxes, [])
        for (name, _, radius, mat), path in zip(leads, paths):
            fallen[name] = (path, mat, radius)
    for first, second in (("hubs 0", "hubs 0b"), ("hubs 1", "hubs 1b")):
        fallen[first] = (fallen[first][0] + fallen.pop(second)[0][1:],) + fallen[first][1:]
    for name, (path, mat, radius) in fallen.items():
        b.tube([tuple(q) for q in path], radius, mat, segs=6, smooth=0)
    drape.apart([(name, path, radius) for name, (path, _, radius) in fallen.items()],
                [("place %d's %s" % (index, name), path, radius) for index, leads in enumerate(laid) for name, path, radius in leads])
    return hubs


RACK_SIDE = (3.405, 0.03, HALF_D - 0.72)   # where the network's lead goes into the den's rack: its base's left side


def classroom(b):
    b.obj("Tables")
    for z in ROWS_Z:
        for x in TABLES_X:
            table(b, x, z)
    b.obj("Chairs")
    import random
    third = random.Random(12)   # (the last row's chances are its own: the room's other chances stay what they were)
    for row, (z, behind) in enumerate(zip(ROWS_Z, CHAIRS_BACK)):
        rand = b.rand if row < 2 else third.uniform
        for x in PLACES_X:
            with b.at((x + rand(-0.05, 0.05), 0, z - behind + rand(-0.05, 0.05)), (0, rand(-25, 25), 0)):
                office_chair(b)
    # The computers: world/bake_pc.py's shells (monitor and tower, their detail baked into their
    # pictures), and at each place its own glass, keyboard, mouse, leads and stickers. No two lie
    # alike: the set a little off and turned, the keyboard and the mouse as a hand left them.
    # Models/stations.json says where, for ShaderEmuStations.cs.
    import json
    lay = random.Random(11)   # (its own: the room's other chances stay what they were)
    places, frames, laid, strips = [], [], [], []
    for row, z in enumerate(ROWS_Z):
        for column, x in enumerate(PLACES_X):
            index = row * len(PLACES_X) + column
            at = (x + lay.uniform(-0.015, 0.015), TABLE_TOP, z + STATION_Z + lay.uniform(-0.015, 0.015))
            turn = lay.uniform(-3.5, 3.5)
            lie = {"keyboard": (lay.uniform(-0.01, 0.01), lay.uniform(-0.01, 0.01), lay.uniform(-2.5, 2.5)),
                   "mat": (lay.uniform(-0.01, 0.01), lay.uniform(-0.01, 0.01), lay.uniform(-6, 6)),
                   "mouse": (lay.uniform(-0.03, 0.03), lay.uniform(-0.03, 0.03), lay.uniform(-20, 20))}
            b.asset("pc_station", at, rot=(0, turn, 0), scale=1.0, parts=("=pc_station",))
            b.obj("Station %d rest" % index, charted=True)   # one object a place: each has a tube of its own
            pc.MODE = "low"   # (the glass alone, as the baked set has it)
            # its table as the place has it: the top, and the board behind the knees
            world = lib.place(at, (0, turn, 0))
            here = world.inverted()
            tx = -1.0 if x < 0 else 1.0
            desk = [(tuple(here @ Vector((tx, TABLE_TOP - 0.022, z))), (2.0, 0.044, 0.9), -turn),
                    (tuple(here @ Vector((tx, 0.45, z + 0.38))), (1.89, 0.4, 0.022), -turn)]
            with b.at(at, (0, turn, 0)):
                # (a hub stands on some towers, and its leads hang down their right sides)
                pc.station(b, "rest", lie, desk=desk, seed=index, stuck=random.Random(300 + index).randint(2, 4),
                           bare=("tower top", "tower right") if index in HUB_HOSTS.values() else (), supply=index in HUB_SUPPLY.values())
                if index in ON_TOWERS:   # what was left on the tower's top (not where a hub stands)
                    b.obj("Classroom things")
                    things, turns = ON_TOWERS[index]
                    pile(b, things, (pc.TOWER_X, pc.TOWER_HIGH, pc.TOWER_Z + pc.FRONT + pc.TOWER_DEEP / 2 + 0.01), turns)
            pc.MODE = "full"
            places.append({"x": at[0], "z": at[2], "turn": turn, "keyboard": list(lie["keyboard"])})
            frames.append(world)
            laid.append([(name, [world @ p for p in path], radius) for name, path, radius, _, _ in pc.LEADS])
            strips.append(pc.STRIP_AT[0])
    hubs = network(b, frames, laid, strips)
    here = os.path.dirname(os.path.abspath(__file__))
    back, card = pc.TOWER_Z + pc.FRONT + pc.TOWER_DEEP, pc.tower_back()["nic"]
    # (flat lists: Unity's JsonUtility reads no list of lists. tables: x, z of each; a hub's ports: x, y, z, place of each)
    json.dump({"places": places, "table": TABLE_TOP, "keyScale": pc.KEY_SCALE, "keyboardZ": pc.KEYBOARD_Z, "setX": pc.SET_X,
               "glass": list(pc.GLASS), "tower": [pc.TOWER_X, pc.TOWER_Z, pc.FRONT, pc.TOWER_HIGH],
               "speed": [pc.SPEED_X, pc.SPEED_DOWN, pc.SPEED_W, pc.SPEED_H], "reset": pc.RESET_X,
               "power": list(pc.POWER_KEY), "osd": [pc.OSD_X, pc.OSD_PITCH, pc.CHIN], "led": pc.LED_X, "lamps": list(pc.LAMPS_X),
               "tables": [v for z in ROWS_Z for x in TABLES_X for v in (x, z)],
               "keyboardHigh": pc.TRAY_LOW, "keyboardSlope": pc.KEYBOARD_SLOPE, "keyboardTops": pc.KEY_TOPS,
               "nic": [pc.TOWER_X + card[0], card[1], back + 0.001], "nicSize": list(pc.NIC_LAMP), "hubs": hubs},
              open(os.path.join(here, "..", "unity", "ShaderEmu", "Models", "stations.json"), "w"), indent=1)


def floor_lamp(b):
    b.obj("Floor lamp")
    with b.at((1.15, 0, -HALF_D + 0.4)):
        b.lathe((0, 0, 0), [(0.16, 0), (0.165, 0.012), (0.13, 0.024), (0.03, 0.04), (0.016, 0.07), (0.013, 1.45), (0.02, 1.46), (0.02, 1.5), (0, 1.5)],
                "Brass", segs=28)
        b.lathe((0, 1.46, 0), [(0.20, 0), (0.204, 0), (0.134, 0.30), (0.13, 0.30), (0.20, 0)], "Shade", segs=32, cap=False)
        for i in range(3):
            a = math.radians(i * 120)
            b.tube([(0.02 * math.cos(a), 1.49, 0.02 * math.sin(a)), (0.13 * math.cos(a), 1.75, 0.13 * math.sin(a))], 0.003, "Brass", segs=6)
        b.tube([(0.013, 1.2, 0), (0.03, 1.19, 0), (0.03, 1.08, 0)], 0.002, "Brass", segs=5, smooth=4)


def shelf(b):
    deep, wide, high = 0.32, 2.0, 2.1
    with on_wall(b, "left"):
        with b.at((-2.6, 0, 0)):
            b.obj("Shelf")
            b.box((0, high / 2, -0.008), (wide - 0.03, high - 0.02, 0.012), "WoodOak", grain=1)
            for x in (-wide / 2 + 0.015, wide / 2 - 0.015):
                b.box((x, high / 2, -deep / 2), (0.03, high, deep), "WoodOak", bevel=0.003, segs=1, grain=1)
            b.box((0, high + 0.012, -deep / 2 - 0.01), (wide + 0.04, 0.03, deep + 0.03), "WoodOak", bevel=0.008, grain=0)
            b.box((0, 0.025, -deep / 2 + 0.01), (wide - 0.06, 0.05, deep - 0.03), "WoodDark", grain=0)
            for s in range(6):
                b.box((0, 0.05 + s * 0.40, -deep / 2), (wide - 0.06, 0.03, deep - 0.012), "WoodOak", bevel=0.003, segs=1, grain=0)
            b.box((0, 1.25 + 0.2, -deep / 2), (0.025, 0.37, deep - 0.03), "WoodOak", grain=1)   # a divider on the games' shelf
            import random
            rng = random.Random(41)   # (its own: the room's other chances stay what they were)
            games, software, magazines = list(SHELF_GAMES), list(SOFTWARE), list(MAGAZINES)
            front = -deep + 0.03   # where the spines stand
            for s in range(5):
                y = 0.05 + s * 0.40 + 0.015
                if s == 3:   # games, face out, and a radio of its decade
                    b.obj("Books")
                    for i, game in enumerate(("red_alert", "command_conquer", "steel_sky")):   # (against the shelf's back, whose face is at -0.014)
                        leaning_box(b, -0.78 + i * 0.24, y, -0.014, 0, 8, game)
                    b.asset("boombox", (0.5, y, -0.17), rot=(0, 180, 0), size=(0.56, None, None))
                    continue
                # an encyclopedia at one end of three shelves; games and software standing spine out, and piles of magazines
                b.obj("Books")
                left = -wide / 2 + 0.05
                right = wide / 2 - 0.05
                if s in (0, 2, 4):
                    set_wide = 0.55
                    start = left if s != 2 else right - set_wide
                    b.asset("book_encyclopedia_set_01", (start + set_wide / 2, y, -deep + 0.03 + 0.08), rot=(0, 180, 0), limit=20000)
                    if s != 2:
                        left += set_wide + 0.01
                    else:
                        right -= set_wide + 0.01
                if s == 0:   # the bottom shelf: the software, then magazines in two piles
                    end = shelf_row(b, software, left, right - 0.62, y, front, rng)
                    for k, at in enumerate((end + 0.17, end + 0.45)):
                        some = [magazines.pop(0) for _ in range(4 if k == 0 else 3)]
                        pile(b, some, (at, y, -deep / 2 - 0.01), [rng.uniform(-6, 6) for _ in some])
                elif s == 4:   # a pile of magazines first, then games
                    some = [magazines.pop(0) for _ in range(5)]
                    pile(b, some, (left + 0.15, y, -deep / 2 - 0.01), [90 + rng.uniform(-5, 5) for _ in some])
                    shelf_row(b, games, left + 0.32, right, y, front, rng)
                else:   # (the games over three shelves: on this one alone they filled it and left the top bare)
                    shelf_row(b, games, left, right, y, front, rng, most=18 if s == 1 else 14)
            b.obj("Books")   # on the shelf's top: a box too large for a shelf
            lying(b, "id_anthology", (-0.45, high + 0.027, -0.17), 4)
            b.asset("cardboard_box_01", (0.5, high + 0.027, -0.17), rot=(0, 98, 0), size=(0.4, 0.3, 0.5), limit=5000)


def boxes(b):
    at = (-HALF_W + 0.5, 0, -HALF_D + 0.5)
    # (a box's top is its flaps, not flat: what stands on one is let down till it touches)
    low = b.asset("cardboard_box_01", (at[0], 0, at[2]), rot=(0, 102, 0), size=(0.5, 0.5, 0.6), limit=5000)
    high = b.asset("cardboard_box_01", (at[0] + 0.03, 0.5, at[2] + 0.02), rot=(0, 81, 0), size=(0.4, 0.34, 0.45), limit=5000, on=low)
    b.asset("cardboard_box_01", (at[0] + 0.7, 0, at[2] - 0.05), rot=(0, 31, 0), size=(0.42, 0.4, 0.5), limit=5000)
    b.asset("television_02", (at[0] + 0.03, 0.84, at[2] + 0.02), rot=(0, 40, 0), on=high)


def posters(b):
    with on_wall(b, "back"):
        b.obj("Posters")
        for i, mat in enumerate(("PosterDie", "PosterWords", "PosterGrid")):
            x = 1.5 - i * 1.1
            b.box((x, 1.95, -0.006), (0.78, 1.02, 0.012), "PlasticDark")
            moulding = [(0, 0), (0.03, 0), (0.03, 0.022), (0.022, 0.03), (0.006, 0.03), (0, 0.018)]
            frame(b, (x, 1.95, 0), 0.75, 0.99, moulding, "MetalBlack")
            b.quad((x, 1.95, -0.014), (0.74, 0.98), mat, uvrect=(0, 0, 1, 1))


def pot(b, at, r=0.2):
    """A clay pot with its saucer and soil; a plant stands in it at r * 1.8 up."""
    with b.at(at):
        b.lathe((0, 0, 0), [(r * 0.62, 0), (r * 0.66, 0.012), (r * 0.95, r * 1.7), (r * 1.06, r * 1.72), (r * 1.06, r * 1.95), (r * 0.94, r * 1.95),
                            (r * 0.9, r * 1.8), (0, r * 1.8)], "Terracotta", segs=32)
        b.lathe((0, 0, 0), [(r * 0.8, 0), (r * 0.9, 0.006), (r * 0.9, 0.02), (r * 0.82, 0.022)], "Terracotta", segs=32)
        b.lathe((0, r * 1.81, 0), [(0, 0.004), (r * 0.88, 0)], "Rubber", segs=20, cap=False)


def plants(b):
    # two money trees in pots of ours, and a smaller plant in its own by the sofa
    b.obj("Pots")
    left, right = (-HALF_W + 0.62, 0, 1.6), (HALF_W - 0.62, 0, 1.2)
    pot(b, left, 0.21)
    pot(b, right, 0.19)
    b.asset("pachira_aquatica_01", (left[0], 0.36, left[2]), rot=(0, 20, 0), parts=("_d",))
    b.asset("pachira_aquatica_01", (right[0], 0.33, right[2]), rot=(0, 250, 0), parts=("_c",))
    b.asset("potted_plant_02", (2.05, 0, -HALF_D + 0.6), rot=(0, 140, 0), parts=("_leaves", "_pot"), scale=1.19)
    b.asset("potted_plant_02", (2.05, 0, -HALF_D + 0.6), rot=(0, 140, 0), parts=("_dirt",), scale=1.19, limit=2500)   # its soil


def build(b):
    sofa(b)
    low_table(b)
    classroom(b)
    floor_lamp(b)
    shelf(b)
    boxes(b)
    posters(b)
    plants(b)
