# Doom

The Doom that comes with Microwindows (`src/contrib/doom`, a Nano-X client) runs in the Linux
image with the GPU drawing its picture.

    wsl -- bash linux/nommu/build.sh nanox/build.sh     (once)
    wsl -- bash linux/nommu/build.sh nanox/doom.sh
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1                                  (then a new shell snapshot)

In the guest: start it from the desktop's Start menu or a terminal, or `nano-X -p & doom` at
the console. `-playdemo demo1` plays the first demo in real time, `-timedemo demo1` one tic a
frame, `-warp 1 1` starts in the first level.
The window is the largest of one, two or three times 320x200 that the display has room for
(960x600 on 1280x720: the GPU does the stretching, and draws the 3D view at that size, at the
same frame rate); `-1`, `-2` or `-3` says which. The window may be any size (the frame's
maximise box): the view and the screen are drawn at the window's size, the screen stretched
from its 320 by 200.
The shareware `doom1.wad` is fetched by the build and is not kept in this repository.

## Speed

Doom wants 35 game tics a second whatever the frame rate, and draws once a tic at most when it
is played. So a frame rate is `(instructions a second - 35 x a tic) / a frame`, and the view's
list is not the first core's. Every five seconds Doom prints its frame rate and those costs
(`doomstat:` lines; `DOOM_STATS_MS=1000` prints every second, `DOOM_TOTALS=1` the sums at exit
as `doomtotal:`).

`-timedemo demo1`, one tic a frame, from its first rate line to the end (5,026 tics in all):

| | Frames a second | Passes a frame | Core 0 a frame | All cores a frame |
|---|---|---|---|---|
| calibrated, 31 October 2026 (`game_par.py doom --calibrated`) | 145.8 | 4.26 | 26.3k | 64.3k |
| card, RTX 5090, as the model was fitted (`game_par.py doom --card`) | 145.4 | | | |

Of the same calibrated run's lines near the end, a tic is 16k of core 0's instructions and a
frame 5k to draw, and the list of a view is 25k to 35k of the workers'. 15,853 of the first
demo's 19,423 sight checks were answered by a worker's walks.

What makes it this fast:

- **The compiler.** `doom.sh` builds `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops
  -flto`, and with `doom.profile`'s counts (`DOOM_PROFILE=count` makes the program that counts,
  `off` leaves them out: `docs/games/tdawn.md`, "What the compiler is told"). It keeps the flags
  in a file of the build folder and builds every object again when they change. The flat
  drivers drop `-fno-pie` and compile `-fPIE` (`linux/nommu/setup.sh`, `docs/system/nommu.md`).
- **Sight checks.** `doom_fast.c` asks whether the player is in front of a monster before
  asking whether it can see him (the same answer, a third fewer walks of the BSP tree), walks
  the tree in one loop with a stack of its own, and does not look a second time at a player
  not seen in the same search.
- **Lines' marks.** Doom marks each line it has visited in the line itself, and lines are 64
  bytes: stores 64 bytes apart share few sets of the machine's write cache. The marks are in
  arrays of their own (`doom_line_mark`, and one for sight).
- **Things at rest.** The thinker loop passes by a thing that has no state to count down and
  is not moving, and asleep it costs no look at all (below). The lines and things of a
  blockmap cell a mover's box is clear of are passed by in the iterators, without the calls.
- **Whose subsector.** The walk of `R_PointInSubsector` starts at the deepest node the point's
  blockmap cell is all inside of, found when a cell is first asked about (the side is the same
  for a whole cell when it is the same at the corners of each part the node's own x and y cut
  the cell into), and the last point is remembered.
- **Things in view** keep their sprite frame, their width in whole pixels and which of eight
  pictures shows; `R_PointToAngle` is asked again only when the thing has turned, or moved
  against the viewer by more than the angle's room in its eighth allows (a move of d at
  distance r turns it by at most asin(d/r)).
- **Lights out of sight** wait: a sector whose light changed and which the viewer's sector
  cannot see (REJECT) stays marked until it can. A level with a doctored REJECT could show a
  wall's light stale.
- **Floors and ceilings** are in order of height with a count of those under the eye; the
  eye's move flips only what it passed. Sectors that have moved are looked at by themselves.
- **The message** at the top of the view stays on the screen while it says the same, and the
  border under it is not restored while the words are the same and nothing has drawn on their
  rows. The status bar's numbers and face are drawn when they change.
- **Time.** Doom reads the machine's clock word (`0x81000034`) instead of a system call. The
  melt's wait (`doom_wait`) and the wait for the next tic's time (`doom_idle`, by `mcw_rest()`)
  end the pass where they would ask the clock in a loop: the clock moves between passes.
