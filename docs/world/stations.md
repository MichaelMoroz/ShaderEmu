# The classroom's computers

In the middle of the den are six tables in the main desk's oak, three rows of two, with twelve
computers of the decade, one a visitor whose machine is on (`classroom` in `world/furniture.py`; the chairs are the
main desk's). The rows are at z 2.45, 0.45 and -1.55 (`ROWS_Z`), a place's number its row times
four plus its column: 1.6 m are left before the main desk, and the last row's chairs are pushed
in (`CHAIRS_BACK`), which keeps their casters off the rug.
Each tube shows its owner's display, or their console, to everybody.

The computer is `world/pc.py`, modelled to real sizes: a 29 inch presentation monitor (27
inches seen: 549 x 411 mm, so that a visitor without a headset can read it; case 645 x 533 x 518 mm)
in two mouldings with a seam, slotted where its housing narrows; a mini tower on the table with
a 5.25 inch CD-ROM drive, a spare bay that holds the place's key, a plain front under them
where Unity lays the speed slider, power and reset keys, lamps, a lock and a grille, and
behind it a PS/2 power supply's plate (80 mm fan, mains inlet and outlet, voltage switch), the
board's sockets and seven card slots 0.8 inch apart, a sound card in the fourth (three jacks and
the joystick's socket), a network card in the sixth (thin coax, twisted pair and a link lamp); a
keyboard of 19 mm keys; a mouse on its mat; and their leads. Its
lettering is decals. The picture is on the glass itself: the glass's texture coordinates are
the picture's, and the glass is a lit, reflecting surface (`CRT.shader`).

What is small on the monitor and the tower is not geometry in the room. `blender -b --python
world/bake_pc.py` builds them twice from `world/pc.py`: whole (`MODE = "high"`), and as
bare shells in one material (`"low"`), and bakes the first onto the second: colour, normals,
roughness and occlusion, and the lamps' light (`Textures/Assets/PH_pc_station_*`). The set it
writes (`build/polyhaven/pc_station`: shells, glass, keyboard, mouse, leads) is placed by
`world/build.py` as any downloaded model is. Its slots and lettering are filtered as a texture is. Run it before `build.py` after any
change to `pc.py`.

## Typing and pointing at a place

A visitor's own place is a terminal of their machine. Its keyboard is the console keyboard's
layout (`world/keyboard.json`) laid over the model's caps: an `EmuKeyboard` with no plates of
its own, whose keys go into the machine's console keyboard while the tube shows the console and
into its display keyboard (key events) while it shows the display (`EmuKeyboard.Feed`, set by
`EmuStations.Assign`). A mark a hand shows the key a beam is on. The wide key over the number
pad hands over the visitor's real keyboard; its name is a sign of its own on the cap
(`captureSign`), since the words change while it is on. The tube is a flat plate
before the glass; a beam on it in display mode is the machine's pointer, as on the wall's
display (`EmuPointer.tubes`). Only the place's owner's beams find either: the other places'
keyboards and tubes are skipped (`ownStation`).

To test it in play mode: press a key by the keyboard's program variables (`__1_hand__param`,
`__1_key__param`, `__0_Press`; `__2_hand__param`, `__0_Release`) and read the tube; for the
pointer, `TeleportTo` before the place, turn "PlayerCamera" at a point of "Station N tube" and
read `EmuPointer`'s `px`, `py`: they must be where the head's ray meets the glass's ball
(centre `tubeGap + tubeBulge` behind the plate), in the plate's own right and up.

## How the places lie, and their own keys

No two places lie alike (`classroom` in `world/furniture.py`, its own `random.Random(11)`): a
place is up to 15 mm off its spot and turned up to 3.5 degrees, its keyboard up to 2.5, its mat
6 and its mouse 20. The script writes what it chose to `Models/stations.json`, and
`ShaderEmuStations.cs` puts each place's tube, keyboard and keys there ("Station N" is a frame
of the place; everything of it is a child). There are no seats: the chairs are furniture.

