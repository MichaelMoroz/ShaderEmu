# Red Alert

Command & Conquer: Red Alert (1996) runs in the Linux image, from the same source tree as
Tiberian Dawn ([Vanilla Conquer](https://github.com/TheAssemblyArmada/Vanilla-Conquer)) and
on the same layer under it (`docs/games/tdawn.md`, `linux/tdawn`): a Nano-X window, the map drawn by
the GPU, the sound card. Its screen is 640 x 400, shown one to one.

    wsl -- bash linux/nommu/build.sh tdawn/build.sh     (first: it makes the C++ driver)
    wsl -- bash linux/nommu/build.sh ralert/build.sh    (two minutes; the first time it also fetches)
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1                                  (then a new shell snapshot)

In the guest: `nano-X -p & ralert` at the console, or Red Alert in the Start menu's Games. The data
is the 1996 demo's: one Soviet and one Allied mission and skirmish games, 24 MB, which the build
fetches and the repository does not keep. Effects, speech and the demo's two tunes play on the sound card.

## What the build is made of (`linux/ralert`)

- `build.sh` puts the tree at Tiberian Dawn's commit, applies Tiberian Dawn's patch (the
  shared files) and then `vanilla-conquer.patch` (Red Alert's own files, and two shared
  ones), copies `linux/tdawn`'s `host.c`, `shaderemu.cpp`, `soundio_shaderemu.cpp` and
  `cxxrt.cpp` in, and builds `vanillara` with `-DSHADEREMU_RA`. It is installed as
  `/usr/lib/ralert/game`, which the starter `ralert` runs where it lies in the ROM, from
  `/tmp/ralert` with links to the data (`docs/system/nommu.md`, "Memory"). It is compiled as one program
  with its own counts (`docs/games/tdawn.md`, "The whole program at once", "What the compiler is told").
- `gl.cpp` is Tiberian Dawn's map renderer with Red Alert's names. What differs: a cell is
  "mapped" once any of it shows and "visible" when all of it does (the other way round there);
  a map may be 128 x 128 cells; a shadow has as many levels of darkness as its pixels ask for.
- The sound pack (`ralert-sound.pak`, 294 sounds) is made by Tiberian Dawn's tool.
  The sounds are in MIX files inside the demo's two, behind an encrypted index: the build
  compiles the source's own `vanillamix` for the host and unpacks them with it.

## What was changed for this machine

Everything under "What was changed" in `docs/games/tdawn.md` that is in the shared files applies.
In Red Alert's own:

- **No hash of the data.** A MIX file is not hashed (SHA-1) as it is read into memory.
- **Fading tables** (the palette's nearest colour to each faded colour, forty tables before the first mission)
  are made by a search that looks only at colours whose green is near enough to matter. The same tables.
- **The vortex's tables**, sixteen more for each theatre, are made when a vortex first
  appears. (The GPU's scene does not draw the vortex at all yet.)
- **Music that is not there.** A mission names its tune; the demo has two (Hell March and Crush). A tune that does
  not start is replaced by another, and after four failures none, so it is not asked for again every frame.
- **The radar's cover plate** is not drawn again whenever a unit moved (the unit asks for its dot on a radar that is not there).
- **An object that moved does not change the cells it left and came to**, and a unit that
  looks again at a cell it has seen does not either. On the GPU only the scene is made again, and the renderer
  keeps a mark on every cell that has nothing of its own to draw.
- **The first run** goes past the menu straight into the Soviet mission, as after an
  install: the starter writes the setting that says the game has been run.
- The logic that repeats itself is Tiberian Dawn's, with the state sums unchanged: threat of
  a unit that steps, houses found from a table, the layer's sort, and the computer's count of
  units no team has (a unit is asked whether it could join only when one is wanted).

## The map's cells, a frame

In `linux/ralert/gl.cpp`:

- a run of settled cells in a row, or of cells never seen, is passed over or made one black
  rectangle without asking the game about each cell;
- a cell with ore, a wall or a scorch mark keeps the rectangles it drew, as an object does,
  and they are put again until the game says the cell changed;
- a plain cell at the shroud's edge keeps its edge and is asked nothing unless the game has
  flagged it;
- a cell that changed is noted once a scene (`told`): a cell first seen tells of itself and its eight neighbours.

The view is 20 x 16 cells, four times Tiberian Dawn's. A held frame is the same picture as with everything drawn
anew (`RALERT_REFRESH=1`).

## A battle

`RALERT_HUNT=N` sends everything the player has after the enemy at frame N, which is the only way a mission left
alone comes to a battle. Its profile has no large piece: threat scans, path finding, the houses' and each object's own
turn; a soldier's turn is its ordinary stages. The game reads the pointer from the machine's input words, the kernel reports a
moving pointer 10 times a second while a program runs, and the pointer itself is the display's cursor
(`docs/machine/input.md`, `docs/games/tdawn.md`).

A shape the game turns (an aircraft, a missile: `CC_Draw_Shape` with a rotation) is a shape of the atlas like any other,
turned once for each turn of each frame (`turned()` in `linux/ralert/gl.cpp`), from a ready frame; the turning itself is this
core's. An empty cell of a scan for a target is passed over without calls (`SHADEREMU_OCCUPIED` in `TechnoClass::Greatest_Threat`).
Both games are built with link-time optimisation in a partition a file and nothing reordered: its globals are made in the order
of its source files (`docs/games/tdawn.md`, "The whole program at once").

## Movies, and the worker cores

The demo's opening movie (ENGLISH.VQA, 640 x 400, 156 frames of it before `TDAWN_AUTO`'s Return ends it). The machine's core keeps
6 KB of stores a pass (its write cache: `docs/machine/multicore.md`), so what limits a frame is stores:

- **The frame is a picture the GPU shows** (`host_picture`, `host_show_picture` in `linux/tdawn/host.c`): decoded once, into GPU
  memory, and drawn from there, stretched over the window if it is one of the small movies. No copy and no doubling by the game.
- **The worker cores decode it** when the machine has any (`linux/ralert/workers.c` is the game's jobs, over `programs/mc/mcw.c`). A
  frame's rows are shared by `mcw_for` (every core takes pieces, this one too), each writing its own rows of the picture, and the next
  frame's pointers are unpacked by `mcw_try_call` while this one is drawn: the player loads a frame ahead for that. Both are libmedia's
  (`media_vq4_rows`, `media_lcw`: `docs/system/media.md`, "VQA"), which also plays a VQA file whole (`nxvideo ENGLISH.VQA`). The movie's codebooks
  and pointers are allocated in whole 16 bytes with their pages already there (`workers_alloc`), so that no core stores beside another's and no
  worker stops at a new page; the picture is GPU memory, which a worker has as the game has it.
- A frame's palette starts on the 16 byte boundary after its pointers: a worker stores whole 16 bytes to the pointers' very end.
  `RALERT_MOVIE_CHECK` compares pictures and cannot see a fault there.
- The workers are the game's while a movie plays and no longer: it asks for them when a movie's buffers are made and gives them back when it
  ends. A mission opens its own for the scene, the frames and the objects (`host.c`).
- More workers do not make a movie faster: what a frame takes is this core's own (reading the file, the sound, the frame's bookkeeping), not the rows.
- `RALERT_MOVIE_SOFT=1` is the game's own way, `RALERT_WORKERS=N` uses no more than N workers, and `RALERT_MOVIE_CHECK=1` decodes every shared frame on core 0
  as well and compares: `movie check: 0 of 156 shared frames differ` (`ralert-movie`). Each movie prints a line (`ralert: movie ...`).

The mission's ground layer is sorted by a worker while the frame is drawn, and the objects are drawn a frame behind by a worker (`docs/games/tdawn.md`, "The objects a frame
behind"); a selected unit's bars and box are `gl.cpp`'s, kept with it ("Selected units: their bars" there).

## The mission's frame

A mission's logic has no large piece: some hundreds of functions under 1.5% each. A job given to a worker and waited for costs two passes, so an object's turn cannot go to another
core for a gain, and the turns are in an order the state sums depend on. What could go is a whole part that may be a game frame behind (the computer player's planning), which changes those sums.

To profile the mission alone: a snapshot at `ralert: begun`, resumed with `--pc-log` and `--ra-log`; `tools/pc_profile.py`, `tools/pc_callers.py` for who calls a leaf function, `tools/pc_hot_in.py` for where in a
function, or `tools/prof.py` (`docs/machine/profiling.md`).

- **A house whose teams can want nothing does not count its units for them.** What the team types that are built ahead ask for is found once a scenario (they do not change), and a house with none of those and
  no team in play skips the counting.

## At two million instructions a second

What the two games share is in `docs/games/tdawn.md`. Red Alert's own part:

- **A house that takes no part** (`HouseClass::ShaderEmu_Takes_No_Part`). Twenty houses have a turn every frame, and all but three or four have no object, no trigger, no special weapon and nothing asked of them: for such a
  house the turn comes to its timers, which are kept running in their order, and nothing else. A house with team types counts as one while none is built ahead for, no team is in play and its team timer has not run out.
- **A scan for walls asks cells that have an overlay.** Only a cell with an overlay can be a wall worth shooting (`Evaluate_Just_Cell`).
- **A trigger that ends is looked for in the cells that have one, by a list** (`MapClass::Detach`). The list is made when a trigger first ends and made again after a cell was given one (`ShaderEmu_Cell_Triggers_Stale`: the
  scenario's cell triggers, a saved game's cells, the editor). `RALERT_CHECK_TRIGGERS=1` asks every cell as well: `0 cells missed`.
- **`Shape_Dimensions`**: a row's first and last pixel are found a word at a time (`host_first_pixel`, which the atlas uses too): the same rectangle.
- **The houses' scan bits are made when something changed.** What `HouseClass::Recalc_Attributes` reads changes at a dozen places (an object made or gone, into or out of limbo, locked onto the map, discovered, captured, and where
  the bits are written directly): each says so (`ShaderEmu_Scans_Stale`), and a frame in which none did keeps the bits. An anim or a bullet does not make them stale (it is in none of the scans). `RALERT_CHECK_SCANS=1` makes them every
  frame anyway and compares: `0 of them wrongly`. A new place that changes `IsLocked`, `IsInLimbo`, `IsDiscoveredByPlayer`, an object's house or a scan word must say so too.
- **An anim or a bullet that ends is not looked for in everything's targets** (`ObjectClass::Detach_All`). Nothing can hold the target value of one whose value was never made, and a value is made in two places (`AbstractClass::As_Target`,
  `TargetClass`'s constructor from a pointer), which note it by the object's place in its heap (`ShaderEmu_Taken`), for good. Only these two kinds: what holds a soldier or a building may hold a pointer (a bullet's `Payback`, a radio contact), which
  no value says. `RALERT_CHECK_DETACH=1` makes the look anyway and sums every byte of every object before and after: `the look changed something for 0 of them`. A saved game's values were made elsewhere: loading one notes all.
- **A `CCPtr` is one load.** Each heap that has such pointers has a table of its objects' places, entry 0 empty for the ID that is none (`ShaderEmu_Heap_Table` in `heap.cpp`, made again when the heap's buffer is another).
- **A scan for targets turns most objects away first for the least** (`TechnoClass::Evaluate_Object`): whether the object's kind is asked for and whose side it is on are tested before its cloak, its zone and its health. Every test only says no, so
  their order does not change an answer.
- **The scan passes empty cells a word at a time**, by Tiberian Dawn's bits of who is occupied (`cell.h`), where it has no walls to look for (the player's units, and a computer that does not shoot walls). `RALERT_CHECK_OCCUPY=1` compares the bits with
  the cells every frame: `0 cells' occupancy bits were wrong`.
- **A new object is put into the drawing order with one question of its own** (`LayerClass::Sorted_Add`, both games).
- **The view's own half of `Coord_To_Pixel`** is worked out when the view moves, not for every object of every frame.
- **A soldier with one weapon** is not asked which of its two is better (`What_Weapon_Should_I_Use`), and **a shape that goes to the scene** is not measured (`Shape_Dimensions` in `Techno_Draw_Object` is for the game's own drawing).
- **What moves is out of the cells' overlap lists** once the scene is live (both games; `SHADEREMU_NO_OVERLAP` in `map.cpp`): the lists say what to draw again when a cell is painted over, which the scene never does. Buildings and trees stay in them; an object
  not seen yet still reveals itself as a list would have made it (`ShaderEmu_Overlap_Seen`). Anything new that reads `Overlapper` for more than drawing finds no units there.

Not done: a soldier that only moved shown from its kept rectangles (what its `Draw_It` would draw is not known without calling it), and the logic itself on more than one core, which is what a battle would take.

## The frame in which the sidebar comes

The longest frame of the Allied mission is the one in which the sidebar and the radar's frame are first drawn whole: large pieces, the tabs, two cameos unpacked, measured for their rows' spans and copied to the page in one frame.

- The sidebar's files are told of when they are found (`ShaderEmu_GL_Ahead_File` in `sidebar.cpp`, `power.cpp`, `tab.cpp`; a cameo when its strip gets it; the radar's two by name, `ShaderEmu_Radar_Ahead`, because the game finds them only when the radar is first
  drawn), the worker cores unpack them as the mission begins, and the game's own drawing copies them from there (`docs/games/tdawn.md`, "Frames made ready"). What is left of that frame is the copies to the page, the page's holes made again (the sidebar's coming forces
  everything) and the scene from nothing.
- The credits' tab and the power bar are `BF_Trans` by runs (`docs/games/tdawn.md`).
- A message that expires, the scene's refresh in turns: `docs/games/tdawn.md`.

## A base destroyed

`RALERT_ATTACK=N[,E]`: from frame N, every E frames, the player's units and soldiers are selected, sent at the enemy's building nearest them (one that costs something: no barrels or civilians) and the view centred on them. With `RALERT_SIDE=allies RALERT_ATTACK=100,250` the
base falls from frame 600 on. What a long battle wants:

- **The atlas's shapes are forgotten when it fills.** Every frame of every shape, each remap and shadow, each turn of an aircraft keeps its place in the atlas (`linux/ralert/gl.cpp`). A scene that begins with fewer than 256 rows left, or after a shape found no room, forgets
  the shapes (`shapes_forget`: the shapes, the shades, the masks of their rows and every rectangle kept with an object or a cell) and puts each again as it is next drawn. `RALERT_ATLAS=N` forgets them every N scenes too, for a test. `rastat:` says how many are left to the game.
- **The screen is shaken without copies of the page.** A building that costs 400 or more shakes the screen when it is destroyed, cost / 400 times, and twice (for its owner and for whoever destroyed it); so do bridges and some triggers. The same random picks and frames are
  made without the copies (`Shake_The_Screen`), since those moved only the page over the map the GPU draws; `RALERT_SHAKE=copy` copies.
- **The objects' worker draws from a copy that points at nothing living.** A copy's place is worked out as it is copied and it is attached to nothing (`AnimClass::ShaderEmu_Detach`). A frame no core had ready is made into the worker's own buffer (`ShaderEmu_Frame_Into`), as the game's
  `Build_Frame`'s buffers and heap are the first core's; the game's own drawing, asked on the worker, is not made there: the scenes after are drawn by the first core (`ShaderEmu_GL_Not_Behind`).
- **Zones.** A wall, bridge or tree that falls makes the map's zones again (`MapClass::Zone_Reset`): only the map's own cells begin a zone, each cell's passability is asked once a reset (`Zone_Open_Bits`), and the fill does not call itself for a cell it would refuse at once; the fill's
  order is the game's, so are the zones' numbers. `RALERT_CHECK_ZONES=1` fills each reset the game's way too: `0 cells of them wrongly`.

Checks: `tools/test_all.py`'s `ralert-attack` (state `dbb267c0` at frame 3,000), `ralert-atlas` and `ralert-atlas-forgotten` (the shapes forgotten every 50 scenes, the same 300 picture sums); `RVC_RACES` 0 undone.

## A unit's path searches

Tiberian Dawn's helps to a unit's searches (`docs/games/tdawn.md`, "A unit's path searches") are Red Alert's too (`findpath.cpp`), in Red Alert's order of searches: `FootClass::Basic_Path` searches at the unit's `PathThreshhold` and, while it finds nothing, at the next threshold up to `MOVE_TEMP`,
where Tiberian Dawn's searches at the highest first and then at the others. `RALERT_CHECK_ENTER=1` checks the kept answers.

- A search that met no answer between its threshold and an earlier search's is not run again: that one's result is put back (`ShaderEmu_Path_Same`; only a search that found nothing is kept for it, since one that found a path is the unit's last).
- A search whose two edge walks both failed starts the unit's next thresholds on up to three worker cores (`ShaderEmu_Path_Help`, `host_path_cores` in `host.c`; `TDAWN_PATH_CORES=0`: none), and the next search takes its worker's result. A core's search stores only into its `ShaderEmu_Core[who]` and
  its own kept answers (the core is in the threshold's upper bits), `Cell_Threat`'s held lifting is settled before they begin, and the searches nobody asked for are stopped and waited for before the unit is put down. Tiberian Dawn begins them once a walk has come by 48 cells; here nearly every long
  search is the unit's only one, and workers begun on every long search would only cost waits.
- The edge walk is `ShaderEmu_Walk`: `Follow_Edge` with `Register_Cell` and `Unravel_Loop` in it, the path's length and cost in registers and a cell's answer read where `Passable_Cell` keeps it (`RALERT_WALK=old` is the game's walk). The workers that ask every cell ahead are not Red Alert's (`host.c` has none for it).

`RALERT_CHECK_PATHS=1` makes every search that was put back or made by another core on the first core too, and every edge walk both ways, and compares: `tools/test_all.py`'s `ralert-paths`, 0 of them wrong twice and state `520254a5` at frame 1,500.

It is worth little here: nearly every search finds a path at the unit's first threshold, and most of a burst of searches is the clockwise walk round the building attacked, whose path is longer than the other's and is not taken. A walk's path can grow shorter again (it is cut back where the walk crosses
itself), so that walk cannot be ended early with the same result. The functions changed have no counts in `ralert.profile`: counting a played game again would give them theirs.

## A mission won

`RALERT_WIN=N[,K]`: the player wins at frame N (`Flag_To_Win`), in K missions in a row, and each step prints a line (`ralert: score: loaded at ... ms, ... instructions, ... KB mapped, ... KB in use`); `TDAWN_AUTO` presses Return through the score screen too.

- **The score screen's table of colours.** Its first picture is the 320 x 200 animation doubled with a colour between every two pixels (`Interpolate_2X_Scale`). The nearest colour is looked for outward from the midpoint's r + g + b in the palette sorted by it (no colour further in that sum than sqrt(3) times
  the best distance can be nearer), half the table is made (it is symmetric) and its rows are shared between the worker cores (`host_for` in `host.c`; `common/interpal.cpp`). The table is the game's (`TDAWN_INTERPOLATION=check`: `0 of 65536 entries differ`; `=game` is the game's search).
- **The doubled pictures** are made two rows of a source row at a time on the worker cores: `0 of 399 rows differ` from the game's own.
- **The score's clocks** (`ScoreTimeClass`, `ScoreCredsClass`) draw each frame once, not also into a copy that is read only when DirectDraw gives the screen back. A frame asked for that is not ready pumps the shape files (`host_frame`; `docs/games/tdawn.md`, "Frames made ready"), as nothing else on the score screen does.
- **The map does not show through** the score's first animation (its black is the page's key): the scene is dropped as the announcement ends.
- **The demo goes to its menu after the score screen** (`Is_Demo()` in `Do_Win`). The full game shows the campaign map (`Map_Selection`: `MSAC.WSA` after the Allied mission); the demo has no such file and no next mission, and `Open_Animation` does not look at whether the file opened, so its garbage header would ask `Alloc` for
  "out of memory". From the menu a new game is the same mission again.
- **The score animation's frame table is read by bytes.** The score animation (`SOV-TRAN.WSA`) is kept whole in its buffer, its frame table where the delta buffer ends: `delta_buffer + largest_frame_size`, at any byte (3 bytes into a word in the Soviet file, 1 in the Allied file). `Get_Resident_Frame_Offset` (`common/wsa.cpp`) reads the
  table by bytes: through a `uint32_t` pointer the compiler may make each `memcpy` of it one word load at an address that is not a word's, and frames are decoded from the wrong places (`vanilla-conquer.patch` of Tiberian Dawn, which has `common/`).

`tools/test_all.py`'s `ralert-won` (twice won, the table and rows against the game's own, the menu twice, state `ea72a5d1` at frame 300) and `ralert-won-soviet` (state `01e0dd60`). The score screen is behind its own pace at each step (its busy waits draw its shapes and count as busy); a lost mission's screens were not tried.

## Speed

`game_par.py GAME --calibrated` (31 October 2026), seed 7, frames 301 to 1200, nobody holding them back; the card beside it:

| | Game frames a second | Passes a frame | Core 0 a frame | All cores a frame |
|---|---|---|---|---|
| Soviet mission, calibrated | 86.7 | 7.39 | 54.4k | 77.9k |
| Soviet mission, card | 86.2 | | | |
| Allied mission, calibrated | 61.6 | 10.53 | 79.3k | 130.9k |
| Allied mission, card | 61.2 | | | |

From the command to the menu and to a mission's first frame is most of the wait: the movies are the GPU's and the workers', and the start is in `docs/games/tdawn.md`, "The start".

## Checking it

    TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7 RALERT_FRAMES=300 ralert      (wait for "ralert: done")

`tools/test_all.py`'s `ralert-*` cases hold what follows.

- `TDAWN_AUTO=1` presses Return through the menus; `RALERT_SIDE=allies` or `soviet` answers the question of sides; `RALERT_SEED`, `RALERT_FRAMES`, `RALERT_STOP_FRAME` (with `RALERT_NO_DELAY=1`), `RALERT_STATS_MS`, `RALERT_RENDER=soft` and `RALERT_REFRESH` are Tiberian Dawn's switches (a scene from nothing every 256th time). `rastat:` lines say
  where a frame went. `RALERT_HUNT=N` sends everything after the enemy at frame N, and `RALERT_CHECK_SCANS=1` must end with `0 of them wrongly`. `RALERT_CHECK_ZONES=1` must say `0 cells of them wrongly`, and `RALERT_CHECK_PATHS=1` `0 of them wrong` twice ("A unit's path searches", with `RALERT_WALK` and `TDAWN_PATH_CORES`). `RALERT_ATTACK`, `RALERT_ATLAS` and `RALERT_SHAKE`: "A base destroyed". `RALERT_WIN` and `TDAWN_INTERPOLATION`: "A mission won". `RALERT_FRAME_LOG`, `RALERT_MARK`,
  `RALERT_MESSAGE_GONE`, `RALERT_READY_DRAW`, `RALERT_CHECK_READY` and `RALERT_CHECK_TRIGGERS` are as Tiberian Dawn's of those names (`docs/games/tdawn.md`).
- A scrolled picture: `TDAWN_DRAG`, `TDAWN_SELECT`, `TDAWN_SUMS`, `TDAWN_CHECK_GPU` and `TDAWN_PICTURE` are Tiberian Dawn's, under those names (`docs/games/tdawn.md`, "A scrolled picture"; the lines begin `rasum:` and `ragpu:`). The Soviet mission's sweep differs from one made whole near the view's lower left in a run of frames: there the kept scene is the game's own drawing (`RALERT_RENDER=soft`) and the scene
  made whole is not: something at the view's left edge that a scene from nothing leaves out. Not explained.
- A change to the game must leave its `state` sum the same: at frame 300 with seed 7 `01e0dd60` for the Soviet mission (SCU01EA) and `ea72a5d1` for the Allied one (SCG02EA); at frame 1,300 `eb6c2265` and `dadd663d`.
- The picture against the game's own drawing (`RALERT_RENDER=soft`, both held at one frame): those that differ are shadows: a true blend where the game's table picks the palette's nearest dark colour.
- Sound: `--fixed-dt 0.004 --sound-capture` and `tools\sound_reference.py ... --rom`: a mission's sound is the model's in every sample. `TDAWN_SOUND_LOG=1` says what the game asked for.
- Palette cycling goes by the clock (Tiberian Dawn's goes by frames in a run with no delay): a few dozen pixels of lamps differ between two runs of one build.

## Not done

- Playing it by hand: the pointer and keys are Tiberian Dawn's code. So is the map's drag by the right button, which is the only way the map moves (`docs/games/tdawn.md`, "The map is dragged"), tested here as there.
- Skirmish, saving and loading.
- The vortex in the GPU's scene; ground units sorted among walls as the game sorts them (the scene draws overlays first, then every object).
- Scroll steps sized by time, and frames left undrawn when late (`docs/games/tdawn.md` has both).
