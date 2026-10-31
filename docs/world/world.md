# The room's models

The VRChat room (`unity/ShaderEmu`) is a den of the 1990s, 9 by 11 m and 3.2 m high, 42 m over a
city on a stormy night. Its shell, the computer's wall, the controllers and the city are modelled
in Blender by scripts in `world/`; furniture, plants and the main surfaces come from Poly Haven
(CC0). Everything can be made again from the repository and a download.

## Making it

    python world\install_packages.py C:\Development\VRChat\ShaderEmu
    python world\fetch.py
    python world\textures.py
    python world\sounds.py
    & "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --python world\build.py

- `install_packages.py` puts four shader packages into the Unity project (once): LTCGI, VRC
  Light Volumes 2.1.3, Mochie's shaders and Silent's Fake Glass. The builder's code needs them to compile.
- `fetch.py` downloads Poly Haven's models and textures to `build/polyhaven` (kept out of the
  repository; 15 s the first time) and writes what Unity needs of them:
  `Textures/World/{Parquet,Plaster,Ceiling,Wood,Walnut,Velvet,Linen}.png`, a model's textures
  in `Textures/Assets`, and `Models/materials.txt`.
- `textures.py` computes the rest (plastic, metal, rugs, book spines, the atlas of small printed
  things, the city's windows and streets, the sky, clouds, rain) and `world/atlas.json` (20 s).
- **Plastic close to** (`plastic_detail` in `fetch.py`, `PlasticDetail` in `ShaderEmuModels.cs`):
  ambientCG's scan "Plastic 004" (CC0; fetched into `build/ambientcg`), a pebbled case grain with
  worn streaks, laid over every plastic's own maps as Mochie's detail maps: its normal map, and
  its roughness and colour as overlays about 0.5, which changes nothing. It is laid by where a
  place is in the room (triplanar, 20 cm a repeat), not by a model's own picture: through the
  baked shells' picture the grain was a different size on every face and broke at each seam.
  The classroom's shells take it through `PH_pc_station_m.png`, which `bake_pc.py` bakes white
  where the shell is plastic. The key caps (`Plate.shader`) have none. A computed grain was
  tried first: with true figures nothing of it showed at arm's length.
- `sounds.py` computes the rain's loop, and cuts thunder and the computer's own sounds from
  recordings (`SOURCES`: Freesound, CC0; fetched once into `build/sounds`, which needs ffmpeg)
  (`Sounds/*.wav`, 3 s).
- **A lightmap layout of its own.** Unity (and Bakery after it) lays out a model's place in the
  lightmap by cutting it wherever it bends, a chart a piece: a keyboard was a chart a key
  and more, each lit by its own few texels, and it showed as blotches and lines. An object made
  with `b.obj(name, charted=True)` brings its layout ("UV2", `lib.Builder.chart`): what is
  built inside `with b.chart(name, uv_of)` is one chart, laid out by `uv_of(place, normal)`
  in metres; anything else is six charts a fragment, seen along each axis, and a lead is
  unrolled in lengths. `finish()` puts the charts side by side at one scale. Such objects are
  exported to a file of their own (`FurnitureCharted.fbx`), because Unity lays out a whole
  file's or none of it. The classroom's "Station N rest" are so: the keyboard's tray and well are one
  chart seen from above (the tray's upright side folded out round it), each cap a chart by
  itself that is its top alone, the sides drawn in to the top's edge (texels blur into their
  neighbours: with places of their own the sides and the well darkened the tops), the mouse one chart unrolled round its section. `blender -b build/world.blend --python world/check_charts.py -- "Station 0 rest"
  build/charts.npz`, then `python world/check_charts.py build/charts.npz OUT.png`, counts the
  texels where two faces lie on one place (0.23%: slivers at keys' edges) and draws the layout.
- `build.py` writes `Models/{Shell,Room,Computer,Furniture,Annex,City}.fbx` and
  `build/world.blend` (21 s; the first time, and after a change to the classroom's leads, five
  minutes more: a place's leads fall in 14 s and a hub's in 35, and are kept in `build/drape`).
  Name parts after `--` to build only those.

Then copy `unity/ShaderEmu` over the Unity project's `Assets/ShaderEmu` and run
"ShaderEmu/Put the modelled room into the open scene" (twice the first time: the storm's Udon
program has to exist before it can be filled in), "ShaderEmu/Restyle the panels in the open
scene", then "ShaderEmu/Bake lighting". "Build world" does the first two itself. Putting the
models in again drops their lightmaps: the room is unlit until the next bake.

After a change to `world/pc.py`, `blender -b --python world/bake_pc.py` (60 s) comes before
`build.py` (`docs/world/stations.md`).

After a build, `blender -b build/world.blend --python world/check_shell.py` (3 s) must print
`SHELL ok` (below), and `blender -b build/world.blend --python world/check_overlap.py` (3 s) lists faces
that lie in one plane, face one way and overlap: those flicker. Most of what it lists is pressed
against a wall and never seen; a pair in the open is a fault.

## The shell

Floors, ceilings and walls of every room are a model of their own, `Shell.fbx`
(`world/shell.py`): the den, both corridors and the eight holodecks. It is made for the
lightmap, which showed a line wherever two pieces of a wall met and at every corner.

- **A surface is one mesh.** `sheet()` in `world/lib.py` takes a rectangle and its openings
  (a doorway, the window) and cuts it along every opening's edges from side to side, so that
  all its pieces share whole edges and are welded.
- **Its lightmap coordinates are written by the script**, not unwrapped. A room's walls are one
  strip, unrolled round the room, so its corners are inside a chart; a floor is a chart, a
  ceiling a chart, a reveal a chart beside its wall. Charts' edges are on whole texels at 60
  texels a metre, with eight texels between charts.
- Unity imports it with `generateSecondaryUV` off (the switch is a whole file's, which is why
  the shell is a file). Panelling, mouldings and beams stay in `Room.fbx` and are unwrapped.
- `check_shell.py` tests the layout: every object has second coordinates, none of them outside
  the square, no two faces overlapping there, and one scale from metres to the layout.

What it does to the baked seams has not been measured: that needs a bake before and after.

## The panels

- Nothing on a panel gives light of its own unless it is a display or a lamp
  (`ShaderEmuRetro.cs`). A canvas's faceplate, the caps of its buttons and its fields are drawn
  once into a picture, `Textures/Panels/NAME.png`, and hung on a plate with a lightmap, a child
  of the canvas, 0.6 mm behind it. What its TextMesh Pro labels print is that surface's own
  too (`Plate.shader`, `Lettering`): every glyph is a row of `NAME glyphs.asset` (its box on
  the plate, its box in `Fonts.png`, the four fonts' distance field atlases in one sheet, and
  its colour), and `NAME index.png` says which glyph a place on the plate belongs to. The
  plate's shader looks the glyph up, samples the sheet and puts the lettering into its colour
  with TextMesh Pro's own edge, a pixel wide, before the light; the ink has a smoothness of
  its own (`_InkGlossiness`), so print catches a lamp differently from its plate. The canvas keeps its rectangles for the beams and what a behaviour changes
  (every label one of its fields names): a read-out in amber or green, which glows; print
  that changes (two labels) is lit by the light volume (`LitText.shader`).
- The look is hardware of the room's decade: dark faceplates, cream lettering, caps on the
  buttons, a liquid crystal strip for a field to type in, the board as fanfold paper, the
  screens' names on label tape. The fonts are IBM Plex (`Fonts/OFL.txt`).
- The keyboards' caps are a model: `world/keyboard.json` is the builder's layout (written out
  of the scene), `keyboard()` in `world/computer.py` stands a tapered cap on every key's
  rectangle (its shoulder and top so far in: `CAP_SHOULDER`, `CAP_TOP` in `world/pc_faces.py`), their tops 1.5 mm under the canvas, and `Keyboard.png` (`textures.py`) is every
  cap's colour seen from above; the legends are the canvas's labels as the caps' lettering
  ("Restyle the panels" gives the model's renderers that material: run it after the models). A key
  added to the builder's layout needs the layout written out again, the texture and the model.
- A panel that is closed when the room is built (the browser's links) is drawn as it will
  open; its plate has no lightmap and takes the probes' light.

## How the pieces fit

- `world/lib.py` is the modelling kit: boxes with bevelled edges, lathes, swept mouldings, tubes
  (curved, for cables), prisms, flat pictures. Everything is written in Unity's coordinates
  (x right, y up, z forward, metres, as in `ShaderEmuBuilder.cs`) and turned into Blender's on
  the way in: Unity's (x, y, z) is Blender's (-x, -z, y), which the FBX export and Unity's
  import undo exactly.
- A wall has a space of its own (`on_wall` in `room.py`): x along it, y up, the room towards -z,
  the way a screen is seen. What hangs on a wall is placed in that.
- A surface's texture repeats every so many metres (`world/materials.py`), and its UVs are its
  own size in metres: a board's grain runs along its longest side (the veneers are turned a
  quarter in `fetch.py` for that).
- `b.asset(NAME, at, rot, size)` stands one of Poly Haven's models on a point, facing +z
  (`world/assets.py`), scaled to a given width or height, its material renamed `PH_...`.
  `parts=` keeps only some of a file's objects: a file may be a kit laid out for show (pens in
  a row, four trees side by side). `limit=` cuts triangles and is only for boxes: it tore
  chairs and leaves apart. Leaves get a second face turned over.
- `world/materials.py` and `Surfaces` in `ShaderEmuModels.cs` name the same materials. The FBX
  carries only names; Unity's importer maps each to the material the builder made.
- What stands on a model stands on its shape, not its bounds: `b.asset(..., on=...)` (what the
  support's own call gave back) lets it down till they touch (`rest` in `world/assets.py`, a
  ray every 4 mm). A cardboard box's top is its flaps: at its bounds' top a box floated 15 mm
  and the television 16. A game's box leans till its top meets the wall (`leaning_box`).
- Small printed things are one atlas, `Details.png`; `world/atlas.json` says where each is, and
  `quad(..., decal=NAME)` uses it. The packer stops if the atlas is full: once it did not, and
  what was packed last was cut off or drawn from outside the picture.
- Books are `Books.png`: sixteen spines, each drawn at its own size (`world/bookdesigns.py`),
  and a book is modelled at the size it was drawn, so its text is never stretched.
- The screens, keyboards, panels and lights are still the builder's own. `world/computer.py`
  repeats their rectangles at its top: a screen moved in the builder must be moved there too.
- The builder's boxes stay where they stop a player, without their renderers (`StripBoxes`):
  furniture in a model must keep the footprint of the box it replaces.
- The city (`world/city.py`) is boxes with windows from three tiling textures, roofs, lights and
  the river in a white one tinted by vertex colour, and streets from four kinds of block in one
  texture. Only faces the window can see are made. A vertex's alpha is its haze.
- The room's two clocks run (the wall's by the door, the alarm clock on the desk). `hands=` of
  `b.asset` makes each hand an object of its own, its origin the dial's middle and its mesh
  pointing at twelve (`spawn_hands` in `world/assets.py`); the builder gives it
  `ClockHand.shader`, which turns it about its own z by the time of day: the visitor's own
  clock, which `EmuWeather` tells the shaders as `_UdonClockDay`. A hand is not static and has
  no lightmap. Its second hand steps; the others sweep.

## Light

- All of the room's light is baked, and every model has a lightmap: 60 texels a metre, more
  for the shelf, the books and the rack, whose depth is their occlusion.
- "ShaderEmu/Bake lighting" uses Unity's own lightmapper on the graphics card (close VR first):
  about two minutes, where the processor was at 49% after most of half an hour. Rendering the
  lightmap in Blender's Cycles instead was tried (Unity's layout exported, 7 s to render) and
  dropped: its light did not match the room's, and it looked worse. Bakery is a paid asset and is
  not in the repository; when the project has it (`BAKERY_INCLUDED`), `BakeLighting` bakes with it
  instead (`ShaderEmuBakery.cs`: lightmaps, light probes and the light volumes in one render,
  then the reflection probes by Unity's bake into files of their own).
- "ShaderEmu/Look at the room without a bake" is for working on the room: no lightmap, the
  lamps real-time, flat light under them, and no static batches (in play mode a batched model
  was dark under real-time lamps: the walls had a fifth of their light in the editor). The
  bake puts all of that back. Never upload the room like this.
- Surfaces are on Mochie's Standard shader (`Surfaced` in `ShaderEmuModels.cs` sets the keywords
  its inspector would), with a packed map beside every albedo (`NAME_p`: red occlusion, green
  roughness, blue metal). Lamps and LEDs are Unity's Standard, emissive.
