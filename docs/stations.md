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
read `EmuPointer`'s `px`, `py`: they must be where the head's ray meets the glass's ball
(centre `tubeGap + tubeBulge` behind the plate), in the plate's own right and up. At a place
turned 3 degrees, from the side and above, that gave 574, 396 for 574, 398. Untried in a headset.

## How the places lie, and their own keys

No two places lie alike (`classroom` in `world/furniture.py`, its own `random.Random(11)`): a
place is up to 15 mm off its spot and turned up to 3.5 degrees, its keyboard up to 2.5, its mat
6 and its mouse 20. The script writes what it chose to `Models/stations.json`, and
`ShaderEmuStations.cs` puts each place's tube, keyboard and keys there ("Station N" is a frame
of the place; everything of it is a child). There are no seats: the chairs are furniture.

The keyboard is 1.3 times a real one (`KEY_SCALE` in `world/pc.py`), for beams. A beam anywhere
on its plate presses the nearest key (`EmuKeyboard.KeyAt`), so there is no gap between keys to
miss into, and a key going down and up clicks (`Sounds/Key1-4.wav`, `keys()` in
`world/sounds.py`, cut from a recording; only the typist's client plays it).

The modelled keys of a place work, for its owner alone (`EmuPlace` on a plate over each group
sends the press to `EmuStations.Act` with the place's number):

| Key | Does |
|---|---|
| tower: POWER, RESET, TURBO | the owner's machine on and off, its reset, its speed mode |
| monitor: RGB 2/+, RGB 1/- | the next and the last of the console's four terminals |
| monitor: VIDEO 2, VIDEO 1 | the tube shows the console, the display |
| monitor: PROCEED, EXIT | the links panel (`docs/fetch.md`) before this tube, small; away again |

A lamp over PROCEED is lit while the owner's browser wants addresses pasted.

A monitor's lamp and its tower's power lamp are lit while the place's owner's machine is on, and
dark on a place nobody has: they are objects the builder puts before the model's lenses
(`PowerLamps`, `EmuStations.powerLamps`; the den's own tower has one too). The bake leaves every
lamp of the set dark (`world/bake_pc.py`): baked into its picture of light, they glowed on
machines that were off.

**The speed window** on each tower (`SevenSeg.shader`) has two rows of three digits: MIPS ALL,
what all the machine's cores ran in the last second, and MIPS CPU, core 0 alone, in millions
of instructions a second with one decimal. It is its owner's machine's speed, to everybody:
the two numbers travel in the display stream's header (`docs/share.md`), and a place nobody
has, or whose machine is off, is dark. The den's own tower shows this visitor's. The DX2-66
badge stays.

To test in play mode: `SendCustomEvent("Turbo")` and the like on "Station N tower hot" and
"Station N monitor hot" of one's own place and of another; only one's own may change
anything (`EmuMachine`'s speed mode, `EmuTerminal.Shown()`, `stationMode`, the links panel's
place). `speeds[N]`'s `_All` and `_Core` are ten times the machine's `mipsAll` and `mipsCore`.

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

The computer by the desk has sounds of its own (`EmuPcSound`, `computer()` in `world/sounds.py`),
all cut from one recording of a computer being started and stopped: its first twelve seconds
when it is switched on (the switch, and its loudspeaker's beep five seconds in), eight quiet
seconds as a loop while it runs, four of its busy moments with the steady sound taken out for
the disk at work, and its end when it is switched off. The cuts were placed by measurement
(where the tone is, which half seconds have knocks in them). Computed sounds were tried first
and were wrong: for a sound, find a recording.

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
- Whoever has a place sends their display as for any screen that shows them
  (`docs/share.md`). `EmuStations` tells `EmuStreams` whose each place is (surfaces 1 to 8),
  and puts that sender's slot's picture or console on the tube; a visitor's own tube shows
  their machine itself. Asking for what was lost, and for everything when a place is new to
  a client, is `EmuStreams`'s.
- A place that is given up is FREE and black at once: its slot is emptied.
- **A place's keys and pointer are its owner's machine's, whatever the wall shows.** Its
  keyboard goes straight into the machine (`EmuKeyboard.sink`, the queues a watcher's keys
  use), and a beam on its tube is marked as the place's own (`pointerOwn`): neither is sent
  to a player the wall happens to show.
- `CRT.shader`: the picture behind bulging glass, the beam's lines (one a row of the picture)
  and stripes of three phosphors. Lines and stripes are waves of mean one, each weakened by how
  much of its period a screen pixel spans: close up they show, further off they are their mean,
  and there is no moire between.

To test with one player: `docs/share.md`'s hook for several players. The place's picture
(the slot's, `StreamPicture`N) must come to within a few levels of the machine's own display.
With the hub's `picked` set to a spawned player, a key pressed at this visitor's own place
must move the machine's `keyTail` and leave the hub's `outTail` alone.
