# How things stand: each kind of mounting world/check_placement.py knows, and the pose of each of
# Poly Haven's models we place. A model's front is +z of its own frame and its top +y (Poly Haven's
# models face +z: a sofa's seat, a television's screen, a clock's dial); what a model of ours is,
# lib.Builder.pose says where it is made.
#
# A kind's distances, in metres, which a pose may change (back=, clear=, reach=):
#   back   its back's face is at most this far before what it stands against (a wall, a shelf's back)
#   clear  nothing large is this near before its front (a wall, a cupboard: a television not to the wall)
#   reach  what it faces (faces=) is at most this far before its front
# and of a model: axis, its front in its own frame when not +z (-z, or +x or -x: a turn about its top); also, the words
# its other objects' names start with (a station's rest), which are its own.
KINDS = {
    "floor":   dict(doc="stands upright on the floor, any way round"),
    "on":      dict(doc="stands upright on something (a desk, a shelf, a table), any way round"),
    "front":   dict(doc="stands upright on something, its front out to the room (a clock, a camera, a toy facing out)", clear=0.12),
    "against": dict(doc="stands upright, its back to a wall or a shelf's back, nothing before it (a sofa, a bookcase, a bench)",
                    back=0.15, clear=0.4),
    "hung":    dict(doc="hangs on a wall, upright, its back on it (a picture, a clock, a sconce)", back=0.02, clear=0.3),
    "ceiling": dict(doc="hangs from the ceiling, its top on it"),
    "clamp":   dict(doc="clamped to a top's edge: its jaw round the top, its front over the top, its spine past the edge"),
    "lying":   dict(doc="lies on something, any face down (a magazine, a pillow, a tape)"),
    "faceup":  dict(doc="lies on its back, its front up (a cassette player whose file stands it on its end: its window up)"),
    "leaning": dict(doc="stands tilted up to 35 degrees, its top against a wall or a neighbour (a bat in a corner)"),
    "any":     dict(doc="any way at all (a ball)"),
    "desk":    dict(doc="a desk or a table one sits at, upright on the floor: its front is the sitter's side, a seat faces it there, "
                        "and no wall is before it", clear=0.6, faces="seat", reach=1.1),
    "seat":    dict(doc="a chair one sits on at a desk, upright on the floor, its front (where the knees go) towards a desk or a console",
                    faces="desk", reach=1.0),
    "door":    dict(doc="a doorway in a wall, its front the side its casing is on: nothing stands within clear before it", clear=0.8),
}

POSES = {
    "sofa_02": dict(kind="against", clear=0.25), "sofa_03": dict(kind="against", clear=0.25), "Sofa_01": dict(kind="against", clear=0.25),   # (a low table 0.3 m before it)
    "modern_coffee_table_01": dict(kind="floor"), "round_wooden_table_01": dict(kind="floor"), "bar_chair_round_01": dict(kind="floor"),
    "side_table_01": dict(kind="floor"), "side_table_tall_01": dict(kind="floor"),
    "desk_lamp_arm_01": dict(kind="clamp", axis=(0, 0, -1)),   # (its jaw opens towards its -z)
    "classic_laptop": dict(kind="front", clear=0.3),
    "television_02": dict(kind="front", clear=1.0), "Television_01": dict(kind="front", clear=1.0),
    "cardboard_box_01": dict(kind="on"), "vintage_suitcase": dict(kind="floor"), "wicker_basket_01": dict(kind="on"),
    "metal_trash_can": dict(kind="floor"), "rubber_boots": dict(kind="floor"),
    "potted_plant_02": dict(kind="on"), "potted_plant_04": dict(kind="on"), "pachira_aquatica_01": dict(kind="on"),   # (a plant in its pot, the pot ours or its own file's part)
    "wall_clock": dict(kind="hung"), "industrial_wall_sconce": dict(kind="hung"), "korean_public_payphone_01": dict(kind="hung"),
    "hanging_picture_frame_01": dict(kind="hung"), "hanging_picture_frame_02": dict(kind="hung"), "fire_alarm": dict(kind="hung"),
    "ornate_mirror_01": dict(kind="hung"),
    "modern_ceiling_lamp_01": dict(kind="ceiling"),
    "boombox": dict(kind="against", back=0.12, clear=0.15), "book_encyclopedia_set_01": dict(kind="front", clear=0.15),   # (its spines at the shelf's front)
    "alarm_clock_01": dict(kind="front", clear=0.25), "mantel_clock_01": dict(kind="front", clear=0.12),
    "korean_fire_extinguisher_01": dict(kind="against", back=0.25, clear=0.3), "painted_wooden_bench": dict(kind="against", back=0.25),
    "throw_pillows_01": dict(kind="lying"), "portable_cassette_player": dict(kind="lying"), "cassette_player": dict(kind="faceup"),
    "chess_set": dict(kind="on"), "football": dict(kind="any"), "baseball_01": dict(kind="any"), "baseball_bat": dict(kind="leaning"),
    "rubber_duck_toy": dict(kind="front"), "carved_wooden_elephant": dict(kind="front", axis=(1, 0, 0)),
    "Camera_01": dict(kind="front"), "vintage_video_camera": dict(kind="front"),
    # (the elephant faces its own +x, its trunk there: taken for +z it stood side-on; rendered from each side, every
    # other model here faces +z or its axis)
    # (the two standing frames face their own -x, their stands behind at +x: their pictures' faces, measured
    # in build/world.blend, point there; taken for +z they stood edge-on to the room on the sill and the shelf)
    "standing_picture_frame_01": dict(kind="front", clear=0.15, axis=(-1, 0, 0)), "standing_picture_frame_02": dict(kind="front", clear=0.15, axis=(-1, 0, 0)),
    # ours (world/bake_pc.py), made as world/pc.py's station: its sitter at its -z; the keys before the screen are its rest
    "pc_tower": dict(kind="front", clear=0.25, axis=(0, 0, -1)), "pc_station": dict(kind="front", clear=0.25, axis=(0, 0, -1), also=("Station ",)),
}