- The camera is HDR. What gives light is brighter than white: the ceiling's lit row of lamps
  (7), LEDs (5), the LEDs drawn in `Details.png` (`Details_e.png`), a lit window of the city (5
  times its texture, in `City.shader`), the screens a little (`_Glow`, 1.35). Bloom makes that
  visible; there is no tone curve beyond a neutral one, so the machine's screens keep their colours.
- The effects are a Post Processing volume on layer 23 and a layer on the scene's reference
  camera, which VRChat's own camera copies (`PostProcessing` in `ShaderEmuModels.cs`). A camera
  cloned from it in play mode throws in its first `Render`: measure with a plain HDR camera.
- The display lights the room as it changes (LTCGI: `Screens` in `ShaderEmuWeather.cs`): it is
  an area light with `DisplayPicture` on it. LTCGI's Udon adapter gives each renderer its
  screens at start, so this only shows in play mode. To check it, fill `DisplayPicture` white,
  then black, and compare a view of the desk: white is the brighter.
- A light volume the size of the room is baked with the lightmaps, for avatars and whatever
  else moves and has a shader that reads one.

## The storm

- The window's pane is Silent's Fake Glass with rain (`GlassMaterial` in `ShaderEmuWeather.cs`):
  drops run down it, and what is behind a drop is bent out of the room's baked reflection probe,
  so nothing grabs the screen. It was Mochie's Glass, which did.
