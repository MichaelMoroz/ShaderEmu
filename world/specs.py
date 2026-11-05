# Which of the things the world's scripts model follow a specification found online (AGENTS.md, "Every
# object the world's scripts model or texture follows a specification"), and which do not yet and are
# to be modelled again from one. Each is its maker function (file.function) with True and the source
# its dimensions come from, or False and what it lacks. A new model goes in False until its sources
# are written beside it; `python world/specs.py` lists the ones still to do. Poly Haven's models
# (world/assets.py) are their makers', and the game cartridges are left out, as asked.
SPECS = {
    # toys.py
    "toys.floppy": (True, "ECMA-147 (1990), 6.1 and figs. 7 to 10: case, holes, recesses, hub, shutter's travel"),
    "toys.soda_can": (True, "can.py: Crown's and American Canning's 211 x 413 body and 202 end"),
    "toys.cassette": (False, "outline quoted second-hand (IEC 60094-7 not read), the label, window, hubs and screws guessed"),
    "toys.puzzle_cube": (False, "57 mm only; the pieces, tiles and colours not from a source"),
    "toys.die": (False, "16 mm and its pips guessed"),
    "toys.yoyo": (False, "profile guessed"),
    "toys.joystick": (False, "no source"),
    "toys.mug": (False, "no source"),
    "toys.pencil": (False, "no source"),
    "toys.ball": (False, "no source"),
    "furniture.mug": (False, "no source (a copy of toys.mug)"),
    # pc.py
    "pc.office_chair": (False, "Steelcase Criterion task chair's brochure and spec sheet, but no address written beside the model"),
    "pc.mains_socket": (True, "IEC 60320 C13 and C14"),
    "pc.monitor": (False, "no source for the housing's shape and size"),
    "pc.keyboard": (False, "keyboard.json's layout at KEY_SCALE, not a real keyboard's size"),
    "pc.mouse": (False, "no source"),
    "pc.tower": (False, "no source"),
    "pc.cd_drive": (False, "no source (a 5.25 in bay's front is SFF-8551's)"),
    "pc.dsub": (False, "no source"),
    "pc.dsub_plug": (False, "no source"),
    "pc.bnc": (False, "no source"),
    "pc.phono": (False, "8.3 mm only"),
    "pc.din5": (False, "13.2 mm only"),
    "pc.lan_socket": (False, "no source (8P8C)"),
    "pc.lan_plug": (False, "no source"),
    "pc.jack": (False, "no source"),
    "pc.jack_plug": (False, "no source"),
    "pc.rca_plug": (False, "no source"),
    "pc.dc_inlet": (False, "no source"),
    "pc.dc_plug": (False, "no source"),
    "pc.y_split": (False, "no source"),
    "pc.adapter": (False, "no source"),
    "pc.strip": (False, "no source"),
    "pc.hub": (False, "no source"),
    "pc.terminal_board": (False, "no source"),
    # computer.py (the classroom's wall of screens)
    "computer.wall_unit": (False, "no source"),
    "computer.console": (False, "no source"),
    "computer.desk": (False, "no source"),
    "computer.rack": (False, "its decals at EIA-310 heights only; the frame's sizes guessed"),
    "computer.housing": (False, "no source"),
    "computer.keyboard": (False, "keyboard.json's layout; the caps' shape and size not from a source"),
    # room.py and rooms.py
    "room.wall_socket": (True, "CEE 7/3 (Schuko): plate, well, pins"),
    "room.light_switch": (False, "no source"),
    "room.lamps": (False, "no source"),
    "room.window": (False, "no source"),
    "room.door_leaf": (False, "no source"),
    "room.door_casing": (False, "no source"),
    "room.wainscot": (False, "no source"),
    "room.door": (False, "no source (the den's door put together: its leaf, casing and handle)"),
    "room.holo_door": (False, "no source"),
    # shell.py: the building itself
    "shell.build": (False, "no source for the rooms' sizes, the ceilings' height, the corridors' and doorways' widths"),
    "rooms.desk": (False, "no source"),
    "rooms.bookcase": (False, "no source"),
    "rooms.floor_lamp": (False, "no source"),
    "rooms.poster": (False, "no source"),
    "rooms.door": (False, "no source"),
    "rooms.window": (False, "no source"),
    "rooms.radiator": (False, "its pipe is BS EN 1057's 15 mm; the radiator's columns guessed"),
    "rooms.trim": (False, "no source"),
    # furniture.py
    "furniture.cushion": (False, "no source"),
    "furniture.low_table": (False, "no source"),
    "furniture.table": (False, "no source"),
    "furniture.game_box": (False, "pictures from scans; the boxes' sizes not from a source"),
    "furniture.shelf": (False, "no source"),
    "furniture.floor_lamp": (False, "no source"),
    "furniture.boxes": (False, "no source"),
    "furniture.posters": (False, "no source"),
    "furniture.pot": (False, "no source"),
    # annex.py
    "annex.console": (False, "no source"),
    "annex.command_chair": (False, "no source"),
    "annex.corridor_door": (False, "no source"),
}


if __name__ == "__main__":
    done = [k for k, (ok, _) in SPECS.items() if ok]
    print("SPECS %d of %d follow a specification" % (len(done), len(SPECS)))
    for name, (ok, why) in SPECS.items():
        if not ok:
            print("SPECS to model again: %-22s %s" % (name, why))
