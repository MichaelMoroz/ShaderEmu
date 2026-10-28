# The holodecks

Through the den's left doorway is a corridor 2 m wide and 16 m long with eight rooms of grid off
it, four a side, each 3.6 m square and 3 m high, with a seat. While a 3D program runs on the
machine of whoever sits there, its last frame is a world round the seat: the command list the
volume display drew (`docs/world/volume.md`, the words at `0x81000300`), seen from the sitter's own
eyes. Games and their controllers are here and nowhere else in the world.

## What a visitor does

- **Sit down** in a free room's seat (a plate by each door says whose the room is, or FREE).
  The seat is a station of the scene, one visitor at a time; whoever sits has the room until
  they get up. Anybody may walk in and stand there. The seat keeps its sitter whatever the
  sticks do (they are the game's: a stick used to put the sitter out of the chair). In a
  headset the sitter's controllers are the game's from the moment they sit
  (`docs/world/gamepad.md`), and both grips held for a second, or the console's Exit, stands them
  up; the console's line says what the controls are.
  The station is told not to let a stick stand its sitter up, and in a headset one did all
  the same, with a controller in hand too: so whoever is stood up without having asked is
  sat down again two frames later (`EmuHolodeck.Exited`, `SitAgain`; a respawn lets go).
- **The seat rises under its sitter** (`EmuSeat`). VRChat puts a visitor on a station standing,
  feet where the station is, and their pose sinks into sitting over about half a second,
  hips where the station is: a station at the seat's height had them stand on the seat
  first, the seat's height too high, and then sink. The station waits on the floor and comes up to
  the seat's height in that half second (`settle`), so the sitter only goes down. The half
  second is what was seen, not a number VRChat gives: `settle` is the thing to change.
- **Without a headset** the sitter's own keyboard is the game's from the moment they sit
  (the display keyboard's capture, raw keys): the game's own keys, as at a desk. End stands
  them up at once, whatever is held and whoever has the keyboard (`EmuHolodeck.StandUp`);
  Insert gives the keyboard back, and Space then stands them up too. The console's line
  says so. There is no controller there: its sticks would be the mouse and keys the
  visitor looks round with, so the game turned as they looked.
- **One mode.** The program's camera stands at the seat's eye point and the horizon is the
  room's: of the camera's turn only what is about the world's up is followed, so the sitter
  faces the way the game's camera faces, and the floor stays the floor when the game looks up
  or down. The head moves freely in that.
- **The console** beside the seat has the room's keys, the sitter's alone: Exit (stands the
  sitter up, whatever they hold: `StandUp`, as the End key), Screen (the console's small one,
  or a large one before the seat), and two sliders: World (the scale: at its largest the
  picture's rectangle on the program's near plane is 1.28 by 0.8 m, and the slider moves that
  plane out towards the far one; it starts where the game's player's eye is the sitter's: "The
  world's size"; it took the place of World + and World - on 16 October 2026) and Screen size
  (the large screen before the seat, half to twice its size, 100% in the middle). A slider is
  VRChat's own for a desktop's sitter (`WorldSlid`, `ScreenSlid`) and the beams' in a headset
  (an event ending in "Slide" is sent again while the trigger is held, with where the beam is
  across it: `EmuPointer.holoSlide`). "ShaderEmu/Put the holodecks' size sliders into the open
  scene" makes the consoles again with them (new plates, lit by the probes until a bake).
  There is no key for the controls: they are the game's (`docs/world/gamepad.md`).
- **A sitter in a headset presses those keys with their own beams** (`EmuPointer`, `holoPlates`:
  a plate a hair before the console's face, the keys' rectangles, a mark on the key a beam is
  on), which are out for nothing else while they sit; the trigger of a hand whose beam is on
  the console is the console's, not the game's. VRChat's laser did not press them from the
  seat (12 October 2026; why is not known: the seat's own Interact box is round the sitter's
  hands), so the console's own collider is off for that sitter (`plateColliders`), or a key
  that the laser did reach would be pressed twice. Anybody else, and a sitter without a
  headset, presses them with VRChat's laser as any panel.
- **The seat's Interact box is off for its own sitter** (`EmuSeat`, `reach`), and for a second
  after they stand up. It is round the sitter's hands, so a trigger pulled there used the seat:
  in a headset the console's Exit stood the sitter up and the same pull sat them down again
  (13 October 2026). The fix is not tried in a headset yet.
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
far 8192), Quake 46 (4, 4096), Quake III 50 (4, 16384); for a 1.6 m eye the planes are 0.185,
0.220 and 0.172, where 0.35 had put the eye 0.45, 0.65 and 0.36 m high. ClassiCube says its
player's eye height (1.625 blocks; near 0.1, far its view distance, 128), from its own window
code (`Window_ShaderEmu.c`, written again only when the eye, the view distance or the
projection changes): 0.246 for a 1.6 m eye. On `rvc_cpu` the four words read back as
`121903f2`, `224e049b`, `71ec03a6`, `323c04fb` (`DOOM_SEAT`, `QUAKE_SEAT`, `QUAKE3_SEAT`,
`CLASSICUBE_SEAT` print it as `eye`). Half-Life says 64 units (near 4, far its level's size):
`92230496` (`HALFLIFE_SEAT`).

ClassiCube says the player's own eye (1.625 blocks), so a block is about a metre (0.98 m for an
eye of 1.6 m) and the sitter is the player's size. In a headset the block in the right hand is
4 cm in the room whatever the world's size (`SE_SeatHeld`, the held model's scale; it was a
sixteenth-size world, 0.101 m blocks, until 17 October 2026, which made the sitter a giant).
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
  turn about up), as Doom lays it on its flat picture. Until 15 October 2026 it was fixed in the
  room: it turned with the game's view, and with a desktop sitter's head.
