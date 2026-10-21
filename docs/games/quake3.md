# Quake III Arena

One level of Quake III Arena's free demo (q3dm17, The Longest Yard) runs in the Linux image
with a bot: ioquake3's source with the game, the client's game and the menus linked in, and a
renderer and a sound layer of our own. Everything on the screen is drawn by the machine's
GPU: its pictures from GPU memory, where they are decoded when a level or the menu loads, and
the rest where a file lies in the ROM.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh     (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/libmedia/build.sh  (once: docs/system/media.md)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/quake3/build.sh    (17 s; 25 s when it bakes; the first time it also fetches)
    python tools\make_linux_image.py

That is the image with the MMU. The world and `rvc_harness` with no arguments run Linux
without one, where the program is flat and its pools smaller (below):
`bash linux/nommu/build.sh quake3/build.sh`, then `root.sh` and `image.py` (`docs/system/nommu.md`).

In the guest: `nano-X -p & quake3 +map q3dm17 +addbot sarge 3`, or Quake III Arena in the
Start menu's Games, which opens the main menu. Lines typed at the terminal it was started
from are console commands. The demo's 47 MB pak is fetched by the build and not kept; what
goes into the image is 10.6 MB (15.8 before its pictures were JPEG and PNG files again).

The main menu is cut down to what works here (`q3_ui/ui_menu.c` in the patch): the level
against Sarge at three skills (I CAN WIN, HURT ME PLENTY, HARDCORE: `addbot sarge` 1, 3, 4)
and EXIT, which asks nothing. Its icon is ioquake3's (`linux/apps/icons/quake3.png`).
A menu's cursor is the desktop's pointer: `SE_Input` tells the menus where it is in the
window, not how far it went (`UI_MouseEvent` in the patch), and their own cursor is not drawn.

The game's picture keeps the size the window had when it started and is stretched to the
window as it is now (`picture_viewport` in `tr_shaderemu.c`); the pointer's place in a menu
and the sky's repeats go by the same two ratios. Before, a resized window showed the first
size's picture in its corner.

Not tried by hand: the keys, the pointer, the menus, the sound by ear. On 13 October 2026
it was run by typed commands only, and its pictures were compared with the GPU's model.

## What the build is made of (`linux/quake3`)

- `build.sh` fetches ioquake3 at a fixed commit, applies `quake3.patch`, and compiles the
  engine, the bots' library and the three modules with Quake's flags (`linux/quake/build.sh`).
  The modules are not libraries: each is linked to one object whose names stay inside it, and
  its variables are put back as they began each time it is "loaded" (`sys_shaderemu.c`).
- `bake.py` makes the two files the game runs from (below); `jpgraw.c` reads the demo's JPEG
  pictures for it, and `q3pack.c` (libmedia for the build computer) writes them as the game's
  JPEG and PNG files and reads them back; `wanted.txt` is what the game asked for in a run with
  `QUAKE3_WANTED=1`.
- `tr_shaderemu.c` is the renderer, `gpu_shaderemu.c` the GPU library (`programs/linux/gles.c`
  as it is, and draws whose every word the renderer says), `snd_shaderemu.c` the sound,
  `server_shaderemu.c` the server's frame on a worker core, `client_shaderemu.c` the client's
  game on another, `sys_shaderemu.c` the system. The three workers' jobs are calls of
  `programs/mc/mcwx.h` (`mcw_call`, `mcw_ready`, `mcw_join`; "The scene on a worker core").
- `quake3.patch` is `git diff -- code` of the tree at `build.sh`'s commit. A tree that is that
  already is left as it is, so that a build compiles only what changed (5 to 9 s; it was every
  file of the patch, 30 s while other builds ran).
- The zone, the server's zone and the hunk are each one anonymous mapping of its size
  (`SE_Pool` in the patch): 16, 8 and 32 MB. The no-MMU build (`linux/nommu/build.sh`) makes
  them 4, 6 and 8 MB, and its starter adds `+set com_hunkMegs 8 +set com_zoneMegs 4` over a
  kept `q3config.cfg` (`docs/system/nommu.md`, "What differs").

## The two files

`bake.py` reads the demo's pak and writes only what `wanted.txt` names:

| | |
|---|---|
| `quake3-pak0.pk3` | 2.5 MB, stored and not compressed: what the game reads itself (the level for collision without its light maps, the bots' navigation, their scripts) |
| `quake3-se.dat` | 8.0 MB: sounds 3.0 (halved to 11 kHz: `--sound-rate`), models 1.6, the level 1.4 (1 MB of it the light grid), pictures 1.3 (6.5 as texels; 13.3 MB the file when they were), light maps 0.4 |

- **Pictures** are decoded and halved to 128 texels a side (256 for the flat screen's) when
  baked: three bytes a texel, or words where a picture's alpha is used. Then they are files
  again ("Pictures decoded when a level loads"): JPEG, and PNG for the 87 with alpha. Light
  maps stay as texels.
- **Shaders** are the scripts' stages reduced to what the GPU has: one of four passes, a
  colour that may be a wave, a texture that may move. A reflection (`tcGen environment`) is
  left out, and what is blended over one is laid on its mean colour.
- **The level** is triangles sorted by shader stage, three vertices each, patches cut into
  4 x 4 pieces; a light map and the texture it lights are one draw. A texture with no script
  is lit by its surface's light map too, as the game does (until 13 October 2026 it was not:
  2,631 of the level's 23,550 vertices were lit, the walls and floors not among them; 17,052
  now), and on a surface with no light map it has the surface's own colours.
- **A model's pose** is a word a vertex (three bytes of place and which of 256 ways it
  faces), every frame of it, and its texture coordinates a word a vertex.
- **One character** (Sarge, for the player and the bot) and one level: `--bots`, `--map`.

## How it is drawn

- **The level is kept in GPU memory** (442 KB, 23,550 vertices in 75 draws that stay in every
  frame's list). A frame gives them its matrices and says what changed: a wave's colour, a
  texture's turn. Nothing of the level is looked for or sorted in a frame; the card's depth
  buffer hides what is behind.
- **Textures are decoded into GPU memory** when what draws with them is registered (below);
  **light maps and models stay in the ROM**, where the GPU reads them.
- **A model is lit by the GPU**: three texels a thing (where the light comes from, its
  colour, the light from everywhere), from the level's light grid, kept by where the thing is.
- **Sprites, beams and the game's polygons** are quads written into the frame; the flat
  screen is quads in the one pass that blends without depth, in the order they come.
- **A model is drawn in the nearer of its two frames**, and its tags are that frame's.

What the GPU was given for it (`docs/machine/gpu.md`): word textures and packed vertices in the ROM,
smooth three-byte textures, a draw lit by a light map (`0x800`), a clamped texture
(`0x1000`), a texture moved by two rows of numbers (`0x10000`), vertices lit by how they face
(`0x20000`). `rvc_backend11.cpp` bound the state texture to every texture of the vertex
shader; it now leaves the ROM's there. A world built before this draws the game wrongly.

Not as Quake III draws: no dynamic lights, no marks on walls or shadows, no fog, portals or
flares; no reflections; `tcMod turb` and vertex deforms stand still; rail rings are missing;
no mipmaps; the weapon can poke into a wall; a 2D shader's first stage only; no music or videos.

## Speed

Instructions a frame of the first core, on `rvc_cpu`, the player standing with 25 models in view:

| | A frame |
|---|---|
| everything on one core, two bots | 1,250k to 1,500k |
| the server's frame on a worker core | 370k to 400k |
| the client's moves not made again each frame, hot calls direct, no shadow traces | 210k to 250k |
| a pad the player is not near not traced | 172k to 188k |
| light kept by place, a thing's matrix written as rows | 168k to 185k |

With one bot, at 3 million instructions a second (computed from `rvc_cpu --ips 3000000`, not
measured on the card): a frame is 146 to 169 thousand, 18 to 20 frames a second; a frame of
the game on the worker is about 215 thousand (the bot 90 to 120, the game 33 to 51, telling
the clients 70), ten a second, 72% of that core. On the card on 13 October 2026, an earlier
build with two bots: 13 to 16.5 frames a second, passes 99.9% used, the worker's 93%.
On 19 October 2026 on an RTX 5070 laptop (`docs/machine/speed.md`), `+set timedemo 1 +set fixedtime 33
+devmap q3dm17 +addbot sarge 3`, frames 300 to 500: 76.6 frames a second on the shader machine (12.9
million instructions a second, 1.9 of them core 0's), 70.6 on `rvc_lean`, 112.9 on the compute
machine and 156.5 on the CPU harness.

- **Two bots are more than the worker has**: 335 to 380 thousand a frame of the game. The
  game then runs at about three quarters of the clock's rate (a job is 200 ms of it at most).
- `sv_fps` is 10 and `bot_thinktime` 200 here. The bot falls off the level once in 39 s
  of the game so; at 20 and 100 once in 62 s. Both were counted with the player standing still.
- From the command to the first frame: 127 million instructions, where it was 1,800. The
  network coder's table is not built (messages go as their bytes), the bots' script reader
  copies a word and not a kilobyte, freed memory is not filled, a string is not padded, the
  pak is not summed, the bots' answers to chat are not read.

`q3stat:` lines every 200 frames (`QUAKE3_STATS_FRAMES=N`; 0: never) say where a frame went.

    QUAKE3_HOLD=N         keep frame N on the screen and stop there ("quake3: holding frame N")
    hold                  the console command: the frame after next stays
    QUAKE3_SERVER=inline  the server on the first core
    QUAKE3_SERVER=frame   on its worker, taken and given out only where the first core's frame calls it,
                          and every frame of the game told to the clients (as until 13 October 2026)
    QUAKE3_SERVER=paced   the worker's jobs made by the first core where they are given out, as the worker's
                          (its stack, its random numbers): RVC_CALLS, which counts the first core, sees them
    QUAKE3_ROUTES=late    the bots' routes made when a bot first asks, not while the level loads
    QUAKE3_ROUTES=all     the routes to every area made ahead (426 million instructions, 1,984 KB)
    QUAKE3_SEED=N         the first core's random numbers from N, not the kernel's (two builds compared)
    QUAKE3_BOTS=serial    bots that think in the same frame think one after another on the server's core
    QUAKE3_ENTER=MS       the player enters the game at that time of it, not when its load ends (sums compared)
    QUAKE3_TRACES=all     the server asks the level every trace again
    QUAKE3_SERVER_LOG=1   a q3srv: line for every job of the server's worker ("The server's pace")
    QUAKE3_CLIENT=inline  the client's game on the first core, drawn as it is made
    QUAKE3_SCENE=inline   a frame drawn by the first core, as until 14 October 2026 ("The scene on a worker core")
    QUAKE3_SIDE=inline    the level's items made by the client's game itself, not on a helper core ("The client's frame on three cores")
    QUAKE3_BODIES=1       a body that is not drawn is made as one that is (the player's own, one out of view)
    QUAKE3_SUM=1          a sum over what each frame of the client's game asked to have drawn, with its time,
                          and one over the GPU's list of the frame ("list"; places in the ROM as offsets);
                          with the server on the first core a q3srvsum: line a frame of the game ("The
                          server's frame made shorter"), QUAKE3_SUM=2 every word of it too
    QUAKE3_SIZE=WxH       the window (640x480)
    QUAKE3_WANTED=1       a line for every file, model, picture and sound asked for; nothing is "missing"
    QUAKE3_DEFINES=-DSE_MEMPROF QUAKE3_LINK=-Wl,--wrap=memset,--wrap=memcpy   (to build.sh) who fills and copies how much
    QUAKE3_DEFINES=-DSE_PARTS            (to build.sh) a q3parts: line: the first core's frame by part (with QUAKE3_CLIENT=inline)
    QUAKE3_DEFINES=-DSE_CHECK_VERSIONS   (to build.sh) a state left where it is is compared with what would have been copied
    QUAKE3_SRC=DIR QUAKE3_BUILD=DIR QUAKE3_OUT=DIR   (to build.sh) a tree, objects and image of one's own (QUAKE3_TREE=keep: the tree as it stands)
    QUAKE3_DATA=DIR QUAKE3_MEDIA=DIR     (to build.sh) the baked files' folder; where libmedia.a is ($WORK/src/libmedia)
    QUAKE3_BAKE="--pictures png"         (to build.sh) every picture a PNG: the same texels as before (`raw`: as texels, as
                                         until 15 October 2026); --quality N the JPEGs' (95), --sheet N a sheet's texels' bytes
    QUAKE3_DECODERS=N     worker cores opened for decoding pictures besides the game's (24)
    QUAKE3_PICTURE_WORKERS=N   the most a sheet is decoded on (0: this core alone)
    QUAKE3_PICTURES_LOG=1      a q3pic: line for every sheet decoded: its size, kind, workers and this core's instructions

## In a holodeck

The room draws the frame's list itself (`docs/world/holodeck.md`), so its pass must read what the
GPU's reads: `GpuVolumePass.cginc` has the ROM's four textures (the materials get them from
"ShaderEmu/Give the holodecks' world the ROM"), and `volume_vertex` in `src/gpu.h` moves a
texture and places a light map as `gpu_vertex_sized` does. Before, nothing of the game had
a texture or a shape there. The level is sent whole whatever the camera sees, up is z
(`seglWorldUp`), and the right hand aims and holds the weapon (`docs/world/holodeck.md`, "Played
from the seat"; `SE_SeatMove`, `SE_SeatView`).
On 13 October 2026 the three shaders compiled in the editor; nobody had sat in it yet.

## The server on a worker core

As Quake's (`docs/games/quake.md`), and easier in that the two are a server and a client already:
the packets a client sends wait for the server's next frame, and the ones the server sends go
when the first core takes the frame. A frame of the game is run at a time, so the bots look
before each. It is on the first core while a level loads or anybody is not yet in the game.

What two cores would both write there is two of, three with the client's game's core (`quake3.patch`): a command's words, the
marks of what a trace has tested and the box a thing is clipped as, the buffers `va()` and
`Info_ValueForKey` return, a zone for the server's allocations. Console lines, commands and
cvar changes made on the worker are kept and done by the first core; an error ends the job
and is raised there. Each module calls the engine by its own way in, not by which was called last.

`rand()` has a state for each core (`sys_shaderemu.c`), and the engine's counts of traces,
points and message bits are not kept (each core added to the same ones: `RVC_RACES` found them
undone). Not looked at: `COM_Parse`'s one token buffer.

## The client's game a frame ahead

A frame of the client's was 154 thousand instructions of the first core with nobody but the
player in the level: the game's part 95 (the server's word taken in 27, the player's move
made ahead 18, every thing placed 35, the status 15) and the picture's 41. The game's part
is now a second worker's (`client_shaderemu.c`, 13 October 2026). It writes what it would
have drawn into a list (`frame_list_t` in `tr_shaderemu.c`: entities and polygons as they
were added, scenes and flat pictures in their order), and the first core makes the GPU's
commands of the list one frame later, while the worker is at the next.

- **A frame is asked for before the last is taken.** The first core asks for frame n + 1 with
  the time, the latest move and the latest word of the server's as they are then, waits for
  frame n and draws it; the worker goes from n to n + 1 without being told. Two lists in
  turn: a frame's list was last drawn from two frames before (three since the scene went to a
  worker: `SE_LISTS`, "The scene on a worker core").
- **What it reads of the first core's is as it was when the frame was asked for** (which move
  and which snapshot are the latest: `SE_ClientPinned`). **What it would write there is
  kept** and done when the frame is taken: sounds (`snd_shaderemu.c`), console lines,
  commands, cvars and commands to the server (`server_shaderemu.c`), its two words for the
  next move. An error ends the frame and is raised on the first core.
- **The first core's it still is** while a level loads, for the first four frames of a game,
  and in any frame that has a command of the server's to run (a print, a config string): the
  worker's frames are waited for and taken first, and what the first core calls of the
  client's game (a console command, a key) waits for them too (`SE_ClientSettle`).
- **Three cores have one each** of what two had: a command's words, a trace's marks and box
  (two cores' boxes no longer share 16 bytes), the buffers `va()` and `Info_ValueForKey`
  return. `SE_OnWorker()` says which core by where the stack is: 0, 1 the server's, 2 this.

And each side's part was gone through:

- **A state that is where it would be copied to is not copied.** An entity's state went from
  the packet to the client's ring, to the game's snapshot, to the entity's next state and to
  its current one: 208 bytes four times for every thing in every snapshot, and most things of
  a level never change. Each state kept has a number (`se_entity_versions` in `cl_parse.c`;
  one that came unchanged has the number of what it came from), and the last three copies are
  left out where the number is the one already there. Not players' or events', which the
  game changes in place. `-DSE_CHECK_VERSIONS` compares instead of trusting the number.
- An item out of view is not made ready and added (`SE_OutOfView` in `cg_ents.c`, the
  renderer's four sides and 128 units to spare); a thing that lies still is where it is
  without the two general calls; `AnglesToAxis` of no angles is the identity (an item's second
  model asked six sines and cosines for it); a trace through the level asked for again is
  answered as before (`SE_LevelTrace`: a player who stands makes the same ones every frame;
  one who moves gains nothing by it); the status bar's numbers are not made by `sprintf`.
- **The renderer**: a stage's state is made when the file is opened and a model's surfaces and
  stages are found once (`model_way`), a thing's matrix is written where the GPU reads it
  (`gpuModelviewBlock`), a field of view's tangent, sine and cosine are kept, the level's
  draws that change are a list, `getenv` is not asked every frame (it was, twice).

Instructions a frame on `rvc_cpu` (8,192 a pass, 3 million a second), `devmap q3dm17`, the
player put at 488 -968 378 looking north (`setviewpos`), 22 models in 38 draws:

| | The first core | The client's worker | Frames a second |
|---|---|---|---|
| before, the level alone | 152k to 155k | | 17.7 to 18.1 |
| after, `QUAKE3_CLIENT=inline` | 100k to 103k | | 26.7 to 27.3 |
| after | 51k | 45k | 52.3 to 52.6 |
| before, with the bot (`addbot sarge 3`) | 163k to 174k | | 15.8 to 16.9 |
| after, with the bot | 53k to 58k | 50k to 54k | 43 to 46 |

On the shader machine (an RTX 5090, D3D11, through the card queue, before and after in turn,
frames 400 to 700 of the same scene). The level alone: 100 frames in 4.33 to 4.36 s before,
23 frames a second; 2.02 to 2.12 s after, 47 to 50. With the bot: 4.9 to 5.5 s before, 18 to
20 frames a second; 2.29 to 2.39 s after, 42 to 44, and 3.1 to 3.9 s in the windows the
bot's part is long in (below): the 300 frames in 15.2 and 15.4 s before, 8.1 and 8.6 after.
The first core's passes are 99.9% used and it rests in a third of them, the worker's
frame being the longer of the two there; a full write cache ended 5 and 7 passes of 3,300
where it ended 96 and 111 of 6,200.

With the bot the server's worker had a frame of the game of one to nine million instructions
now and then (the bot's part, as before): the level stood still for a second or more, and
the client's frames of that time were longer (a moved player's moves are made again). What
it was, and the server's pace since: "The server's pace" below.

On `rvc_cpu` a worker sees a store at once, and a worker that goes to sleep in the pass a job
is posted in has not seen it and sleeps on (the shader machine's sleeps on the job word of
the pass before, and wakes): `client_shaderemu.c` posts again after eight passes of nothing.
The server's worker has the same there, rarely: its jobs are not posted at once. (Not posted
again any more: `rvc_cpu` wakes such a worker, "The scene on a worker core".)

**Checking it.** `QUAKE3_SUM=1 QUAKE3_SERVER=inline quake3 +set fixedtime 33 +devmap ...`
prints a sum of what every frame asked to have drawn, with the frame's time; run once with
`QUAKE3_CLIENT=inline` and once without, the sums of the same times must be the same. On
`rvc_cpu`: 522 of 522 standing, 598 of 598 jumping and turning (`+set go "+moveup " ... +vstr
go`). Which frame the first snapshot comes in depends on the machine's speed, and a run that
began the other way has other times for a while and a few frames of other sums: run it
again. The shader machine's floats are not `rvc_cpu`'s, so its sums are compared with its
own: 572 of 573 there, the one being the first frame the worker makes, which the first core
has made already (a model's pose goes a step on when a time is made twice).

Pictures, held with `QUAKE3_HOLD` under `rvc_harness --cpu` (`QUAKE3_SERVER=inline`, `+set
fixedtime 33`, two views): the build before at frame 700 and this one at frame 702 are the
same in every pixel of the GPU's target (the worker's frame is shown a frame later, and
starting the workers takes a frame more), and so is this one with `QUAKE3_CLIENT=inline` at
700. Against the GPU's model: 100.000% within 2 levels away from triangle edges, 128
commands as before.

Tried and not kept: a job a frame, posted when the last was taken (the worker waits two
passes of every frame for the first core to see and to post: 200 frames in 4.37 s on
`rvc_cpu`, which has one of the two passes, against 4.32 asked for ahead; on the card that
build, without the numbered states, made 100 frames in 2.41 to 2.57 s). The first core
still waits 2.3 passes a frame on the card: not looked into. Not tried: the picture's part on a worker too (a worker keeps
half the first core's stores a pass, and the commands are all stores; done since: "The scene on
a worker core"), the server's
snapshot handed over without the packet, the bot's part on a worker of its own.

## The server's pace

What moves in the level moves at the server's pace: the bot, the items, a hit, a door. The
game wants ten frames of it a second (`sv_fps`), and a job of the worker's makes up to 200 ms
of them, so a job that takes longer loses the rest and the game goes slower than the clock.
`QUAKE3_SERVER_LOG=1` prints a `q3srv:` line a job: when it was posted, begun, ended and
taken (the machine's clock), the game's time it was given, its frames and instructions, and
the parts' (the bots, the game, telling the clients, the client's moves and how many).

On the shader machine (an RTX 5090, D3D11, through the card queue, a pass 2.1 to 2.2 ms), the
bot's scene of above, frames 200 to 1,000, on 13 October 2026. "Before" is the build of 12
October and "that morning" this day's commit, one run each; the others four runs each, in turn:

| | Frames a second | Jobs a second | Frames of the game a second | The game's time, of the clock's | A job, posted to taken: median, 90th, longest |
|---|---|---|---|---|---|
| before | 22.9 | 7.4 | 8.4 | | |
| that morning | 46.3 | 7.5 | 8.7 | | |
| the same by this build's switches | 40.1 to 44.0 | 5.7 to 7.2 | 8.8 to 9.1 | 88 to 91% | 100 to 152 ms, 180 to 217, 2,300 to 2,530 |
| that, `QUAKE3_CLIENT=inline` (one run) | 31.1 | 7.8 | 9.0 | 90% | 98, 160, 2,077 |
| now | 43.3 to 46.9 | 9.8 to 9.9 | 10.0 to 10.1 | 100% | 70 to 74 ms, 92 to 118, 243 to 274 |

The level alone was at ten a second before and is (a job 171 thousand instructions, 130 now).
The worker runs 3.3 to 3.7 million instructions a second inside its jobs, 92 to 95% of its
passes: it is not held by its 1.5 KB of stores a pass, it has no more instructions. On
`rvc_cpu` at 3 million a second: 8.2 to 8.7 frames of the game a second and 82% of the clock
with the switches, 10.0 and 100% without; the longest job 3,234 ms and 322.

What held it, and what was done (`server_shaderemu.c`, `quake3.patch`):

- **The long frame was the bots' routes.** A bot that chooses an item asks the way to every
  item of the level at once, and the way to a place is worked out for the whole level the
  first time anybody asks (`AAS_UpdateAreaRoutingCache`: 280 to 450 thousand instructions a
  place of q3dm17's 570 areas, 21 places for 29 items). That was 9 million instructions in
  one frame of the bots': 2.1 to 2.5 s in which nothing of the level moved (3.2 s at 3
  million a second), once when the bot came and once more when it first had the rocket
  launcher and the health for a rocket jump, which is another set of ways. Both sets are now
  made by the server's worker while the client loads the level (`SE_ServerWarm`, from
  `CL_InitCGame`: 17 million instructions, 69 KB), when a bot is there by then, as it is from
  the menu and from `+addbot` on the command line. The time to the 100th frame is the same
  (26.7 to 27.0 s against 26.6 to 28.4): the first set was made in those frames before.
- **A frame is taken and the next given out where the first core finds it ended**, in every
  pass it waits for the client's worker and at a frame's parts (`SE_ServerPoll`), not once a
  frame of the client's: 15 ms a job to 4 or 5. Not when the frame kept a line, a command or
  an error for the first core: that waits for the frame's own call, as before.
- **Of several frames of the game in one job only the last tells the clients**
  (`se_server_quiet`): they got the others in the same moment, and telling is 70 thousand
  instructions of a frame's 200. It is what a client that asks for fewer snapshots gets.
- **The level's answer to a trace straight down is kept** (`SV_Trace`, 16 of them, by the
  trace's own numbers): a player who stands asks the same two with every move, 4,450
  instructions each, and the client sends a move every frame of its own. A move of a
  standing player was 16 to 18 thousand instructions of the server's, 6.2 to 7.6 of them a
  job since the client's frames doubled where `QUAKE3_CLIENT=inline` has 3.7: it is 11.2
  thousand now. A player who moves gains nothing by it.

So the client's worker did slow the server, by the moves: twice the frames are twice the
moves, 100 to 121 thousand instructions a job against 65. Nothing else the two workers share
was found to cost: a pass is as long with the client's worker (2.11 to 2.14 ms against 2.11).
The server's worker's page stops and system calls were not counted; its passes being 92 to
95% used leaves little for them.

A key's way to the screen, counted from the code and the times above and not measured: a turn
or a step is the client's own and is on the screen two of its frames after the key (the frame
that reads it asks the worker for the next, which is drawn a frame later), 43 ms at 46 frames
a second, where it was one frame of 44 ms. What the server answers (a hit, an item taken)
waits for the job in flight, then a job of its own and the frame that shows it: about 100
to 190 ms now, 160 to 350 that morning.

**Checking it.** `QUAKE3_SUM=1 QUAKE3_SERVER=inline ... +set fixedtime 33` as above, on
`rvc_cpu`: the sums are this day's commit's (574 of 574), with `QUAKE3_TRACES=all` and
without (574 of 574), on the client's worker and not (573 of 573). The server on its worker
has no such sum: which frame a move lands in is the machine's speed.

Not done, with what was measured for it:

- **A player who moves.** A move is then the game's whole step, several traces of the level
  at 4 to 5 thousand instructions each: not measured here, by that count 30 thousand or more
  a move, and at 45 moves a second about a third of the server's core. With it the server is
  short of its ten frames again. `pmove_fixed 1` with `pmove_msec 33` is the game's own way
  to at most 30 steps a second on both sides; the player's own motion then goes in steps of
  33 ms under frames of 22. Not tried.
- **A route to a place nobody went to before** is still 280 to 450 thousand instructions in
  the frame that asks, a job of twice the length: a bot that follows a player asks for one
  for each area the player comes to. All of them made ahead are 1,140 routes, 320 million
  instructions and 1.4 MB: for `bake.py`, as the file the library reads (`AAS_ReadRouteCache`).
- **A bot added by hand later** (`addbot` at the console) asks for its routes itself, as before.
- Two bots; telling the clients (70 thousand a frame: every entity's 208 bytes copied and
  compared for each client); the bots' part (85 thousand a frame of the game).
- Nothing of this was played by hand: the figures are of a player who stands.

## The scene on a worker core

The first core made the GPU's list of the client's frame from the worker's record of it (half
its frame: `scene_draw`, `entity_matrix`, `stage_moved`, `light_at`, `gpuDrawAt` and
`gpuDrawRoom`). Since 14 October 2026 a third worker does (`tr_shaderemu.c`): the first core
records its frame as the client's worker does (a clear, its own pictures and scenes, and which
of the client's lists comes where), and at the frame's end gives the record out with
`mcw_call (scene_job, ...)` and goes on to the next frame. The job is all of the frame's drawing
but the swap: the level's kept draws, the models, the pictures, the sum. At the next frame's
end the first core joins it (`mcw_join`, in `SE_SceneFlush`) and swaps, then gives out the next.
A frame is so shown one frame of the first core's later than it was made, and the first core
waits 0.2 to 0.5 passes a frame for the job (3.6 to 4.1 for the client's worker). (Since 15
October 2026 the job goes from frame to frame and a frame is shown when it is drawn: "The
client's frame on three cores".)

- **The client's game writes three lists in turn**, not two (`SE_LISTS`): one being written,
  one taken, one being drawn. The first core has two of its own in turn.
- **What the job writes is its own while it is out**: the GPU library's state and the set being
  built, the renderer's view, matrices, light and moved textures kept. Anything else of the
  first core's that touches them waits for the job first (`SE_SceneFlush`: a level loaded or
  freed, a window resized, the renderer started or stopped, a held frame, `SE_Settle`). The
  light a model is lit by is kept by the drawing core; the client's game asks without keeping.
  The frame's counts (`se_counts`) are the job's to write and are read, never cleared, by the
  `q3stat:` lines; a job's sum is printed with `mcw_print`.
- **What it touches is there before**: the program's code is read and the lists, the GPU's two
  sets and the renderer's tables are written by the first core when the scene's worker is
  started. `q3stat:` says how many pages were brought for workers so far: 1 before the level,
  about 70 after a run of 1,000 frames, 51 to 58 with `QUAKE3_SCENE=inline` (the server's and
  the client's).
- **The server and the client's game are calls too**: `mcw_call` where a job was posted to
  worker 1 or 2, `mcw_ready` where `mcw_done` was asked, `mcw_join` for the answer. Which worker
  has which is the library's; `SE_OnWorker()` says which part by its stack, as before. Their
  own kept lines, errors and lists stay (a job of the client's makes several frames, each
  taken apart). The client's worker is no longer posted again after eight passes: `rvc_cpu`
  wakes a worker posted to in the pass it went to sleep in.

And the client's frame was made shorter, which is now the frame's length (the first core and
the scene each wait for it):

- **A body that is not drawn is not made**: the player's own in the first person (only for
  mirrors, which this renderer has none of) and one wholly out of view by 192 units that is not
  firing a beam. Its animation, sprites and the weapon's sounds and barrel are made as before,
  and the numbers its muzzle flash would have drawn from `rand()` are drawn (`CG_Player`,
  `SE_PlayerWeaponUnseen` in the patch). 4 to 5 thousand instructions a frame.
- **A move made in a frame before is copied once**: of the moves taken from what was kept, only
  the last is copied into the predicted state (`se_pending` in `cg_predict.c`); they were 468
  bytes each.
- **An angle of nothing asks no sine**: `AngleVectors` takes the sine of an angle that is zero
  as the angle and its cosine as 1, which is what `q_sin` and `q_cos` give, sign and all (a
  roll is nearly always nothing, a pitch often). The server's moves and the bots have it too.

Instructions a frame on `rvc_cpu`, `python tools\game_par.py quake3` (q3dm17, `addbot sarge 3`,
`fixedtime 33`, put at 488 -968 378 at frame 100; frames 300 to 700, a private image). A run is
the same at every salt but the first (`--salt 0`, and 1 or more: the command typed is longer by
its `: N;`), so the figures are the mean of those two, the share of each worker from the
`q3stat:` lines of its jobs:

| | The first core | All cores | All over the first | Frames a second | The client's worker | The scene's | The server's | Frames of the game a second |
|---|---|---|---|---|---|---|---|---|
| before | 56.9k | 169.1k | 2.97 | 53 to 56 | 53.5k | | 56.5k | 14.2 to 15.5 |
| `QUAKE3_SCENE=inline`, the client as now but for the angles | 59.4k | 166.1k | 2.80 | 53 to 56 | 51k | | 57k | |
| now | 18.3k | 147.9k | 8.1 | 71 to 73 | 44.0k | 38.5k | 44.9k | 14.5 to 16.0 |

The workers' instructions are their jobs' to 1.4 to 2.1% (0.8 to 3.2% before): none of them
waits by turning, a worker with nothing to do sleeps. All cores run 21 thousand fewer a frame
(12.5%): 9.5 of them the client's (the bodies, the copies, the angles), 11.6 the server's, and
the first core's 38.6k fewer are the scene's 38.5k. The server's worker is busy all the time at
this `fixedtime` (each frame is 33 ms of the game whatever it takes) and makes as many frames
of the game a second before and after: a shorter frame has fewer of them. So 10 of the 21 are
work no longer done, 6% of the frame.

**Checking it.** `QUAKE3_SUM=1 QUAKE3_SERVER=inline quake3 +set fixedtime 33 +devmap q3dm17`
to frame 600: the scene's sums and the GPU lists' are the same with the scene on its worker
and with `QUAKE3_SCENE=inline` (572 of 572 both), and the scene's with everything on the
first core (572 of 572; the lists' 571, the one the first frame the client's worker makes, as
above). With and without `QUAKE3_BODIES=1`, with the bot too, the lists are the same in every
frame (570 of 570) and the scene's sums are not (the bodies not made are not asked for); the
angles left the scene's and the lists' sums as they were (574 of 574, 572 of 572). Two
runs compared must be typed the same length: the kernel's random numbers come of the typing's
timing, and the game's of them (a spawn point). `RVC_RACES` over 1,000 frames: nothing undone
(602 undone before this, the engine's counts and `rand()`), only the client's worker reading
the moves and the frame asked for as they were. The menu draws and quits; `QUAKE3_HOLD` holds.
No picture was captured: the pictures' check is the list's sum.

## The server's frame made shorter

After the scene went to a worker the server's worker was the slowest of the four: on the card
it made 16 frames of the game to the client's 100 at `fixedtime 33`, where the game asks for
33, so the level moved at half the clock. A job was 342 thousand instructions, and where a frame
of the game went (`QUAKE3_SERVER_LOG=1`, `rvc_cpu`, frames 300 to 700): the bots 123 thousand,
the game 33, telling the clients 34, the player's moves 38 (4.5 of them a frame of the game).
What was cut (15 October 2026, `quake3.patch`, `server_shaderemu.c`):

- **A bot's snapshot is the numbers of what it sees.** The bot asks which entities its snapshot
  has and reads their states from the game (`SV_BotGetSnapshotEntity`, `BotAI_GetEntityState`),
  so their 208 bytes are not copied into the ring for it (`SV_BuildClientSnapshot`). The
  entities' order finds a word's lowest bit by a table: the machine has no instruction for it,
  and the compiler called `__ctzsi2` for every entity. Telling: 39 thousand instructions a time
  to 27.
- **A bot's look at its snapshot copies only what it reads** (`BotCheckSnapshot`): an entity
  whose event the bot has seen and that is no missile is passed over without copying its state.
  23 thousand instructions a think to 3.5.
- **The game calls the engine directly** for what it asks by the dozen, as the client's game
  does (`g_syscalls.c`: traces, contents, links, the bots' library's entities, clock, inputs and
  characters): 77 questions a frame of the first core's through `SV_GameSystemCalls` became 8.
- A snapshot's walk over the level's entities steps a pointer, not two calls and a product.
- `MSG_ReadString` and its two kin keep a string for each core: the server's worker and the
  first core read messages at once (`RVC_RACES` found one undone).

| `game_par.py quake3`, five typings (salts 0, 1, 10, 100, 1000) | Server's frames of the game a second | Passes each | Client's frames a second | Passes a client frame |
|---|---|---|---|---|
| before, `rvc_cpu` | 15.1 (15.0 to 15.2) | 28.4 | 56.9 | 7.42 |
| after, `rvc_cpu` | 17.0 (15.8 to 18.4) | 24.1 | 59.3 | 6.92 |
| before, the card (one run, salt 0) | 16.0 | 27.1 | 56.1 | 7.73 |
| after, the card (one run, salt 0) | 17.8 | 26.0 | 59.1 | 7.84 |

On the card (an RTX 5090, D3D11, `game_par.py --card`, 15 October 2026) all cores ran 10.22
million instructions a second where they ran 9.83, the first core 1.65 where 1.53.

Before, every typing gave the bot the same fight; after, it fights in two or three ways by the
typing, and the frames of the game a second go with the fight (a think of the bot is 80 to 200
thousand instructions by what it does).

Nothing of the frame was split over more workers, for nothing of it is independent with one
bot: the bot thinks on what the player's moves and the game's frame wrote, and they on its
command; the player's snapshot reads what the frame wrote and the next moves write the
client's record it is made from; the bot's snapshot is 6 thousand instructions now. Each bot's
think on a worker of its own pays from two bots on (not done: the bots' library's routes and
its scratch are shared).

**Checking it.** The server's state can be compared between two builds now: with the server on
the first core, `QUAKE3_SUM=1` prints a `q3srvsum:` line after each frame of the game, a sum
over every linked entity's state and every player's (its ping, which is the clock's, left out),
on standard output and not the console (its lines are drawn and would be in the lists' sums).
`QUAKE3_SEED=7 QUAKE3_SERVER=inline QUAKE3_CLIENT=inline QUAKE3_SCENE=inline quake3 +set
fixedtime 33 +devmap q3dm17 +addbot sarge 3 +wait 100 +setviewpos 488 -968 378 90` to frame
700: the build before and this one, 223 of 223 frames the same (and with the client's game on
its worker), and this one's first core 8.8 million instructions fewer in frames 300 to 700.
The server inline draws its random numbers as on its worker, from a state of its own
(`se_rand_core` in `sys_shaderemu.c`): before, the client's game drew from the same one and a
spawn point went by the client's frames. The checks above: the scene's sums the same with the
scene on its worker and inline and with the client's game inline (572 of 572), the lists' 572
and 571 of 572. `RVC_RACES` in the five runs: nothing undone (the client's worker reading the
moves and the frame asked for as they were).

Tried and not kept:

- **Jobs chained**: the next round given out while the worker makes the last frame of one, so
  that it goes on to it with no pass between (a job ended, was seen, and the next began 3.2
  passes later on average). Rounds became a frame each, and every frame told the clients: 16.2
  to 17.3 frames of the game a second against 18.3 and 18.6 without.
- **The routes to every area made ahead** (`QUAKE3_ROUTES=all`, 426 million instructions of the
  worker while the level loads, 1,984 KB): with the player standing the bot asks no route in
  frames 300 to 700 already, and with him running in circles it asked none either; the bots'
  part was the same.
- `QUAKE3_SERVER=paced` makes a job a frame of the game long (it is never behind), so its
  instructions a frame are not the worker's. Two builds' first state in it, and inline too, went
  by the machine's speed: `addbot` drew the bot's spawn point from the first core's numbers,
  which the client's code draws from as many times as it has frames while the level loads. With
  the server on the first core a command of the game's now draws from the server's
  (`SE_ServerCommandDraws`, below).

Found and not explained: `QUAKE3_SERVER=inline` with the client's game and the scene on their
workers, two builds of this work (not the last) stopped at the same frame in every run with
`CG_ProcessSnapshots: n < cg.latestSnapshotNum`; not with the server on its worker or the scene
inline, and not after a print was added to look at it.

## Bots that think at once

Players fill the level with bots, and with three (`game_par.py quake3-3bots`: `+addbot sarge 3`
three times) the server made 6.4 frames of the game a second, 64 passes each, its bots' part 220
thousand instructions a frame. The bots now think at once (15 October 2026, `quake3.patch`,
`server_shaderemu.c`):

- **All bots think in the same frame** (`BotScheduleBotThink`: every one's residual 0, where they
  were spread over the 200 ms), and `BotAIStartFrame` hands the list to `SE_BotThinks`: the
  server's core makes the first, and up to `SE_THINKERS` (3, `q_shared.h`) helpers one each.
  From the server's worker the helpers are jobs the first core gave out with its job, waiting a
  pass at a time from job to job (a job's end does not wait for them); from the first core
  (`QUAKE3_SERVER=inline`, `paced`) they are calls (`mcwx.h`). A think beside the server's runs on
  a stack of its own, and `SE_OnWorker()` says 3 to 5 there: whatever the engine keeps one of for
  each core (a trace's marks, the box a thing is clipped as, `va()`, a command's words, the
  strings read from a message, lines kept for the first core) has one for these too.
- **What one think wrote and another read is each bot's or each core's now**: a bot draws its own
  random numbers and what it says or asks as a client (`say`, `use`) is done after all the thinks,
  in client order (`SE_BotCommandKept`); the server's commands are made into console messages
  before the thinks (`BotAIServerCommands`) and a message a think frees goes back after them
  (`SE_BotFreesDone`); the last teleport a bot saw, its inputs (16 bytes each), the AI's node
  switches and the routing scratch are each bot's or core's; a route found is only read (it is
  not moved to the front of the oldest-first list, which only says what to free when memory runs
  short, and never did here), and a route a helper makes is its own, in chunks the server's core
  gives it before the thinks (`AAS_SE_ThinkPools`). A bot that thinks alone draws and says as it
  always did.
- **What a think saw is kept for the think** (`BotEntityVisible`): a bot asks whether it sees its
  enemy from the same eye several times in one, and nothing moves while it thinks.

| `game_par.py`, five typings each, `rvc_cpu` (the card model with `RVC_ADFAULT=1`) | Server's frames of the game a second | Passes each | Client's frames a second | Passes a client frame |
|---|---|---|---|---|
| one bot, before this day | 15.2 | 27.2 | 57.1 | 7.19 |
| one bot, "The server's frame made shorter" | 17.4 | 23.4 | 59.0 | 6.85 |
| one bot, now | 17.3 | 23.3 | 56.0 | 7.33 |
| three bots, before this day | 6.4 | 64.5 | 46.7 | 8.53 |
| three bots, "The server's frame made shorter" | 6.7 | 61.7 | 47.9 | 8.09 |
| three bots, now | 10.5 | 38.9 | 45.8 | 8.92 |
| three bots, now, `QUAKE3_BOTS=serial` (two runs) | 8.1, 9.0 | 49.4, 44.3 | | |
| three bots, before this day, the card (one run) | 6.8 | 65.9 | 49.0 | 9.17 |
| three bots, now, the card (one run) | 11.2 | 39.5 | 51.1 | 8.64 |
| one bot, before this day, the card (one run) | 16.9 | 26.3 | 60.4 | 7.35 |
| one bot, now, the card (one run) | 18.8 | 24.0 | 59.9 | 7.52 |

On the card (an RTX 5090, D3D11, `game_par.py --card`, 15 October 2026) all cores ran 11.28 million
instructions a second with three bots where they ran 10.57.

**Checking it.** `QUAKE3_SUM=1 QUAKE3_SEED=7 QUAKE3_SERVER=inline quake3 +set fixedtime 33 +devmap
q3dm17 +addbot sarge 3 +addbot sarge 3 +addbot sarge 3 +wait 100 +setviewpos 488 -968 378 90` to
frame 700: the server's sums the same with `QUAKE3_BOTS=serial` and without (223 of 223), and
with the command typed longer (223 of 223). `RVC_RACES` in the ten runs above: nothing undone. On
the way it found what is listed above, and a fault (a helper's trace through a box the arrays
had no room for: `BOX_BRUSHES` and its kin are `SE_CORES`'s now). A fault in the program now
says where (`fault` in `sys_shaderemu.c`). The scene's sums and the lists' as before (572 and 571
of 572).

Not done: the bots' moves are made one after another in the game's frame (30 thousand
instructions a bot a frame of the game; they meet in each other's traces); the hand-off between
jobs (3.2 passes a job) was left as it is: chaining jobs lost more to telling the clients every
frame than it gained ("The server's frame made shorter"), and telling while the next job begins
meets its moves in the client's record. The routes a helper makes are not shared with the
others, and their chunks are freed only with the level.

## Traces and messages made cheaper

The server's worker per frame of the game (`game_par.py quake3` and `quake3-3bots`, `rvc_cpu`,
frames 300 to 700, seven typings: salts 0, 1, 10, 100, 1000, 10000, 100000; 18 October 2026):

| | Instructions a frame of the game (bots, game, telling) | Frames of the game a second | Passes each |
|---|---|---|---|
| one bot, before | 185.1k (84, 34, 28) | 16.3 | 24.5 |
| one bot, after | 146.1k (59, 26, 31) | 18.7 | 21.4 |
| three bots, before | 287.8k (101, 106, 39) | 9.5 | 44.6 |
| three bots, after | 229.1k (78, 78, 39) | 12.1 | 33.6 |

Each gives the answer it gave (`quake3.patch`):

- **A leaf lists its patches alone** (`cm.se_leafpatches`, made as the map loads): a trace went
  over every surface of a leaf for the few that are patches. `CM_TraceThroughLeaf` keeps the
  core, its marks and the leaf's lists in registers and tests a brush's box inline; the core is
  asked once a trace (`tw.se_w`), and `CM_Trace` clears only what is read before it is set.
- `CM_PointContents` makes the point's box once, not for every brush; `SV_AreaEntities_r` keeps
  the box in registers and takes the entity from `gEnt`; `SV_Trace` no longer clears its clip.
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

With one bot the frames a second gain less than the instructions: a job ended waits for the
first core to see it (`SE_ServerPoll`), and the client's frames went from 5.83 passes to 6.15,
with more snapshots to read. Checks: `tools/test_all.py quake3 --long` 5 of 5, `quake3-server`'s
223 sums of the one bot's game as before; with `--races` nothing undone (`quake3-bots-serial`
there ended a frame of the game earlier, 222 lines, the thinks at once the same 222).
Left: the bots' moves are most of what is left (`BotMoveInDirection` 47 thousand instructions a
call, nearly all `AAS_TraceClientBBox` and contents), then the player's moves (a Pmove a packet).

**Time behind made up, not dropped** (the same day). A job of the worker's took at most
`MOST_MSEC`, 200 ms of the game (two frames at `sv_fps 10`), and what the client's frames had
added beyond was dropped: 15% of the game's time with one bot (`QUAKE3_SERVER_LOG=1`). A job now
takes up to `JOB_MSEC`, 500 ms, five frames of which the last tells the clients
(`server_shaderemu.c`; the server on the first core still takes 200 a time). In the frames that
tell nothing the bots' snapshots are still made (`SE_SendBotSnapshots` in `sv_snapshot.c`), so
a bot sees each frame what it would see told, as with the server inline; before, it saw the
last told one. An area with entities in it no longer clears a trace it never reads
(`AAS_AreaEntityCollision`).

| Seven typings as above | Server's frames of the game a second | Passes each | Instructions a frame | Client's frames a second |
|---|---|---|---|---|
| one bot, before both | 16.3 | 24.5 | 185.1k | 70.2 |
| one bot, cheaper traces | 18.7 | 21.4 | 146.1k | 67.2 |
| one bot, time made up | 20.7 (+27%) | 19.4 | 137.1k | 67.3 |
| three bots, before both | 9.5 | 44.6 | 287.8k | 62.4 |
| three bots, cheaper traces | 12.1 | 33.6 | 229.1k | 57.0 |
| three bots, time made up | 12.9 (+35%) | 31.2 | 222.1k | 55.7 |

With one bot the server is near what the client's frames give it: 33 ms of the game a client
frame, a third of a server's frame, so 0.33 of the client's frames a second (91 to 95% of it
reached). The player is told once a job, now up to five frames apart where two before; the
client's frames a second went down with that and with the fights the typings give. Tried:
`JOB_MSEC` 300, 19.5 and 12.4 frames a second; 1000, 21.9 and 13.9, more than 500 but
up to ten frames between two messages to the player; the bots' snapshots only in the frames that
tell, 21.4 and 14.1 (a bot sees what is up to four frames old). Checks as above: 5 of 5, the 223
sums of `quake3-server` the same, `--races` nothing undone (`quake3-bots-serial` 222 lines there,
the thinks at once the same 222).

## The client's frame on three cores

With the server's frame made shorter a client's frame was 7.4 passes with one bot and 8.3 with
three (`game_par.py`, below), its game 45 and 57 thousand instructions on its worker (5.5 and
7.0 passes) and the scene 39 and 45 (4.8 and 5.5). An empty loop of a pass added to the one or
the other (a build of its own) made a frame with one bot 0.7 to 1.3 passes longer in the scene
and 0.3 to 0.6 in the client's game: the scene was the frame's length, 2 passes more than its
instructions. Its job ended a pass before the first core saw it, the next began a pass after it
was given out, and the first core gave it out only at the frame's end, after the client's game
and the frame before had both been waited for. What was changed (15 October 2026,
`tr_shaderemu.c`, `client_shaderemu.c`, `gpu_shaderemu.c`, `cg_ents.c` in `quake3.patch`):

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
- **The level's items are made on a helper core** (`SE_SideItems` in `cg_ents.c`): the client's
  game asks for the frame's items once the view is known, a helper core makes them while it goes
  on with the players, the effects and the flat pictures, and it waits for the helper at the
  frame's end (`side_join`). In an item's place the scene has a placeholder (`SIDE_PLACE`), and
  the renderer draws and sums the helper's entities there (`side_of`), so the lists and the sums
  are as they were. An item reads only its own entity and what the frame set before it (the time,
  the view, the items' turn), and writes only its own entity and what it adds; the places it
  gives its sounds are done by the client's game when it joins (`SE_SoundSideReplay`). Items with
  a sound or a light of their own stay the client's game's. The helper is a job the first core
  gives out with the client's game's first and keeps out (`side_job`, joined at `SE_ClientReset`);
  `SE_OnWorker()` says `SE_SIDE` there (6, `SE_CORES` is 7), and a `q3stat:` line says how many
  items it made, its instructions and the passes the client's game waited for it: 25 to 27 items,
  12 to 14 thousand instructions, 0 to 0.2 passes.

`game_par.py quake3` and `quake3-3bots` on `rvc_cpu` (the card model), private images, the mean of
seven typings (salts 0, 1, 10, 100, 1,000, 10,000 and 100,000: a build's runs fall into two or
three fights by the typing, and two builds' into other ones, so one typing can be 10% off):

| | One bot: passes a client frame | Its frames a second | Three bots: passes | Frames a second | Server's frames of the game a second (three bots) |
|---|---|---|---|---|---|
| before | 7.39 | 56.4 | 8.32 | 48.8 | 10.6 |
| the scene's job from frame to frame, a frame shown once drawn | 6.30 | 65.0 | 7.62 | 53.4 | 10.3 |
| and the items on a helper core | 5.62 | 73.4 | 7.02 | 59.0 | 8.9 |
| before, the card (RTX 5090, D3D11 + fxc2, `--card`, one run, salt 0) | 6.82 | 68.3 | 8.08 | 56.6 | 12.6 |
| after, the card | 5.56 | 81.9 | 6.47 | 72.5 | 11.7 |

On the card all cores ran 13.1 million instructions a second with one bot where they ran 11.0,
and 13.8 with three where 11.9; the first core 1.98 and 1.91 where 1.67 and 1.63.
Instructions a frame (`q3stat:`): the client's game 45 thousand to 39 with one bot, 57 to 50 with
three; the scene's 40 and 48, as before; the first core's 27 to 25 and 30 to 29. The client's game
and the scene are now about as long, and each is the frame's length in turn.

The server makes 4% fewer frames of the game a second with three bots (10.4 to 10.0, 21 typings;
with one bot 16.7 to 17.5): its worker runs as many instructions a second as before (2.88 and
2.90 million), and a frame of the game has 4% more in it, the player's moves the most of it. A
frame of the client's sends one move, and the server makes each (7.1 thousand instructions for
the player standing): more client frames, more moves a frame of the game. The bots' thinks are
on their helpers all the while (`q3stat:` says how many thinks a window had, and how many of them
were on helpers: two thirds with three bots, in every window); a window with the bots' part twice
as long (180 thousand where it is 85) is the fight the typing gives, which comes more often as
the client's frames come faster (22 of 140 windows against 5 of 140 before).

**Checking it.** `QUAKE3_SUM=1 QUAKE3_SERVER=inline quake3 +set fixedtime 33 +devmap q3dm17` to
frame 600, typed the same length (`QUAKE3_CLIENT=worker QUAKE3_SCENE=worker QUAKE3_SIDE=worker` and
each with `inline`): the scene's sums and the lists' the same with the items on the helper and
without (570 of 570, and 570 of 570 with the bot, `+addbot sarge 3 +wait 100 +setviewpos 488 -968
378 90`), with the scene on the first core (570 of 570), and as the build before made them (570 of
570; everything on the first core 574 of 574). The client's game on its worker against on the first
core differs in 17 frames early in the run with the bot, in the build before as well. Held frames
(`QUAKE3_HOLD` 400 and 700 with the bot, `rvc_cpu --save-ram`, drawn by `tools/gpu_reference.py`'s
model): every pixel the same as the build before's, and with `QUAKE3_SIDE=inline`. `RVC_RACES`:
nothing undone in any run of the table or of the sums. The menu draws and quits; `map
q3dm17` typed at frame 400 with two bots loads the level again and the game goes on.

Found on the way:

- **The game stopped with three bots** in two typings of five (`game_par.py quake3-3bots --salt
  10`, the tree as it was): `SE_ServerSettle` waited for the server's job without helping on a
  helper of its stopped at a page (two helpers stood in `BotEntityVisible`), and
  the job waited for the helper. It polls the helpers now (`helpers_poll`).
- **A bot thought on two cores at once** in a frame of the server's made on the first core (a
  level loading again, somebody joining): there its thinks are calls of the first core's, and a
  helper left waiting by the server's worker took the same task (`RVC_RACES`: 65 changes undone
  after `map q3dm17` with two bots). The helpers are joined before such a frame: 0.
- Two thinks of bots on two cores both counted the routing caches they made
  (`numareacacheupdates` in `be_aas_route.c`: `RVC_RACES` found one change undone in 77 runs); the
  counts were only printed, and are not kept now.
- With three bots and the server on the first core, `QUAKE3_BOTS=serial` and the thinks at once
  part from the game's third frame on in some builds (the build before this work too, `QUAKE3_SEED=7`
  to frame 700: 2 of 223 the same; in others 222 of 222). It goes by the length of the line
  typed: the build before the cheaper traces parted at 3 of 8 lengths (a variable of 1 to 8
  letters before the command), and a build whose helpers thought one after another at 4 of 8.
  `RVC_RACES` found only reads as they were. Found on 21 October 2026 (`QUAKE3_SUM=2`): it is not
  the bots. The player enters the game at 564 or at 633 ms of it (entity 3's `pos.trTime`), by how
  long the client's game took to load (`CL_InitCGame` 6.12 or 6.26 s), and everything after
  follows. Two sums only, in every build: the build of that day parts at lengths 2, 4 and 6, the
  one before at 6, and `quake3-bots` as `test_all.py` types it fails on that day's. The game's seed fixed (a
  build of its own) gave the same two sums. `QUAKE3_ENTER=1000` now has the player enter at
  1000 ms of the game, whenever its load ended (`SV_UserMove`): the thinks at once and one after
  another the same at all eight lengths (223 of 223), and the 16 runs the same as each other.
- With three bots two builds' `q3srvsum:` lines part from the game's second frame on, also this
  build's and the same with two prints added: the game's random numbers are seeded from the clock
  as the level starts (`GAME_INIT`'s `Com_Milliseconds`). Builds are compared with one bot.

Tried and not kept: the client's frames asked two ahead (`SE_LISTS` 5): 5.62 to 5.48 passes with
one bot and 6.73 to 6.92 with three, within the typings' spread, and a frame more before a move is
seen; the flat pictures joined without copying their state: the scene's instructions as they were.
Not done: the models made ready a frame ahead on a core of their own (their matrices, light and
culling, about 10 thousand of the scene's 40 to 48), which pays only with the client's game
shorter too; the players on the helper (they draw random numbers, make local entities and sounds
in their order, which a core beside would change).

## Pictures decoded when a level loads

Until 15 October 2026 the 237 pictures lay in `quake3-se.dat` as the GPU reads them, 6.5 MB of
the file's 13.3. They are files again, decoded with libmedia (`docs/system/media.md`) on the worker
cores into GPU memory when the game registers what draws with them, and drawn from there.

- **In sheets.** `bake.py` lays the textures of one kind and width one under another (rows of
  a texture are then rows of the sheet, as the GPU reads them) in sheets of 512 KB of texels
  at most, and writes each sheet as one file with libmedia's encoder (`q3pack`): 13 JPEGs of
  the 150 opaque pictures (quality 95, colours at full size, restart markers) and 6 PNGs of the
  87 with alpha (in segments; each texel's red, green, blue and transparency, so that the GPU's
  words come out as they were). A JPEG texture's rows begin on its 8 x 8 blocks. 1,291 KB: JPEG
  792 of 4,451 KB of texels, PNG 499 of 2,042. `bake.py` reads every sheet back with the same
  library: a PNG that does not come back texel for texel is left as texels, and it says so.
  The light maps (432 KB) stay as texels in the ROM, where the lit draws read them.
- **One file a picture was tried first**: 247 decodings, each a few rounds of libmedia's workers
  and a few mappings, made the level's loading 30,411 passes where it was 15,260 (one core alone:
  55,384). A sheet is decoded in one call on every core.
- **When.** A shader, model or skin registered on the first core has its sheets decoded then;
  one registered on the client's game's worker is asked for and decoded by the first core at
  its next frame's end (`pictures_take`). The level's own runs are decoded in `RE_LoadWorld`
  before their kept commands are made. `stage_kept` is made again for the stages of a sheet
  decoded, or whole at the frame's end when the scene's job was out. The main menu decodes 9
  sheets, the level the other 10; they are kept until the window closes (the file has one level).
- **Where.** The display's own framebuffer first (`seglMemorySpare`, 4,032 KB while the desktop
  is layers), then `seglMemory` (2,625 KB left after the level's 442, which has a room of its own
  now). All 19 sheets are 6,492 KB: a sheet with no room is not decoded and its stages are drawn
  in their colours, and the line `quake3: pictures decoded so far` says how many were not.
- **On which cores.** The program opens 24 more worker cores than the game's six
  (`QUAKE3_DECODERS`), and a sheet is decoded on all that have no job out (`mcw_for`'s teams).
  In a game they are parked: each has a call that is not joined until the next sheet
  (`pictures_park`), because `mcw_call` goes round every worker with no call out, and round 30
  it woke a cold one for each of the game's jobs: 7.46 passes a client frame against 7.01.
- **The game's workers are lent to libmedia**, and the server's may be making the bots' routes:
  the library steps round a busy worker on lent ones and never posts to one by number
  (`media.md`, at the top), which once hung the game here. Every sheet is shared out over the
  workers that are free; a sheet under 32 rows is the first core's alone.

Measured on `rvc_cpu` (private images, the build before and this one):

| | Before | After |
|---|---|---|
| `quake3-se.dat` | 13,372,812 bytes | 8,048,183 (all PNG: 9.3 MB, the pictures 2,543 KB) |
| `quake3.bin` | 2,392,184 bytes | 2,609,272 (libmedia's pictures) |
| `+devmap q3dm17 +addbot sarge 3`, the command to the level's first frame: the first core | 124.5 million instructions | 130.2 (21.7 of them in decoding, while it waits too) |
| the same: passes, guest seconds | 15,260, 37.7 s | 16,293, 40.2 s |
| the same, every sheet shared out (libmedia steps round busy workers, 15 October) | | 15,759, 38.9 s; first core 126.5 million, 17.0 of them decoding |
| `devmap q3dm17` typed at the menu (no bot, 10 sheets): the first core, passes | 75.0 million, 9,185 | 87.1 million, 10,785 |
| `game_par.py quake3`, five typings: passes a client frame | 7.50, 6.84, 6.93, 6.84, 6.93 | 6.85, 6.94, 6.94, 7.49, 7.00 |
| the same: the first core's instructions a frame | 27.8k, 25.2k, 24.9k, 25.2k, 24.9k | 25.4k, 25.3k, 25.3k, 27.6k, 25.2k |

The level's sheets cost less with the bot: they are decoded while the server's worker makes its
routes, which the first core waits for after. Decoding on six workers was 21,165 passes, on 16
17,742, on 30 16,289, on 62 16,267. Quality 90 saved 172 passes of 10,785 and doubled the texels'
error (1.99 levels on the mean, 43 at most): not taken.

**Checking it.** Held frames (`QUAKE3_SEED=7`, the server, the client's game and the scene on the
first core, `QUAKE3_SUM=1`, `QUAKE3_HOLD`), saved with `rvc_cpu --save-ram` and drawn by
`tools/gpu_reference.py`'s model from the RAM and the image's ROM: frame 400 at 488 -968 378,
frame 250 where the player begins, and the menu at frame 60. The scenes' sums are the same in
every frame before and after (373 of 373, 223 of 223). With `--pictures png` every pixel is the
same as before in all three; with the JPEGs 63 to 89% are, 98.1 to 99.6% within 2 levels, none
more than 10 away (a texel 0.86 levels off on the mean, 22 at most). `RVC_RACES` from the command
to frame 300: nothing undone, the waits on libmedia's team words and the client's moves "as it
was". `RVC_ADFAULT=1`: no stop on a written bit (5 of the workers' page stops, 1 before).

Not done: anything on the card (how fast the GPU reads a texture from GPU memory against the
ROM was not measured), in the world or in a holodeck (`GpuVolumePass.cginc` reads GPU memory as
for Quake's textures, not tried with these). Without the display's spare memory (a display that
is not layers) 2.6 MB are left and most sheets would not fit. libmedia: one picture at a time in
a program (its decoders keep their state in statics, and map their memory with system calls, so
a worker cannot decode a picture of its own beside another); a JPEG whose markers are unevenly
spread posts to busy workers (above); its PNG encoder wrote a 128 x 1312 sheet of three bytes a
texel that neither it nor Pillow reads back (the all-PNG bake leaves it as texels).

## The server's frame told and linked cheaper

Where the server's frame went (21 October 2026, `prof.py` on `QUAKE3_SEED=7` with everything on the
first core and no sums): a snapshot of the player's wrote every entity's delta field by field
though most had not changed since the frame it is told against, the bots' library was told every
entity again each think, and a link looked up the same leaves for an entity that had not moved.
Each gives the answer it gave (`quake3.patch`, `server_shaderemu.c`):

- **An entity the same as in the frame the snapshot is told against is not compared again**
  (`se_snap` in `sv_snapshot.c`): `SV_BuildClientSnapshot` copies its state into the ring and
  compares it with the old frame's in one loop, and `SV_EmitPacketEntities` writes nothing for it
  where `MSG_WriteDeltaEntity` would have written nothing: that function 15 thousand instructions a
  frame less, the copy 9 more.
- **The entities linked and not hidden from all are listed once a time** (`se_sent`): the first
  snapshot of a `SV_SendClientMessages` walks them all, the others walk that list, and the area
  test is inline on the flood numbers (`CM_SE_Floods`).
- **A link of an entity whose box is as last time keeps its leaves** (`se_boxed`, `se_absmin`,
  `se_absmax` in `svEntity_t`); `SV_AreaEntities` walks the sector tree without recursion, in the
  same order.
- **The bots' library is told an entity whole only when its state changed** (`SE_BotLibTell` in
  `ai_main.c`); one unchanged is only seen again (`AAS_SE_SeenAgain`, what `AAS_UpdateEntity`
  does for the same state). The level's name for the map scripts is read once, not every think.

A second round the same day:

- **Each client's view keeps what it found of an entity's areas and clusters** (`se_seen` in
  `sv_snapshot.c`) while the view's cluster, its area, the level and the areas' flood numbers
  (`CM_SE_FloodGen`) are as they were and the entity's leaves were not made again (`se_gen`,
  raised by `SV_LinkEntity` before it makes them): the flags are still tested every time.
- **A list of entity numbers found in order is not sorted** (`SV_BuildClientSnapshot`): without
  portals the walk finds them in order, and the bits that sorted them cost more than the walk. A
  bot's numbers go into the ring without a remainder each.
- `BotAIStartFrame` no longer clears a state it sets whole; the Pmove's kept ground and water and
  `SV_Trace`'s kept traces compare words, not `memcmp`.

| Everything on the first core, frames 300 to 1000 (231 frames of the game) | Instructions a frame (bots, game, telling) |
|---|---|
| one bot, before | 166.6k (70.3, 28.6, 67.7) |
| one bot, first round | 152.0k (65.4, 27.9, 58.7), -8.8% |
| one bot, second round | 140.1k (63.1, 27.3, 49.7), -15.9% |
| three bots, before | 304.1k (96.9, 80.7, 126.6) |
| three bots, first round | 278.7k (90.9, 79.3, 108.6), -8.4% |
| three bots, second round | 252.4k (88.7, 77.0, 86.7), -17.0% |

| The server's worker, `game_par.py`, seven typings, frames 300 to 700 | Instructions a frame: mean, median, 90th, worst | Its core's ms a frame: mean, worst | Frames a second |
|---|---|---|---|
| one bot, before | 141.8k, 133.4k, 180.1k, 482.7k | 43.6, 148 | 18.6 to 21.2 |
| one bot, first round | 132.6k, 123.3k, 170.4k, 470.9k | 40.1, 143 | 19.6 to 22.1 (20.7) |
| one bot, second round | 130.0k, 122.2k, 167.3k, 453.6k | 39.9, 136 | 20.3 to 23.8 (21.6) |
| three bots, before | 224.9k, 221.6k, 304.6k, 387.9k | 80.0, 155 | 11.2 to 13.6 |
| three bots, first round | 213.7k, 208.4k, 269.3k, 328.6k | 73.4, 145 | 11.6 to 13.3 (12.7) |
| three bots, second round | 194.1k, 190.9k, 222.8k, 323.2k | 71.2, 142 | 12.6 to 14.0 (13.4) |

On the worker the fights go by the typing, so its numbers move more than the first core's, and
there a bot is told once a frame of the game where with everything on the first core it is told
at every frame of the client's too: the second round cut that telling most. Tried and not kept:
the list of entities to look at as a pass of its own before the walk, and the copy and the
comparison as two loops (both slower); `se_visible` not inlined once it grew (the view's cache
then saved nothing). Left: the bots' thinks and moves are traces, AAS and the level's, about 25
nodes a trace at 27 instructions a node (`BotGapDistance` 13 traces a call, `BotEntityVisible`,
the Pmoves), and the copy of the player's snapshot, 5 instructions a word of 52 an entity.

**Checking it.** The one bot's sums (`QUAKE3_SUM=1`, everything on the first core, the server's
and the client's game's) the same as before both rounds: 223 of 223 and 673 of 673; three bots'
server sums with `QUAKE3_ENTER=1000` the same as the first round's, 223 of 223 (the client's
game's sums with three bots part in 13 of 658 frames by the line typed alone, the same build
too). Held frames 400 and 700 with the bot (`QUAKE3_HOLD`, `rvc_cpu --save-ram`, drawn by
`gpu_reference.py`'s model): every pixel the same as before. `tools/test_all.py quake3 --long` 5
of 5, with `--races` 5 of 5 and nothing undone.

## Checking it

Hold a frame under `rvc_harness --cpu` with `--save-state` and `--gpu-capture`, then

    python tools\gpu_reference.py SNAP BMP --size 640x480 --rom build\images\NAME\rootfs.bin

Four views of the level (`devmap`, `setviewpos`, `wait 90; hold`): away from triangle edges
99.4 to 100% of pixels within 2 levels of the model. What differed in one was a moving
texture, the capture being a frame older than the list; `hold` now waits for the GPU.

## Not done

- Playing it by hand, and hearing it. The pointer turns the view while the left button drags it
  (`docs/machine/input.md`).
- A second character, the other three levels of the demo, the menu's setup pages.
- Marks and dynamic lights; the server's frame for two bots.
- The server's frame with a player who moves, a second bot, and a bot's route to a place
  nobody went to before ("The server's pace"). The client's game on its worker was not
  played by hand or in the world, and its sounds were not heard.
- The menu's picture was not captured (the card was in use): it was driven by `nxkey` on
  `rvc_cpu`, 93 thousand instructions a frame, and a game started from it.
