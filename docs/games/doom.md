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
same frame rate); `-1`, `-2` or `-3` says which.
The shareware `doom1.wad` is fetched by the build and is not kept in this repository.

## Speed

Where it stands: `-timedemo demo1` at 145.4 frames a second on an RTX 5090 (October 2026), 145.9 calibrated
(31 October 2026, `game_par.py doom --calibrated`). What follows goes as the work went: at its start the machine
ran 2.7 to 2.9 million instructions a second where it matters (FXC on D3D11, which is what VRChat runs), and Doom wants 35 game tics a second whatever the frame rate. So
a frame rate is `(instructions a second - 35 x a tic) / a frame`, and both had to shrink.
Every five seconds Doom prints its frame rate and those two costs (`doomstat:` lines;
`DOOM_STATS_MS=1000` prints every second).

| demo1 in real time | A tic | A frame | Frames/s, D3D11 + FXC | D3D12 + DXC |
|---|---|---|---|---|
| before | 50k to 70k | 60k to 160k | 4 to 9 (measured at 2.0M/s) | |
| now | 25k to 43k | 45k to 60k | 19 to 35, mostly over 26 | 27 to 35 |

`DOOM_VIEW=told doom -1 -timedemo demo1` (5,026 tics, as fast as it goes) on 19 October 2026 on
an RTX 5070 laptop (`docs/machine/speed.md`): 60.0 frames a second on the shader machine, 53.5 on
`rvc_lean`, 101.0 on the compute machine, 295.2 on the CPU harness.

35 is the most there is: Doom draws once a tic. The other two demos run at 21 to 35 on
D3D11. The one window of the first demo that fell to 19 is its largest fight, where a tic
is 43k. Where the time went, and what was done:

- **The compiler's defaults.** The toolchain made position-independent code with a stack
  guard: a load from a table for every use of a global, and a stored and checked word in most
  functions. Doom is built `-fno-pie -flto` now, and nothing has the guard
  (`linux/userland/toolchain.sh`; libraries stay position-independent, because the guest's
  own linker, TinyCC's, takes no other kind). Without an MMU every program is
  position-independent again: the flat drivers drop `-fno-pie` and compile `-fPIE`
  (`linux/nommu/setup.sh`, `docs/system/nommu.md`).
- **Sight checks** were half of a tic. A monster that has not noticed the player asks whether
  it can see him several times a second, and the port asked that before asking whether he is
  in front of it. `doom_fast.c` asks in the other order (the same answer, a third fewer walks
  of the BSP tree) and walks the tree in one loop with a stack of its own.
- **Lines 64 bytes apart.** Doom marks each line it has visited in the line itself. Lines are
  64 bytes, and stores 64 bytes apart share 32 sets of the machine's write cache: a walk that
  touched more than 128 lines ended the machine's frame. The marks are in arrays of their own
  now (`doom_line_mark`, and one for sight).
- **Things at rest.** The thinker loop passes by a thing that has no state to count down and
  is not moving, without calling its thinker.
- **The view** was two thirds of a frame: walking the BSP tree for what is visible and sending
  every wall, floor and thing again. The level is kept in GPU memory now (below).
- **The message** at the top of the view was drawn again every frame, a letter at a time (6k
  to 9k instructions while one showed). It stays on the screen while it says the same. That
  was first done only for a view as wide as the screen, and the view Doom starts with is
  not (288 wide, the message on the border above it): there Doom also restores the border
  under the message every frame, and both are left out now while the words are the same and
  nothing has drawn on their rows. The status bar was never the cost: its numbers and face
  are drawn when they change, about 800 patches in the first demo against some 58,000 letters.
- **Looking for what changed.** Every frame compared every sector's heights, pictures and
  light with what the GPU had, and every wall of a line that does something with its
  picture: 9k instructions of a frame on a level of 215 sectors where a handful change. The
  game says what it changes now (below).
- **Time.** Doom asks the time several times a frame, 416 instructions a system call. It
  reads the machine's clock word (`0x81000034`) instead.
- **Matrices.** `glOrthox` and the library's 64-bit divisions cost a frame about 5k; the flat
  views are constant matrices, and a window's size divides in 32 bits.
- **Fixed-point division** takes as many bits a step as the divisor leaves room for, instead
  of one.

The last two took the first demo, one tic a frame (`-timedemo demo1`, 5,026 tics), from 51.8k
to 37.8k instructions a frame beside its 32k a tic (the view itself from 33.5k to 26.7k), and
the third demo from 64.4k to 51.3k; the whole first demo from 439 to 369 million instructions.

A demo must play the same after any of this. `DOOM_HASH_TICS=N` prints a sum of the game's
state (every thing's place, state and health, and the random numbers used) every N tics;
`-timedemo demo1 -nodraw` runs tics only. `DOOM_PORT=sight,look,think` brings the port's own
code back for those three, and the sums of all three demos agree with it.

### Gone through again (12 October 2026)

The first demo, one tic a frame: 31.0k to 21.8k instructions a tic, the view 26.3k to 18.4k
a frame, showing it 1.37k to 1.02k, and 24.6 million to the first frame of a timedemo
became 13.2; the other two demos by about as much. `DOOM_TOTALS=1` prints the sums at exit
(`doomtotal:`), and `DOOM_PORT=find,names,lines,group` bring the port's code back for the
new pieces. The state sums of the three demos and held frames are as they were.

- **Whose subsector.** Every move a thing tries asked `R_PointInSubsector` twice, each a
  walk of the BSP tree from its root (17% of a tic). The walk starts at the deepest node the
  point's blockmap cell is all inside of, found when a cell is first asked about (the side
  is the same for a whole cell when it is the same at the corners of each part the node's
  own x and y cut the cell into), and the last point is remembered.
- **Looking twice.** With one player the port's search for a player looks at him twice; a
  player not seen is not looked at again in the same search. Sight walks fell from 34,008
  to 19,458 in the first demo.
- **Things at rest** have their state counted down in the thinker loop, and the lines and
  things of a blockmap cell a mover's box is clear of are passed by in the iterators,
  without the calls.
- **Things in view** keep their sprite frame, their width in whole pixels and which of
  eight pictures shows; `R_PointToAngle` is asked again only when the thing has turned, or
  moved against the viewer by more than the angle's room in its eighth allows (a move of d
  at distance r turns it by at most asin(d/r)). 181 to 126 instructions a thing.
