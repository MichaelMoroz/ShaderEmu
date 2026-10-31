# Quake III Arena

One level of Quake III Arena's free demo (q3dm17, The Longest Yard) runs in the Linux image
with a bot: ioquake3's source with the game, the client's game and the menus linked in, and a
renderer and a sound layer of our own. Everything on the screen is drawn by the machine's
GPU: its pictures from GPU memory, where they are decoded when a level or the menu loads, and
the rest where a file lies in the ROM.

    wsl -- bash linux/nommu/build.sh nanox/build.sh     (once)
    wsl -- bash linux/nommu/build.sh libmedia/build.sh  (once: docs/system/media.md)
    wsl -- bash linux/nommu/build.sh quake3/build.sh    (the first time it also fetches)
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1

In the guest: `nano-X -p & quake3 +map q3dm17 +addbot sarge 3`, or Quake III Arena in the
Start menu's Games, which opens the main menu. Lines typed at the terminal it was started
from are console commands. The demo's pak is fetched by the build and not kept; what goes into
the image is 10.6 MB.

The main menu is cut down to what works here (`q3_ui/ui_menu.c` in the patch): the level
against Sarge at three skills (I CAN WIN, HURT ME PLENTY, HARDCORE: `addbot sarge` 1, 3, 4)
and EXIT, which asks nothing. Its icon is ioquake3's (`linux/apps/icons/quake3.png`).
A menu's cursor is the desktop's pointer: `SE_Input` tells the menus where it is in the
window, not how far it went (`UI_MouseEvent` in the patch), and their own cursor is not drawn.

The game's picture keeps the size the window had when it started and is stretched to the
window as it is now (`picture_viewport` in `tr_shaderemu.c`); the pointer's place in a menu
and the sky's repeats go by the same two ratios.

## What the build is made of (`linux/quake3`)

- `build.sh` fetches ioquake3 at a fixed commit, applies `quake3.patch`, and compiles the
  engine, the bots' library and the three modules with Quake's flags (`linux/quake/build.sh`),
  with `quake3.profile`'s counts (`QUAKE3_PROFILE=count` builds the program that counts, `off`
  none). The modules are not libraries: each is linked to one object whose names stay inside
  it, and its variables are put back as they began each time it is "loaded" (`sys_shaderemu.c`).
- `bake.py` makes the two files the game runs from (below); `jpgraw.c` reads the demo's JPEG
  pictures for it, and `q3pack.c` (libmedia for the build computer) writes them as the game's
  JPEG and PNG files and reads them back; `wanted.txt` is what the game asked for in a run with
  `QUAKE3_WANTED=1`.
- `tr_shaderemu.c` is the renderer, `gpu_shaderemu.c` the GPU library (`programs/linux/gles.c`
  as it is, and draws whose every word the renderer says), `snd_shaderemu.c` the sound,
  `server_shaderemu.c` the server's frame on a worker core, `client_shaderemu.c` the client's
  game on another, `sys_shaderemu.c` the system. The workers' jobs are calls of
  `programs/mc/mcwx.h` (`mcw_call`, `mcw_ready`, `mcw_join`; "The scene on a worker core").
- `quake3.patch` is `git diff -- code` of the tree at `build.sh`'s commit. A tree that is that
  already is left as it is, so that a build compiles only what changed.
- The zone, the server's zone and the hunk are each one anonymous mapping of its size
  (`SE_Pool` in the patch). The no-MMU build (`linux/nommu/build.sh`) makes them 4, 6 and 8 MB,
  and its starter adds `+set com_hunkMegs 8 +set com_zoneMegs 4` over a kept `q3config.cfg`
  (`docs/system/nommu.md`, "What differs").

## The two files

`bake.py` reads the demo's pak and writes only what `wanted.txt` names:

| | |
|---|---|
| `quake3-pak0.pk3` | 2.5 MB, stored and not compressed: what the game reads itself (the level for collision without its light maps, the bots' navigation, their scripts) |
| `quake3-se.dat` | 8.0 MB: sounds (halved to 11 kHz: `--sound-rate`), models, the level with its light grid, pictures, light maps |

- **Pictures** are decoded and halved to 128 texels a side (256 for the flat screen's) when
  baked: three bytes a texel, or words where a picture's alpha is used. Then they are files
  again ("Pictures decoded when a level loads"): JPEG, and PNG for those with alpha. Light
  maps stay as texels.
- **Shaders** are the scripts' stages reduced to what the GPU has: one of four passes, a
  colour that may be a wave, a texture that may move. A reflection (`tcGen environment`) is
  left out, and what is blended over one is laid on its mean colour.
- **The level** is triangles sorted by shader stage, three vertices each, patches cut into
  4 x 4 pieces; a light map and the texture it lights are one draw. A texture with no script
  is lit by its surface's light map too, as the game does, and on a surface with no light map
  it has the surface's own colours.
- **A model's pose** is a word a vertex (three bytes of place and which of 256 ways it
  faces), every frame of it, and its texture coordinates a word a vertex.
- **One character** (Sarge, for the player and the bot) and one level: `--bots`, `--map`.

## How it is drawn

- **The level is kept in GPU memory**, in draws that stay in every frame's list. A frame gives
  them its matrices and says what changed: a wave's colour, a texture's turn. Nothing of the
  level is looked for or sorted in a frame; the card's depth buffer hides what is behind.
- **Textures are decoded into GPU memory** when what draws with them is registered (below);
  **light maps and models stay in the ROM**, where the GPU reads them.
- **A model is lit by the GPU**: three texels a thing (where the light comes from, its
  colour, the light from everywhere), from the level's light grid, kept by where the thing is.
- **Sprites, beams and the game's polygons** are quads written into the frame; the flat
  screen is quads in the one pass that blends without depth, in the order they come.
- **A model is drawn in the nearer of its two frames**, and its tags are that frame's.

What the GPU has for it (`docs/machine/gpu.md`): word textures and packed vertices in the ROM,
smooth three-byte textures, a draw lit by a light map (`0x800`), a clamped texture
(`0x1000`), a texture moved by two rows of numbers (`0x10000`), vertices lit by how they face
(`0x20000`). `rvc_backend11.cpp` leaves the ROM's textures bound in the vertex shader.

Not as Quake III draws: no dynamic lights, no marks on walls or shadows, no fog, portals or
flares; no reflections; `tcMod turb` and vertex deforms stand still; rail rings are missing;
no mipmaps; the weapon can poke into a wall; a 2D shader's first stage only; no music or videos.

## Speed

`game_par.py GAME --calibrated` (31 October 2026): q3dm17, `fixedtime 33`, the player put at
488 -968 378 looking north at frame 100, frames 300 to 700; the card beside it:

| | Frames a second | Passes a frame | Core 0 a frame | All cores a frame | Busiest worker | Server's frames of the game a second | Passes each |
|---|---|---|---|---|---|---|---|
| one bot, calibrated | 79.1 | 8.36 | 24.5k | 165.6k | 46.3k | 26.1 | 25.3 |
| one bot, card | 79.4 | 8.56 | | | | | |
| three bots, calibrated | 78.8 | 8.71 | 26.3k | 229.7k | 58.7k | 21.5 | 31.9 |

A run's fight goes by the line typed (`--salt`): the kernel's random numbers come of the typing's
timing, and the game's of them, so one typing can differ from another by several percent. In that
one-bot run a server's frame was 87 thousand instructions of bots, 25 of the game and 35 of telling
the clients, and the client's helper made 27 items a frame in 15 thousand instructions.

`sv_fps` is 10 and `bot_thinktime` 200 here. At `fixedtime 33` the server's worker is busy all the
time, since each client frame is 33 ms of the game whatever it takes.

From the command to the first frame the network coder's table is not built (messages go as their
bytes), the bots' script reader copies a word and not a kilobyte, freed memory is not filled, a string
is not padded, the pak is not summed, the bots' answers to chat are not read.

