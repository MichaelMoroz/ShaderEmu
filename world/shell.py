# The rooms' plain surfaces: every floor, ceiling and wall, each one mesh however many openings
# it has, with its place in the lightmap written here (lib.sheet). They are a model of their
# own, Shell.fbx, which Unity imports without unwrapping it again: a wall made of pieces that
# only touch was a chart a piece, and showed a line where two met.
from room import (DOOR_HIGH, DOOR_WIDE, DOOR_X, HALF_D, HALF_W, HEIGHT, HOLO_HIGH, HOLO_WIDE, HOLO_Z, WALL_THICK, WALLS,
                  WINDOW_HIGH, WINDOW_WIDE, WINDOW_Y, WINDOW_Z)
import annex

GAP = 0.15   # between two charts of one layout: eight texels and more at the room's sixty a metre


def den(b):
    wide, deep = HALF_W * 2, HALF_D * 2
    b.obj("Floor")
    with b.at((0, 0, 0), (90, 0, 0)):
        b.sheet((-HALF_W, -HALF_D, HALF_W, HALF_D), "Parquet", extent=deep)
    b.obj("Ceiling")
    with b.at((0, HEIGHT, 0), (-90, 0, 0)):
        b.sheet((-HALF_W, -HALF_D, HALF_W, HALF_D), "Ceiling", extent=deep)

    # four walls, a row of the layout each; under them a row for the window's reveal
    b.obj("Walls")
    row, extent = HEIGHT + GAP, 4 * (HEIGHT + GAP) + WINDOW_HIGH + GAP
    cx, w, y0, y1 = -WINDOW_Z, WINDOW_WIDE / 2, WINDOW_Y - WINDOW_HIGH / 2, WINDOW_Y + WINDOW_HIGH / 2
    openings = {
        "front": [],
        "back": [(-DOOR_X - DOOR_WIDE / 2, -1, -DOOR_X + DOOR_WIDE / 2, DOOR_HIGH)],       # the door to the corridor
        "left": [(HOLO_Z - HOLO_WIDE / 2, -1, HOLO_Z + HOLO_WIDE / 2, HOLO_HIGH)],         # the holodecks' doorway
        "right": [(cx - w, y0, cx + w, y1)],                                                # the window
    }
    for i, name in enumerate(("front", "right", "back", "left")):
        at, rot, half = WALLS[name]
        with b.at(at, rot):
            b.sheet((-half, 0, half, HEIGHT), "Wall", openings[name], light=(0, i * row), extent=extent, uv_from=(0, HEIGHT / 2))
    at, rot, _ = WALLS["right"]
    with b.at(at, rot):
        # the window's reveal, 0.2 deep: its four faces side by side in the last row
        under = 4 * row
        with b.at((cx, y0, 0.1), (90, 0, 0)):
            b.sheet((-w, -0.1, w, 0.1), "Wall", light=(0, under), extent=extent)
        with b.at((cx, y1, 0.1), (-90, 0, 0)):
            b.sheet((-w, -0.1, w, 0.1), "Wall", light=(WINDOW_WIDE + GAP, under), extent=extent)
        with b.at((cx - w, WINDOW_Y, 0.1), (0, -90, 0)):
            b.sheet((-0.1, -WINDOW_HIGH / 2, 0.1, WINDOW_HIGH / 2), "Wall", light=(2 * (WINDOW_WIDE + GAP), under), extent=extent)
        with b.at((cx + w, WINDOW_Y, 0.1), (0, 90, 0)):
            b.sheet((-0.1, -WINDOW_HIGH / 2, 0.1, WINDOW_HIGH / 2), "Wall", light=(2 * (WINDOW_WIDE + GAP) + 0.2 + GAP, under), extent=extent)