- A program sends only what its own camera sees. Behind the seat there is usually nothing.
- A program's textures, light maps and vertices may be files in the ROM (Quake III Arena's all
  are, `docs/games/quake3.md`): the pass reads the ROM's four textures as the GPU's does, and the
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
the next room's seat and console, behind a wall the mask had emptied the depth of, showed
as their shapes cut out of this room's world.

Tried before this and taken out again (10 October 2026): a large screen before the seat that
went clear while a program drew in space, with only the program's flat drawing on it (status
bar, weapon, menus; `GpuOverlay.shader`). It stood in the program's world: far off in it, over
the weapon, or under the world's nearer walls, and whichever way its depth was settled it read
wrong. A frame's flat commands can be told from its draws in space (a projection whose w does
not come from z; a sky is a flat draw at the far plane; Quake tints its view with two fills
the view's size), and the frame's size is the fourth of its words (`0x8100030c`), which the
libraries still write.

## What the machine is told of the room

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
| 5 | in a headset the sitter's sticks, a signed byte each, 127 pushed home: the left one to the right, forward, the right one to the right, up (zero while a menu has them as keys). Without a headset the left one is the keys walked by, and bits 16-23 count the notches the wheel has turned up while a game is played (`SEGL_HOST_WHEEL`, wrapping). Before 13 October 2026: where the left hand points |
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
  scale (`PerspectiveCamera_GetPickedBlock`): from the eye along the hand's way it was a ray
  beside the beam, which went past the block the beam was on (15 October 2026).
- **ClassiCube's chunks are the head's** while seated with tracked hands: its frustum is 120
  degrees from where the head looks (`SE_SeatCull` in `Game.c`), where the body's camera's
  left out what the head turned to. And since a holodeck shows no clear colour (the far
  background was black), it draws a box round the eye twice its far plane away, its sky's
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
  buttons. Before 16 October 2026 a headset's triggers were the pointer's buttons at the
  screen's middle (`EmuGamepad.Point`), both by the right hand's pick, and a desktop's sitter
  could neither break nor place: ClassiCube read no trigger, and nothing sent their buttons.