The keyboard is 1.3 times a real one (`KEY_SCALE` in `world/pc.py`), for beams. It lies flat
(no feet, `KEYBOARD_SLOPE` 0), its caps are the console keyboard's shape (the insets of
`world/pc_faces.py`, which `world/computer.py` and `Keyboard.png` share) and all as high, and
it has the capture key ("Use my keyboard") where the console's layout has it, over the number
pad. `stations.json` says where Unity lays the keys' rectangles: `keyboardHigh` (the tray's own
frame over the table), `keyboardSlope` and `keyboardTops` (the rectangles over that frame). A beam anywhere
on its plate presses the nearest key (`EmuKeyboard.KeyAt`), so there is no gap between keys to
miss into, and a key going down and up clicks (`Sounds/Key1-4.wav`, `keys()` in
`world/sounds.py`, cut from a recording; only the typist's client plays it).

The modelled keys of a place work, for its owner alone (`EmuPlace` on a plate over each group
sends the press to `EmuStations.Act` with the place's number):

| Key | Does |
|---|---|
| tower: POWER, RESET | the owner's machine on and off, its reset (the speed mode is the scope panel's) |
| tower: POWER of a free place | anybody's: their machine starts, and the place is theirs |
| tower: EXIT, in the spare bay | the place is given up |
| monitor: TERM +, TERM - | the next and the last of the console's four terminals |
| monitor: CONSOLE, DISPLAY | the tube shows the console, the display; on another's place, for this visitor alone |
| monitor: Physical buttons, Steady pointer | anybody's: this visitor's own switches, as the control panel's |
| monitor: LINKS ON, LINKS OFF | the links panel (`docs/system/fetch.md`) before this tube, small; away again |

A lamp over LINKS ON is lit while the owner's browser wants addresses pasted.

POWER has its name over it in lit letters, and a bar under it that beckons while the place is
free (`beacons`). The capture key is the keyboard's.

**The fields.** One strip ("Station fields") stands between a visitor's own keyboard and its
monitor, for them alone: an address for the machine to open (`placeField`, as the display
keyboard's link field: `docs/system/open.md`) and text that is typed at the place's keyboard
(`EmuPaste`, which paces itself by the machine's own queue there: `RemoteWaiting`). Its third
line (`placeLabel`) says what came of the link: "start the desktop first" while the machine
has no display (the desktop's bar is what takes an address), then the fetch's own news, which
the wall's control panel shows too (`EmuMachine.FetchSays`).

**Console, then display.** A tube shows the console while its owner's machine has no display
and turns to the display when it has one, and back when it is gone (`tubeFor`, `tubeHad` in
`EmuStations.Assign`, written to `stationMode`); between the two CONSOLE and DISPLAY choose.
On somebody else's place CONSOLE, DISPLAY and the place's key turn that tube for this visitor
alone (`localMode`, `EmuStations.Look`): its owner's choice is untouched, and the choice is
forgotten when the place changes hands. Their console comes in their stream whatever they show.

The tower's third lamp, the disk's, is a dull red lens, and before it a red lamp that flickers
(`Blink.shader`, `diskLamps`): sixteen bits, an eighth of a second each, set where core 0's
registers had changed (`EmuMachine.activity`). Its owner's machine's bits travel in the stream's status
(`docs/world/share.md`), so everybody sees a place's lamp, two seconds late. The card's lamp and its
hub port's are lit while the machine is on and flicker when it sends (`docs/system/lan.md`). A monitor's lamp and its tower's power lamp are lit while the place's owner's machine is on, and
dark on a place nobody has: they are objects the builder puts before the model's lenses
(`PowerLamps`, `EmuStations.powerLamps`; the den's own tower has one too). The bake leaves every
lamp of the set dark (`world/bake_pc.py`): baked into its picture of light, they would glow on
machines that are off.

**The speed window** on each tower (`SevenSeg.shader`) has two rows of four digits: MIPS ALL,
what all the machine's cores ran in the last second, and MIPS CPU, core 0 alone, in millions
of instructions a second, the point where the number puts it (3.752, 37.52, 375.2, 3752). It is its owner's machine's speed, to everybody:
the two numbers travel in the display stream's header (`docs/world/share.md`), and a place nobody
has, or whose machine is off, is dark. The den's own tower shows this visitor's. The DX2-66
badge stays.

**The speed's slider** on each tower stands under the place's key
(`StationSpeed` in `ShaderEmuStations.cs`, "Station N speed"): Snail to Turbo over it, and under
it the instructions a frame of each step, 4k, 8k, 16k, 32k, 64k, 128k and 256k: steps 1 to 7 of the
control panel's speed slider (`EmuMachine.speedSlider`, 2,048 doubled a step), which moves with
it. It shows its owner's machine's step to everybody: the step travels in their `EmuShare` as
`speed`, which `EmuStations.Assign` keeps equal to their slider. Only the owner can move it: on
anybody else's client it is not interactable, and a move that gets through is put back
(`EmuStations.SpeedMoved`). A free place's shows step 4. Step 0 set on the
control panel shows at the slider's end. To test in play mode: `Take0` on "Stations", then set
"Station 0 speed/Speed slider"'s value: the control panel's slider follows; set another
place's: it goes back. Set the control panel's: the tower's follows within half a second.

**The volume's slider** on each monitor lies on the bezel between its row of keys and the glass
(`StationVolume`, "Station N volume"): the owner's own machine's sound, 0 to 100 as the control
panel's Volume (`EmuSound.volumeSlider`), which moves with it both ways (`EmuStations.VolumeMoved`).
The sound is each visitor's own and is not shared, so only the owner's slider is interactable and
shows their volume. To test in play mode: `Power` on "Station 0", then set "Station 0
volume/Volume slider"'s value: the panel's Volume follows; set the panel's: the monitor's follows.

**The pointer's switches** stand either side of the volume (`StationSwitches`, "Station N physical"
and "Station N steady"): Physical buttons and Steady pointer, the control panel's switches
(`EmuPointer.PhysicalButtons`, `SteadyPointer`), every visitor's own at any place; all the labels
follow (`physicalLabels`, `steadyLabels`). "ShaderEmu/Add the monitors' pointer switches to the
open scene" puts them into a built scene, without a bake.

To test in play mode: `SendCustomEvent("ResetKey")` and the like on "Station N tower hot" and
"Station N monitor hot" of one's own place and of another; only one's own may change
anything (`EmuMachine.ResetMachine`, `EmuTerminal.Shown()`, `stationMode`, the links panel's
place). `speeds[N]`'s `_All` and `_Core` are ten times the machine's `mipsAll` and `mipsCore`.

## Strips, the network, stickers

- **One strip of sockets** (`strip` in `world/pc.py`) is the classroom's and the den's: five
  sockets drawn on its top (the decal `strip_socket`: an empty one shows), a lit rocker
  (`Amber`), a sleeve for its own lead. `strip_socket(k, at, turn)` is socket k's middle: plugs
  and the leads' ends are placed by it, so a plug stands on its socket.
- **The network.** Every tower's card has a lead to its row's hub (`network` in
  `world/furniture.py`): the front and the back rows' hubs stand on the second place's tower,
  turned, their leads hanging down its right side; the middle row's is on the floor behind the
  tables' middle. The hubs are joined through between the two tables' feet, and the first to
  the den's rack along the main desk's foot. A lead goes over the table's back edge and under
  the table (two lanes, `lanes`), over each strip's own lead it meets. Each hub's leads fall
  together as a group of their own (`drape.settle`, with the tables' edges 2.5 mm further out
  as `guards`: a lead drawn tight over the true edge was cut by it), and `drape.apart` says if
  one lies in a place's own lead. `LAID ...` in the build's output is a mistake in the layout,
  before anything falls.
- **The hubs' supplies.** A hub has a barrel socket for its supply on its back beyond its
  eighth port (`dc_inlet`; a hub on a tower has it at the end towards the tower's back,
  `hub_power`). A plug-in supply of 44 x 58 x 50 mm (`adapter`: a moulded block narrowing to its
  top, a rating plate, a strain relief its lead leaves by upwards) stands on socket 4 of the
  strip of the row's second place (`HUB_SUPPLY`, `station(..., supply=True)`, which lets the
  place's own leads fall round it too). Its lead (`supply` in `network`) falls with the hub's
  others: from a hub on a tower round the tower's back corner and down by it (laid down the
  back right of the plugs; it falls closer to the corner), over the table's back edge, along the floor before the strip (before where the mains
  leads come down, and behind the network's leads rising to the hub) and up into the supply;
  from the middle row's hub along the floor to the left, clear of the leads into it.
- **The sound.** A place's sound card's line out (the jack beside its joystick socket) has a
  lead to the monitor's two phono sockets (`station`): a 3.5 mm plug (`jack_plug`), a black
  lead, a moulded Y (`y_split`) that hangs behind the monitor from two short tails, and a white
  and a red phono plug (`rca_plug`), each on its socket. The lead lies on the table before the
  picture's where there is room behind the monitor (and the keyboard's and the mouse's lanes
  stay before it), else it hangs behind the table as the picture's does. The plugs reach the
  monitor's and the tower's backs: the baked shells have no sockets standing out of them. The
  den's computer has one too, from its line out up through the wall beside the screens' two
  (`cables` in `world/computer.py`).
- **Its lamps are Unity's.** The model has dull lenses only (a hub's eight, a card's one), and
  `stations.json` says where: `nic` and `nicSize` (the card's lens in a place's frame), `hubs`
  (each hub's place and turn, and four numbers a connected port: its lens's middle in the hub's
  frame, the lamps towards the hub's -z, and the place it serves, -1 for a lead to another hub
  or the rack). The file is flat (`tables` is x, z, x, z ...): Unity's `JsonUtility` reads no
  list of lists. A hub's power lamp is the model's own, lit.
- **Stickers** (`stickers` in `world/pc.py`, `pc_faces.STICKERS`, `stickers` in
  `world/textures.py`): two to four a place by the place's own chance, on the bare parts of the
  tower's sides, top and front and of the monitor's bezel, top and sides, 0.4 mm proud, in the
  place's "rest" object. A tower with a hub on it has none on its top and right side.

## What is where on the cases

Every control and socket has its own legend, placed from its own position: `world/pc_faces.py`
lists them, `world/textures.py` draws one picture a legend, and `pc.legend` puts it over (or
under) its control. The monitor's terminal board is the table `BOARD` in `world/pc.py`, the
tower's back `tower_back()`; the leads in `station()` end at those places, each in a plug whose
shell reaches the panel (`dsub_plug`, `rca_plug`, `jack_plug`). Sockets are built to their
standards' sizes (`DSUB`: a flange 12.55 mm high, 30.81 wide for 9 and 15 pins, 53.04 for 25;
BNC 9.7 mm; phono 8.3; DIN 13.2; IEC C14 and C13), and so are the plugs: a 3.5 mm plug's grip
6.4 mm across, a phono plug's 10.4, a supply's barrel plug's 9.6. The tower's front is plain
under the blank bay, and Unity lays the speed slider there (a plate of about 146 x 42 mm, `TowerSpeedBay` in
`ShaderEmuStations.cs`). The den's own tower is the same case: the bake writes the tower
alone as `pc_tower`, which `world/computer.py` places. The key in the tower's spare bay says
whose the place is and, under it, what a press does (`EmuStations.modes`).

The computer by the desk has sounds of its own (`EmuPcSound`, `computer()` in `world/sounds.py`),
all cut from one recording of a computer being started and stopped: its first twelve seconds
when it is switched on (the switch, and its loudspeaker's beep five seconds in), eight quiet
seconds as a loop while it runs, four of its busy moments with the steady sound taken out for
the disk at work, and its end when it is switched off. The cuts were placed by measurement
(where the tone is, which half seconds have knocks in them). For a sound, find a recording: computed ones are wrong.

Where the sizes are from: the drive bays (146.1 x 41.3 mm, and 101.6 x 26.1 mm) from
Wikipedia's "Drive bay"; the power supply's 150 x 86 mm from Corsair's and Advantech's PS/2
figures; the tube (29 inches, 27 seen, a 108 degree stripe-trio tube with 0.6 to 0.71 mm
between stripes, front buttons, exhaust fans behind) from crtdatabase.com's page for NEC's
MultiSync XM29 Plus, and its cabinet's 645 x 533 x 518 mm from NEC's user manual (manualslib,
page 31). The manual's specifications give the tube (type A68, 27 inches seen, 108
degrees, stripes 0.60 mm apart at the middle and 0.78 in the corners) and two oval loudspeakers
of 9 x 5.5 cm; its "Front View" lists POWER, a STANDBY/POWER indicator (green on, red standby),
the remote's window and six keys (RGB 2/+, RGB 1/-, VIDEO 2, VIDEO 1, EXIT, PROCEED: here they are
named for what they do, TERM +, TERM -, CONSOLE, DISPLAY, LINKS OFF, LINKS ON); its
"Terminal Board" the BNC, D-sub and phono sockets, the clips for outside loudspeakers and the
switches. The manual's text does not say where on the cabinet each is: their places here are a
guess, and so is the cabinet's colour (the room's computers are beige). The tower is a mini tower's 180 x 360 x 420 mm (Chieftec's lists give 180 x 352 x
425); no sheet for a tower of the decade was found. The card slots' 0.8 inch (20.32 mm) is the AT's card spacing as US patent 5961352
states it; the keys' 19.05 mm is ISO 1091's 19 mm +- 1 (0.75 inch), per Deskthority's "Key pitch".