def corridor(b):
    """The corridor behind the den's back wall (annex.py has what stands in it)."""
    long, wide, high = annex.COR_LONG, annex.COR_WIDE, annex.COR_HIGH
    mid_x, mid_z = annex.COR_MID_X, annex.COR_MID_Z
    b.obj("Corridor floor")
    with b.at((mid_x, 0, mid_z), (90, 0, 0)):
        b.sheet((-long / 2, -wide / 2, long / 2, wide / 2), "Carpet", extent=long)
    b.obj("Corridor ceiling")
    with b.at((mid_x, high, mid_z), (-90, 0, 0)):
        b.sheet((-long / 2, -wide / 2, long / 2, wide / 2), "Ceiling", extent=long)
    b.obj("Corridor walls")
    row = high + GAP
    door_at = DOOR_X - mid_x   # the den's door, in the north wall's space
    w, y0, y1 = annex.WIN_WIDE / 2, annex.WIN_Y - annex.WIN_HIGH / 2, annex.WIN_Y + annex.WIN_HIGH / 2
    with b.at((mid_x, 0, annex.COR_Z1)):
        b.sheet((-long / 2, 0, long / 2, high), "Wall", [(door_at - DOOR_WIDE / 2, -1, door_at + DOOR_WIDE / 2, DOOR_HIGH)],
                light=(0, 0), extent=long, uv_from=(0, high / 2))
    with b.at((mid_x, 0, annex.COR_Z0), (0, 180, 0)):
        b.sheet((-long / 2, 0, long / 2, high), "Wall", light=(0, row), extent=long, uv_from=(0, high / 2))
    with b.at((annex.COR_X1, 0, mid_z), (0, 90, 0)):   # the outer wall: a window onto the city
        b.sheet((-wide / 2, 0, wide / 2, high), "Wall", [(-w, y0, w, y1)], light=(0, 2 * row), extent=long, uv_from=(0, high / 2))
        side = 2 * (wide + GAP)
        with b.at((0, y0, 0.1), (90, 0, 0)):
            b.sheet((-w, -0.1, w, 0.1), "Wall", light=(side, 2 * row), extent=long)
        with b.at((0, y1, 0.1), (-90, 0, 0)):
            b.sheet((-w, -0.1, w, 0.1), "Wall", light=(side, 2 * row + 0.2 + GAP), extent=long)
        with b.at((-w, annex.WIN_Y, 0.1), (0, -90, 0)):
            b.sheet((-0.1, -annex.WIN_HIGH / 2, 0.1, annex.WIN_HIGH / 2), "Wall", light=(side + annex.WIN_WIDE + GAP, 2 * row), extent=long)
        with b.at((w, annex.WIN_Y, 0.1), (0, 90, 0)):
            b.sheet((-0.1, -annex.WIN_HIGH / 2, 0.1, annex.WIN_HIGH / 2), "Wall", light=(side + annex.WIN_WIDE + 0.2 + 2 * GAP, 2 * row), extent=long)
    with b.at((annex.COR_X0, 0, mid_z), (0, -90, 0)):
        b.sheet((-wide / 2, 0, wide / 2, high), "Wall", light=(wide + GAP, 2 * row), extent=long, uv_from=(0, high / 2))


def holodecks(b):
    """The holodecks' corridor through the den's left wall, plastered and carpeted as the corridor
    by the lift, and its eight rooms of grid (holo_room)."""
    long, wide, high = annex.HC_LONG, annex.HC_WIDE, annex.HC_HIGH
    mid_x, mid_z = (annex.HC_X0 + annex.HC_X1) / 2, HOLO_Z
    b.obj("Holo corridor floor")
    with b.at((mid_x, 0, mid_z), (90, 0, 0)):
        b.sheet((-long / 2, -wide / 2, long / 2, wide / 2), "Carpet", extent=long)
    b.obj("Holo corridor ceiling")
    with b.at((mid_x, high, mid_z), (-90, 0, 0)):
        b.sheet((-long / 2, -wide / 2, long / 2, wide / 2), "Ceiling", extent=long)
    b.obj("Holo corridor walls")
    row = high + GAP
    for side, (z, turn) in enumerate(((annex.HC_Z1, 0), (annex.HC_Z0, 180))):
        doors = []
        for k in range(side, annex.ROOMS, 2):
            x = annex.room_place(k)[0] - mid_x
            x = x if turn == 0 else -x   # the south wall's own x runs against the world's
            doors.append((x - annex.ROOM_DOOR_WIDE / 2, -1, x + annex.ROOM_DOOR_WIDE / 2, annex.ROOM_DOOR_HIGH))
        with b.at((mid_x, 0, z), (0, turn, 0)):
            b.sheet((-long / 2, 0, long / 2, high), "Wall", doors, light=(0, side * row), extent=long, uv_from=(0, high / 2))
    with b.at((annex.HC_X0, 0, mid_z), (0, -90, 0)):
        b.sheet((-wide / 2, 0, wide / 2, high), "Wall", light=(0, 2 * row), extent=long, uv_from=(0, high / 2))
    with b.at((annex.HC_X1, 0, mid_z), (0, 90, 0)):   # the den's wall from behind, with its doorway
        b.sheet((-wide / 2, 0, wide / 2, high), "Wall", [(-HOLO_WIDE / 2, -1, HOLO_WIDE / 2, HOLO_HIGH)], light=(wide + GAP, 2 * row),
                extent=long, uv_from=(0, high / 2))
    for k in range(annex.ROOMS):
        holo_room(b, k)


