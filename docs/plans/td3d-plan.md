# Plan: Tiberian Dawn in 3D, with free models

Written 1 November 2026. On 2 November the host's part was made and tested, and the game's part written, built and run on `rvc_cpu` and
`rvc_harness --cpu --warp` ("What came of it", at the end; `docs/games/tdawn.md`, "In 3D"); nothing has run on the card. It comes from reading `docs/games/tdawn.md`, `docs/machine/gpu.md` and OpenCNC3D's README and
list of documents. When a page `docs/games/tdawn.md` has on this and this plan differ, the page is right.

The idea is OpenCNC3D's (`github.com/freschism3000/OpenCNC3D`, GPL-3.0): the rules of the 2D game, drawn in 3D as the Nintendo 64's version was. Not its
assets, which come from a cartridge dump that cannot go into a public image. Our models are free ones, or ones we make, so that it can
run for everyone in the world.

## What stays and what changes

- **The game's rules do not change.** It is a new drawing. Every state sum and `TDAWN_CHECK_PATHS=1` must hold in both views. A hill is a look: it changes no unit's speed or sight.
- **The 2D view stays**, and stays the default until the last step: `TDAWN_VIEW=2d|3d`, which also gives before and after from one binary. A kind of object with no model is drawn as today's sprite standing up, so the game is whole at every step.
- **The sidebar, text and dialogs stay** on the game's page, over the view. So do the pointer's pictures.
- Tiberian Dawn first. Red Alert has its own `gl.cpp`, and comes after it, with what it has of its own (ships, the Soviets' and Allies' models).

## What the machine already has for it

From Quake III and `docs/machine/gpu.md`:

| Wanted | There |
|---|---|
| A perspective camera | The matrix draws; a frame's matrix over kept draws (`seglKeep`), as today's scroll is |
| Models that cost no RAM and few list words | Packed and tagged vertices in the ROM (`0x1000`, `0x2000`, `0x4000`), a table of colours, facing-lit (`0x20000`) |
| Depth | The target has it: less-or-equal, written. Today's sprites are in the blended pass with none, to keep the game's order |
| Terrain's pictures | The 1,482 icons of the atlas, as they are: the first terrain is today's picture draped over the ground |
| Keeping what did not change | `Kept`, `IsToDisplay`, `ShaderEmu_Away`, `host_quads`, the scene on a worker |

At most 196,608 vertices are in a list. Nothing here has been measured for this use.

## Hard parts, to be met in the steps

1. **Clicks.** The game takes a pixel and makes it a cell. A tilted view needs the pixel's ray met with the ground, and an object picked by what stands on the screen, not by its cell. This must go through the game's own entry (`Input`, `Click_Cell_Calc` and the band box) so that orders are the same orders.
2. **What is in view.** The game's window on the map is a rectangle of cells, and it culls by it. A tilted view shows a trapezium, so the rectangle grows, and what the game draws beyond its window changes with it. Scrolling and the sidebar's radar must still agree with the view.
3. **The shroud** is flat today. Over hills it must lie on the ground or hide what it should. The N64 version's way, and OpenCNC3D's `docs/n64-camera-math.md` and `docs/auto-heightmap.md`, are to be read for how they did it.
4. **Mission heights.** Maps have no heights. They are made from each map by a script at image build (water and shore low, cliffs from the terrain's own cliff icons, smooth ground), and kept in the ROM. Which cell has what comes from the icons, so it needs nothing by hand for each mission.
5. **Effects.** Explosions, smoke and shots stay sprites facing the camera. A unit's flash and a laser are looked at last.
6. **Licences.** OpenCNC3D's code is GPL-3.0, and this repository is MIT with the game's patch under EA's GPL. Nothing of its code is copied: its documents are read for how, and its numbers (the camera's) are measured or chosen again. If copying would save much, that is the developer's call.

## The models

About 20 vehicles and aircraft, 7 soldiers, 10 civilians, 30 or so buildings, and trees, walls and rocks: the list comes from the game's type tables at step 3, not from this line.

