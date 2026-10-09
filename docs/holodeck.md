# The holodeck

Through the den's left wall, where the volume display hung, is a room of grid 8 m a side and 4 m
high. While a 3D program runs on the machine its last frame is a world round whoever stands
there: the same command list the volume display drew (`docs/volume.md`, the words at
`0x87000300`), seen from the visitor's own eyes. The wall screen is gone; this replaces it.

## What a visitor does

- The plate by the door in the den has the controls: the program on or off (off, and whenever no
  program has a frame, the room is its grid), the scale, what stands at the control, which way
  is up, "Centre on player" and "Recall control".
- The control is a small thing that floats in the room and can be carried. The program's world
  is fixed to it: move or turn it and the world moves and turns.
- "At the control stands the": **World** (the start) is the program's own coordinates, with the
  place the player was at when "Centre on player" was last pressed brought to the control: the
  level stays where it is and the player walks about in it. **Player** keeps the player at the
  control and lets the world slide past. **Camera** is the volume display's way: the world
  turns with the player's head.
- Scale is the volume display's slider: at its largest the picture's rectangle on the program's
  near plane is 1.28 by 0.8 m, and the slider moves that plane out towards the far one.
- Up is found from the program's camera (the axis its up is nearest to and its right is square
  to: Doom and Quake are z up, ClassiCube y), or set by hand.

## How it is drawn

- `GpuHolodeck.shader` is `GpuVolume.shader`'s four passes with `HOLODECK` defined
  (`GpuVolumePass.cginc`). The frame's camera is the modelview of its first draw with a place in
  space (`volume_view` in `src/gpu.h`): a program draws its world before what moves in it, so
  that is the view alone. A vertex in camera space goes back through its rows to the world's own
  turn, and with its translation to the world's own place.
- "Centre on player" draws `HolodeckOrigin.shader` once into a one-texel float texture: where
  the camera stands in the program's world. The scene's shader takes that from every vertex.
- `HolodeckMask.shader` is on a copy of the room's six faces, a hair inside the grid: where they
  are in view, and while the frame's words name a list, it marks the stencil and empties colour
  and depth; the scene is drawn there; `VolumeSeal.shader` gives those pixels the faces' depth
  again. So the world shows from inside the room and through its door, and nowhere else. A
  visitor is drawn before all of it and is never hidden by the program's world.
- The scene is drawn twice. Where the stencil is marked (the room's faces) it has no bounds.
  Everywhere else (the doorway, the arch, the control, a visitor) it is drawn with the ordinary
  depth test and only where it is inside the room's box: a ledge of the program's that stands
  before the doorway hides the den behind it, and nothing of the program's is outside the room.
- Seen from outside the room, what of the program's world is nearer than the room is left out
  (the way from the eye to it has not entered the room's box yet): it would hang in the doorway.
- A sky that a program lays on its picture (`FRAGMENT_LAID`: Doom's) is a panorama here, four of
  its picture to a turn round the visitor.
- A program sends only what its own camera sees. Behind the player there is usually nothing.

## Checking it

In play mode start `doom &` or `quake` at the console, stand in the holodeck and render a camera
with "Holodeck content" on and off. `EmuVolume` (on "Holodeck panel") has the settings as Udon
variables: `anchor`, `up`, `on`; `Centre` and `Recall` are its events.

# The corridor

Behind the den's back wall, through its door (which stands open): 10.5 m from the lift, where a
visitor arrives, to a window at the building's outer wall. `world/annex.py` models both rooms;
its furniture is Poly Haven's (bench, boots, payphone, extinguisher, lamps, frames, mirror).
`Annex()` in `ShaderEmuHolodeck.cs` gives both rooms colliders, lamps, light volumes and probes,
and moves the spawn to the lift.
