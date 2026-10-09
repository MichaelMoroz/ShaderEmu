# The classroom's computers

In the middle of the den are four tables in the main desk's oak with eight computers of the
decade, one a visitor (`classroom` in `world/furniture.py`; the chairs are the main desk's).
Each tube shows its owner's display, or their console, to everybody.

The computer is `world/pc.py`, modelled to real sizes: a 29 inch presentation monitor (27
inches seen: 549 x 411 mm, so that a visitor without a headset can read it; case 645 x 533 x 518 mm)
in two mouldings with a seam, slotted where its housing narrows; a mini tower on the table with
a 5.25 inch CD-ROM drive, a spare bay that holds the place's key, a 3.5 inch floppy drive,
power, reset and turbo keys, lamps, a lock and a grille, and behind it a PS/2 power supply's
plate (80 mm fan, mains inlet and outlet, voltage switch), the board's sockets and seven card
slots 0.8 inch apart; a keyboard of 19 mm keys; a mouse on its mat; and their leads. Its
lettering is decals. The picture is on the glass itself: the glass's texture coordinates are
the picture's, and the glass is a lit, reflecting surface (`CRT.shader`).

What is small on the monitor and the tower is not geometry in the room. `blender -b --python
world/bake_pc.py` (43 s) builds them twice from `world/pc.py`: whole (`MODE = "high"`), and as
bare shells in one material (`"low"`), and bakes the first onto the second: colour, normals,
roughness and occlusion, and the lamps' light (`Textures/Assets/PH_pc_station_*`). The set it
writes (`build/polyhaven/pc_station`: shells, glass, keyboard, mouse, leads) is placed by
`world/build.py` as any downloaded model is. A place is 7,500 triangles where it was 19,000,
and its slots and lettering are filtered as a texture is. Run it before `build.py` after any
change to `pc.py`.

## Typing and pointing at a place

A visitor's own place is a terminal of their machine. Its keyboard is the console keyboard's
layout (`world/keyboard.json`) laid over the model's caps: an `EmuKeyboard` with no plates of
its own, whose keys go into the machine's console keyboard while the tube shows the console and
into its display keyboard (key events) while it shows the display (`EmuKeyboard.Feed`, set by
`EmuStations.Assign`). A mark a hand shows the key a beam is on. The tube is a flat plate
before the glass; a beam on it in display mode is the machine's pointer, as on the wall's
display (`EmuPointer.tubes`). Only the place's owner's beams find either: the other seven
keyboards and tubes are skipped (`ownStation`).

To test it in play mode: press a key by the keyboard's program variables (`__1_hand__param`,
`__1_key__param`, `__0_Press`; `__2_hand__param`, `__0_Release`) and read the tube; for the
pointer, `TeleportTo` before the place, turn "PlayerCamera" at a point of "Station N tube" and
read the machine's `pointerX`, `pointerY` (a quarter in from the corner of 800 x 600 gave
200, 147). Untried in a headset.

## What is where on the cases

Every control and socket has its own legend, placed from its own position: `world/pc_faces.py`
lists them, `world/textures.py` draws one picture a legend, and `pc.legend` puts it over (or
under) its control. The monitor's terminal board is the table `BOARD` in `world/pc.py`, the
tower's back `tower_back()`; the leads in `station()` end at those places, each in a plug whose
shell reaches the panel (`dsub_plug`). Sockets are built to their standards' sizes (`DSUB`: a
flange 12.55 mm high, 30.81 wide for 9 and 15 pins, 53.04 for 25; BNC 9.7 mm; phono 8.3;
DIN 13.2; IEC C14 and C13). The den's own tower is the same case: the bake writes the tower
alone as `pc_tower`, which `world/computer.py` places. The key in the tower's spare bay says
whose the place is and, under it, what a press does (`EmuStations.modes`).

The computer by the desk has sounds of its own (`EmuPcSound`, `world/sounds.py`'s `computer()`):
switched on, fan and disk run up, the memory test ticks, the floppy's head goes home, one beep,
then the disk is read; a fan loop while it runs, with the disk's knocks now and then; a
run-down when it is switched off. They are computed, as the rain and thunder are.

Where the sizes are from: the drive bays (146.1 x 41.3 mm, and 101.6 x 26.1 mm) from
Wikipedia's "Drive bay"; the power supply's 150 x 86 mm from Corsair's and Advantech's PS/2
figures; the tube (29 inches, 27 seen, a 108 degree stripe-trio tube with 0.6 to 0.71 mm
between stripes, front buttons, exhaust fans behind) from crtdatabase.com's page for NEC's
MultiSync XM29 Plus, and its cabinet's 645 x 533 x 518 mm from NEC's user manual (manualslib,
page 31). The manual's specifications give the tube (type A68, 27 inches seen, 108
degrees, stripes 0.60 mm apart at the middle and 0.78 in the corners) and two oval loudspeakers
of 9 x 5.5 cm; its "Front View" lists POWER, a STANDBY/POWER indicator (green on, red standby),
the remote's window and six keys (RGB 2/+, RGB 1/-, VIDEO 2, VIDEO 1, EXIT, PROCEED); its
"Terminal Board" the BNC, D-sub and phono sockets, the clips for outside loudspeakers and the
switches. The manual's text does not say where on the cabinet each is: their places here are a
guess, and so is the cabinet's colour (the room's computers are beige). The tower is a mini tower's 180 x 360 x 420 mm (Chieftec's lists give 180 x 352 x
425); no sheet for a tower of the decade was found. The card slots' 0.8 inch (20.32 mm) is the AT's card spacing as US patent 5961352
states it; the keys' 19.05 mm is ISO 1091's 19 mm +- 1 (0.75 inch), per Deskthority's "Key pitch".

- A visitor's place is `station` in their own `EmuShare` (-1: none): a synced number only its
  owner can write, so nobody moves anybody else. `EmuStations` gives a new visitor the first
  free place; the key under a tube takes that place if it is free, and at a visitor's own
  turns the tube between display and console (`stationMode`, synced the same way). Two who name one place: the
  visitor with the lower player number has it, and the other looks for a free one.
- Whoever has a place sends their display as for a watcher (`docs/share.md`): the hub counts
  the room as one. `EmuStations.Received` puts a packet's tiles into that place's own store and
  decodes it into that place's picture; a visitor's own tube shows their machine itself.
- A client that is new to a place, or lost a packet, sets `askFrom` and counts `askCount` up
  in its own share; the place's owner sends everything again (at most every 3 s a place).
- A place's console is the rows the same packets carry, kept in a grid of its own and drawn by
  `Terminal.shader` into a 640 x 480 picture; a visitor's own is their terminal's.
- `CRT.shader`: the picture behind bulging glass, the beam's lines (one a row of the picture)
  and stripes of three phosphors. Lines and stripes are waves of mean one, each weakened by how
  much of its period a screen pixel spans: close up they show, further off they are their mean,
  and there is no moire between.

To test with one player: `ClientSimMain.SpawnRemotePlayer`, set the new `EmuShare`'s `station`,
copy this player's `flags`, `seq` and `packet` into it each time `seq` changes, and send
`EmuStations` the event `__0_Received` with `__1_share__param` set to it. The place's picture
must come to within a few levels of the machine's own display.