- **Budget, to be checked at step 3:** a soldier a few dozen triangles, a vehicle 100 to 300, a building 200 to 800. Parts that move apart (hull and turret, rotor, a refinery's arm) are parts of a model. Vertex colours or a small palette texture, no large textures.
- **Where they come from, in the order to try:**
  1. **Made by script.** `linux/tdawn/models/*.py`, each model a few dozen lines from boxes, cylinders and wedges, out to a packed file by the same way `bake.py` packs Quake III's. No licence question at all, a whole set in one style, and any model changed by a line. They will look plain but consistent, as an N64 game's did, and this is the plan's default.
  2. **CC0 packs,** for what a script draws badly (a soldier's figure, trees). Kenney and Quaternius are known for them, and OpenGameArt and Sketchfab list some. A search found CC0 soldiers (`RTS Mercenaries 2` on Sketchfab, `Low Poly Soldier with weapons` on OpenGameArt) but no free CC0 tank or building pack: to look on those sites directly. Only CC0, or what the file's licence is seen to allow, noted model by model in `linux/tdawn/models/CREDITS.md`. Paid packs (POLYGON Military and the like) cannot go in a public repository.
  3. **Made by the developer,** in Blender, where they want a thing to look a particular way: the tool takes a glTF file.
- **No part of a model comes from EA's games, the N64's or anyone's rip.**
- **Colours:** the house's colour goes through the table (`0x2000`), so GDI and Nod are one model.
- **Soldiers:** a walk of a few poses, or kept as sprites standing up if the poses cost too much. Sprites may well be the better look at this size: decided by a picture at step 4.

## Steps

Each shows something, and ends with its gate. Stop and report if a gate fails.

0. **Read, and one number.** Read `gl.cpp`, `host.c` and the camera documents. Draw a mission's kept cells through a tilted matrix with no other change, with nobody clicking. *Gate: a picture, and frames a second on the card against the 60 of the 2D view (`game_par.py tdawn --card`), in the world's measure too (`--unity`).*
1. **A tilted, flat game.** Sprites standing up with depth, shadows flat on the ground, the view's window and the radar right, clicks and the band box through the ray. Heights none. *Gate: a mission is played to its end by script and by hand; state sums the same as in 2D; the card at 30 frames a second or better in the busiest mission tried.*
2. **Ground.** Heights from the script, cells kept as meshes with the atlas's pictures, water and cliffs, the shroud on them. *Gate: a look at every theatre (temperate, desert, winter) in captures, and no slower than step 1 by more than an agreed share.*
3. **One tank.** The tool, the packed format, one model by script and turret and hull apart, facing as the game's 32, house colour, `Kept` so that one standing still costs nothing, with sprites for the rest. Then the same scene with 50 tanks. *Gate: the triangle budget above is set from what this measures: instructions on core 0 and all cores, passes, the card's GPU time.*
4. **The rest of the set**, a kind at a time, buildings first, as they are most of what is seen; the soldiers' decision here. *Gate: a capture of each kind beside its sprite. `tools/wm_shots.py`'s way: the cases say places and states; what is drawn is looked at.*
5. **Speed.** By the `optimizing-games` skill, on the busiest mission: *a frame's work on workers as the scene is now, `RVC_RACES` with `RVC_RACES_NEAR=4` at 0 undone, 30 frames a second on the card at least, and in the world's measure and in VR's as agreed at step 0.*
6. **In.** `test_all.py` cases for 3D beside 2D's (the same state sums, pictures not black, clicks reach the same cells), `docs/games/tdawn.md`, the image size, the world. Whether 3D is then the default is decided on the numbers. Then Red Alert, from the same tool and code where its `gl.cpp` lets it.

## To find out first

- The card's cost of many small depth-tested draws under the world's frame, which Quake III's whole level drawn at once does not say. Step 0 and step 3 measure it.
- Whether the demo's data has all that the game's full set of objects needs: the models are for what the build's missions use first.
- Whether the N64 camera's look is wanted at all, or a steeper one that hides less behind buildings. A camera setting at step 1, chosen by play.

## The risks, largest first

1. **Clicks and the view's window** reach into the game's input and drawing code more than a renderer's swap would suggest (step 1). It is also where the 3D view could change game state by mistake: state sums watch it.
2. **The look:** models by script could come out too plain to be worth it. Step 3's one tank, and step 4's first three kinds, show it cheaply. Then CC0 packs or the developer's own.
3. **The number of draws in a battle.** A model's vertices stay in the ROM and a move or a turn is only its matrix, with no new sprite frame for each facing, so the cores' work should be like a sprite's or less. What grows is a command (64 bytes) and a matrix for each moving part, against 6 KB of texels a pass (some 7 texels a part: 100 parts a frame are about 2 passes, an estimate), the vertex shader's search over more commands, and the card's time for the triangles. Step 3 sets the budget before the set is made.
4. **The world's frame:** the card's time with depth at 1280 x 720 in VR.

## What came of it

**2 November 2026.** Written first with no guest toolchain (the host's part tested, the game's checked by the host's `gcc`), then built
in a private WSL home and run: `test_all.py tdawn ralert` 21 of 21 (2D unchanged; the 2D lists the same as before, `TDAWN_CHECK_LIST`),
`tdawn-3d-300` and `tdawn-3d-paths` pass with `--races` too, and pictures of missions 1, 3 and 10 under `rvc_harness --cpu --warp`
(`tools/td3d_shots.py`) show the tilted map, the models and the soldiers standing. The gates that need the card (frames a second, the
world's measure, the triangle budget) are not met: the card was another agent's.

- **Step 0.** The tilted matrix over the kept cells is written (`host_scene_tilt`, `td3d_clip_matrix`), and tested on the host against the
  arithmetic it stands for (the matrix's pixel within 0.05 of it, depth inside -1 to 1 and growing away from the eye). No picture, no frames
  a second: `game_par.py tdawn --card` and `--unity` are the owner's.
- **Step 1.** Written: sprites standing up (scaled about their middle, the game's order kept, no depth), the ground's rectangles on the
  map, the view's window unchanged (the camera is fitted inside it, so neither the radar nor the culling changes), clicks, the pointer and the
  band through the game's input by the ray (`ShaderEmu_View_Pick`), the band and the bars where the camera shows them. A camera setting is
  `TDAWN_PITCH` (35 to 85: at 30 or less the near row is behind the eye). OpenCNC3D's camera documents were not needed: the camera is
  fitted to the game's window from its pitch alone (`td3d.h`).
- **Step 2.** Heights: `linux/tdawn/td3d/heights.py` from the map's templates at image build, against the fetched tree's own table of them.
  On the demo's five maps (`SCG01EA` to `SCG10EA`) it finds water, beaches, rivers and 88 to 540 cliff cells a map; the cliff lines all have
  gaps, so no map has terraces: cliffs are ridges. Which side of a cliff is high is not in the game's data (OpenCNC3D's auto-heightmap
  learnt it from the N64's maps): a table of the 38 cliff templates' high sides, made by looking at them (`td3d/tem.py` draws them), would
  give terraces. Drawn: the cells' corners, the objects' and models' feet, the ray. Not drawn: the overlays and the shroud on the hills
  (they need a four-cornered sprite in the blended pass, which `gles.c` has not), so heights are off by default.
- **Step 3 and 4.** The tool (`linux/tdawn/models`: `mesh.py`'s boxes, wedges, prisms and tubes, `pack.py`'s packed file, `view.py`'s
  pictures on the host) and 20 models made by script, all within the budget: 12 vehicles (MTNK, LTNK, HTNK, JEEP, BGGY, APC, HARV, MCV,
  ARTY, FTNK, STNK, MSAM; 68 to 148 triangles) and 8 buildings (NUKE, SILO, PYLE, FACT, PROC, HQ, GTWR, GUN; 46 to 126), 1,884 triangles and
  25,904 bytes in all. Turrets apart, house colours by the table, a `Kept` model costs a note. None was downloaded: there is no licence
  question, and the plan's CC0 packs were not needed for these. Measured: nothing (the budget is to be set from the card, step 3's gate).
  Soldiers, aircraft, trees and walls are sprites standing up.
- **Step 6, in part.** `tools/test_all.py`'s `tdawn-3d-300` and `tdawn-3d-paths` (tag `td3d`) say the 3D view changes no rule.

What is left: the card's numbers of step 0 (`game_par.py tdawn --card`, `--unity`) with `TDAWN_VIEW=3d`; step 3's 50 tanks; a mission
played to its end in 3D by hand (clicks, the band) and by script; then the look, decided on pictures.

What `gles.c` would need (its owner's: a change there moves every program's bytes): a sprite with a depth (the blended pass, tested), so
that sprites and models hide each other rightly; and a quad of four corners with a colour in that pass, for the ground's rectangles over
hills. And for the ROM: `seglPacked` takes memory from `seglMemory()`, so the models' file is copied into GPU memory (26 KB) rather than
drawn where it lies in the ROM as Quake III's are; a call taking a ROM address would save that.
