# The holodecks

Through the den's left doorway is a corridor 2 m wide and 16 m long with eight rooms of grid off
it, four a side, each 3.6 m square and 3 m high, with a seat. While a 3D program runs on the
machine of whoever sits there, its last frame is a world round the seat: the command list the
volume display drew (`docs/volume.md`, the words at `0x87000300`), seen from the sitter's own
eyes. Games and their controllers are here and nowhere else in the world.

## What a visitor does

- **Sit down** in a free room's seat (a plate by each door says whose the room is, or FREE).
  The seat is a station of the scene, one visitor at a time; whoever sits has the room until
  they get up. Anybody may walk in and stand there.
- **One mode.** The program's camera stands at the seat's eye point and the horizon is the
  room's: of the camera's turn only what is about the world's up is followed, so the sitter
  faces the way the game's camera faces, and the floor stays the floor when the game looks up
  or down. The head moves freely in that.
- **The console** beside the seat has the room's keys, the sitter's alone: Shooter, Strategy and
  Put back (the controllers, `docs/gamepad.md`), Screen (on, off),
  World + and World - (the scale: at its largest the picture's rectangle on the program's near
  plane is 1.28 by 0.8 m, and the keys move that plane out towards the far one).
- **The console** at the seat's right hand is turned to the sitter and has one sloped face: the
  room's keys below, and above them, in the same plane, the room's one screen (0.40 by 0.30 m).
  It shows the sitter's display, whole: to the sitter their own machine's, which answers
  their beams as the den's wall does; to anybody else the sitter's display from the stream
  (`docs/share.md`: surfaces 9 to 16 of `EmuStreams`, the syncing the classroom's tubes use).
  Only the sitter's own client draws the program's world; a visitor standing in another's
  room sees a room of grid and the console.
- The seat, the console, its keys and its screen are before the program's world however near
  that world reaches, and behind everything else of the room and its visitors as any thing is.

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
- Up is found from the program's camera (the axis its up is nearest to and its right is square
  to: Doom and Quake are z up, ClassiCube y).
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
- A sky that a program lays on its picture (`FRAGMENT_LAID`: Doom's) is a panorama here, four of
  its picture to a turn round the visitor.
- A program sends only what its own camera sees. Behind the seat there is usually nothing.

## The room's own things before the program's world

The program's world is drawn inside the room with the depth test, so a wall of it that came
nearer than the console covered the console. `HoloGuard.shader` is on a copy of the seats'
and consoles' models, of each screen's glass and of each console's panel ("Holodeck guards",
`HoloGuards` in `ShaderEmuHolodeck.cs`): it draws nothing but a mark in the stencil (87 + 128)
where the thing itself is seen, after the room's mask and before the program's world (queue
2502; the builder moves the world's and the seal's materials one later), and no pass of the
world draws on a pixel so marked. "ShaderEmu/Add the holodecks' guards to the open scene"
makes them without putting the room in again.

Tried before this and taken out again (10 October 2026): a large screen before the seat that
went clear while a program drew in space, with only the program's flat drawing on it (status
bar, weapon, menus; `GpuOverlay.shader`). It stood in the program's world: far off in it, over
the weapon, or under the world's nearer walls, and whichever way its depth was settled it read
wrong. A frame's flat commands can be told from its draws in space (a projection whose w does
not come from z; a sky is a flat draw at the far plane; Quake tints its view with two fills
the view's size), and the frame's size is the fourth of its words (`0x8700030c`), which the
libraries still write.

## What the machine is told of the room

A program cannot see that it is looked at from a seat. The host writes eight words every frame
at `0x87000360` (`HOST_STATE` in `src/gpu.h`; the control pass, from `EmuHolodeck.Tell`), and
`seglHostState()` gives a program their address (`GLES/segl.h`, with a macro for each field):

| Word | Contents |
|---|---|
| 0 | bits 0-3: the host (1 the harness, 2 the world on a desktop, 3 in a headset; 0: a host that says nothing). Bit 4: the machine's owner sits in a holodeck's seat. Bit 5: the hands are tracked. Bits 8-15: visitors in the instance. Bits 16-23: this visitor's number |
| 1 | the head's turn to the right, and from bit 16 its tilt upwards: 65,536 to a turn, signed |
| 2 | the head's place, millimetres, signed: to the right, and from bit 16 up |
| 3 | the head's place forward, and from bit 16 its roll |
| 4 | the left hand's place in units of 4 mm, ten signed bits each: right, up (from bit 10), forward (from bit 20); its trigger (bit 30) and grip (bit 31) |
| 5 | where the left hand points: its turn to the right, and from bit 16 its rise, as the head's |
| 6, 7 | the right hand, the same |

Places and turns are against the seat's eye point while seated, and against the visitor's own
feet otherwise. Quake reads them (`vid_shaderemu.c`): while its owner sits in a holodeck it
leaves out its test of what the camera sees, so there is world behind the seat too; and when
the hands are tracked the aim's rise is the right hand's (the room keeps the horizon level, so
the stick's up and down showed nothing). Its turn stays the stick's: the room turns with the
game, and a hand's turn would chase itself. The head is not aimed with.

Checked in the harness (`--holodeck`, and `--holodeck-aim DEGREES` for a tracked right hand
that points up by so much) with a held frame: the view's forward in the frame's modelview
rises 25.0 degrees for 25 and falls 15.0 for -15, and with no hands it is level and the
stick's. In a headset it is untried.

## The parts

- The holodecks are a starship's, not the den's: the corridor has ribs that lean in to a beam at
  every bay, a dark band with a strip of light at the hand's height, lit strips under the
  ceiling, panels and carpet of its own (`corridor_fittings`, the `Sci...` materials), and a
  room's seat is a command chair on a pedestal with consoles at its arms' ends
  (`command_chair`).
- `world/annex.py`: the corridor, the rooms' arches, seats, consoles and door
  plates; it writes `Models/holodeck.json` (each room's frame, the seat, the console's foot,
  its keys' and its screen's places and turn, where the controllers hang), which `Annex()` in `ShaderEmuHolodeck.cs` reads. The floors, ceilings and walls
  themselves are the shell's (`world/shell.py`, `docs/world.md`).
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
until World + has been pressed some ten times (`_Plane` near 0.84).

Voices: set `log` on "Voices" and move spawned players (`ClientSimMain.SpawnRemotePlayer`) in
and out of rooms: a `[Voices]` line says how many are heard at full strength, and
`GetVoiceDistanceFar` of each is 400 or 25.

The guards: with Quake running and the sitter seated, set the scene material's `_Plane` to 0.8
(the world small, its walls nearer than the console) and render a camera at `eyes[k]` aimed at
the console: all of it, its keys and its screen must be seen.

The host's words in play mode: `EmuMachine`'s `row` (the readback) has them at texel 64 + 0x36;
on a desktop, standing, word 0 is `0x00010102` and word 2's upper half the head's height.

Not known yet: none of it has been in a headset.

# The corridor

Behind the den's back wall, through its door (which stands open): 10.5 m from the lift, where a
visitor arrives, to a window at the building's outer wall. `world/annex.py` models it; its
furniture is Poly Haven's (bench, boots, payphone, extinguisher, lamps, frames, mirror).
`Annex()` in `ShaderEmuHolodeck.cs` gives every room colliders, lamps, light volumes and probes,
and moves the spawn to the lift.
