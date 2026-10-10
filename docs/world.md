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

- `install_packages.py` puts three shader packages into the Unity project (once): LTCGI, VRC
  Light Volumes 2.1.3 and Mochie's shaders. The builder's code needs them to compile.
- `fetch.py` downloads Poly Haven's models and textures to `build/polyhaven` (kept out of the
  repository; 15 s the first time) and writes what Unity needs of them:
  `Textures/World/{Parquet,Plaster,Ceiling,Wood,Walnut,Velvet,Linen}.png`, a model's textures
  in `Textures/Assets`, and `Models/materials.txt`.
- `textures.py` computes the rest (plastic, metal, rugs, book spines, the atlas of small printed
  things, the city's windows and streets, the sky, clouds, rain) and `world/atlas.json` (20 s).
- `sounds.py` computes the rain's loop, and cuts thunder and the computer's own sounds from
  recordings (`SOURCES`: Freesound, CC0; fetched once into `build/sounds`, which needs ffmpeg)
  (`Sounds/*.wav`, 3 s).
- `build.py` writes `Models/{Shell,Room,Computer,Furniture,Annex,City,Gamepad}.fbx` and
  `build/world.blend` (11 s). Name parts after `--` to build only those.

Then copy `unity/ShaderEmu` over the Unity project's `Assets/ShaderEmu` and run
"ShaderEmu/Put the modelled room into the open scene" (twice the first time: the storm's Udon
program has to exist before it can be filled in), "ShaderEmu/Restyle the panels in the open
scene", then "ShaderEmu/Bake lighting". "Build world" does the first two itself. Putting the
models in again drops their lightmaps: the room is unlit until the next bake.

After a change to `world/pc.py`, `blender -b --python world/bake_pc.py` (53 s) comes before
`build.py` (`docs/stations.md`).

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
  rectangle, their tops 1.5 mm under the canvas, and `Keyboard.png` (`textures.py`) is every
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
- The controllers are `world/gamepad.py`; `GamepadModel` hangs each on the object the builder
  makes for it.

## Light

- All of the room's light is baked, and every model has a lightmap: 60 texels a metre, more
  for the shelf, the books and the rack, whose depth is their occlusion.
- "ShaderEmu/Bake lighting" uses Unity's own lightmapper on the graphics card (close VR first):
  about two minutes, where the processor was at 49% after most of half an hour. Rendering the
  lightmap in Blender's Cycles instead was tried (Unity's layout exported, 7 s to render) and
  dropped: its light did not match the room's, and it looked worse. Bakery was also suggested: it is a
  paid asset and is not in the project; with it there, `BakeLighting` is the one place to change.
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

- The window's pane is Mochie's Glass with drops running down it; it grabs what is behind it.
- Rain is sheets of scrolling streaks at four distances (`Rain` in `world/city.py`,
  `Rain.shader`), and haze that hides the far towers (a vertex's alpha, 480 m).
- The sky is `Sky.shader`: the panorama under a layer of cloud that drifts.
- `EmuWeather` strikes lightning every 30 to 80 s: the global `_UdonWeatherFlash` lights the
  sky, the city and the rain, and thunder follows after the time its distance takes.
- Sound is two sources by the window: the rain's loop, and thunder. Check them by
  measurement, as the sound card's: `sounds.py` prints each file's level, and none may clip.
