# The room's models

The VRChat room (`unity/ShaderEmu`) is a den of the 1990s, 9 by 11 m and 3.2 m high, 42 m over a
city on a stormy night. Its shell, the computer's wall, the controllers and the city are modelled
in Blender by scripts in `world/`; furniture, plants and the main surfaces come from Poly Haven
(CC0). Everything can be made again from the repository and a download.

## Making it

    python world\install_packages.py C:\Development\VRChat\ShaderEmu
    python world\fetch.py
    python world\gameboxes.py
    python world\textures.py
    python world\sounds.py
    & "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --python world\build.py

- `install_packages.py` puts four shader packages into the Unity project (once): LTCGI, VRC
  Light Volumes 2.1.3, Mochie's shaders and Silent's Fake Glass. The builder's code needs them to compile.
- `fetch.py` downloads Poly Haven's models and textures to `build/polyhaven` (kept out of the
  repository) and writes what Unity needs of them:
  `Textures/World/{Parquet,Plaster,Ceiling,Wood,Walnut,Velvet,Linen}.png`, a model's textures
  in `Textures/Assets`, and `Models/materials.txt`.
- `gameboxes.py` fetches scans of the decade's games, software and magazines into `build/gameboxes`
  and writes `Textures/Fetched/{GameBoxes,GameBoxes2,Magazines}.png` (and their `_p`), which are
  not in the repository (`.gitignore`): the pictures are their publishers', and the sites only
  show them. Boxes come from bigboxcollection.com (six faces and gloss maps a box, its size from
  its page) and bigboxdb.com (a box's model; the software: MS-DOS 6, Windows 3.1 and 95, Netscape
  Navigator, Flight Simulator 5, the Entertainment Pack), the Command & Conquer box from
  gamebox.museum; magazines are archive.org's scans of Computer Gaming World, BYTE, PC Computing
  and Linux Journal (the cover, the last page that is not the cover again, a drawn spine and
  page edges). `world/gameboxes.json` (each one's size, atlas and faces) is in the repository.
- `textures.py` computes the rest (plastic, metal, rugs, book spines, the atlas of small printed
  things, the city's windows and streets, the sky, clouds, rain) and `world/atlas.json`.
- **Plastic close to** (`plastic_detail` in `fetch.py`, `PlasticDetail` in `ShaderEmuModels.cs`):
  ambientCG's scan "Plastic 004" (CC0; fetched into `build/ambientcg`), a pebbled case grain with
  worn streaks, laid over every plastic's own maps as Mochie's detail maps: its normal map, and
  its roughness and colour as overlays about 0.5, which changes nothing. It is laid by where a
  place is in the room (triplanar, 20 cm a repeat), not by a model's own picture: through the
  baked shells' picture the grain would be a different size on every face and break at each seam.
  The classroom's shells take it through `PH_pc_station_m.png`, which `bake_pc.py` bakes white
  where the shell is plastic. The key caps (`Plate.shader`) have none.
- `sounds.py` computes the rain's loop, and cuts thunder and the computer's own sounds from
  recordings (`SOURCES`: Freesound, CC0; fetched once into `build/sounds`, which needs ffmpeg)
  (`Sounds/*.wav`).
- **Every model's lightmap layout is Blender's** (5 November 2026, `world/unwrap.py`, called by `build.py`
  for every part but the shell and the city; Unity's importer unwraps nothing, `ImportModel(..., false)`):
  cut at edges sharper than 50 degrees and wherever the picture's coordinates are cut (a lathe's bands
  and meridian, a tube's line, a downloaded model's islands), unwrapped by angle, a piece that comes out
  folded or stretched (`FOLD`) laid again by Smart UV Project, one density, packed with a margin of 2.5
  texels at the mesh's least lightmap scale (`lightmap_scale`, as `ShaderEmuModels.cs`'s; halved until
  it fits, the build names those under it), cached by the mesh's shape in `build/unwrap`. Unity's
  charts of a mug were slivers. What follows (`b.chart`, `charted=True`) is written first and laid
  over by it; the files keep their names.
- **A model made many times is one mesh** (6 November 2026, `b.instance(model, make)` in `world/lib.py`): made
  once at the origin and linked into each copy, as Poly Haven's models are, so a part's FBX has it once and
  Unity one mesh with a renderer for each copy, each its own place in the lightmap (`lightmapScaleOffset`).
  The office chairs are: 12 in the classroom and 3 in the side rooms were 15 x 14,916 triangles, now one mesh a
  part. Its weighted normals are put in the mesh (a modifier on each copy would make the copies apart in the
  FBX); `make` draws no chances. The five parts no longer build in one run under a minute (the unwrap's
  cache misses a dozen meshes at random, the tree before this too): `-- Room Computer Shell Furniture`, then
  `WORLD_ADD=1 ... -- Annex`, which builds into the `world.blend` of before.
- **A lightmap layout of its own.** Unity (and Bakery after it) lays out a model's place in the
  lightmap by cutting it wherever it bends, a chart a piece: a keyboard is a chart a key
  and more, each lit by its own few texels, and it shows as blotches and lines. An object made
  with `b.obj(name, charted=True)` brings its layout ("UV2", `lib.Builder.chart`): what is
  built inside `with b.chart(name, uv_of)` is one chart, laid out by `uv_of(place, normal)`
  in metres; anything else is six charts a fragment, seen along each axis, and a lead is
  unrolled in lengths. `finish()` puts the charts side by side at one scale. Such objects are
  exported to a file of their own (`FurnitureCharted.fbx`), because Unity lays out a whole
  file's or none of it. The classroom's "Station N rest" are so: the keyboard's tray and well are one
  chart seen from above (the tray's upright side folded out round it), each cap a chart by
  itself that is its top alone, the sides drawn in to the top's edge (texels blur into their
  neighbours: with places of their own the sides and the well would darken the tops), the mouse one chart unrolled round its section. `blender -b build/world.blend --python world/check_charts.py -- "Station 0 rest"
  build/charts.npz`, then `python world/check_charts.py build/charts.npz OUT.png`, counts the
  texels where two faces lie on one place and draws the layout.
- `build.py` writes `Models/{Shell,Room,Computer,Furniture,Annex,City}.fbx` and
  `build/world.blend` (the first time, and after a change to the classroom's leads, minutes more:
  the leads' fall is kept in `build/drape`).
  Name parts after `--` to build only those.

Then copy `unity/ShaderEmu` over the Unity project's `Assets/ShaderEmu` and run
"ShaderEmu/Put the modelled room into the open scene" (twice the first time: the storm's Udon
program has to exist before it can be filled in), "ShaderEmu/Restyle the panels in the open
scene", then "ShaderEmu/Bake lighting". "Build world" does the first two itself. Putting the
models in again drops their lightmaps: the room is unlit until the next bake.

After a change to `world/pc.py`, `blender -b --python world/bake_pc.py` comes before
`build.py` (`docs/world/stations.md`).

After a build, `blender -b build/world.blend --python world/check_shell.py` must print
`SHELL ok` (below), and the overlap check must say `OVERLAP 0 faults` (a whole build runs it at its
end; alone: `blender -b build/world.blend --python world/check_overlap.py -- [--list N] [WORD ...]`,
which exits 1 when anything is left).

## Faces that fight, parts that cross

`world/check_overlap.py` takes every triangle of the built world (1.29 million on 4 November 2026)
and the parts they belong to: a piece of one of our meshes that hangs together (a box, a lathe), or
the whole of one of Poly Haven's models. Between two parts it finds

- **FIGHT**: two triangles within 0.2 mm of one plane, facing one way, overlapping by more than
  0.5 mm inside it: they flicker. A pair whose faces both lie against a third part's face that looks
  the other way (two feet on the floor, two backs on a wall) is out of sight and left out. A fight is
  always a fault: there are none.
- **CROSS**: two triangles through each other, the line where they cross longer than 1 mm and each
  with corners more than 0.2 mm on both sides of the other's plane. Touching is not crossing: a box
  on a table, two bars end to end. A crossing is a seam, not a flicker; the joins that are hidden
  inside a made thing (a chair's spokes in its hub, leads in their plugs, the city's towers through
  their roofs) are listed with their reasons in `world/overlap_allowed.txt`, and anything else is a fault.

It prints each fault with both objects, their parts' middles and sizes and the place where they
meet, in Unity's coordinates. The pairs come from a grid in levels: a triangle goes into the level
whose cells are as large as its box's middle side, into every cell of that level its box meets (a
long thin bar into a row of at most 9, not into a cell as long as it), and asks the coarser levels'
cells; a cell with one part alone is dropped before any pair is made. Sorting the cells' keys is
the N log N; the tests are numpy's over all pairs at once: about 15 s for the world, 2 million
pairs of boxes. On the world as it was before it (4 November 2026) it says 6,152 faults, 3,206
fights and 2,946 crossings not allowed: the holodecks' grid (480 fights, 2,528 crossings: the edge bars were centred on the edges, so the
three at a corner and every line's end went into each other in one plane), the doors' rails
through their stiles, the panelling's runs crossing in the corners, the beams through the cove,
the city's parapets and roof clutter, the screen wall's ledge and shelf through its uprights, the
lift's frame, the bookshelf's back and shelves through its sides, the alarm clock's hands in the
planes of its hubs, the low table's magazines over the game beside them. They are remodelled: what
meets ends against the other's face (`wainscot(butt=)`, `round_room`, `less` in `annex.py`).

## Things as they would stand

Every thing in the rooms must stand as it would: in nothing, held by something, closed, and the
right way round where it belongs, and drawn as it would look: no two faces flickering in one
plane, no picture stretched. Six tests say it (5 November 2026), each on `build/world.blend`
after a build of all five parts (one build call: each starts empty), each well under a minute:

    blender -b build/world.blend --python world/check_intersect.py    # INTERSECT pairs: 0
    blender -b build/world.blend --python world/check_float.py        # FLOAT groups: 0
    blender -b build/world.blend --python world/check_closed.py       # CLOSED ... 0, ... 0
    blender -b build/world.blend --python world/check_placement.py -- --unposed   # PLACE wrong: 0, no "unposed"
    blender -b build/world.blend --python world/check_zfight.py       # ZFIGHT pairs: 0
    blender -b build/world.blend --python world/check_uv.py           # UV stretched pieces: 0

- **Things fall into place** (`world/settle.py`). What lies about (a magazine, a toy, a cushion,
  a game's box: whatever is made in `Builder.loose()` or placed with `loose=True`) is dropped by
  Blender's rigid bodies onto what is under it when its part is finished, and then let down the
  last of the bodies' margins to 0.2 mm over what holds it. The rests are kept in
  `build/settle/PART.json` by the geometry; `SETTLE_AGAIN=1` drops them. The build says
  `SETTLE starts in` for a thing that began inside another (lifted out first) and `SETTLE far`
  for one that moved more than 50 mm or turned more than 20 degrees: a place asked badly (a
  cassette player stood on its end fell over; a cushion dropped on a sofa may move that far).
- `check_intersect.py`: no piece of one object more than 6 mm in another (`KNOWN` pairs are
  meant: a plant in its pot, a lead in its plug).
- `check_float.py`: every group of touching pieces reaches the floor, a wall or a ceiling (a
  shelf's knob on its drawer, a shade on its lamp, a lid on its box): what floats is listed with
  `--near`, what it is nearest to and how far.
- `check_closed.py`: every piece is watertight (each edge between two faces), wound one way and
  turned out. A sheet (a poster's paper, a decal) is let be open; a seam's line laid on a body is
  made with `mesh(..., sheet=True)`. Poly Haven's models are their makers' (`--assets` shows them).
- `check_placement.py`: each thing has a pose, its front, its top and how it is mounted
  (`world/poses.py`'s `KINDS`: on the floor, against a wall, hung, from the ceiling, clamped to
  an edge, lying, a desk with its seat, a seat facing its desk, a doorway). Poly Haven's models
  have theirs by name in `POSES` (front +z of their own frame, or `axis` when not); ours say it
  where they are made, `b.pose(kind, size, at, rot)` with the front +z of that frame. Rays
  test it against walls, neighbours and doorways: a television's screen not to a wall, a desk's
  drawers and knees to its chair, a clamp lamp's jaw round an edge, a picture's back on its wall,
  nothing in the 0.8 m before a door's casing. A new model needs a line in `POSES` (the test
  says so); a new thing of ours a `b.pose` (`--unposed` lists the large pieces in no pose).
  `--shots` draws each wrong one from its front and from above into `build/placement/`, to
  look at when the rays cannot tell (which side of a model is its front).
- `check_zfight.py`: no two faces facing the same way within 0.3 mm of one plane (`DEPTH`: what a
  depth buffer cannot tell apart a few metres off) overlapping where an eye can see, at any slope
  and of any size. A point of an overlap is hidden when a face lies on it looking back (a foot on
  the floor), something stands within 4 mm before it, or it is inside a solid (out along its
  normal more faces are left than entered: a stile's back in its door's leaf). A pair is listed
  when what is seen of it comes to 10 mm2 (`SHOWS`); a sliver along an edge two faces share is
  rounding. `--why` gives a few overlaps of each pair, where they face and both triangles'
  extents. What it found and how such things are made instead: a decal or a label at least
  0.4 mm off its face (a rack's plates, a hub's front, a floppy's label, shutter and window, a
  cassette's label and window); boards that meet are scribed, not run into each other (the
  panelling's skirting, rail and cap: `wainscot(..., scribe=)`, the long walls' run into the
  corners; a bookcase's sides under its top; a lift's jambs under its header; a holodeck's
  edge bars butt in its corners); parts that end flush with another's face end a millimetre
  short (a door's stiles and rails, the screen wall's uprights); a mesh's closing cap is not
  laid in another face's plane (a keyboard's tray under its well).
- `check_uv.py`: the picture is laid evenly on every continuous chunk of a surface (faces of one
  material joined by an edge whose two ends have the same coordinates on both faces): each
  face's area in the room over its area in the picture within 1.5 times along a side of its
  chunk's median, no face stretched more than 1.5 times one way than the other (sheared or
  squeezed, in the picture's pixels: the same area, a weave drawn out), and no face with a
  picture of no area (a smear). Chunks cut apart by a seam may have scales of their own (a decal,
  an atlas's charts). A material with no picture (a plain colour) and a triangle narrower than
  0.2 mm (no texel) are not looked at. So: a lathe's band is unrolled flat (a cone's a piece of a
  ring round its apex, a cylinder's a rectangle), and outside a can's print onto a bare spot of
  it; `loft` lays its faces in panels, each along the axis its normal is nearest (`pc.panel_uv`;
  `ringwise=True` the way round and along, which sheared the chair's weave 40 times); a box's
  face more than 46 degrees off every axis is laid in its own plane and a twisted quad is two
  triangles; `sweep` lays each run along its own direction, a seam at each mitre; `tube` measures
  each line along its own track, starting again with a seam every 0.25 radians of turn, drops
  points nearer than half its radius and rounds a kink, and `bend=` rounds its corners in arcs
  (a pipe's elbow, a rail); a cushion's panel is flattened by springs as cloth; a decal is
  drawn at its face's own shape (the rack's units at their EIA-310 heights); and a keyboard's
  caps' sides are unrolled into two bands under the layout in `Keyboard.png`
  (`pc_faces.cap_side_uvs`; Unity's caps read their legends from the layout's rectangle,
  `Plate.shader`'s `_Canvas`). Poly Haven's models are their makers' (`--assets`).

### What use leaves on a thing

A thing's material is its kind's (`Plastic`, `Metal`, the scans), and what use has left on it
is laid over that in Unity: `world/wear.py` makes layers (a grain of a VDI 3400 grade or a rolled
finish, scratches, fingerprints, dust), each in metres of the surface with its source
or "a guess" beside it, and mixes them by a recipe a material (`RECIPES`) into a detail set,
`Wear<NAME>.png` (colour), `_r` (roughness), `_n` (normal), listed in `Wear.txt`. Unity
(`ShaderEmuModels.cs`, `Detail`) puts a material's set in Mochie's detail slot, laid by where a
place is in the room (triplanar), as the plastics' scan (`PlasticDetail`) is for the rest: one shader,
no coordinates of the model's. A thing that wants its own use has a material of its own named as
its recipe (the floppies' `FloppyShell` and `FloppyShutter`, the side rooms' desks' `DeskWalnut`),
the same base as before. `python world/wear.py` (and `textures.py`, which calls it) writes them.
What lies on a thing by its shape (worn edges, dust in corners) is not yet in it: that needs a
mask a thing, baked from its shape; so are mugs' rings, which a triplanar set put on a desk's
edges and sides too (tried and given up, 5 November 2026).

## The shell

Floors, ceilings and walls of every room are a model of their own, `Shell.fbx`
(`world/shell.py`): the den, both corridors and the eight holodecks. It is made for the
lightmap, which shows a line wherever two pieces of a wall meet and at every corner.

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
  ray every 4 mm). A cardboard box's top is its flaps, under its bounds' top. A game's box leans till its top meets the wall (`leaning_box`).
- Small printed things are one atlas, `Details.png`; `world/atlas.json` says where each is, and
  `quad(..., decal=NAME)` uses it. The packer stops if the atlas is full.
- The shelf's books are boxes and magazines (`world/gameboxes.py`), most of them showing their
  fronts: games face out on two shelves, leaning on its back (`faceout_row`, `leaning_box` in
  `furniture.py`), one shelf of games and the software spine out (`shelf_row`), three games lying in
  a pile, magazines in piles (`pile`). The games the shelf has no room for are about the den
  (`scatter`: a pile on the floor by the sofa's end); boxes and magazines lie on the low table, the
  desk, seven of the classroom's towers (`ON_TOWERS`; not the hubs') and on the holodecks' corridor's
  table and bench. A thing lying face up is turned so that its top is away from whoever stands
  before it (`TOP_AWAY`, the turn at which its top points to +z: the bench's had read upside down
  from the corridor); the low table's read from the sofa. The drawn spines of `Books.png`
  (`world/bookdesigns.py`) are no longer used.
- The screens, keyboards, panels and lights are still the builder's own. `world/computer.py`
  repeats their rectangles at its top: a screen moved in the builder must be moved there too.
- The builder's boxes stay where they stop a player, without their renderers (`StripBoxes`):
  furniture in a model must keep the footprint of the box it replaces.
- The city (`world/city.py`) is boxes with windows from three tiling textures, roofs, lights and
  the river in a white one tinted by vertex colour, and streets from four kinds of block in one
  texture. Only faces the window can see are made. A vertex's alpha is its haze.
  Its point lights are drawn on the surface of a box that carries them (`box(light=...)`, `City.shader`'s
  `CarriedLight`; 6 November 2026), the box their dark housing: a mast's obstruction light a disc 0.3 m round
  on a metre box, the bridges' and the banks' lamps discs on theirs, a roof's crown a neon tube 16 cm high in
  a band of 1.2 m, a sign neon in strokes. Each is box-filtered by the pixel's footprint (`fwidth`): under a
  pixel it is its share of the light, so it never winks out between pixels, at 80 times its colour
  (`_LampBright`) for the bloom. A light shows only where its carrier is drawn: the metre box is a pixel
  at the city's far edge, 900 m off at 1080p.
- The room's two clocks run (the wall's by the door, the alarm clock on the desk). `hands=` of
  `b.asset` makes each hand an object of its own, its origin the dial's middle and its mesh
  pointing at twelve (`spawn_hands` in `world/assets.py`); the builder gives it
  `ClockHand.shader`, which turns it about its own z by the time of day: the visitor's own
  clock, which `EmuWeather` tells the shaders as `_UdonClockDay`. A hand is not static and has
  no lightmap. Its second hand steps; the others sweep.

## Light

- All of the room's light is baked, and every model has a lightmap: 60 texels a metre, more
  for the shelf, the books and the rack, whose depth is their occlusion.
- "ShaderEmu/Bake lighting" uses Unity's own lightmapper on the graphics card (close VR first).
  Blender's Cycles is no substitute: its light does not match the room's. Bakery is a paid asset and is
  not in the repository; when the project has it (`BAKERY_INCLUDED`), `BakeLighting` bakes with it
  instead (`ShaderEmuBakery.cs`: lightmaps, light probes and the light volumes in one render,
  then the reflection probes by Unity's bake into files of their own).
- "ShaderEmu/Look at the room without a bake" is for working on the room: no lightmap, the
  lamps real-time, flat light under them, and no static batches (in play mode a batched model
  is dark under real-time lamps). The
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

- The window's pane is `WindowRain.shader` (`GlassMaterial` in `ShaderEmuWeather.cs`): the drops are
  BigWings's "Heartfelt" (shadertoy.com/view/dslczf), ported to HLSL; that file is under its licence,
  CC BY-NC-SA 3.0, not the repository's MIT. Its value is the water's height on the glass (`_Scale`
  metres a unit of its picture, `_Height` mm at 1), the normal from its differences, and the eye's ray
  is traced through that water: bent into it at the glass and out of it at its surface (index 1.333),
  the city looked up in that direction in the window probe's picture, given to the material as `_Sky`
  (a camera's picture taken outside the pane, `WindowProbe`, linear and HDR: captured into a half-float
  cube target, as `RenderToCubemap` into the asset gave 8-bit sRGB values the glass read as linear, the
  city washed out; 1024 a face and BC6H since 6 November 2026, the city's lights being points), and what the surface reflects mirrored back out (`_Rim`). Between the drops the pane
  is clear. Against aliasing: no screen derivatives in the water (taken over 2 x 2 blocks of pixels they
  drew a grid); a drop is never narrower than a pixel's footprint, from the projection; the water is
  taken on a 3 x 3 lattice across the pixel with Heartfelt's exact gradient at each point (`HeartfeltD`,
  its steps carried with their derivatives; `_Debug` 6 checks it against differences), each quarter's
  shares and rays its corners' (a rim, a fifth of a drop, is thinner than a quarter from a few metres:
  its mean slope had reflected a fifth of the light), each lookup blurred over what its rays sweep.
  Measured against an 8 x 8 supersampled picture (`_Footprint`, `_Widen`, `_Layers`, `_Test` are that
  test's; 4 November 2026): the shimmer under half-pixel moves 0.78 to 1.11 of the reference's, the
  reflection 0.81 to 0.94 of it, what goes through 7 to 14% too bright; 0.40 ms a pane at 1080p. What a drop's surface reflects goes back into the room and shows the room, a picture of its own (`_Room`,
  `Reflection probe window room.cubemap`, the panes put away while it is taken). The dry glass has Heartfelt's
  fog (`_Mist`, `_MistBlur`): the city behind it blurred, cleared by a drop and by his trail value, so a drop
  stands out of it; the room's reflection alone hardly showed (a shallow drop reflects 2% but at its rim), and
  drops 2.7 mm high (a puddle's height) blurred into the city from afar. Lightning reaches the drops as a second picture of the city lit by a flash of 1 (`_SkyFlash`),
  mixed by the flash. The outside's own light is a third of what it was (`Night`, 4 November 2026): City.shader's
  `_Night` takes what is under a quarter of blooming to a third and leaves what blooms (1 and above), the
  haze, the sky and the rain sheets are a third, and lightning lights each as before (`_HazeFlash`, `_CityGlow`). The material sets every number of its own: a value left from
  another shader of the same name (Fake Glass's `_IOR` of 1) bent nothing. Anything of the
  room's on a pane makes its drops white paint: a lightmap, light probes, the room's probe. A pane
  is held, not set once (`ShaderEmuGlass.cs`): every pane named `Window glass…` (Blender adds
  `.001` in some build orders) is put back unlit by the room, anchored to that probe, and the probe
  to its picture, when a model is imported, the scene opened or saved, scripts reloaded, a bake
  done, every two seconds in the editor, and at play and at a build, where a pane still wrong fails
  the build. "ShaderEmu/Check the windows' glass" says what it finds. Model copies, a bake of a
  static pane and Unity's own bake of the probe had each undone it.
- Rain is sheets of scrolling streaks at four distances (`Rain` in `world/city.py`,
  `Rain.shader`), and haze that hides the far towers (a vertex's alpha, 480 m).
- The sky is `Sky.shader`: the panorama under a layer of cloud that drifts.
- Fog lies over the streets and thins with height (`HeightFog.cginc`, in `City.shader` and
  `Sky.shader`; `HeightFog` in `ShaderEmuWeather.cs` sets both alike, 5 November 2026): a density
  of 0.125 a metre at the street that falls by e every 20 m up, summed along the eye's ray in
  closed form in each fragment, no depth texture; the sky is the ray's end at infinity, so the
  horizon is all fog and the towers stand into it.
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
capture (`WorldIconCapture.jpg` is the one it was made from), in a column right of the capture's
screen and at least 170 pixels inside the picture's right edge: VRChat crops the picture to 16:9
and to a square, and lettering over the screen hid it. The capture is a camera made in editor
code in play mode with the machine on, rendered 2400 x 1800 with 8 samples: low at the left of
the front row's first computer, at `(-2.36, 0.78, 1.60)` looking at `(-1.46, 1.19, 2.41)`,
moved 0.15 m back along its view and turned 15 degrees right about the vertical (6 November
2026), 54 degrees, after `Take0` on "Stations" twice (the second makes its tube show the
display), `quake3 &` typed into the machine (`RemoteChar`) and, at its skill menu, Enter (`RemoteKey` 28
with 65536, then 28): The Longest Yard in play, its window on the desktop. "ShaderEmu/Show the world's
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
every column fits (18.5 on 29 October 2026). "ShaderEmu/Write the system sheet into the open
scene" puts it there styled as the board's paper, or, when it has its plate, letters that plate
again from the file: no build, no bake. The frame is the board's mesh ("Board" of `Computer.fbx`)
turned and stretched, not static, on copies of its materials without the light volume
(`Generated/* by probes.mat`): with it the wood is black this close to the wall. The plate and
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
  laser presses (`TouchPanels`),
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

The holodecks' consoles (keys of a plate, not buttons) are not on the list.

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