def holo_room(b, k):
    """Room k of grid as one mesh, welded: its four walls one strip round it (the doorway cut out
    of the first), its floor and its ceiling, every face a whole panel of the grid (annex.GRID),
    so that no cell is cut at a corner or by the doorway. In the lightmap the walls are one strip
    unrolled from the door wall's corner at +x, so three of the four corners are inside a
    chart; the floor and the ceiling are charts under it. The doorway's reveal is the corridor's
    plaster, a mesh of its own beside them. The lines of the grid are annex.py's bars."""
    s, t, g = annex.ROOM, annex.ROOM_HIGH, annex.GRID
    n, m = round(s / g), round(t / g)                 # cells along a wall and up it
    dw, dh = round(annex.ROOM_DOOR_WIDE / g), round(annex.ROOM_DOOR_HIGH / g)
    d0 = (n - dw) // 2                                # the doorway's first cell along the door wall
    x, z, turn = annex.room_place(k)
    # the walls' strip, in the room's own frame: from the door wall's corner at +x round the room,
    # each wall's start, its way along, and the way into the room
    walls = [((s / 2, 0.0), (-1, 0), (0, 1)), ((-s / 2, 0.0), (0, 1), (1, 0)), ((-s / 2, s), (1, 0), (0, -1)), ((s / 2, s), (0, -1), (-1, 0))]
    extent = 4 * s
    vertices, faces, uvs, light, normals = [], [], [], [], []
    index = {}

    def vertex(p):
        key = tuple(round(c, 5) for c in p)
        if key not in index:
            index[key] = len(vertices)
            vertices.append(p)
        return index[key]

    def cell(corners, inward, u0v0, lighting):
        faces.append(tuple(vertex(p) for p in corners))
        uvs.append([(u0v0[0] + du, u0v0[1] + dv) for du, dv in ((0, 0), (1, 0), (1, 1), (0, 1))])
        light.append([(lx / extent, ly / extent) for lx, ly in lighting])
        normals.append(inward)
    for w, ((sx, sz), (ax, az), (ix, iz)) in enumerate(walls):
        for i in range(n):
            for j in range(m):
                if w == 0 and d0 <= i < d0 + dw and j < dh:
                    continue   # the doorway
                a, c = i * g, (i + 1) * g
                corners = [(sx + ax * a, j * g, sz + az * a), (sx + ax * c, j * g, sz + az * c),
                           (sx + ax * c, (j + 1) * g, sz + az * c), (sx + ax * a, (j + 1) * g, sz + az * a)]
                along = w * s
                cell(corners, (ix, 0, iz), (0, 0), [(along + a, j * g), (along + c, j * g), (along + c, (j + 1) * g), (along + a, (j + 1) * g)])
    for y, up, at in ((0.0, 1, 0.0), (t, -1, s + GAP)):   # the floor, and the ceiling beside it, under the strip
        for i in range(n):
            for j in range(n):
                x0, x1, z0, z1 = -s / 2 + i * g, -s / 2 + (i + 1) * g, j * g, (j + 1) * g
                cell([(x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)], (0, up, 0), (0, 0),
                     [(at + i * g, t + GAP + j * g), (at + (i + 1) * g, t + GAP + j * g), (at + (i + 1) * g, t + GAP + (j + 1) * g), (at + i * g, t + GAP + (j + 1) * g)])
    b.obj("Holo room %d" % k)
    with b.at((x, 0, z), (0, turn, 0)):   # the room's own frame: its door behind, the room towards +z
        b.mesh(vertices, faces, uvs, "HoloPanel", normal=normals)
        bm_uv2(b, faces, light, len(vertices))
        # the doorway's reveal, through the wall to the corridor's face: jambs, head and threshold, in the plaster's own place of the layout
        w, h, deep = annex.ROOM_DOOR_WIDE, annex.ROOM_DOOR_HIGH, WALL_THICK
        at = 2 * (s + GAP)
        pieces = [((-w / 2, h / 2, -deep / 2), (0, -90, 0), (deep, h), (at, t + GAP)),
                  ((w / 2, h / 2, -deep / 2), (0, 90, 0), (deep, h), (at + deep + GAP, t + GAP)),
                  ((0, h, -deep / 2), (-90, 0, 0), (w, deep), (at + 2 * (deep + GAP), t + GAP)),
                  ((0, 0.0, -deep / 2), (90, 0, 0), (w, deep), (at + 2 * (deep + GAP), t + GAP + deep + GAP))]
        for centre, rot, (pw, ph), spot in pieces:
            with b.at(centre, rot):
                b.sheet((-pw / 2, -ph / 2, pw / 2, ph / 2), "Wall", light=spot, extent=extent, uv_from=(0, -ph / 2))


def bm_uv2(b, faces, light, count):
    """The lightmap's places of the faces b.mesh just added to the current object (`count`
    vertices, `faces` of them in order): a corner's place goes by its vertex, as _emit may have
    turned a face over."""
    bm = b.objects[b.current][0]
    uv2 = bm.loops.layers.uv.get("UV2") or bm.loops.layers.uv.new("UV2")
    bm.faces.ensure_lookup_table()
    bm.verts.ensure_lookup_table()
    first_face, first_vert = len(bm.faces) - len(faces), len(bm.verts) - count
    for f, corners, places in zip(bm.faces[first_face:], faces, light):
        for loop in f.loops:
            loop[uv2].uv = places[corners.index(loop.vert.index - first_vert)]


def build(b):
    den(b)
    corridor(b)
    holodecks(b)