- **Lights out of sight** wait: a sector whose light changed and which the viewer's sector
  cannot see (REJECT) stays marked until it can. (A level with a doctored REJECT could show
  a wall's light stale, where before only things vanished there.)
- **Floors and ceilings** are in order of height with a count of those under the eye; the
  eye's move flips only what it passed. Sectors that have moved are looked at by themselves.
- **The melt** waited for the clock by asking the time in a loop, 40M instructions a level
  start; it ends the machine's pass instead (`doom_wait`).
- **Start.** Lump and texture names come from tables (the texture table keeps the port's
  rule that a name fits whatever it begins or is begun by), a sprite's size is read from its
  first eight bytes, the tables of Doom's own renderer are made when that renderer is
  wanted, and a level's lines go to their sectors in one pass.
- **Flags.** `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops` in place of `-O2`:
  3% a tic, 11% a frame. `doom.sh` keeps the flags in a file of the build folder and builds
  every object again when they change.
- `I_GetTime` began at `clock_ms() | 1`: a second call in the same millisecond came out
  four million tics too many ("timed N gametics in -4294918 realtics"). Mended.

What is left of a tic is `P_RunThinkers`' own loop and the sight walk. Not built: a box test
a sector for things (it costs what it saves). `--pc-log` writes nothing under `rvc_harness
--cpu`: a profile there is a build with `-finstrument-functions`.

### The view on worker cores (13 October 2026)

A tic and a frame were both the first core's, 45 thousand instructions together, and so the
35 frames a second the game draws at most were there at 3 million instructions a second and
not at 2. A frame's list is now made by up to four worker cores (`docs/machine/multicore.md`) while
the first core runs the next tic. Thousands of instructions are the first core's for a tic
and a frame together; frames a second are the game's own count by the machine's clock.

| `rvc_cpu`, 3M instructions a second, one tic a frame (`-timedemo`) | Frames a second | The first core |
|---|---|---|
| demo1 | 58.0 to 91.0 | 45.4 to 26.4 |
| demo2 | 60.2 to 94.8 | 42.7 to 24.9 |
| demo3 | 49.7 to 72.3 | 50.9 to 32.6 |

| The shader machine, 4 ms a pass (2M a second), demo3 | Before | One worker | Two | Four |
|---|---|---|---|---|
| one tic a frame to tic 600: two counts of five seconds | 25.4, 29.7 | 27.8, 34.5 | 39.4, 38.9 | 42.7, 45.2 |
| the same: passes from power-on to tic 600 | 11,307 | 11,220 | 10,415 | 10,108 |
| in real time (`-playdemo`): the worst five seconds, the median | 14.1, 34.0 | 23.7, 34.8 | | 31.4, 34.9 |
| the same at 6 ms a pass (1.3M a second) | 4.9, 12.3 | | | 7.0, 31.0 |

(The counts of five seconds are not of the same tics from one column to the next; the worst
five seconds in real time are the level's start, its making and the melt.) By the card's own
clock (an RTX 5090, about 4M a second), demo1 one tic a frame to tic 1,500, a run of each in
turn, twice: 87.4 and 90.8 frames a second in two counts of five seconds before, 125.4 with
four workers; then 87.5 and 90.9 against 124.1. Under `rvc_cpu` in
real time the first demo's worst five seconds at 2M went from 20.1 frames a second to 33.4
and the slowest tenth from 26.9 to 33.7; at 3M it was at 35 before and is.

- **The view reads nothing a tic writes.** That is what kept it on the first core: it walked
  the things the tics make and free. Between two tics the first core tells it what it needs
  (`view_take` in `doom_gl.c`): the eye and the weapon; the things the game changed since the
  last view, each once, with their place, angle and picture; the things it took out; the
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
- **One job for the game's life** (`view_loop` in `doom_video.c`), its pass ended by
  `fence.i` while there is no view to make: a worker's pause is its sleep until the next job,
  and under `rvc_cpu` a job posted in the very pass the last one ends in is never begun (a
  job a view stopped after some 1,950 frames). (Replaced by a round of `mcw_for_begin()`:
  "Sight checks made ahead, and the second layer" below.)
- **The first core shows the list when it finds it made**: at a tic's start and end, while
  it waits for the next tic's time, and before the next frame's drawing, where it waits. A
  frame is on the window some passes later than it was. What the first core then adds to
  the 2D picture and the palette is not waited for (a pass): they may show a frame early.
  What it clears and draws again is waited for (`D_Display` settles when the frame before
  drew on the screen, in a menu, on the map and outside a level): the GPU drew the list
  while the menu's rows held the key or the bare page, and the logo was cut or gone.
- **A picture no core has made yet** is read from the game's data, which is the tic's: a
  worker asks for it, the thing keeps its place and shows the picture it had (or nothing
  yet), and the first core makes it before the next view. A level's first three frames and
  a melt's are the first core's own.
- **What a worker writes is compiled apart** (`doom_gl.c`, the GPU's library, the workers'):
  a variable a section, each on 16 bytes of its own, last in the link.
- **A thing at rest is asleep** (`P_RunThinkers`): whether it still rests was six loads a
  thing a tic. The same calls wake it, and one where a hit pushes it (`P_DamageMobj`); a
  player's thing never sleeps. A tic of the first demo: 22.6 to 20.8 thousand (22.0 with
  the calls that tell the view).
- **The wait for the next tic's time** asked the clock in a loop, and the clock moves between
  passes: 177 million of the first demo's 402 in real time at 3M. The pass ends there
  (`doom_idle`), by `mcw_rest()`.

`DOOM_VIEW=N` is N worker cores (4 if not said). `DOOM_VIEW=inline` is none, which is also
what a machine without workers does: the list is made by the first core from the game's own
things, as it was; the first demo is then 246 million instructions where it was 235 (the
library is no longer compiled into its callers). `DOOM_VIEW=told` is none, with the list made
from what workers would be told.

Checking it, all under `rvc_cpu`:

- the state sums of the three demos, with 4 workers, inline and told, and with
  `DOOM_PORT=rest` (no thing asleep): as before;
- `DOOM_VIEW=told DOOM_VIEW_CHECK=1` holds what the view was told against the game at every
  view (`doomcheck:` lines): every thing of every sector's list in its order, the sectors,
  the walls that change, the pictures after animation. The 11 thousand views of the three
  demos: none wrong;
- `DOOM_SUM=1` with `DOOM_STOP_TIC=N` prints a sum of all the GPU is given for the held
  frame (each command drawn, its matrices, vertices and texture, the palette), and one that
  is the same in any order. At twelve tics of the three demos the second is the same as
  before in every mode; the first inline and told, on an image where the game's memory lies
  as it did for the build before. A thing's place among the commands goes by where the game
  keeps the thing: that differs from one image to another, when pictures are made at other
  times (workers), and by its share. A held frame is drawn once more by the first core when
  a worker went without a picture.
- the game's own title and demo loop for 170 seconds, and `killall -9` in the middle of a
  demo, twice, with `mctest 3 20000` after each (PASS).
- the screens with no view of their own, which the sums above never held: `rvc_harness --cpu
  --cpu-ips 2000000 --screen-log` (a slow first core is what swaps at a frame's start), a key
  by `nxkey`, 60 to 240 frames in a row against the build before: the pixels still there
  must be right in every frame. The menu over a paused level (wrong in 240 of 240 frames
  before the mend), over the title (47 of 60) and over a demo (4 of 240), the tally between
  levels (`-deathmatch -timer 1`: 200 of 240), the help page and the map: none wrong now.

On the shader machine: the state sums of demo3 to tic 500 and of demo1 to tic 1,500 as
under `rvc_cpu`, the sums in any order at demo3's tic 600 and demo1's tic 1,500 as the build
before gives them there, and the window's picture of those two frames against the ones from
before: no pixel of the game's 640 by 400 different with four workers (and with one, at tic
600); the caption's frame rate is. (By the card's own clock the sum in order is not the same
from one run of one build to the next.)