- A visitor's place is `station` in their own `EmuShare` (-1: none): a synced number only its
  owner can write, so nobody moves anybody else. A place is a running machine's:
  `EmuStations` gives a visitor the first free one when their machine is switched on, takes it
  back when it is switched off, and counts another visitor's claim only while their machine
  is on (so no place stands dark for a machine that is off). The key in the tower's bay takes that place if it is free (and starts the machine),
  and at a visitor's own turns the tube between display and console (`stationMode`, synced
  the same way); EXIT beside it gives the place up, and none is found for that visitor again
  until they take one or switch their machine on (`left`). Two who name one place: the
  visitor with the lower player number has it, and the other looks for a free one.
- Whoever has a place sends their display as for any screen that shows them
  (`docs/world/share.md`). `EmuStations` tells `EmuStreams` whose each place is (surfaces 1 to 12),
  and puts that sender's slot's picture or console on the tube; a visitor's own tube shows
  their machine itself. Asking for what was lost, and for everything when a place is new to
  a client, is `EmuStreams`'s.
- A place that is given up is FREE and black at once: its slot is emptied.
- **A place's keys and pointer are its owner's machine's, whatever the wall shows.** Its
  keyboard goes straight into the machine (`EmuKeyboard.sink`, the queues a watcher's keys
  use), and a beam on its tube is marked as the place's own (`pointerOwn`): neither is sent
  to a player the wall happens to show.