- **Without a headset the mouse turns the game** (word 4's counter, 0.066 degrees a count)
  and the head's tilt is the aim's rise. The mouse turns the sitter's head too, so the
  program's world goes round with their head (`EmuHolodeck.PostLateUpdate`): the game turned
  twice as far as they looked. ClassiCube breaks at the view's middle by the left button or
  Ctrl (the right trigger) and places there by the right button (the left trigger).
- **Without a headset the keys played by are told in the words too**, the same in Doom,
  Quake, Quake III and ClassiCube whatever each game's own keys are: W, A, S, D or the arrows
  are the left stick (word 5: they walk and step aside), the left button or Ctrl is the right
  hand's trigger (fire), the right button the left hand's (use in Doom, place in ClassiCube) and
  Space the jump button (word 0's bit 6: jump; use in Doom), E the right hand's grip (word 6's
  bit 31: use in Half-Life, as a headset's right grip is; E stays a key too). While the sitter's
  world is one of those games' (`EmuHolodeck.ByWords`, `played`: the program's number, kept
  while another window has the guest's keyboard) these keys are no key events
  (`EmuKeyboard.playAway`): before 14 October 2026 they were the window's keys, and a click
  that landed beside the game's window, the same click that fired, left the game with none.
  The wheel is counted in word 5 (ClassiCube: the block in hand). Every other key (the numbers, B, Esc, Enter) is still the window's. In a menu or with the
  keyboard given back (Insert) all keys are as they were. In a headset the right trigger fires
  by the same bit, beside the key its controller sends, and VRChat's jump button jumps by bit 6
  (Enter only in a menu). No trigger jumps: until 15 October 2026 the left one was the jump
  (and in ClassiCube it built too), and the developer saw triggers jump in every game. Doom's
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
  the first of both grips (standing up) sends nothing. Before this the screen answered a headset's
  beams only while the program showed a flat screen, and a desktop's mouse not at all while
  it played by the words. While the console is put away the screen answers no beam and no
  click (`EmuPointer.holoHidden`); it is still shown when the program's picture is flat.
- **A keyboard before the seat** (`EmuHolodeck.keyboard`, "Holodeck keyboard"): the den's
  display keyboard copied, 0.54 m wide, at knee height before the sitter and tipped up to
  them (`keyboardAt`, `keyboardTurn`), its caps and legends drawn by the canvas and unlit. One
  for all rooms, moved to the sitter's when they sit, fed to their own machine as raw keys
  (`Feed`), pressed by beams and by a desktop's view and click as any keyboard of the world
  (`EmuPointer.holoKeyboard`, its place in `keyboards`), and put away with the console: a key
  held as it goes comes up. A mark of its own (`HoloGuard.shader`) keeps the program's world
  off it. "ShaderEmu/Add the holodecks' keyboard to the open scene" makes it, and Build world
  runs it. Seen in play mode
  on a desktop (14 October 2026): shown before the seat, a key under the view found and lit,
  gone with the console and the screen then answering nothing; not pressed there, and not in
  a headset.
  On `rvc_cpu` with the words of a desktop's sitter (`--host 00000112,0,0,0,0,00007f00,0,0`)
  each of the four walked on from where it stood, and stood still with the word 0; fire and
  jump were not seen there, and none of it was played by hand.
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
  physics demo, 8). The console's Beam key turns it off and on (`ToggleBeam`), until the visitor
  leaves the world; it is on at first. Only the sitter sees it, as only they see the world.
  A sitter without a headset has none: their aim is the middle of their view.
  "ShaderEmu/Add the holodecks' hand beams to the open scene" puts the two ribbons, the
  key and the seal's queue into a scene, without a bake; it also gives that key and Hide
  the caps of the room's panels, which Hide had not. In play mode on a desktop (13 October
  2026, hands invented before the head by `debugBeam`, which nothing but a test sets): on
  with Quake's e1m1 round the seat and at the invented hand to the last digit, off while
  the level loaded, off by the key and on again, off standing; a beam aimed at the floor
  ended at Quake's floor 0.9 m off, where one drawn after the seal went on to the room's
  at 1.5 m; the machine ran 1.88 million instructions a second with it and without. Not in
  a headset: none of it.

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
in `Tell`): a console's key pressed took a box. The four games do not read those two bits.
The guest's side was checked on `rvc_cpu` with the words given; the world's in the editor
with invented hands and the number forced (the words read back: sticks in word 5, the bits
gone under a beam on the console); none of it in a headset, and not the two together.
- **The number is the keyboard's window's** (`watch_focus` in `nxbar.c`): a click that lands
  beside the program's window gives the keyboard to what is there, the word goes to 0, and
  the controls are the pointer's and the beam the right hand's until a click on the window.
  On `rvc_cpu`, Quake started from the console on the desktop: 2 after 76 s with the pointer
  still, moving beside the window, and clicked on the window; 0 after one click beside it.

## The status in the room