Tried and not kept, or not done:

- **A job a view**, by `mcw_post`: lost under `rvc_cpu` as above.
- **The whole program as one object** with every variable on 16 bytes (`-flto -r`, then
  `objcopy`), so that the library stays compiled into its callers: a tic 7% longer, the
  small variables no longer within reach of the global pointer.
- **The tics on a worker** and the view on the first core: not tried. A worker keeps 1.5 KB
  of new stores a pass where the first core keeps 6, and a tic stores to hundreds of things.
- **Sight checks made ahead** on a worker: they are asked after the player's move of the
  same tic, so the places are not known a pass earlier.
- Not looked at by hand: the menu, a saved game loaded, a window resized while workers draw.

### Sight checks made ahead, and the second layer (14 October 2026)

The view's shares are a round of `mcw_for_begin()` (`programs/mc/mcwx.h`, "The second
layer"): a piece a worker, none on the first core, begun after a frame's drawing and taken
(`mcw_for_ready()`, `mcw_for_wait()`) when all are made, where `view_loop` and its words were.
A worker no longer stores to the library's "unsettled" word beside another: the first share
clears it and the others wait for that (`drawn_wait`), and the one variable every share wrote
(`on_worker`) is gone. `RVC_RACES` said 29,685 undone before; it says 0 now.

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
numbers it was walked for, and a level loaded makes every walk of the old one stale. 81% of
the first demo's checks are answered so (15,800 of 19,400; under `RVC_RACES` 81% too); the
rest are a player who hit a wall or a step.