`q3stat:` lines every 200 frames (`QUAKE3_STATS_FRAMES=N`; 0: never) say where a frame went.

    QUAKE3_HOLD=N         keep frame N on the screen and stop there ("quake3: holding frame N")
    hold                  the console command: the frame after next stays
    QUAKE3_SERVER=inline  the server on the first core
    QUAKE3_SERVER=frame   on its worker, taken and given out only where the first core's frame calls it,
                          and every frame of the game told to the clients
    QUAKE3_SERVER=paced   the worker's jobs made by the first core where they are given out, as the worker's
                          (its stack, its random numbers): RVC_CALLS, which counts the first core, sees them
    QUAKE3_ROUTES=late    the bots' routes made when a bot first asks, not while the level loads
    QUAKE3_ROUTES=all     the routes to every area made ahead
    QUAKE3_SEED=N         the first core's random numbers from N, not the kernel's (two builds compared)
    QUAKE3_BOTS=serial    bots that think in the same frame think one after another on the server's core
    QUAKE3_ENTER=MS       the player enters the game at that time of it, not when its load ends (sums compared)
    QUAKE3_TRACES=all     the server asks the level every trace again
    QUAKE3_SERVER_LOG=1   a q3srv: line for every job of the server's worker ("The server's pace")
    QUAKE3_CLIENT=inline  the client's game on the first core, drawn as it is made
    QUAKE3_SCENE=inline   a frame drawn by the first core ("The scene on a worker core")
    QUAKE3_SIDE=inline    the level's items made by the client's game itself, not on a helper core ("The client's frame on three cores")
    QUAKE3_BODIES=1       a body that is not drawn is made as one that is (the player's own, one out of view)
    QUAKE3_SUM=1          a sum over what each frame of the client's game asked to have drawn, with its time,
                          and one over the GPU's list of the frame ("list"; places in the ROM as offsets);
                          with the server on the first core a q3srvsum: line a frame of the game ("The
                          server's frame made shorter"), QUAKE3_SUM=2 every word of it too
    QUAKE3_SIZE=WxH       the window (640x480)
    QUAKE3_SEAT=1         a line every 20 moves: how it is seated, the aim, the body
    QUAKE3_WANTED=1       a line for every file, model, picture and sound asked for; nothing is "missing"
    QUAKE3_DEFINES=-DSE_MEMPROF QUAKE3_LINK=-Wl,--wrap=memset,--wrap=memcpy   (to build.sh) who fills and copies how much
    QUAKE3_DEFINES=-DSE_PARTS            (to build.sh) a q3parts: line: the first core's frame by part (with QUAKE3_CLIENT=inline)
    QUAKE3_DEFINES=-DSE_CHECK_VERSIONS   (to build.sh) a state left where it is is compared with what would have been copied
    QUAKE3_SRC=DIR QUAKE3_BUILD=DIR QUAKE3_OUT=DIR   (to build.sh) a tree, objects and image of one's own (QUAKE3_TREE=keep: the tree as it stands)
    QUAKE3_DATA=DIR QUAKE3_MEDIA=DIR     (to build.sh) the baked files' folder; where libmedia.a is ($WORK/src/libmedia)
    QUAKE3_BAKE="--pictures png"         (to build.sh) every picture a PNG (`raw`: as texels); --quality N the JPEGs' (95), --sheet N a sheet's texels' bytes
    QUAKE3_DECODERS=N     worker cores opened for decoding pictures besides the game's (24)
    QUAKE3_PICTURE_WORKERS=N   the most a sheet is decoded on (0: this core alone)
    QUAKE3_PICTURES_LOG=1      a q3pic: line for every sheet decoded: its size, kind, workers and this core's instructions
    QUAKE3_CORES=1        each worker's core and instructions a pass with every q3stat

## In a holodeck

The room draws the frame's list itself (`docs/world/holodeck.md`), so its pass must read what the
GPU's reads: `GpuVolumePass.cginc` has the ROM's four textures (the materials get them from
"ShaderEmu/Give the holodecks' world the ROM"), and `volume_vertex` in `src/gpu.h` moves a
texture and places a light map as `gpu_vertex_sized` does. The level is sent whole whatever the
camera sees, up is z (`seglWorldUp`), and the right hand aims and holds the weapon
(`docs/world/holodeck.md`, "Played from the seat"; `SE_SeatMove`, `SE_SeatView`).

## The server on a worker core

As Quake's (`docs/games/quake.md`), and easier in that the two are a server and a client already:
the packets a client sends wait for the server's next frame, and the ones the server sends go
when the first core takes the frame. It is on the first core while a level loads or anybody is
not yet in the game.

What several cores would write there is one each (`quake3.patch`): a command's words, the
marks of what a trace has tested and the box a thing is clipped as, the buffers `va()` and
`Info_ValueForKey` return, a zone for the server's allocations. Console lines, commands and
cvar changes made on the worker are kept and done by the first core; an error ends the job
and is raised there. Each module calls the engine by its own way in, not by which was called last.

`rand()` has a state for each core (`sys_shaderemu.c`), and the engine's counts of traces,
points and message bits are not kept. Not looked at: `COM_Parse`'s one token buffer.

## The client's game a frame ahead

The game's part of a client's frame is a worker's (`client_shaderemu.c`). It writes what it would
have drawn into a list (`frame_list_t` in `tr_shaderemu.c`: entities and polygons as they were
added, scenes and flat pictures in their order), and the scene's worker makes the GPU's commands of
the list later.

- **A frame is asked for before the last is taken.** The first core asks for frame n + 1 with
  the time, the latest move and the latest word of the server's as they are then, and takes
  frame n; the worker goes from n to n + 1 without being told. It writes four lists in turn
  (`SE_LISTS`): one being written, one taken, two drawn.
- **What it reads of the first core's is as it was when the frame was asked for** (which move
  and which snapshot are the latest: `SE_ClientPinned`). **What it would write there is
  kept** and done when the frame is taken: sounds (`snd_shaderemu.c`), console lines,
  commands, cvars and commands to the server (`server_shaderemu.c`), its two words for the
  next move. An error ends the frame and is raised on the first core.
- **The first core's it still is** while a level loads, for the first four frames of a game,
  and in any frame that has a command of the server's to run (a print, a config string): the
  worker's frames are waited for and taken first, and what the first core calls of the
  client's game (a console command, a key) waits for them too (`SE_ClientSettle`).
- `SE_OnWorker()` says which core by where the stack is: 0, 1 the server's, 2 this.

And each side's part:

- **A state that is where it would be copied to is not copied.** Each state kept has a number
  (`se_entity_versions` in `cl_parse.c`; one that came unchanged has the number of what it came
  from), and the last three copies are left out where the number is the one already there. Not
  players' or events', which the game changes in place. `-DSE_CHECK_VERSIONS` compares instead of
  trusting the number.
- An item out of view is not made ready and added (`SE_OutOfView` in `cg_ents.c`, the
  renderer's four sides and 128 units to spare); a thing that lies still is where it is
  without the two general calls; `AnglesToAxis` of no angles is the identity; a trace through
  the level asked for again is answered as before (`SE_LevelTrace`: a player who stands makes the
  same ones every frame; one who moves gains nothing by it); the status bar's numbers are not made
  by `sprintf`.
- **The renderer**: a stage's state is made when the file is opened and a model's surfaces and
  stages are found once (`model_way`), a thing's matrix is written where the GPU reads it
  (`gpuModelviewBlock`), a field of view's tangent, sine and cosine are kept, the level's
  draws that change are a list, `getenv` is not asked every frame.

**Checking it.** `QUAKE3_SUM=1 QUAKE3_SERVER=inline quake3 +set fixedtime 33 +devmap ...`
prints a sum of what every frame asked to have drawn, with the frame's time; `tools/test_all.py`'s
`quake3-workers` and `quake3-inline` compare them. Which frame the first snapshot comes in depends
on the machine's speed, and the shader machine's floats are not `rvc_cpu`'s, so its sums are compared
with its own.

## The server's pace

What moves in the level moves at the server's pace: the bot, the items, a hit, a door. The
game wants ten frames of it a second (`sv_fps`). `QUAKE3_SERVER_LOG=1` prints a `q3srv:` line a
job: when it was posted, begun, ended and taken (the machine's clock), the game's time it was given,
its frames and instructions, and the parts' (the bots, the game, telling the clients, the client's
moves and how many).

(`server_shaderemu.c`, `quake3.patch`):

- **The bots' routes are made ahead.** A bot that chooses an item asks the way to every item of
  the level at once, and the way to a place is worked out for the whole level the first time
  anybody asks (`AAS_UpdateAreaRoutingCache`). Asked in play, nothing of the level would move for
  seconds. Two sets (a second one for the rocket jump) are made by the server's worker while the
  client loads the level (`SE_ServerWarm`, from `CL_InitCGame`), when a bot is there by then, as it
  is from the menu and from `+addbot` on the command line.
- **A frame is taken and the next given out where the first core finds it ended**, in every
  pass it waits for the client's worker and at a frame's parts (`SE_ServerPoll`). Not when the frame
  kept a line, a command or an error for the first core: that waits for the frame's own call.
- **Of several frames of the game in one job only the last tells the clients**
  (`se_server_quiet`). A job takes up to `JOB_MSEC`, 500 ms of the game, five frames
  (`server_shaderemu.c`; the server on the first core takes `MOST_MSEC`, 200, a time). In the frames
  that tell nothing the bots' snapshots are still made (`SE_SendBotSnapshots` in `sv_snapshot.c`),
  so a bot sees each frame what it would see told, as with the server inline.
- **The level's answer to a trace straight down is kept** (`SV_Trace`, 16 of them, by the
  trace's own numbers): a player who stands asks the same two with every move, and the client sends
  a move every frame of its own. A player who moves gains nothing by it.

A turn or a step is the client's own and is on the screen two of its frames after the key; what the
server answers (a hit, an item taken) waits for the job in flight, then a job of its own and the frame
that shows it.

The server on its worker has no sum to compare: which frame a move lands in is the machine's speed.

Not done:

- **A player who moves.** A move is then the game's whole step, several traces of the level.
  `pmove_fixed 1` with `pmove_msec 33` is the game's own way to at most 30 steps a second on both
  sides. Not tried.
- **A route to a place nobody went to before** is still worked out in the frame that asks: a bot
  that follows a player asks for one for each area the player comes to. All of them could come from
  `bake.py`, as the file the library reads (`AAS_ReadRouteCache`).
- **A bot added by hand later** (`addbot` at the console) asks for its routes itself.

## The scene on a worker core

A worker makes the GPU's list of the client's frame (`tr_shaderemu.c`): the first core records its
frame as the client's worker does (a clear, its own pictures and scenes, and which of the client's
lists comes where) and gives the record out. The job is all of the frame's drawing but the swap: the
level's kept draws, the models, the pictures, the sum. How it goes from frame to frame: "The client's
frame on three cores".

- **What the job writes is its own while it is out**: the GPU library's state and the set being
  built, the renderer's view, matrices, light and moved textures kept. Anything else of the
  first core's that touches them waits for the job first (`SE_SceneFlush`: a level loaded or
  freed, a window resized, the renderer started or stopped, a held frame, `SE_Settle`). The
  light a model is lit by is kept by the drawing core; the client's game asks without keeping.
  The frame's counts (`se_counts`) are the job's to write and are read, never cleared, by the
  `q3stat:` lines; a job's sum is printed with `mcw_print`.