- `CRT.shader`: the picture behind bulging glass, the beam's lines (one a row of the picture)
  and stripes of three phosphors. The picture spans 0.97 of the glass (`_Fill`; the beam's
  place on the tube is mapped by the same number, `tubeFill`): 8 mm of glass at the sides and
  6 above and below. The opening's corners are rounded by 22 mm and the picture's are square:
  above 0.973 the bezel cuts them. The stripes are 560 trios across the
  glass (0.95 mm, coarser than the tube's own 0.6), to be seen from the chair; the lines
  stay one a row. Lines and stripes are waves of mean one, each weakened by how
  much of its period a screen pixel spans: close up they show, further off they are their mean,
  and there is no moire between.

To test in play mode: `SendCustomEvent("Power")` on "Station 5" (its `EmuPlace`) starts the
machine and `station` is 5; `ClientSimMain.SpawnRemotePlayer` three times, write `flags`,
`station`, `watching` and `netSeq` into the new players' `EmuShare` and send each
`_onDeserialization`: one whose `flags` is 0 has no place whatever its `station` says, the
labels name the others, and `hubLamps` are lit only for places that are taken. `Leave` on
the place's `EmuPlace` leaves `station` at -1.

To test with one player: `docs/world/share.md`'s hook for several players. The place's picture
(the slot's, `StreamPicture`N) must come to within a few levels of the machine's own display.
With the hub's `picked` set to a spawned player, a key pressed at this visitor's own place
must move the machine's `keyTail` and leave the hub's `outTail` alone.