`python tools/game_par.py doom --runs 3` (`-timedemo demo1`, passes of 8,192):

| | Core 0 a frame | All cores a frame | Factor | Guest seconds (passes) |
|---|---|---|---|---|
| before | 27.2k | 56.9k | 2.09 | 38.6 (16,715) |
| now | 25.4k | 61.4k | 2.42 | 36.6 (16,200) |

The workers' extra is the walks moved and those not used (about 5k a frame), and the team's
waiting for its next round (a few hundred). `DOOM_AHEAD=0` leaves the checks to the first core.
Checked: the state sums of the three demos as before, with and without `DOOM_PORT=rest` and
under `RVC_RACES` (0 undone); `DOOM_VIEW=told DOOM_VIEW_CHECK=1` 0 of 10,500 views not as told;
`DOOM_SUM`'s order-free sum at nine held tics of the three demos as before.

Tried and not kept: a monster's next move checked against the lines ahead the same way
(`P_CheckPosition`'s lines on the worker, its things on the first core): 27% of moves
answered and no fewer instructions on the first core. Each share going through only the
sectors with its own things: no fewer instructions in the order of the sectors' numbers, and
in another order a held frame's thing turned another way. A call a share (`mcw_call`):
2 thousand more of the first core's instructions a frame than the round.

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
(`seglKeep` in `programs/linux/gles.c`). The first level of the shareware game is 2,032 quads
in 764 runs. The depth buffer decides what is seen.

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
  them (one frame in eight of the third demo);
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
filled the write cache every few dozen: a texture took hundreds of machine frames to make.
Making a level this way costs 5.4 million instructions (it was 11 million with a generic sort
and textures copied a pixel at a time).

`DOOM_PORT=view` draws the way it was done before: Doom walks its BSP tree every frame and
what it finds visible is sent. `DOOM_STOP_TIC=N` holds the picture of game tic N, which is how
pictures are compared. Against that older way the kept level differs in a few hundred pixels
of a frame, and where they differ by more it is the older way that was wrong: it showed sky
through a floor Doom's own renderer draws.

With `DOOM_RENDER=soft` Doom's own renderer fills the view and the GPU only shows the screen;
`DOOM_SOFTWARE=1` is the port as it comes. The GPU's view is the same scene as Doom's own but
not the same picture:

- light is the sector's light level on everything in it; Doom's darkening with distance is
  not there, and the gun's flash adds light where Doom's multiplies it;
- a texture that really uses the key index (247) loses those pixels where it has holes.

The window may be any size (the frame's maximise box): the view and the screen are drawn at
the window's size, the screen stretched from its 320 by 200.

## Still on the CPU

A tic is the game itself: sight checks (about a third still), thinkers, movement. A frame is
the things in view (about 250 instructions each, and 20 for each one looked at and left out),
which are the worker cores' now, and on the first core what the view is told (2 thousand),
the status bar and the frame's bookkeeping (2 thousand). Sound costs it next to nothing: effects and music
are the sound card's (`docs/machine/sound.md`, `linux/nanox/doom_sound.c`).