- Rain is sheets of scrolling streaks at four distances (`Rain` in `world/city.py`,
  `Rain.shader`), and haze that hides the far towers (a vertex's alpha, 480 m).
- The sky is `Sky.shader`: the panorama under a layer of cloud that drifts.
- `EmuWeather` strikes lightning every 30 to 80 s: the global `_UdonWeatherFlash` lights the
  sky, the city and the rain, and thunder follows after the time its distance takes.
- Sound is two sources by the window: the rain's loop, and thunder. Check them by
  measurement, as the sound card's: `sounds.py` prints each file's level, and none may clip.

## The board's words, and the world's listing

The board's three long texts are constants (`WhatText` and `CreditsText` in
`ShaderEmuDecor.cs`, `DoText` in `ShaderEmuGamepad.cs`); the control panel's words beside its
switches are `PanelHelp` (`ShaderEmuTouch.cs`). Both say that "your machine" is a computer
emulated in a pixel shader on the visitor's own graphics card. "ShaderEmu/Write the board's words
again in the open scene" puts them into a scene that has a board, each at the largest size at
which it fits its box (it logs the size and the height), and letters the plate again: no
build, no bake.

The world's entry at VRChat is `unity/listing`: `description.txt`, and `WorldIcon.png`, which
`python tools\make_world_icon.py CAPTURE.png unity\listing\WorldIcon.png` letters onto a
capture (`WorldIconCapture.jpg` is the one it was made from). The capture is a camera made in
editor code in play mode with the machine on, rendered 2400 x 1800 with 8 samples: low at the
left of the front row's first computer, `(-2.30, 0.78, 0.55)` looking at `(-1.40, 1.19,
1.36)`, 54 degrees, after `Take0` on "Stations" twice (the second makes its tube show the
display) and `doom &` typed at the console keyboard's `queue`. "ShaderEmu/Show the world's
listing at VRChat" reads the entry, and "Update the world's picture and description at
VRChat" sends those two and nothing else (`ShaderEmuListing.cs`: the SDK's
`UpdateWorldInfo` and `UpdateWorldImage`, which need its panel logged in and no build).

## The system sheet

On the den's back wall over the television, from the corner to the posters: a sheet of fanfold
paper (2.3 x 1.54 m) in the board's own frame, which lists every program a visitor would use
with its command (from `ls` of the machine's `/bin`, `/usr/bin`, `/sbin`, `/usr/sbin` and
`/usr/local/bin` under `rvc_cpu`: the base image has MicroPython, QuickJS, nano, mc, ssh and
more besides ours), which files each opens, the NES mappers, the samples and how to start things.
Its words are `unity/ShaderEmu/Editor/ShaderEmuSheet.txt` (columns, headings, `NAME | TEXT`
lines; ASCII only), laid out by `ShaderEmuSheet.cs` in mono type at the largest size at which
every column fits (21 on 15 October 2026, 18.5 on 29 October with the terminal's programs). "ShaderEmu/Write the system sheet into the open
scene" puts it there styled as the board's paper, or, when it has its plate, letters that plate
again from the file: no build, no bake. The frame is the board's mesh ("Board" of `Computer.fbx`)
turned and stretched, not static, on copies of its materials without the light volume
(`Generated/* by probes.mat`): with it the wood was black this close to the wall. The plate and
the frame take the probes' light. After a change to `linux/apps/nxopen.conf`, a program's
`nxopen.*` or `nxapps.*` line, or a program added to the image, edit the file and run the command.

## The power warning

Where a visitor powers on, the world says that the machine used to crash the game on Windows with an AMD
card (its shader was too much for that driver's compiler), that this may now be fixed though few AMD cards
have run it, that other cards have problems at times, and which work so far. `EmuMachine.PowerWarning` is on the console while the machine is off and on the control
panel's read-out beside "Power on" (the visitor's own; the line sent to others stays "off");
`PowerWarningShort` (`ShaderEmuConsole.cs`) is a sign over the storage sign on the tower, in the power
sign's leaning plane and as wide: "ShaderEmu/Add the power warning to the open scene", no bake.

## Physical buttons

The control panel's "Physical buttons" switch (`ShaderEmuTouch.cs`; off until a player turns it
on, kept in their PlayerData as `buttons.physical`) lets a fingertip press what a beam presses,
in a headset: `EmuPointer.Touch` takes the tip of each index finger (its last bone and 0.7 of
that bone's length again) and

- presses a keyboard's key when the tip comes within 4 mm of the keys' tops (or goes into
  them, up to 5 cm), and lets it go when the tip is 12 mm clear: the keyboards a beam may
  press, the classroom's only at the visitor's own place;
- presses a button of a panel once when the tip reaches it, and again only after it has left
  it (`Button.OnSubmit`, which is what VRChat's own click does), and moves a slider to where the
  tip is across it. The panels are every canvas with a collider whose buttons or sliders a
  laser presses (`TouchPanels`: 223 buttons and 59 sliders on 93 panels since the monitors' volume sliders),
  found by `Physics.OverlapSphere` round the tip, 5 cm. "Build world" and "Put the modelled room into the open scene"
  make the list again; "ShaderEmu/Add physical buttons and the classroom's speed sliders to the
  open scene" adds the switch and the list to a built scene, with the steady pointer's switch
  under it (`docs/machine/input.md`); "ShaderEmu/Add the monitors' volume sliders and the towers'
  new speeds to the open scene" makes the list again too.
- A press shows on the keyboards as the key's colour (`EmuKeyboard.Paint`) and on a panel as the
  button's tint. A held key's cap sinks in the keyboard's model, 0.4 of its height, its feet
  staying: `EmuKeyboard.Sink` gives each hand's key as a box in `_UdonPress0`/`1` globals, and
  `Plate.shader`'s vertex stage (the caps' material) moves what lies in it. "ShaderEmu/Sink held
  keys' caps in the open scene" sets the caps' depths (`capUnder`, `capHigh`) on a built scene.

The holodecks' consoles (keys of a plate, not buttons) are not on the list. Not yet tried in a
headset; in the editor the switch and its label were.

## The pens

ureishi's QvPen 3.3.5, as the fluid world has it: fifteen pens, three erasers and their keys in
a row under the board on the den's left wall (`ShaderEmuPens.cs`, "ShaderEmu/Put the pens into
the open scene"). It is a package of its own (`world/install_packages.py`), its prefab put in
as it is, outside "ShaderEmu", so the restyle leaves its keys alone. Nothing of it is baked.

## The danger zone

On the den's right wall, between the room's switches and the rack: a panel whose keys harm
the visitor's own machine's memory (`docs/world/danger.md`). It is a canvas, styled as the others
(`RetroNewPanels`); "ShaderEmu/Add the danger zone to the open scene" puts it there without a
bake. The memory screen has a trigger collider for its laser, which stops no beam and no player.
