# The holodecks

Through the den's left doorway is a corridor 2 m wide and 15.4 m long, as plain as the one by the lift
(plaster, oak panelling, a carpet, lamps on rods, pictures, a side table and a bench), with eight rooms
of grid off it, four a side, each 3.6 m square and 3 m high, with a seat. While a 3D program runs on the
machine of whoever sits there, its last frame is a world round the seat: the command list the
volume display drew (`docs/world/volume.md`, the words at `0x81000300`), seen from the sitter's own
eyes. Games and their controllers are here and nowhere else in the world.

## The rooms' model

A room is one welded mesh (`holo_room` in `world/shell.py`): its walls one strip round it with the
doorway (1.2 by 2.4 m) cut out, its floor and its ceiling, every face a whole panel of 0.6 m
(`annex.GRID`: 6 by 5 a wall), so that no panel is cut at a corner or by the doorway. Its lightmap
is the strip unrolled from the door wall's corner at +x, so three of its four upright corners are
inside a chart, and the floor and the ceiling are charts under it (`check_shell.py` checks it). The
doorway's reveal is the corridor's plaster. The lines between the panels are lit bars
(`grid_bars` in `world/annex.py`, `HoloLine`): 8 mm proud on the walls (7 across them), 3 and 2 on
the floor, 8 and 7 on the ceiling, and a bar 15 mm square in each of the room's twelve edges, lying
in the corner against both faces. No two bars go into each other or share a face's plane where they
meet (the corners had flickered where three edge bars and the lines' ends met in one plane): the
upright edge bars are the room's height and the level ones end against them, the walls' upright lines
end against the edge bars and the level ones against the upright lines (`less`), the floor's and the
ceiling's across lines against those along, and the floor's stop 3 mm short of the seat's foot and
the console's base (`feet`, `cut_line`). `world/check_overlap.py` holds it (`docs/world/world.md`). The mask is 20 mm inside the panels (`MASK_IN`), in front of every bar, so the
program's world is not crossed by them. The corridor's furniture is in `holodeck.json`
(`furniture`), which `ShaderEmuHolodeck.cs` makes colliders of.

## What a visitor does

- **Sit down** in a free room's seat (a plate by each door says whose the room is, or FREE).
  The seat is a station of the scene, one visitor at a time; whoever sits has the room until
  they get up. Anybody may walk in and stand there. The seat keeps its sitter whatever the
  sticks do (they are the game's). In a
  headset the sitter's controllers are the game's from the moment they sit
  (`docs/world/gamepad.md`), and both grips held for a second, or the console's Exit, stands them
  up; the console's line says what the controls are.
  A stick can stand a sitter up in a headset whatever the station is told, so whoever is stood up
  without having asked is sat down again two frames later (`EmuHolodeck.Exited`, `SitAgain`; a
  respawn lets go).
- **The seat rises under its sitter** (`EmuSeat`). VRChat puts a visitor on a station standing,
  feet where the station is, and their pose sinks into sitting over about half a second,
  hips where the station is. So the station waits on the floor and comes up to
  the seat's height in that half second (`settle`), and the sitter only goes down. The half
  second is what was seen, not a number VRChat gives: `settle` is the thing to change.
- **Without a headset** the sitter's own keyboard is the game's from the moment they sit
  (the display keyboard's capture, raw keys): the game's own keys, as at a desk. End stands
  them up at once, whatever is held and whoever has the keyboard (`EmuHolodeck.StandUp`);
  Insert gives the keyboard back, and Space then stands them up too. The console's line
  says so. There is no controller there: its sticks would be the mouse and keys the
  visitor looks round with.
- **One mode.** The program's camera stands at the seat's eye point and the horizon is the
  room's: of the camera's turn only what is about the world's up is followed, so the sitter
  faces the way the game's camera faces, and the floor stays the floor when the game looks up
  or down. The head moves freely in that.
- **The console** beside the seat has the room's keys, the sitter's alone: Exit (stands the
  sitter up, whatever they hold: `StandUp`, as the End key), Screen (the console's small one,
  or a large one before the seat), 3D, Hide and Beam (below), and four sliders: World (the scale: at
  its largest the picture's rectangle on the program's near plane is 1.28 by 0.8 m, and the slider
  moves that plane out towards the far one; it starts where the game's player's eye is the sitter's:
  "The world's size"), Screen size (the large screen before the seat, half to twice its size, 100% in
  the middle), Volume and Voices ("A flat screen in place of the world"). A slider is
  VRChat's own for a desktop's sitter (`WorldSlid`, `ScreenSlid`) and the beams' in a headset
  (an event ending in "Slide" is sent again while the trigger is held, with where the beam is
  across it: `EmuPointer.holoSlide`). "ShaderEmu/Put the holodecks' size sliders into the open
  scene" makes the consoles again with them (new plates, lit by the probes until a bake).
  There is no key for the controls: they are the game's (`docs/world/gamepad.md`).
- **A sitter in a headset presses those keys with their own beams** (`EmuPointer`, `holoPlates`:
  a plate a hair before the console's face, the keys' rectangles, a mark on the key a beam is
  on), which are out for nothing else while they sit; the trigger of a hand whose beam is on
  the console is the console's, not the game's. VRChat's laser does not press them from the
  seat, so the console's own collider is off for that sitter (`plateColliders`), or a key
  that the laser did reach would be pressed twice. Anybody else, and a sitter without a
  headset, presses them with VRChat's laser as any panel.
- **The seat's Interact box is off for its own sitter** (`EmuSeat`, `reach`), and for a second
  after they stand up. It is round the sitter's hands, so a trigger pulled there would use the seat,
  and the console's Exit would stand the sitter up and sit them down again.
- **The console** at the seat's right hand is turned to the sitter and has one sloped face: the
  room's keys below, and above them, in the same plane, the room's one screen (0.40 by 0.30 m).
  It shows the sitter's display, whole: to the sitter their own machine's, which answers
  their beams as the den's wall does; to anybody else the sitter's display from the stream
  (`docs/world/share.md`: surfaces 13 to 20 of `EmuStreams`, the syncing the classroom's tubes use).
  Only the sitter's own client draws the program's world; a visitor standing in another's
  room sees a room of grid and the console.
- The seat, the console, its keys and its screen are before the program's world however near
  that world reaches, and behind everything else of the room and its visitors as any thing is.

## The world's size

A game says how high its player's eye is over its floor (`seglWorldEye` in `GLES/segl.h`: the eye
in its units, its near and far planes and its projection's scales), in the control word
`0x8100006c`: bits 0-15 the plane (in 4,096ths, signed) at which that eye is 1 m high, bits 16-27
how far the plane moves for e times higher (4,096ths), bits 28-31 the game's number
(`linux/apps/gameid.h`). The word is written only when it changes, and the host takes it only
while that game is the one played (`EmuHolodeck.Natural`): the room's scale is then the plane
that puts the player's eye at the sitting visitor's avatar's eye height
(`GetAvatarEyeHeightAsMeters`), and the World slider moves it from there (back to it when another
game is played). A program that says nothing has `plane` (0.35). Doom says 41 units (near 4,
far 8192), Quake 46 (4, 4096), Quake III 50 (4, 16384), Half-Life 64 (4, its level's size).
ClassiCube says its player's eye height (1.625 blocks; near 0.1, far its view distance) from its own
window code (`Window_ShaderEmu.c`, written again only when the eye, the view distance or the
projection changes). On `rvc_cpu` `DOOM_SEAT`, `QUAKE_SEAT`, `QUAKE3_SEAT`, `CLASSICUBE_SEAT` and
`HALFLIFE_SEAT` print the word as `eye`; Doom's is `121903f2` (`seglWorldEye` with its numbers).

ClassiCube says the player's own eye (1.625 blocks), so a block is about a metre (0.98 m for an
eye of 1.6 m) and the sitter is the player's size. In a headset the block in the right hand is
4 cm in the room whatever the world's size (`SE_SeatHeld`, the held model's scale).
Made smaller by the World slider, below 0.6 m a block, the view is lifted over the player's eye
until the ground is 0.6 m under the seat's eye point (`SE_SeatLift`), where the hands are; the
picks start from the lifted eye too.

## Voices

Whoever is inside any holodeck's box, seated or standing, hears everybody who is inside one, the
same or another, at full strength whatever the distance. Between inside and outside (the
corridor is outside) a voice travels as VRChat's does. `EmuVoices` decides it on each client
twice a second from where the players are and sets `SetVoiceDistanceNear` and `Far` on its own
side (400 m both, or 0 and 25 m); nothing is synced.

## How it is drawn

- `GpuHolodeck.shader` is `GpuVolume.shader`'s four passes with `HOLODECK` defined
  (`GpuVolumePass.cginc`). The frame's camera is the modelview of its first draw with a place in
  space (`volume_view` in `src/gpu.h`): a program draws its world before what moves in it, so
  that is the view alone. A vertex in camera space goes back through its rows to the world's own
  turn, the camera staying where it is, then about the room's up until the camera's right is
  the seat's.
- Up is what the program says it is: the top four bits of the frame's size word
  (`0x8100030c`: 1 x, 2 y, 3 z; `seglWorldUp()` in `GLES/segl.h`). Quake and Doom say z,
  ClassiCube y. Of a program that says nothing it is found from the camera (the axis its up
  is nearest to and its right is square to), which is wrong once the camera looks more down
  than ahead: with the aim on a hand, a hand let down made forward the room's up.
- One mask, one seal and one scene serve all eight rooms: `EmuHolodeck.Entered` moves them to
  the room this client's visitor sat down in, with the room's box (`_RoomMin`, `_RoomMax`).
- `HolodeckMask.shader` is on a copy of a room's six faces, a hair inside the grid: where they
  are in view, and while the frame's words name a list, it marks the stencil and empties colour
  and depth; the scene is drawn there; `VolumeSeal.shader` gives those pixels the faces' depth
  again. So the world shows from inside the room and through its door, and nowhere else. A
  visitor is drawn before all of it and is never hidden by the program's world.
- The scene is drawn twice. Where the stencil is marked (the room's faces) it has no bounds.
  Everywhere else (the doorway, the seat, the stand, a visitor) it is drawn with the ordinary
  depth test and only where it is inside the room's box.
- Seen from outside the room, what of the program's world is nearer than the room is left out
  (the way from the eye to it has not entered the room's box yet): it would hang in the doorway.
- A sky that a program lays on its picture (`FRAGMENT_LAID`: Doom's, Quake's) is a panorama here,
  four of its picture to a turn, fixed in the program's world (its way turned back by the camera's
  turn about up), as Doom lays it on its flat picture.
- A program sends only what its own camera sees. Behind the seat there is usually nothing.
- A program's textures, light maps and vertices may be files in the ROM (Quake III Arena's all
  are, `docs/games/quake3.md`): the pass reads the ROM's texture as the GPU's does, and the
  two materials are given them by "ShaderEmu/Give the holodecks' world the ROM" (the first
  image's; a second boot image would need them set when it is chosen).

## The room's own things before the program's world

The program's world is drawn inside the room with the depth test, so a wall of it that came
nearer than the console covered the console. `HoloGuard.shader` is on a copy of the seats'
and consoles' models, of each screen's glass and of each console's panel ("Holodeck guards",
`HoloGuards` in `ShaderEmuHolodeck.cs`): it draws nothing but a mark in the stencil (87 + 128)
where the thing itself is seen, after the room's mask and before the program's world (queue
2502; the builder moves the world's and the seal's materials one later), and no pass of the
world draws on a pixel so marked. "ShaderEmu/Add the holodecks' guards to the open scene"
makes them without putting the room in again. One copy has all eight rooms' things, and
only those inside the box of the room this visitor sits in mark (`_UdonHoloMin`,
`_UdonHoloMax`, which `EmuHolodeck` sets when they sit down and empties when they get up):
the next room's seat and console, behind a wall the mask has emptied the depth of, would show
as their shapes cut out of this room's world.

A large screen before the seat that goes clear while a program draws in space stands in the
program's world and reads wrong whichever way its depth is settled: the console's face has the screen.

## What the machine is told

A program cannot see that it is looked at from a seat. The host writes eight words every frame
at `0x81000360` (`HOST_STATE` in `src/gpu.h`; the control pass, from `EmuHolodeck.Tell`), and
`seglHostState()` gives a program their address (`GLES/segl.h`, with a macro for each field):

| Word | Contents |
|---|---|
| 0 | bits 0-3: the host (1 the harness, 2 the world on a desktop, 3 in a headset; 0: a host that says nothing). Bit 4: the machine's owner sits in a holodeck's seat. Bit 5: the hands are tracked. Bit 6: the jump button is held (`SEGL_HOST_JUMP`). Bits 8-15: visitors in the instance. Bits 16-23: this visitor's number. Bits 24-31: the room's scale in 255ths (`plane`: the plane that is 1.28 by 0.8 m of the room is near x (far / near) ^ this from the camera) |
| 1 | the head's turn to the right, and from bit 16 its tilt upwards: 65,536 to a turn, signed |
| 2 | the head's place, millimetres, signed: to the right, and from bit 16 up |
| 3 | the head's place forward, and from bit 16 its roll; with hands, from bit 16 the left hand's way instead, a signed byte each: its turn to the right in 256ths of a turn, and from bit 24 its rise in 512ths (`SEGL_HOST_LEFT_YAW`, `SEGL_HOST_LEFT_PITCH`; word 5 has the sticks) |
| 4 | the left hand's place in units of 4 mm, ten signed bits each: right, up (from bit 10), forward (from bit 20); its trigger (bit 30) and grip (bit 31) |
| 5 | in a headset the sitter's sticks, a signed byte each, 127 pushed home: the left one to the right, forward, the right one to the right, up (zero while a menu has them as keys). Without a headset the left one is the keys walked by, and bits 16-23 count the notches the wheel has turned up while a game is played (`SEGL_HOST_WHEEL`, wrapping). |
| 6, 7 | the right hand, the same |

A hand's place and way are its beam's (`EmuPointer`): from the controller the way VRChat's laser
went when it clicked the control panel's "Calibrate laser" dot (`Calibrate`, kept in PlayerData
as `beam.left` and `beam.right`), else from the knuckles along the back of the hand (the avatar's
bones), else along the controller. With no hands (a desktop's sitter) the low half of word 4 is how far their mouse has
gone to the right, a counter that wraps (`mouseCounts` of them to one of Unity's "Mouse X").

Places and turns are against the seat's eye point while seated, and against the visitor's own
feet otherwise. While its owner sits in a holodeck Quake leaves out its test of what the
camera sees, so there is world behind the seat too.

## Played from the seat

Doom, Quake, Quake III, ClassiCube and Half-Life read those words, each in its own port file
(`doom_video.c` and `doom_gl.c`, `vid_shaderemu.c`, `tr_shaderemu.c`, `Window_ShaderEmu.c`,
`linux/halflife/vid_shaderemu.c`):

- **With tracked hands the right hand aims.** The game keeps two turns: the body's, which
  the sticks and the game itself turn and by which the room is turned, and the aim's, which
  is the body's and the hand's turn and rise from it. The picture the room draws is level and
  the body's; shots, and ClassiCube's picked block, go the aim's way. A hand's turn does not
  turn the room. ClassiCube's pick starts where the beam does, at the hand at the room's
  scale (`PerspectiveCamera_GetPickedBlock`).
- **ClassiCube's chunks are the head's** while seated with tracked hands: its frustum is 120
  degrees from where the head looks (`SE_SeatCull` in `Game.c`), where the body's camera's
  left out what the head turned to. And since a holodeck shows no clear colour , it draws a box round the eye twice its far plane away, its sky's
  colour above the horizon fading to the fog's (`EnvRenderer_SE_RenderHorizon`), past the
  far plane on a flat screen.
- **The sticks are sticks** (word 5), not keys: the left one walks, as fast as it is pushed,
  the way the head looks (the move is turned from the aim's way to the head's); the right
  one turns the body, 140 degrees a second pushed home, slower near the middle.
- **The weapon is in the right hand**, where the beam starts, pointing as it points, at the
  room's scale (word 0's `plane`): Quake's and Half-Life's view models held a third from their back end
  (Half-Life's with its muzzle on the beam, and its shots leave the hand along it, no autoaim),
  Quake III's by its own origin, ClassiCube's held block (and no arm when nothing is
  held), Doom's weapon pictures standing in the world at 4 mm a pixel. Quake's axe keeps its
  first pose: the arm that holds it swings it. Guns animate as they do. Not done: Quake's
  and Doom's pictures have the character's hands in them.
- **ClassiCube is played with both hands**: each has a beam (`twoHanded`), the right trigger
  breaks the block the right hand's beam is on and the left trigger places a block by the one
  the left hand's beam is on (`SE_LeftPick` in `Camera.c`, the place in `InputHandler.c`), read
  from the host's words as levels (`Seat` in `Window_ShaderEmu.c`), not from the pointer's
  buttons.
- **Without a headset the mouse turns the game** (word 4's counter, 0.066 degrees a count)
  and the head's tilt is the aim's rise. The mouse turns the sitter's head too, so the
  program's world goes round with their head (`EmuHolodeck.PostLateUpdate`): the game turned
  twice as far as they looked. ClassiCube breaks at the view's middle by the left button or
  Ctrl (the right trigger) and places there by the right button (the left trigger).
- **Without a headset the keys played by are told in the words too**, the same in Doom,
  Quake, Quake III, ClassiCube and Half-Life whatever each game's own keys are: W, A, S, D or the arrows
  are the left stick (word 5: they walk and step aside), the left button or Ctrl is the right
  hand's trigger (fire), the right button the left hand's (use in Doom, place in ClassiCube) and
  Space the jump button (word 0's bit 6: jump; use in Doom), E the right hand's grip (word 6's
  bit 31: use in Half-Life, as a headset's right grip is; E stays a key too). While the sitter's
  world is one of those games' (`EmuHolodeck.ByWords`, `played`: the program's number, kept
  while another window has the guest's keyboard) these keys are no key events
  (`EmuKeyboard.playAway`), so a click that lands beside the game's window, the same click that
  fires, does not leave the game without them.
  The wheel is counted in word 5 (ClassiCube: the block in hand). Every other key (the numbers, B, Esc, Enter) is still the window's. In a menu or with the
  keyboard given back (Insert) all keys are as they were. In a headset the right trigger fires
  by the same bit, beside the key its controller sends, and VRChat's jump button jumps by bit 6
  (Enter only in a menu). No trigger jumps. Doom's
  use is the left trigger or the jump button.
- **The room's screen is a pointer's all the time**, the console's small one or the large
  one, with a world drawn round the seat or not: the view's middle and the left button on a
  desktop, each hand's beam and its trigger and grip in a headset (`EmuPointer`: the screen
  answers `holoKeys`, a sitter's beams, and the pointer is the beam's while one is on it). A
  click or a trigger on the screen or the console is theirs and fires nothing in the game
  (`Landed`: in `Tell`, and in `EmuGamepad` for the keys its controls send), and a sitter's
  right stick turns them, not the screen's wheel. A press that began there stays theirs until
  it is let go, wherever the beam goes (a Hide press, a slider dragged past its end), and a
  desktop's view on the console or its small button lands too (they press it by VRChat's own
  interact). A beam that leaves the screen lets go of its button. The games drop a seated
  click on their window while a level is played (Doom, Quake, Quake III, Half-Life): a sitter fires by the
  trigger. A sitter's keys go to their own machine whatever the den's wall shows
  (`EmuMachine.keysOwn`). A single grip's key waits 0.15 s, or is a tap when let go sooner, so
  the first of both grips (standing up) sends nothing. While the console is put away the screen answers no beam and no
  click (`EmuPointer.holoHidden`); it is still shown when the program's picture is flat.
- **A keyboard before the seat** (`EmuHolodeck.keyboard`, "Holodeck keyboard"): the den's
  display keyboard copied, 0.54 m wide, at knee height before the sitter and tipped up to
  them (`keyboardAt`, `keyboardTurn`), its caps and legends drawn by the canvas and unlit. One
  for all rooms, moved to the sitter's when they sit, fed to their own machine as raw keys
  (`Feed`), pressed by beams and by a desktop's view and click as any keyboard of the world
  (`EmuPointer.holoKeyboard`, its place in `keyboards`), and put away with the console: a key
  held as it goes comes up. A mark of its own (`HoloGuard.shader`) keeps the program's world
  off it. "ShaderEmu/Add the holodecks' keyboard to the open scene" makes it, and Build world
  runs it.
  On `rvc_cpu` with the words of a desktop's sitter (`--host 00000112,0,0,0,0,00007f00,0,0`)
  each game walks on from where it stands, and stands still with the word 0.
- **A beam shows where a hand points** (`HandBeam.shader`), in a headset only: a ribbon 3.5 mm
  wide and 3 m long from where the hand's aim starts, the way it goes, brightest at the hand
  and fading to nothing, added to the picture (no depth written, no light given). `EmuPointer`
  puts it there each frame from the same place and way `Tell` sends the machine
  (`handAt`, `handWay`), so the beam and the game's aim are one thing; it is in the room's
  space, where the hand is. It is drawn after the program's world and before the room's
  seal (queues 2503, 2504, 2505): the world's own depth cuts it at the first surface, and
  there is no dot at its end, which would need that depth read back. It is on while the
  sitter's program draws a world (not on a flat screen), and off for a hand whose own beam
  is on the console. The right hand has one in every program; both have one in a program
  named in `twoHanded` of `EmuHolodeck.cs` by its number in `linux/apps/gameid.h` (the
  physics demo, 8; ClassiCube, 3). The console's Beam key turns it off and on (`ToggleBeam`), until the visitor
  leaves the world; it is on at first. Only the sitter sees it, as only they see the world.
  A sitter without a headset has none: their aim is the middle of their view.
  "ShaderEmu/Add the holodecks' hand beams to the open scene" puts the two ribbons, the
  key and the seal's queue into a scene, without a bake; it also gives that key and Hide
  the caps of the room's panels, which Hide had not. In play mode on a desktop `debugBeam`, which nothing but a test
  sets, invents hands before the head.

## Boxes in the hands

`nxphys` (`docs/apps/physics.md`, "The controls") reads the same words and is no shooter: each
hand takes the box it is at, or the first it points at, by its trigger or grip, carries it and
throws it, the two hands at once; the sticks walk and turn the sitter, who is as large in
its room as the scale makes them. It is the first program to use the left hand's place and
buttons (word 4). Its controls in the world are the hands' (`GAME_PHYS`, 8; `ForHands` in
`EmuGamepad.cs`, "HANDS (Physics)" on the notice): the sticks go as sticks in word 5, no
trigger or grip is a key or a button of the pointer's (the left grip is not Esc there), and
the jump button is Enter. Both its hands have a beam (`twoHanded`). A hand whose own beam
is on the console or a screen has neither trigger nor grip in the words (`EmuPointer.Landed`,
in `Tell`), so pressing a console's key takes no box. The shooters do not read those two bits, but for
Half-Life's right grip. The guest's side is checked on `rvc_cpu` with the words given; the world's in the
editor with invented hands (`debugBeam`) and the number forced.
- **The number is the keyboard's window's** (`watch_focus` in `nxbar.c`): a click that lands
  beside the program's window gives the keyboard to what is there, the word goes to 0, and
  the controls are the pointer's and the beam the right hand's until a click on the window.

## The status in the room

A sitter with tracked hands has no flat screen while a world is drawn, so the game's status
bar is not seen. Quake draws it in the world instead (`SE_Hud` in `vid_shaderemu.c`, after
the view model), facing the sitter: the ammunition over the weapon in the right hand (red
at 10 and under; none for the axe), health over the left hand (red at 25 and under) with
armour above it, and the lines the game says and prints mid-screen 1.2 m ahead, a little
under the eyes. Not done: Quake III and Doom; items, keys, the face.

ClassiCube draws its whole 2D screen (the hotbar, the chat, the crosshair) on a sheet half a
metre before the camera, and no nearer than twice its near plane, 22 degrees up and down at the
game's aspect (`SheetMatrix` in `Graphics_ShaderEmu.c`, `SE_SeatSheet`): a 2D draw's modelview puts
its pixels there and its projection is the camera's, so the room draws it in space. It is less than the
game's view (70 degrees high) so as not to be cut off under the edge of a desktop sitter's (60). In a headset
the sheet stays with the body, not the head. A seated ClassiCube changes the block in hand by the
wheel (word 5) or the right stick held up or down (a step, then one every 0.3 s), also in the
inventory; the left grip opens and closes the inventory, and the right grip is Esc, the pause menu
(a headset's left grip is no Esc in ClassiCube, and the right one no Enter). A
button held as play starts or resumes (the triggers, the jump, the grip) is no press until let go.

Its menus (the inventory, the pause menu and its options) are on that sheet too, in the world,
and not flat (`SE_Flat`): only a world not loaded yet and text being typed are (chat, a text
field, a key being bound: `Menus_SE_Typing` in the patch), for while a desktop sitter plays by
words W, A, S and D are the stick. A menu stays where it opened in the
game's world, turned as it opened and carried with the camera's place (a fall takes it along), and
the camera goes on turning, by the mouse and the head or by the body: the sheet's
2D draws go before the camera the menu opened under (`menu_view`), into the world, and into this
frame's camera. Its pointer is where the right hand's beam, or a desktop sitter's view, meets the
sheet (`SheetAim`, the ray taken from this camera into that one); the right trigger (a desktop's left
button) is the left button, and a dot on the sheet is the pointer (`SheetCursor`, a hook of the 2D
pass). An aim off the sheet clicks nothing and shows no dot. The camera is core 0's
(`SE_Frame->view`): `Gfx.View` is the worker's, the held block's for part of its frame. A trigger
held as a menu opens waits for its next press, and a menu opened before the world's
first frame waits for that frame's camera. A menu picks what the pointer last moved onto: a pointer
that has not moved since the menu opened picks the block in hand. `test_all.py classicube-seat-vr-pick`:
the beam moves onto the sixth row and the trigger picks block 58 and closes the inventory.

## A flat screen in place of the world

A program says that its frames are a flat screen and no world (`seglWorldFlat()`, bit 31 of
the frame's size word at `0x8100030c`): a menu, the console, a level loading, the screen
between levels (Quake's totals and an episode's text, `cl.intermission`; Quake III's scoreboard at
a level's end, `pm_type` 5 or 6). `EmuMachine` reads it back (`flatFrame`, and `worldFrame`: there is a list
at all), and while it is so, or the sitter's program draws no world (the desktop, a 2D
game), the room shows no world: the sitter's display is on the large screen before the seat,
their beams are its pointer, and the left stick is the arrow keys (`EmuHolodeck.Flat`). The screen
takes the sitter's pointer whenever it shows, by the rule that shows it (`HoloScreenShown`: the
console out, or the program flat); a desktop sitter's is the view's middle and their left button
its button, and with the view on it neither the click nor the wheel is the game's.
On `rvc_cpu` (its closing line says the bit): Quake III's menu 1, its level 0.

What each game sends while seated, and what the room shows of it (3 November 2026). "Flat" is the large screen before
the seat; "in space" the world round it; "flat over it" the draws the world cannot place, which the HUD layer (below)
shows. Measured on `rvc_cpu` with a desktop sitter's words (`--host 00000112,0,0,0,0,00007f00,0,0`) and `--volume-log`,
summed by `tools/volume_frames.py`:

| Game | Flat (the large screen) | Its HUD, before | Its HUD, now | Transitions seen seated |
|---|---|---|---|---|
| Doom | not in a level (the title, an intermission, the finale) or a menu up (`doom_video.c`) | its 4 flat draws (status bar, weapon, messages) left out | on the HUD layer | 42 s of a level: world and HUD in every one of 1,487 frames |
| Quake | a menu or the console, not connected or not signed on, the loading plaque, a level's end (`vid_shaderemu.c`) | 5 to 7 flat draws (status bar, cross-hair, centre prints) left out | on the HUD layer | `kill` and `changelevel e1m2`: the last frame before each load is flat (the plaque on the large screen) and stays so while no frame comes (14 s and 18 s), then the world again; 0 frames of an empty world |
| Quake III | any key catcher (menu, console, chat), not active, the scoreboard at a level's end (`tr_shaderemu.c`) | up to 19 flat draws (HUD, cross-hair, messages) left out | on the HUD layer | the load (21 s flat), then 4,005 frames of world and HUD, `kill` included |
| Half-Life | not playing: not active or signed on, the menu or console, an intermission (`playing()`, `vid_shaderemu.c`) | up to 17 flat draws left out | on the HUD layer | `hldemo1`'s load 69 s flat, then 2,341 frames of world and HUD; 0 empty |
| ClassiCube | no world loaded, or a screen that takes the pointer while not seated, or typing (`SE_Flat`) | its 2D was already a sheet in space before the sitter (`SheetMatrix`): 0 flat draws | as before (nothing on the HUD layer) | the start: 7 frames flat, 1 frame of an empty world (0.1 s), then the world |
| Jedi Outcast (since 3 November 2026) | not in play: a menu, the console, a load (`SE_InPlay`, `se_world_tell` in `sys_shaderemu.cpp`); up is z, the eye 48 units | not a holodeck game: the large screen | on the HUD layer: up to 50 flat draws; its opening scene draws none | `+map demo`'s load flat (the large screen) for 39 minutes of the guest's at `rvc_cpu`'s speed, then 1,070 frames of the world, 2 single frames of an empty world |
| the 2D games | not registered: always on the large screen, menus and all | | | |

## The HUD layer

A program's frame has draws with a place in space (the world) and draws flat over its picture: a status bar, a cross-hair,
messages, the console, a weapon drawn flat. The world round the seat can only place the first; until 3 November 2026 the
rest was left out, so a seated game had no HUD at all. `GpuHolodeckHud.shader` (`GpuHolodeckHudPass.cginc`, and
`volume_flat_command` and `volume_picture` in `src/gpu.h`) draws them on a quad of their own, "Holodeck HUD", in the GPU's
pass order and blending, over everything of the room (no depth), each vertex at its place in the picture as a share of
the quad. `EmuHolodeck.PlaceHud` keeps the quad before the sitter's eyes while a world is shown: 1.1 m ahead and 0.12 m
below them (`hudDistance`, `hudDrop`), 1.0 m wide at the picture's shape (`hudWidth`); on a desktop it turns with the view,
in a headset it follows the head's turn about the upright at `hudFollow` (2.5 a second), never its tilt or roll. A menu
or a load is flat, and then the whole picture is on the large screen instead. `_HudFlip` (-1) turns it over if a
platform's clip space is upside down there. "ShaderEmu/Add the holodecks' HUD layer to the open scene" puts it into a
built scene. Compiled with fxc against Unity's includes (the same warnings as `GpuVolumePass.cginc`) and the Editor and
runtime scripts with csc; not run in Unity or a headset.

A picture of the layer without Unity (4 November 2026): `rvc_harness --cpu --host 00000112,0,0,0,0,00007f00,0,0 ... --save-state F.snap
--hud-capture F.bmp` draws, as the run ends, the last volume frame's flat draws the way the HUD layer does (`shader_vm/hud.shader`: the
same `volume_flat_command` and `gpu_vertex_sized`, `_VolumePass` 0 to 3 with the HUD's blendings in its order, no depth) over a
plain grey, and `python tools/hud_shot.py F.snap F.bmp OUT.png` crops it to the frame's picture by the volume words. Pictures of
4 November: Doom (the border, the status bar, the weapon), Quake (the status bar), Quake III (the HUD, the cross-hair, the scores),
Half-Life (health, suit, the flashlight's icon) and ClassiCube (nothing: its 2D is in space). The first of Doom showed its sky over
the whole view: the rectangle of sky behind everything (`sky()` in `doom_gl.c`) is a flat draw, so the layer put it before the
world; from the seat it is no longer drawn (the room has the sky ceilings laid as a panorama, and its own sky behind them).

Transitions by cases (4 November 2026; `test_all.py` runs `volume_frames.py` on a case's `--volume-log` when the case says
`volume=True`): `quake-seated` opens the menu (30 frames flat), kills (the level made again: 14 s of no frame, then the world) and
changes the level (one flat frame, 18 s of load, then the world with its HUD), 0 empty worlds; `halflife-seated` (an image with
Uplink) opens the menu from `HL_SCRIPT` (flat for 9 s, Escape closes it), kills, and goes to `hldemo2` (one empty frame as the old
level goes, 63 s of load, 3 flat frames, then the world; the new level's player has no suit, so no HUD); `jk2-seat` loads the demo's
level flat and walks it from the seat.

In the editor's play mode (5 November 2026, fxc2's overlay editor, a desktop sitter in holodeck 1 under ClientSim, driven by
`tools/unity_check`): Doom, Quake, Quake III and ClassiCube showed their world round the seat with the HUD layer before the sitter;
Quake's menu, `kill` and `changelevel e1m2` were flat (the large screen) until the level's GPU copy was made, then the world and
HUD; Doom's and Quake III's menus by Escape the same. Two things were wrong: a Quake III level loaded from within a frame (`map`
while one is played) drew "Awaiting gamestate" as a world (the room black, the picture on the HUD layer), as its flat word was told
only at the input poll (`SE_TellFlat` now at the frame's end too), and Doom's default view (screenblocks 9) put its border on the
HUD layer as a frame of stone before the sitter (`doom_video.c` takes the whole width while seated); both changes were written
that day without a build (the session could not reach WSL); built the same day, the doom and quake3 cases pass 12 of 12, and on
`rvc_cpu` a seated Doom has at most 3 flat draws (4 before) and Quake III's `map q3dm17` while playing is 26 frames flat (45 of
world and 33 flat draws before). A desktop sitter's click on the
console both pressed its key and reached the game (one click turned "3D: games only" and hosted ClassiCube on the LAN): the
console's plates are the desktop view's too now (`EmuPointer`, `holoAim`). Escape is ClientSim's menu key, as it is VRChat's: in
the editor every Escape also opened ClientSim's menu. Half-Life and Jedi Outcast stop at their licence windows, which that check
left unaccepted.

A second run the same day accepted them, as the owner allowed (`build/unity_checks/20261105c`). Half-Life from the seat: the
world round it, the menu by `HL_SCRIPT` flat on the large screen, Escape back to the world; `kill` stays the world (the dead view
looks up from the floor), and 20 s after `hldemo2`'s spawn the room was still black with a world told. The game does draw
there: `halflife-seated` on the same image gives 784 frames of world after `hldemo2`'s load and 0 empty, and `rvc_harness --cpu
--gpu-capture` at its end shows a lit wall; so the holodeck's drawing of that level is wrong, not the game (still to be found in
Unity: `volume_view` takes the camera from the first 24 commands, a place to look first).
Jedi Outcast's level drew as torn triangles: `volume_vertex` (`src/gpu.h`) read only compact and plain vertices, so Ghoul2's
skinned draws (`VERTEX_BONES`) were read as plain ones; it reads them now as `gpu_vertex_sized` does (2fda440a). That image's
`jk2.bin` came from a tree with the uncommitted arrays' vertices (`VERTEX_ARRAYS`), which the shaders of that run did not have
at all, so its level was wrong for that reason too; the world from the seat is still to be seen with both. Two setup lessons:
`tools/fxc2/scripts/unity-overlay.ps1` puts the plain `d3dcompiler_47.dll` in the overlay, which Unity's shader compiler cannot
load (copy `tools/fxc2/bin/unity`'s two files into the overlay's `Data\Tools` instead), and an editor started once with a
compiler that fails gives `Machine.shader` a default asset ("the importer crashed on it last time"): the machine then never
powers on until the shaders are imported again. The probe's `power` wants a few seconds' `sleep` before it.

`rvc_cpu --volume-log FILE` writes a line for each whole frame a program gives a host (its words, its draws in space, flat
and text), and `python tools/volume_frames.py FILE` prints them as runs of a kind: a flat screen, a world with or without
draws over it, or an empty world (not flat and nothing in space: the room would show nothing; over a second is an exit 1).

Jedi Outcast is played from the seat as Quake III is (`SE_SeatMove`, called from `CL_CreateCmd` by `jk2.patch`): the left stick
walks, a desktop sitter's mouse counts turn and the head's tilt is the aim's rise, a headset's right stick turns and the right
hand's rise is the aim's; the right trigger fires, the left one the other fire, the right grip uses and the jump button jumps.
`JK2_SEAT=1` says so every 20 moves: with `--host 00000112,0,0,0,0,00007f00,0,0` "seat: 2 ... move 127 0 0", and with the jump
and the left trigger added "move 127 0 127 buttons 80". It is `ByWords`' too (game 10). The hand's aim (as Quake III's beam from
the hand) is not made: the aim is the body's.

Only the registered games have a world round the seat (`EmuHolodeck.Registered`: Doom, Quake,
ClassiCube, Quake III, nxphys, Half-Life and Jedi Outcast, by the number of the program whose window has the keyboard,
`linux/apps/gameid.h`); any other program is on the large screen however it draws, as on a
desktop. Tiberian Dawn and Red Alert draw their pages by a GPU list, which is no world to stand in. The
console's 3D key, under Screen, gives every program's list a world for this visitor ("3D: every
program") and back ("3D: games only", `ToggleWorld`). "ShaderEmu/Lay out the holodecks' consoles
again in the open scene" puts it on consoles made already, without a bake.

Under the World and Screen size sliders are Volume, the machine's sound (the sound panel's own
slider, `EmuSound`, which the console's follows), and Voices, how loud the other players are to
a sitter: their voices and their avatars' sounds, scaled from VRChat's own gains (15 and 10 dB),
for this visitor alone, while seated and set back when they stand up (`Hear`). The console
is laid out again where it stands and drawn again onto the plate it has
(`ConsolesInPlace`, `RetroInPlace`, `PlateInPlace` in `ShaderEmuRetro.cs`): a plate made anew
loses its place in the lightmap (these are lit by probes).

## The console put away

Its Hide key, or Home while a world is drawn (on a flat screen Home is the program's), takes the console away for its sitter: the stand's model and the chair's
and the marks of both (one mesh each for all eight rooms, so the other rooms' go too, for this
visitor alone; the chair's baked shadow stays on the floor), the room's plate, keys and screen. A small translucent button stands where the plate
was and brings it back (`HideConsole`, `ShowConsole`, `Arrange`; a beam presses it by
`EmuPointer.holoShow`). Standing up brings it back. "ShaderEmu/Add the holodecks' console
switch to the open scene" puts both into a scene, without a bake; Build world runs it. The Hide and Beam keys and the small button have caps and lettering of
their own that the room does not light (`OwnCap`): the room's light volume would leave them black in a client.

On `rvc_cpu` (`--host W0,...,W7` gives the eight words, `--host-mouse N` a mouse going right, `--host-wheel N` the wheel turning up, `--host-from S W0,...,W7` other words from guest second S, up to four times;
`QUAKE_SEAT`, `QUAKE3_SEAT`, `DOOM_SEAT`, `CLASSICUBE_SEAT`, `HALFLIFE_SEAT=1` print the turns): a right
hand 30 degrees to the right and 10 up must give each game that aim with the body where it is, 100 counts
a second must turn each about 6.6 degrees a second, and the left stick pushed forward must walk each the
body's way, and to its right with the head turned 90 degrees to the right. the `classicube-seat-*` cases of
`test_all.py` run ClassiCube's.

## The parts

- The holodecks are a starship's, not the den's: the corridor has ribs that lean in to a beam at
  every bay, a dark band with a strip of light at the hand's height, lit strips under the
  ceiling, panels and carpet of its own (`corridor_fittings`, the `Sci...` materials), and a
  room's seat is a command chair on a pedestal with consoles at its arms' ends
  (`command_chair`).
- `world/annex.py`: the corridor, the rooms' arches, seats, consoles and door
  plates; it writes `Models/holodeck.json` (each room's frame, the seat, the console's foot,
  its keys' and its screen's places and turn), which `Annex()` in `ShaderEmuHolodeck.cs` reads. The floors, ceilings and walls
  themselves are the shell's (`world/shell.py`, `docs/world/world.md`).
- `EmuHolodeck` (one a client): who sits where (from `OnStationEntered` by way of `EmuSeat`,
  and a synced `holo` number in each visitor's `EmuShare` for whoever joins later), what each
  room's screen shows, the labels.
- `EmuSeat`: a room's station. `EmuVoices`: the voices. `EmuGamepad`: the controllers.

## Checking it

In play mode type `glxgears &` or `quake +map e1m1 &` at the console, then from editor code
`stations[k].UseStation(Networking.LocalPlayer)` on "Holodeck" (`EmuHolodeck`): its `own` is k,
`content` and `scene` are active and at the room, the door's label is the player's name, and
`holo` in the player's `EmuShare` is k. Render a camera at `eyes[k]` with `scene` on and off;
rolled, the program's world must stay level with the room. glxgears is far off
until the World slider is moved far along.

Voices: set `log` on "Voices" and move spawned players (`ClientSimMain.SpawnRemotePlayer`) in
and out of rooms: a `[Voices]` line says how many are heard at full strength, and
`GetVoiceDistanceFar` of each is 400 or 25.

The guards: with Quake running and the sitter seated, set the scene material's `_Plane` to 0.8
(the world small, its walls nearer than the console) and render a camera at `eyes[k]` aimed at
the console: all of it, its keys and its screen must be seen.

The host's words in play mode: `EmuMachine`'s `row` (the readback) has them at texel 64 + 0x36;
on a desktop, standing, word 0 is `0x00010102` and word 2's upper half the head's height.

A sitter's hips are put 0.23 m over the floor (`SeatHeight` in `ShaderEmuHolodeck.cs`) and the program's camera 0.7 m over that.
"ShaderEmu/Make the holodecks' seats rise under their sitters in the open scene" puts a
changed height into a scene, the eye points with it.

In play mode: seated, `_UdonHoloMax` is the room's far corner and the console says "Your keys
play. End: stand up" on a desktop (`plateLabels` in `EmuHolodeck`); with the keys given
back (Insert) `RunInputEvent("_inputJump", ...)` on "Holodeck" stands the player up at the
seat's exit and empties the box.

# The corridor

Behind the den's back wall, through its door (which stands open): 10.5 m from the lift, where a
visitor arrives, to a window at the building's outer wall. `world/annex.py` models it; its
furniture is Poly Haven's (bench, boots, payphone, extinguisher, lamps, frames, mirror).
`Annex()` in `ShaderEmuHolodeck.cs` gives every room colliders, lamps, light volumes and probes,
and moves the spawn to the lift.