A sitter with tracked hands has no flat screen while a world is drawn, so the game's status
bar is not seen. Quake draws it in the world instead (`SE_Hud` in `vid_shaderemu.c`, after
the view model), facing the sitter: the ammunition over the weapon in the right hand (red
at 10 and under; none for the axe), health over the left hand (red at 25 and under) with
armour above it, and the lines the game says and prints mid-screen 1.2 m ahead, a little
under the eyes. In a held frame in the harness each stood where it should on the picture
(13 October 2026). Not done: Quake III and Doom; items, keys, the face.

ClassiCube draws its whole 2D screen (the hotbar, the chat, the crosshair) on a sheet half a
metre before the camera, and no nearer than twice its near plane, 22 degrees up and down at the
game's aspect (`SheetMatrix` in `Graphics_ShaderEmu.c`, `SE_SeatSheet`): a 2D draw's modelview puts
its pixels there and its projection is the camera's, so the room draws it in space. Filling the
game's view (70 degrees high) it was cut off under the edge of a desktop sitter's (60). In a headset
the sheet stays with the body, not the head. A seated ClassiCube changes the block in hand by the
wheel (word 5) or the right stick held up or down (a step, then one every 0.3 s), also in the
inventory; the left grip opens and closes the inventory, and the right grip is Esc, the pause menu
(a headset's left grip is no Esc in ClassiCube, and the right one no Enter: it opened chat). A
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
that has not moved since the menu opened picks the block in hand. On `rvc_cpu` (16 October 2026):
the beam moved onto the sixth row and the trigger picked block 58 and closed the inventory
(`classicube-seat-vr-pick`), and with the body turning right under an open inventory the pointer of
a still beam went right across it to its edge.

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
its button, and with the view on it neither the click nor the wheel is the game's. Until 16 October
2026 a desktop sitter aimed at nothing, and a sitter whose console was put away could not reach
the screen a flat program brought back.
On `rvc_cpu` (its closing line says the bit): Quake III's menu 1, its level 0.

Only the registered games have a world round the seat (`EmuHolodeck.Registered`: Doom, Quake,
ClassiCube, Quake III, nxphys and Half-Life, by the number of the program whose window has the keyboard,
`linux/apps/gameid.h`); any other program is on the large screen however it draws, as on a
desktop. Tiberian Dawn and Red Alert draw their pages by a GPU list, which the room took for a
world: the screen went with the console, and there was no playing them with the console away. The
console's 3D key, under Screen, gives every program's list a world for this visitor ("3D: every
program") and back ("3D: games only", `ToggleWorld`). "ShaderEmu/Add the holodecks' 3D key to the
open scene" (now "Lay out the holodecks' consoles again in the open scene") puts it on consoles made
already, Screen half as tall, without a bake.

Under the World and Screen size sliders are Volume, the machine's sound (the sound panel's own
slider, `EmuSound`, which the console's follows), and Voices, how loud the other players are to
a sitter: their voices and their avatars' sounds, scaled from VRChat's own gains (15 and 10 dB),
for this visitor alone, while seated and set back when they stand up (`Hear`). The console
is laid out again where it stands and drawn again onto the plate it has
(`ConsolesInPlace`, `RetroInPlace`, `PlateInPlace` in `ShaderEmuRetro.cs`): a plate made anew
loses its place in the lightmap, and these had none since the size sliders (lit by probes).

## The console put away

Its Hide key, or Home while a world is drawn (on a flat screen Home is the program's), takes the console away for its sitter: the stand's model and the chair's
and the marks of both (one mesh each for all eight rooms, so the other rooms' go too, for this
visitor alone; the chair's baked shadow stays on the floor), the room's plate, keys and screen. A small translucent button stands where the plate
was and brings it back (`HideConsole`, `ShowConsole`, `Arrange`; a beam presses it by
`EmuPointer.holoShow`). Standing up brings it back. "ShaderEmu/Add the holodecks' console
switch to the open scene" puts both into a scene, without a bake; Build world runs it (until
18 October 2026 it did not, and a rebuilt world's Hide took only the screen away). In play mode on a desktop
(13 October 2026): seated, hidden, shown, stood up and seated again, each thing was on or
off as it should be. The Hide and Beam keys and the small button have caps and lettering of
their own that the room does not light (`OwnCap`): lit by the room's light volume the two
keys were black in the client. Seen in play mode from the seat's eye point on 14 October
2026, shown and hidden; a beam's press of the button is unseen.

On `rvc_cpu` (`--host W0,...,W7` gives the eight words, `--host-mouse N` a mouse going right, `--host-wheel N` the wheel turning up, `--host-from S W0,...,W7` other words from guest second S, up to four times;
`QUAKE_SEAT`, `QUAKE3_SEAT`, `DOOM_SEAT`, `CLASSICUBE_SEAT=1` print the turns): a right hand
30 degrees to the right and 10 up gave each game that aim with the body where it was, and
100 counts a second turned each about 6.6 degrees a second; the left stick pushed forward
walked each the body's way, and to its right with the head turned 90 degrees to the right. Where the weapon is drawn was
read as numbers in Quake only; no picture of any of it was made, and none of it has been in
a headset.

## The parts

- The holodecks are a starship's, not the den's: the corridor has ribs that lean in to a beam at
  every bay, a dark band with a strip of light at the hand's height, lit strips under the
  ceiling, panels and carpet of its own (`corridor_fittings`, the `Sci...` materials), and a
  room's seat is a command chair on a pedestal with consoles at its arms' ends
  (`command_chair`).
- `world/annex.py`: the corridor, the rooms' arches, seats, consoles and door
  plates; it writes `Models/holodeck.json` (each room's frame, the seat, the console's foot,
  its keys' and its screen's places and turn, where the controllers hang), which `Annex()` in `ShaderEmuHolodeck.cs` reads. The floors, ceilings and walls
  themselves are the shell's (`world/shell.py`, `docs/world/world.md`).
- `EmuHolodeck` (one a client): who sits where (from `OnStationEntered` by way of `EmuSeat`,
  and a synced `holo` number in each visitor's `EmuShare` for whoever joins later), what each
  room's screen shows, the labels, the controllers on the stands and in hands.
- `EmuSeat`: a room's station. `EmuVoices`: the voices. `EmuGamepad`: the controllers.

## Checking it

In play mode type `glxgears &` or `quake +map e1m1 &` at the console, then from editor code
`stations[k].UseStation(Networking.LocalPlayer)` on "Holodeck" (`EmuHolodeck`): its `own` is k,
`content` and `scene` are active and at the room, the door's label is the player's name, and
`holo` in the player's `EmuShare` is k. Render a camera at `eyes[k]` with `scene` on and off;
rolled, the program's world must stay level with the room. glxgears is far off
until the World slider is near its right end (`_Plane` near 0.84).

Voices: set `log` on "Voices" and move spawned players (`ClientSimMain.SpawnRemotePlayer`) in
and out of rooms: a `[Voices]` line says how many are heard at full strength, and
`GetVoiceDistanceFar` of each is 400 or 25.

The guards: with Quake running and the sitter seated, set the scene material's `_Plane` to 0.8
(the world small, its walls nearer than the console) and render a camera at `eyes[k]` aimed at
the console: all of it, its keys and its screen must be seen.

The host's words in play mode: `EmuMachine`'s `row` (the readback) has them at texel 64 + 0x36;
on a desktop, standing, word 0 is `0x00010102` and word 2's upper half the head's height.

A sitter's hips are put 0.23 m over the floor (`SeatHeight` in `ShaderEmuHolodeck.cs`; 0.50
and then 0.38 sat them too high in a headset) and the program's camera 0.7 m over that.
"ShaderEmu/Make the holodecks' seats rise under their sitters in the open scene" puts a
changed height into a scene, the eye points with it.

In play mode: seated, `_UdonHoloMax` is the room's far corner and the console says "Your keys
play. End: stand up" on a desktop (`plateLabels` in `EmuHolodeck`); with the keys given
back (Insert) `RunInputEvent("_inputJump", ...)` on "Holodeck" stands the player up at the
seat's exit and empties the box. In a headset the lock, the jump, the
guards, the said up and the seat's height are untried.

# The corridor

Behind the den's back wall, through its door (which stands open): 10.5 m from the lift, where a
visitor arrives, to a window at the building's outer wall. `world/annex.py` models it; its
furniture is Poly Haven's (bench, boots, payphone, extinguisher, lamps, frames, mirror).
`Annex()` in `ShaderEmuHolodeck.cs` gives every room colliders, lamps, light volumes and probes,
and moves the spawn to the lift.