- **What it touches is there before**: the program's code is read and the lists, the GPU's two
  sets and the renderer's tables are written by the first core when the scene's worker is
  started. `q3stat:` says how many pages were brought for workers so far (0 in the run of "Speed").
- **The server and the client's game are calls too**: `mcw_call`, `mcw_ready` where the job's end
  is asked, `mcw_join` for the answer. Which worker has which is the library's; `SE_OnWorker()` says
  which part by its stack.

And what the client's frame does not make:

- **A body that is not drawn is not made**: the player's own in the first person (only for
  mirrors, which this renderer has none of) and one wholly out of view by 192 units that is not
  firing a beam. Its animation, sprites and the weapon's sounds and barrel are made as before,
  and the numbers its muzzle flash would have drawn from `rand()` are drawn (`CG_Player`,
  `SE_PlayerWeaponUnseen` in the patch).
- **A move made in a frame before is copied once**: of the moves taken from what was kept, only
  the last is copied into the predicted state (`se_pending` in `cg_predict.c`).
- **An angle of nothing asks no sine**: `AngleVectors` takes the sine of an angle that is zero
  as the angle and its cosine as 1, which is what `q_sin` and `q_cos` give, sign and all.

**Checking it.** The scene's sums and the GPU lists' must be the same with the scene on its worker
and with `QUAKE3_SCENE=inline`; with and without `QUAKE3_BODIES=1` the lists are the same and the
scene's sums are not (the bodies not made are not asked for). Two runs compared must be typed the same
length (`tools/test_all.py`'s `quake3-workers`, `quake3-inline`). `RVC_RACES`: nothing undone, only
the client's worker reading the moves and the frame asked for as they were.

## The server's frame made shorter

(`quake3.patch`, `server_shaderemu.c`)

- **A bot's snapshot is the numbers of what it sees.** The bot asks which entities its snapshot
  has and reads their states from the game (`SV_BotGetSnapshotEntity`, `BotAI_GetEntityState`),
  so their 208 bytes are not copied into the ring for it (`SV_BuildClientSnapshot`).
- **A bot's look at its snapshot copies only what it reads** (`BotCheckSnapshot`): an entity
  whose event the bot has seen and that is no missile is passed over without copying its state.
- **The game calls the engine directly** for what it asks by the dozen, as the client's game
  does (`g_syscalls.c`: traces, contents, links, the bots' library's entities, clock, inputs and
  characters).
- A snapshot's walk over the level's entities steps a pointer, not two calls and a product.
- `MSG_ReadString` and its two kin keep a string for each core: the server's worker and the
  first core read messages at once.

Nothing of the frame is split over more workers but the bots' thinks: the bot thinks on what the
player's moves and the game's frame wrote, and they on its command; the player's snapshot reads what
the frame wrote and the next moves write the client's record it is made from.

**Checking it.** With the server on the first core, `QUAKE3_SUM=1` prints a `q3srvsum:` line after
each frame of the game, a sum over every linked entity's state and every player's (its ping, which is
the clock's, left out), on standard output and not the console (its lines are drawn and would be in
the lists' sums). `tools/test_all.py`'s `quake3-server` runs `QUAKE3_SEED=7 QUAKE3_SERVER=inline
QUAKE3_CLIENT=inline QUAKE3_SCENE=inline` with one bot to frame 700: 223 lines, digest `aaa1379a`.
The server inline draws its random numbers as on its worker, from a state of its own (`se_rand_core`
in `sys_shaderemu.c`); a command of the game's draws from the server's (`SE_ServerCommandDraws`).

Chaining jobs (the next given out while the worker makes the last frame of one) loses: rounds become
a frame each, and every frame tells the clients. `QUAKE3_ROUTES=all` gains nothing in play; the bot
asks no route in the frames measured already.

`QUAKE3_SERVER=inline` with the client's game and the scene on their workers has been seen to stop
with `CG_ProcessSnapshots: n < cg.latestSnapshotNum`, in some builds only. Not explained.

## Bots that think at once

- **All bots think in the same frame** (`BotScheduleBotThink`: every one's residual 0), and
  `BotAIStartFrame` hands the list to `SE_BotThinks`: the server's core makes the first, and up to
  `SE_THINKERS` (3, `q_shared.h`) helpers one each. From the server's worker the helpers are jobs the
  first core gave out with its job, waiting a pass at a time from job to job (a job's end does not wait
  for them); from the first core (`QUAKE3_SERVER=inline`, `paced`) they are calls (`mcwx.h`). A think
  beside the server's runs on a stack of its own, and `SE_OnWorker()` says 3 to 5 there: whatever the
  engine keeps one of for each core has one for these too.
- **What one think wrote and another read is each bot's or each core's**: a bot draws its own
  random numbers and what it says or asks as a client (`say`, `use`) is done after all the thinks,
  in client order (`SE_BotCommandKept`); the server's commands are made into console messages
  before the thinks (`BotAIServerCommands`) and a message a think frees goes back after them
  (`SE_BotFreesDone`); the last teleport a bot saw, its inputs (16 bytes each), the AI's node
  switches and the routing scratch are each bot's or core's; a route found is only read, and a route
  a helper makes is its own, in chunks the server's core gives it before the thinks
  (`AAS_SE_ThinkPools`). A bot that thinks alone draws and says as it always did.
- **What a think saw is kept for the think** (`BotEntityVisible`): nothing moves while it thinks.

**Checking it.** `tools/test_all.py`'s `quake3-bots` and `quake3-bots-serial` (`--long`): three bots,
`QUAKE3_ENTER=1000`, the server's sums the same with the thinks at once and one after another. Without
`QUAKE3_ENTER` they part by the line typed: the player enters at a time that goes by how long the client's
game took to load, and everything follows. Builds are compared with one bot or with `QUAKE3_ENTER`:
`RVC_RACES` nothing undone. A fault in the program says where (`fault` in `sys_shaderemu.c`).

Not done: the bots' moves are made one after another in the game's frame (they meet in each other's
traces). The routes a helper makes are not shared with the others, and their chunks are freed only with
the level.

## Traces and messages made cheaper

Each gives the answer it gave (`quake3.patch`):

- **A leaf lists its patches alone** (`cm.se_leafpatches`, made as the map loads). `CM_TraceThroughLeaf`
  keeps the core, its marks and the leaf's lists in registers and tests a brush's box inline; the core
  is asked once a trace (`tw.se_w`), and `CM_Trace` clears only what is read before it is set.
- `CM_PointContents` makes the point's box once, not for every brush; `SV_AreaEntities_r` keeps
  the box in registers and takes the entity from `gEnt`; `SV_Trace` does not clear its clip.
- **A Pmove keeps its ground's trace and the water's contents** for the same origin
  (`se_ground`, `se_water` in `bg_pmove.c`): the end of a step asks what the next step asks.
- **The bots' library**: `AAS_TraceClientBBox` goes on to a node's child without the stack when
  the piece lies on one side; an area with no entity is passed without a box and a cleared trace
  (`AAS_AreaEntityCollision`). A bot's look at an entity asks the contents at its eye once a
  think and at the entity's middle once for both uses (`BotEntityVisibleNow`).
- **A message's bits go in and come out at once** (`MSG_ReadBits`, `MSG_WriteBits`): the coder
  is the identity here (`huffman.c`), so a field is a shift of its bytes, not a call a bit.
- A routing cache is filled before it is put in the shared list (`AAS_GetAreaRoutingCache`,
  `AAS_GetPortalRoutingCache`): a think on a helper beside could find one there half made.

Most of what is left is the bots' moves (`BotMoveInDirection`: `AAS_TraceClientBBox` and contents),
then the player's moves.

## The client's frame on three cores

(`tr_shaderemu.c`, `client_shaderemu.c`, `gpu_shaderemu.c`, `cg_ents.c` in `quake3.patch`)

- **The scene's job goes from frame to frame** (`scene_job`): it draws every frame the first core
  has recorded, one after another, and waits up to 8 passes for the next before it ends
  (`SCENE_LINGER`); the first core gives it out again where it waits (`SE_ScenePoll`) when it
  ended with frames left. The swap is in two halves (`gpu_shaderemu.c`): `gpuEnd` on the scene's
  core keeps the frame's set, commands and passes and turns to the other set, `gpuShow` on the
  first core submits it. A frame is written to a set once the frame two before, drawn from it,
  is shown and the GPU has no list waiting (`gpuIdle`).
- **A frame is shown once it is drawn**, from wherever the first core waits, and the first core
  waits only for the frame before last, whose list it records the next frame into: it has three
  lists in turn (`CORE_LISTS`), and the client's game four (`SE_LISTS`).
- **The level's items are made on a helper core** (`SE_SideItems`, `SE_SideFits` in `cg_ents.c`):
  the client's game asks for the frame's items once the view is known, a helper core makes them while
  it goes on with the players, the effects and the flat pictures, and it waits for the helper at the
  frame's end (`side_join`). In an item's place the scene has a placeholder (`SIDE_PLACE`), and
  the renderer draws and sums the helper's entities there (`side_of`), so the lists and the sums
  are the same. An item reads only its own entity and what the frame set before it (the time,
  the view, the items' turn), and writes only its own entity and what it adds; the places it
  gives its sounds are done by the client's game when it joins (`SE_SoundSideReplay`). Items with
  a sound or a light of their own stay the client's game's. The helper is a job the first core
  gives out with the client's game's first and keeps out (`side_job`, joined at `SE_ClientReset`);
  `SE_OnWorker()` says `SE_SIDE` there (6, `SE_CORES` is 7), and a `q3stat:` line says how many
  items it made, its instructions and the passes the client's game waited for it.

The client's frames a second and the server's pull against each other: a frame of the client's sends
one move, and the server makes each, so more client frames are more moves a frame of the game.

**Checking it.** `tools/test_all.py`'s `quake3-workers` and `quake3-inline`. Held frames
(`QUAKE3_HOLD`, `rvc_cpu --save-ram`, drawn by `tools/gpu_reference.py`'s model) must be the same in
every pixel with `QUAKE3_SIDE=inline`. `map q3dm17` typed at frame 400 with two bots loads the level
again and the game goes on.

Wherever the first core waits for a job, it polls that job's helpers too (`helpers_poll`,
`SE_ClientPoll`): a helper stopped at a page otherwise stops the game. The helpers are joined before
a frame of the server's made on the first core (a level loading again, somebody joining), where a
bot's thinks are the first core's calls.

Not done: the models made ready a frame ahead on a core of their own; the players on the helper (they
draw random numbers, make local entities and sounds in their order, which a core beside would change).

## Pictures decoded when a level loads

The pictures are files in `quake3-se.dat`, decoded with libmedia (`docs/system/media.md`) on the worker
cores into GPU memory when the game registers what draws with them, and drawn from there.

- **In sheets.** `bake.py` lays the textures of one kind and width one under another (rows of
  a texture are then rows of the sheet, as the GPU reads them) in sheets of 512 KB of texels
  at most, and writes each sheet as one file with libmedia's encoder (`q3pack`): JPEGs of
  the opaque pictures (quality 95, colours at full size, restart markers) and PNGs of those
  with alpha (in segments; each texel's red, green, blue and transparency, so that the GPU's
  words come out as they were). A JPEG texture's rows begin on its 8 x 8 blocks. `bake.py` reads
  every sheet back with the same library: a PNG that does not come back texel for texel is left as
  texels, and it says so. The light maps stay as texels in the ROM, where the lit draws read them.
  A sheet is decoded in one call on every core: one file a picture costs far more in rounds of
  workers and mappings.
- **When.** A shader, model or skin registered on the first core has its sheets decoded then;
  one registered on the client's game's worker is asked for and decoded by the first core at
  its next frame's end (`pictures_take`). The level's own runs are decoded in `RE_LoadWorld`
  before their kept commands are made. `stage_kept` is made again for the stages of a sheet
  decoded, or whole at the frame's end when the scene's job was out. The menu and the level each
  decode their own sheets; they are kept until the window closes (the file has one level).
- **Where.** The display's own framebuffer first (`seglMemorySpare`, 4,032 KB while the desktop
  is layers), then `seglMemory` (2,625 KB). All 19 sheets are 6,492 KB: a sheet with no room is not
  decoded and its stages are drawn in their colours, and the line `quake3: pictures decoded so far`
  says how many were not (0 in the run of "Speed", on 31 workers).
- **On which cores.** The program opens 24 more worker cores than the game's
  (`QUAKE3_DECODERS`), and a sheet is decoded on all that have no job out (`mcw_for`'s teams).
  They are not parked in a game: a call goes to the lowest free worker of the large write caches
  (`call_pick` in `mcwx.c`), so the game's jobs keep to the big cores by themselves
  (`docs/machine/multicore.md`, "Packing the small workers"). `QUAKE3_CORES=1` prints each worker's
  core and instructions a pass with every `q3stat` (the geometry's words first), and
  `QUAKE3_SERVER_LOG`'s lines say the server job's worker and core.
- **The game's workers are lent to libmedia**, and the server's may be making the bots' routes:
  the library steps round a busy worker on lent ones and never posts to one by number
  (`media.md`, at the top). Every sheet is shared out over the workers that are free; a sheet under
  32 rows is the first core's alone.

**Checking it.** Held frames (`QUAKE3_SEED=7`, the server, the client's game and the scene on the
first core, `QUAKE3_SUM=1`, `QUAKE3_HOLD`), saved with `rvc_cpu --save-ram` and drawn by
`tools/gpu_reference.py`'s model from the RAM and the image's ROM. With `--pictures png` the texels
are those of the demo's pictures as baked; JPEGs differ by a few levels. `RVC_RACES` from the command
to frame 300: nothing undone, the waits on libmedia's team words and the client's moves "as it was".
`RVC_ADFAULT=1`: no stop on a written bit.

Not done: the pictures in the world or in a holodeck (`GpuVolumePass.cginc` reads GPU memory as
for Quake's textures, not tried with these). Without the display's spare memory (a display that
is not layers) most sheets would not fit. libmedia: one picture at a time in a program (its decoders
keep their state in statics, and map their memory with system calls, so a worker cannot decode a
picture of its own beside another).

## The server's frame told and linked cheaper

Each gives the answer it gave (`quake3.patch`, `server_shaderemu.c`):

- **An entity the same as in the frame the snapshot is told against is not compared again**
  (`se_snap` in `sv_snapshot.c`): `SV_BuildClientSnapshot` copies its state into the ring and
  compares it with the old frame's in one loop, and `SV_EmitPacketEntities` writes nothing for it
  where `MSG_WriteDeltaEntity` would have written nothing.
- **The entities linked and not hidden from all are listed once a time** (`se_sent`): the first
  snapshot of a `SV_SendClientMessages` walks them all, the others walk that list, and the area
  test is inline on the flood numbers (`CM_SE_Floods`).
- **A link of an entity whose box is as last time keeps its leaves** (`se_boxed`, `se_absmin`,
  `se_absmax` in `svEntity_t`); `SV_AreaEntities` walks the sector tree without recursion, in the
  same order.
- **The bots' library is told an entity whole only when its state changed** (`SE_BotLibTell` in
  `ai_main.c`); one unchanged is only seen again (`AAS_SE_SeenAgain`, what `AAS_UpdateEntity`
  does for the same state). The level's name for the map scripts is read once, not every think.
- **Each client's view keeps what it found of an entity's areas and clusters** (`se_seen` in
  `sv_snapshot.c`) while the view's cluster, its area, the level and the areas' flood numbers
  (`CM_SE_FloodGen`) are as they were and the entity's leaves were not made again (`se_gen`,
  raised by `SV_LinkEntity` before it makes them): the flags are still tested every time.
- **A list of entity numbers found in order is not sorted** (`SV_BuildClientSnapshot`): without
  portals the walk finds them in order. A bot's numbers go into the ring without a remainder each.
- `BotAIStartFrame` does not clear a state it sets whole; the Pmove's kept ground and water and
  `SV_Trace`'s kept traces compare words, not `memcmp`.

What is left is the bots' thinks and moves, which are traces (`BotGapDistance`, `BotEntityVisible`,
the Pmoves), and the copy of the player's snapshot.

**Checking it.** The one bot's sums (`quake3-server`), and three bots' server sums with
`QUAKE3_ENTER=1000`. Held frames with the bot (`QUAKE3_HOLD`, `rvc_cpu --save-ram`, drawn by
`gpu_reference.py`'s model) the same in every pixel. `tools/test_all.py quake3 --long`, and with
`--races` nothing undone.

## Checking it

Hold a frame under `rvc_harness --cpu` with `--save-state` and `--gpu-capture`, then

    python tools\gpu_reference.py SNAP BMP --size 640x480 --rom build\images\NAME\rootfs.bin

and compare views of the level (`devmap`, `setviewpos`, `wait 90; hold`), away from triangle edges.
`hold` waits for the GPU. `QUAKE3_SEED` compares two builds' runs ("The server's frame made shorter").

## Not done

- Playing it by hand, and hearing it. The pointer turns the view while the left button drags it
  (`docs/machine/input.md`).
- A second character, the other three levels of the demo, the menu's setup pages.
- Marks and dynamic lights.
- The server's frame with a player who moves, and a bot's route to a place nobody went to before
  ("The server's pace").