- **Matrices and division.** The flat views are constant matrices, a window's size divides in
  32 bits, and fixed-point division takes as many bits a step as the divisor leaves room for.
- **A level's start.** Lump and texture names come from tables (the texture table keeps the
  port's rule that a name fits whatever it begins or is begun by, and an answer is kept), a
  sprite's size is read from its first eight bytes, the tables of Doom's own renderer are made
  when that renderer is wanted, a level's lines go to their sectors in one pass, and the wall
  and floor pictures are made ahead on the workers (`pictures_ahead` in `doom_gl.c`). A picture
  of whole columns is not cleared first, `sort_pieces` goes by a halving search, and the
  clip's 64-bit division is a float guess made exact by the remainder.

A demo must play the same after any of this. `DOOM_HASH_TICS=N` prints a sum of the game's
state (every thing's place, state and health, and the random numbers used) every N tics;
`-timedemo demo1 -nodraw` runs tics only. `DOOM_PORT=sight,look,think,find,names,lines,group`
brings the port's own code back for those pieces, and the sums of all three demos agree with
it.

## The view on worker cores

A frame's list is made by up to four worker cores (`docs/machine/multicore.md`) while the first
core runs the next tic. The tics stay the first core's: a worker keeps 1.5 KB of new stores a
pass where the first core keeps 6, and a tic stores to hundreds of things.

- **The view reads nothing a tic writes.** Between two tics the first core tells it what it
  needs (`view_take` in `doom_gl.c`): the eye and the weapon; the things the game changed since
  the last view, each once, with their place, angle and picture; the things it took out; the
  things it put into a sector's list, in that order; the sectors, walls and animations it
  wrote. The cores that make the list keep their own copy of the things (`struct thing`, in
  lists by sector that are in the order of the game's own) and of the sectors (`now_`).
- **The game says what it changes** (`doom_fast.c`, the calls put in by `doom.sh`): a
  thing's state (`P_SetMobjState`), its place (`P_SetThingPosition`), its height
  (`P_ZMovement`, `P_ThingHeightClip`), its end (`P_RemoveMobj`). A thing's `validcount`,
  which the game has and never uses, says that it is among the changed already.
- **Up to four shares.** A job given out and its answer are a pass each way, so the cores do
  not wait for each other within a view: each has the things whose place among the kept
  commands ends in its number, and its own lists and notes of them; the first has the level,
  the weapon, the sky and the 2D picture's quad as well, and the things that are blended.
- **A round of `mcw_for_begin()`** (`programs/mc/mcwx.h`, "The second layer"): a piece a worker,
  none on the first core, begun after a frame's drawing and taken (`mcw_for_ready()`,
  `mcw_for_wait()`) when all are made. No worker stores to the library's "unsettled" word beside
  another: the first share clears it and the others wait for that (`drawn_wait`).
- **The first core shows the list when it finds it made**: at a tic's start and end, while
  it waits for the next tic's time, and before the next frame's drawing, where it waits. A
  frame is on the window some passes after it was asked for. What the first core then adds to
  the 2D picture and the palette is not waited for (a pass): they may show a frame early.
  What it clears and draws again is waited for: `D_Display` settles when the frame before drew
  on the screen, in a menu, on the map and outside a level.
- **A picture no core has made yet** is read from the game's data, which is the tic's: a
  worker asks for it, the thing keeps its place and shows the picture it had (or nothing
  yet), and the first core makes it before the next view. A level's first three frames and
  a melt's are the first core's own.
- **What a worker writes is compiled apart** (`doom_gl.c`, the GPU's library, the workers'):
  a variable a section, each on 16 bytes of its own, last in the link.
- **A thing at rest is asleep** (`P_RunThinkers`). The calls that tell the view wake it, and
  one where a hit pushes it (`P_DamageMobj`); a player's thing never sleeps. So a new place the
  game writes a thing's state, place, height or speed needs `doom_thing_changed`, or a sleeping
  thing never wakes.

`DOOM_VIEW=N` is N worker cores (4 if not said). `DOOM_VIEW=inline` is none, which is also
what a machine without workers does: the list is made by the first core from the game's own
things. `DOOM_VIEW=told` is none, with the list made from what workers would be told.

### Sight checks made ahead

A fifth piece of the round walks the sight checks of the tic after the next (`doom_ahead` in
`doom_fast.c`). The first core lists, as it counts states down, the monsters whose state ends
two tics on; the piece takes those whose next action is `A_Look` or `A_Chase` and would ask
(as `p_enemy.c` decides it: in front, close enough to bite, free to shoot), and walks each
against the player where his next two commands take him if nothing is in the way (from the
demo, or as made; `P_MovePlayer`'s thrust, `P_XYMovement`'s steps and friction). A check
finds its answer there when t1 and t2 and their eight numbers (places and heights) are the
same and no floor or ceiling the walk went by has moved since (`moved_at`, by sector); a
look behind a monster's back is kept too and saves `P_LookForPlayers` its angle. The worker
reads things while the first core changes them; what is kept is what was walked, under the
numbers it was walked for, and a level loaded makes every walk of the old one stale. The
checks not answered so are a player who hit a wall or a step. `DOOM_AHEAD=0` leaves the checks
to the first core.

A monster's next move checked against the lines ahead the same way does not pay: too few moves
are answered, and the first core runs as much. Each share going through only the sectors with
its own things does not save instructions either, and a call a share (`mcw_call`) costs the
first core more than the round.

## Checking it

`tools/test_all.py`'s `doom-1`, `doom-2`, `doom-3`, `doom-told` and `doom-rest`, under `rvc_cpu`:

- the state sums of the three demos at `DOOM_HASH_TICS=100` (`0ab4df93`, `c5123ba8`,
  `df43029a`), with 4 workers, inline and told, and with `DOOM_PORT=rest` (no thing asleep), and
  `RVC_RACES` 0 undone (`--races`);
- `DOOM_VIEW=told DOOM_VIEW_CHECK=1` holds what the view was told against the game at every
  view (`doomcheck:` lines: every thing of every sector's list in its order, the sectors, the
  walls that change, the pictures after animation) and must say `5000 views, 0 not as told`.

By hand, after a change to the drawing:

- `DOOM_SUM=1` with `DOOM_STOP_TIC=N` prints a sum of all the GPU is given for the held
  frame (each command drawn, its matrices, vertices and texture, the palette), and one that
  is the same in any order. Compare by that second one: a thing's place among the commands goes
  by where the game keeps the thing, which differs from one image to another, when pictures
  are made at other times (workers), and by its share. A held frame is drawn once more by the
  first core when a worker went without a picture.
- the game's own title and demo loop, and `killall -9` in the middle of a demo, with `mctest 3
  20000` after each (PASS);
- the screens with no view of their own: `rvc_harness --cpu --cpu-ips 2000000 --screen-log` (a
  slow first core is what swaps at a frame's start), a key by `nxkey`, 60 to 240 frames in a
  row against the build before: the pixels still there must be right in every frame. Those are
  the menu over a paused level, over the title and over a demo, the tally between levels
  (`-deathmatch -timer 1`), the help page and the map.

`--pc-log` writes nothing under `rvc_harness --cpu`: a profile there is a build with
`-finstrument-functions`. Not looked at by hand: a saved game loaded, a window resized while
workers draw.

## What the build changes

The Microwindows tree is left as it is; `doom.sh` compiles a few files differently:

- The table of sprite names has no end marker and is counted up to the first zero after it.
  The count is bounded.
- The shareware data's demos are version 1.9; the source only took 1.10.
- `char` is signed (`-fsigned-char`): Doom's tables end with -1.
- The video functions, the renderer's entry points, the sight check, the search for players,
  the thinker loop, the clock, the message drawer and eraser and the screen melt are compiled
  under other names, and `doom_video.c`, `doom_gl.c` and `doom_fast.c` take their places. So are `R_PointInSubsector`,
  `R_InitTextureMapping`, `W_CheckNumForName`, `R_CheckTextureNumForName`,
  `R_InitSpriteLumps`, `P_GroupLines` and the two blockmap iterators, and the melt's loop
  has a call of `doom_wait()` put into it.
- Where the game writes a sector's heights, floor picture or light, or a wall's picture, it
  tells `doom_gl.c` (`told` in `doom.sh`: `T_MovePlane`, which every mover goes through, and
  a call or a flag put before each of the other writes; the script counts them and stops if
  the source has another number). `P_InitThinkers` says that a level or a saved game was
  loaded.
- Where the game changes what a view shows of a thing, or takes a thing out, it says so
  (`doom_thing_changed`, `doom_thing_linked`, `doom_thing_gone` in `doom_fast.c`: a line put
  into `P_SetMobjState`, `P_ZMovement`, `P_RemoveMobj`, `P_SetThingPosition`,
  `P_ThingHeightClip` and `P_DamageMobj`), and the wait for a tic's time calls `doom_idle()`.
- `doom_gl.c`, the GPU's library and the workers' (`programs/mc/mcw.c`, `mcwx.c`) are compiled apart
  from the rest, a variable a section on 16 bytes of its own (`APART` in `doom.sh`).

## The screen (`doom_video.c`)

Doom draws on a 320x200 screen of palette indices. That screen is allocated in GPU memory
(before Doom takes pointers into it) and each frame is one rectangle textured with it. The
palette is 256 words the GPU looks indices up in, so Doom's palette effects (damage, pickups)
are 256 stores.

A full-screen page (title, help, intermission) is decoded once; after that only the rows
something else drew on (a menu) are copied back, and none when nothing did.

The melt between two screens is done by the GPU: 160 strips of the picture the window showed
last (the window's own pixels, used as a texture) slide down over the new frame, each moved on
by that frame's fall. Doom's own melt copies the whole screen every frame, and has no 3D view
in it here. The fall and its random numbers are Doom's.

## The view (`doom_gl.c`)

Walls and floors do not move, but for doors and lifts. When a level starts, all of it is
written into GPU memory once: a quad for every wall a seg can show (upper, lower, middle, and
a texture with holes across an opening) and for every two triangles of a subsector's floor and
ceiling. The polygons come from cutting a large square along the BSP tree's partition lines
down to each leaf. Quads are sorted into runs with one sector, one texture and one shade, and
each run is one command that stays at the head of every frame's list
(`seglKeep` in `programs/linux/gles.c`). The depth buffer decides what is seen.

A frame then sends what changed, and knows what did without looking at the level: the game
marks a sector when it writes it (`doom_sector_changed`) and a frame looks at the marked ones
only, once for each of the two sets of commands, since a light is a word of each. A switch's
picture, its going back and a sliding door set one flag (`doom_walls_changed`), and only then
are the walls of lines that do something looked at; the walls of lines that scroll are looked
at every frame. A level or a saved game being loaded makes the kept level anew, whatever the
addresses of its segs. So a frame sends:

- the view's matrices (one block of 28 words);
- a sector whose floor or ceiling moved: its planes' heights and the walls that stand on
  them (each sector has a list);
- a sector whose light changed: the grey of its runs, in Doom's sixteen light levels, and
  only in the set of commands the frame is built in (a run's grey is one word of its command,
  and commands are 64 bytes apart: the write cache holds few such words);
- the gun's flash, which lights everything for a tic or two, is one white rectangle added
  over the view, not a new grey for every run;
- a floor or ceiling seen from the wrong side is not drawn, as in Doom. That depends on the
  eye's height, which changes at every step: kept are the two heights between which the eye
  turns no floor or ceiling over, and every sector is looked at again only when it leaves
  them;
- a wall on a line that does something has a run of its own, so a switch changing its
  picture or a wall sliding is one command and one quad;
- animated textures: the runs that show one take the next picture;
- things. The sectors the viewer's sector can see into (Doom's REJECT table) are searched for
  things ahead of the viewer and within the view's quarter turn. A thing in view keeps a
  place among the kept commands while it stays in view: a frame turns its quad to face the
  viewer and sends a picture, a height or a light only when they change.

The sky is a problem of its own once everything is drawn: Doom paints sky over whatever is
behind a sky ceiling, and a depth buffer shows it. So sky ceilings, and the wall between two
of them, are drawn too, with the sky's picture laid on the screen instead of on the surface
(`FRAGMENT_LAID`, `docs/machine/gpu.md`): they hide what is behind them and look like the sky. One
rectangle of sky at the far end of the depth range is still drawn behind everything.

A wall's texture, and a sprite's, is kept on its side (a row of texels is a column of the
picture), so each of Doom's columns is one run of bytes to copy; whoever draws with it swaps
the coordinates. Copied upright, a column's bytes are a row apart, and stores a row apart
fill the write cache.

`DOOM_PORT=view` has Doom walk its BSP tree every frame and sends what it finds visible.
`DOOM_STOP_TIC=N` holds the picture of game tic N, which is how pictures are compared. Against
that way the kept level differs in a few hundred pixels of a frame, and where they differ by
more it is that way that is wrong: it shows sky through a floor Doom's own renderer draws.

With `DOOM_RENDER=soft` Doom's own renderer fills the view and the GPU only shows the screen;
`DOOM_SOFTWARE=1` is the port as it comes. The GPU's view is the same scene as Doom's own but
not the same picture:

- light is the sector's light level on everything in it; Doom's darkening with distance is
  not there, and the gun's flash adds light where Doom's multiplies it;
- a texture that really uses the key index (247) loses those pixels where it has holes.

`DOOM_SEAT` prints a line every 20 tics of how a holodeck's sitter is seated (`docs/world/holodeck.md`).

## Still on the CPU

A tic is the game itself: sight checks, thinkers, movement. A frame is the things in view,
which are the worker cores', and on the first core what the view is told, the status bar and the
frame's bookkeeping. Sound costs it next to nothing: effects and music are the sound card's
(`docs/machine/sound.md`, `linux/nanox/doom_sound.c`).
