# Star Wars Jedi Knight II: Jedi Outcast, the 2002 demo

Begun 2 November 2026, by `docs/plans/jk2-plan.md`. What there is: OpenJK's single player of Jedi Outcast built as
one program by `linux/jk2/build.sh`, with the game and the renderer linked in and a platform layer of our own. The
demo's level loads and plays on the build computer and **on the machine**, and the same runs give the same sums on
both. Since the plan's step 2 (2 November 2026) it is **drawn on the machine's GPU** in a Nano-X window, through
rd-vanilla over Half-Life's OpenGL ("The picture"); keys and the pointer drive it, its sound goes to the card, and a
script of keys plays its start from the menu as a test. **It is far too slow**: a frame of play is 11.4 million
instructions of core 0 with the picture (7.8 without), against about 400 thousand for 5 frames a second at 2 MIPS,
and the load to the level 11.5 billion. It goes into private images only.

    wsl -- python3 linux/jk2/data.py host ~/jk2-host/data --accept-eula   (the demo, fetched once into a cache; its licence)
    wsl -- bash linux/jk2/host_sum.sh                                     (builds the build computer's program, plays, sums)
    wsl -- bash linux/jk2/program.sh OUT PRIVATE_HOME                     (the machine's program, flat; data.py OVERLAY does it)
    python linux/jk2/survey.py PAK                                        (what the demo's pak holds)
    python linux/jk2/draws.py TREE                                        (calls drawing two random numbers: must say 0)

In the guest (a private image with `/usr/share/jk2/base/assets0.pk3` and the program, "A private image"): `nano-X -p &
jk2` for the menu, `jk2 +map demo` for the level; with no window system it draws nothing (the sums' runs): `JK2_FRAMES=4000
JK2_SUM=400 JK2_SEED=7 jk2 +set fixedtime 50 +set com_maxfps 0 +map demo`.

(`JK2_CACHE=DIR` puts the cache elsewhere: `~/.cache/shaderemu/jk2/demo` of whoever runs it, WSL's or Windows'.)

## The demo's data

Nothing of it is in the repository. `linux/jk2/data.py` fetches it into `~/.cache/shaderemu/jk2/demo` only with
`--accept-eula`, checks it, and takes its pak and licence out of the installer itself (the installer is a Microsoft
cabinet behind a program: data.py reads the cabinet's MSZIP blocks with Python's zlib, and nothing of it is run).

| | |
|---|---|
| Where | `https://archive.org/download/jk2demo/jk2demo.exe`, the Internet Archive's copy of LucasArts' 10 May 2002 download (item `jk2demo`) |
| The installer | 69,680,101 bytes, md5 `7d9b901df28f2f4b11f1b00d5d2a0ca5` (the Archive's listing gives the same md5 and sha1 `e4317ace4b248de89c10f3cc8651b8203e7b0ad5`), sha256 `d3f8e1807ad6bdb81e6aef678a648017e27d86f341ff778527405ce8d4b80bfb` |
| In it | `GameData/demo/assets0.pk3`, 64,001,989 bytes, md5 `93110de202bdf255928a4a931001a4a2`, sha256 `2550446678c25c293760dbf374c64b7d62e04290f3e9c87cad9994b8c0b7f1cd`; the 2002 engine (`jk2sp.exe`, `jk2gamex86.dll`, which we do not use); `Install/license.txt` (3,529 bytes) |
| Other copies | ModDB, Gamepressure, Softpedia list a 66.4 to 66.5 MB `jk2demo.exe` (their pages refused to be read); the Archive's `star-wars-jedi-knight-ii-jedi-outcast-demo-for-open-jo` is the pak repacked with three changes of its own for OpenJK, not the original: not used |

**The licence** (`license.txt`, "LICENSE AGREEMENT - DEMO", LucasArts 2002), as it reads: the demo may be copied and
given or sent to others only in its original form and for no money or other consideration; it may not be rented,
modified, have files added or deleted, and no new levels or derivative works may be made from it; no decompiling or
reverse engineering of the demo except as the law permits; no warranty. So:

- An image with the pak **byte for byte as it is**, at no charge, is the nearest the licence comes to allowing; but an
  image is not "the Demo in its original form", so whether it may go into the world is the owner's call (as the plan says).
- A **baked** file like Quake III's `quake3-se.dat` (pictures halved and re-encoded, sounds packed for the card, the
  level re-cut) is a modified and derived file of the demo: the licence's words forbid it, even made on the user's own
  computer. Until the owner decides, the port reads the pak as it is and converts at load time, in the machine
  (`data.py` puts the pak in an overlay unchanged).
- The owner accepted the licence on 2 November 2026 ("Yes to everything") for the fetch into the cache.

**What the pak holds** (`survey.py`, 2 November 2026): 1,990 files, 108.3 MB unpacked.

| | |
|---|---|
| The level | `maps/demo.bsp`, RBSP version 1, 9.47 MB: 7,607 planar faces (22,128 triangles) and 423 patches, 42,342 vertices of 80 bytes, 140 shaders, 19 light maps of 128 x 128, 1,447 entities; `maps/demo.nav` |
| Its people | 76 NPCs placed: 49 Stormtroopers, 15 Imperials, 6 Reborn, 2 Jan, Kyle, a probe, an R5 and a mouse droid; 95 compiled ICARUS scripts (`.ibi`), 94 named by entities; 3 `fx_snow` |
| Ghoul2 meshes | 19 `.glm`, version 6: the people have 72 bones and 81 to 83 surfaces, 2,292 to 3,286 vertices and 2,769 to 3,960 triangles at LOD 0, four LODs (a Stormtrooper 2,843 / 2,098 / 1,026 / 746 vertices); a surface refers to at most 16 bones; a vertex has 1 to 4 weights |
| Animations | `_humanoid.gla`: 17,278 frames of 72 bones, 445,610 distinct compressed bones of 14 bytes, 9.98 MB; four small ones |
| Other models | 108 MD3, 0.67 MB |
| Sounds | 914, 12.2 MB, 1,168 s: 823 MP3 (44.1 kHz mono, 48 or 64 kbit/s), 91 WAV (mostly 22 kHz 16-bit mono) |
| Music | 12 MP3, 10.8 MB, 663 s (11 of them 160 kbit/s stereo) |
| Films | 4 RoQ of 512 x 512: the opening crawl (`jk0101_sw`), the opening scene (`jk0101`), logos, the end |
| Pictures | 648, 72.2 million texels: 439 JPEG, 209 TGA (32-bit); 20 of 1024 x 1024 |
| Its checksum | 1431467275 as the engine computes it (MD4 over the files' CRCs): the number the 1.03 and 1.04 engines require of a demo pak (`DEMO_PAK_CHECKSUM` in the original source; 1.02 had another). The demo is the final game's data version. |

## What OpenJK needed for the demo

OpenJK reads it. The 2002 engine ran the demo when it found no full game (`FS_SetRestrictions`: `com_demo 1`, the
folder `demo`), and three things depended on that, which OpenJK took out with the rest of the demo's code in 2014
(its commits `d55c0b16`, `6855e356`). The patch puts them back behind `com_demo`, which the starter sets:

- the menu's new game starts `map demo`, not `kejim_post` (`ui_main.cpp`);
- the opening crawl is `menu/video/tc_demo` (`cl_cin.cpp`);
- the level's loading screen is the first map's, "a long time ago" (`cg_info.cpp`).

And one OpenJK brought: its world effects are Jedi Academy's, whose snow is a picture `gfx/effects/snowflake1.bmp`
the demo does not have, which ended the level (`CParticleCloud: Could not texture`). Jedi Outcast's snow was
untextured points; under `JK2_MODE` a missing picture is now the white one (`tr_WorldEffects.cpp`).

## What the build is made of (`linux/jk2`)

- `build.sh` fetches OpenJK at the plan's commit `260c59c2`, puts `jk2.patch` on it (writing only files whose text
  changed, as OpenTTD's), and compiles the engine (`code/` with `JK2_MODE`), the game with its client's game
  (`codeJK2/`) and the renderer with Ghoul2 (`code/rd-vanilla`), with OpenJK's own zlib, minizip, libjpeg and libpng.
  `JK2_HOST=1` builds for the build computer (gcc, in `~/jk2-host`), otherwise for the machine with OpenTTD's C++
  compiler (`linux/openttd/toolchain.sh`: the engine's errors are C++ exceptions).
- **The game and the renderer were libraries**, each with its own `q_shared`, `Com_Printf` and globals. Each is
  linked into one object (`ld -r --force-group-allocation`: C++'s section groups made plain, or the program's link
  keeps one module's inline function and drops the other's that its names point into), every name made its own but
  its entries (`GetGameAPI`, `dllEntry`, `vmMain`; `GetRefAPI`), which become `se_GetGameAPI` and the like and are
  what `Sys_LoadLibrary` and `Sys_LoadFunction` give (`sys_loadlib.h` under `SHADEREMU`). The modules are compiled with
  `-fno-gnu-unique`: a static inside an inline function is otherwise a "unique" name that objcopy leaves global.
- **The game is loaded again** at every new level, new game or saved game, and a library's variables were then fresh:
  linked in, a saved game's load read a table of the last load's freed names (`G_LoadRoff`, found by AddressSanitizer)
  and crashed. The game's variables are two sections of their own (`jk2game_data`, `jk2game_bss`) and its 13
  constructors a third (`jk2game_init`, which the program's start does not run; build.sh's `game.ld`): at every load
  the data is put back as it began (a copy of 30 KB on the machine), the rest zeroed and the constructors run
  (`se_game_fresh`). A copy of all of it, the first way, was 5.5 MB more on the machine. `JK2_AT="1000:load auto"` loads
  the autosave: it plays on, on the build computer (clean under AddressSanitizer with the first way) and on the machine.
- `sys_shaderemu.cpp` is what `shared/sdl` was: a 640 x 480 "mode", the terminal's lines as console commands, and the
  end (`SE_Exit`: the C library's `exit` ran a null destructor of a linked-in module). On the machine (`SE_WINDOW`, the
  build's "window" part) it also opens the Nano-X window, takes its keys and the pointer, and plays the mix on the
  sound card ("The picture"); on the build computer, and on the machine with no window system, it has no window and
  mixes the sound into a buffer nobody hears at the pace of the client's time.
- `gl_jk2.c` (the machine) is the 70 OpenGL 1.1 calls the renderer makes, renamed `jk2_glX` in its object by build.sh:
  Half-Life's `linux/halflife/gl_shaderemu.c`, included as it is, over Quake III's draws (`linux/quake3/gpu_shaderemu.c`
  on `gles.c`) once the window is open, and `nullgl.c`'s answers before.
- `nullgl.c` (the build computer) is the same calls doing nothing: everything the renderer does but draw still happens
  (the level, shaders, pictures decoded, Ghoul2 bones and vertices made, the scene sorted).
- `se_rand.c` is the game's `rand`: glibc's and musl's differ, MSVC's is what the game was written against.
- `cg_rand.c` is the client's game's random numbers, on states of its own (build.sh renames `Q_flrand`, `Q_irand` and
  `rand` in its objects): in the 2002 library both drew from one state, so what the client's game drew for its effects
  moved the game's numbers. Made while the machine's sums were hunted; the other two causes alone may have sufficed,
  but it is kept: the game then plays the same whatever the client's game drew, and the client's game can go to a worker.
- `survey.py` reads the pak; `data.py` fetches it; `host_sum.sh` is the reference sums; `program.sh` the machine's
  program; `draws.py` finds calls whose arguments draw two random numbers.

What the patch changes besides the demo's three things: the engine without SDL (`sys_main.cpp`, `sys_loadlib.h`);
`fixedtime` not a cheat (a map without cheats put it back to 0); `JK2_SEED` for the game's seed (`sv_game.cpp`);
films not played unless `JK2_CINEMATICS=play` (an in-game film pauses the game until it ends or a key is pressed);
`SE_ZoneBytes` (`z_memman_pc.cpp`); the state sum (`g_main.cpp`) and `SE_Holdrand` (`q_math.c`); the level's file
freed once the renderer has the world (`tr_bsp.cpp`: 9.5 MB); the `.nav` file's words read as 32 bits
(`g_navigator.cpp`); fifteen calls whose arguments drew two random numbers, drawn first (`draws.py`).

    JK2_FRAMES=N        quit after N frames, with a jk2stat: line
    JK2_STATS=N         a jk2stat: line every N frames: the frames' host time, the zone's bytes and blocks, its peak
    JK2_LAST=CMD        a console command before quitting (zone_details: the zone by kind)
    JK2_AT=F:CMD;...    console commands at frames F ("1000:load auto")
    JK2_SUM=N           a jk2sum: line every N frames of the game ("Is it the same game")
    JK2_SEED=N          the game's random numbers from N, not the clock
    JK2_SOUND=off       no sound at all (the game then loads none, and a save of the game crashed before nullgl's fix)
    JK2_CINEMATICS=play the films played (the default skips them)
    JK2_KEYS="F:K F:K+ F:K-"  keys pressed, held or let go at client frames F; F:atX,Y puts the menus' pointer at X, Y of 640 x 480
    JK2_PICTURE=none    no window even with a window system (nullgl.c's answers)
    JK2_SOUND=mix       the mix into a buffer nobody hears even with a sound card (for sums: the card's pace moves them)
    JK2_WEATHER_CHECK=1 the snow's outside cache also asked point by point, and the words that differ counted
    HL_POOL_MB=N        the GPU's texture memory, MB of RAM (36); HLGL_LOG=1 every texture uploaded
    jk2gpu:             with JK2_STATS, the textures in GPU memory, how many found no room, vertices drawn a frame
    +set com_demo 1     the demo (the starter's)
    +set fixedtime 50   each frame 50 ms of the game's time, whatever it took
    JK2_CFLAGS=, JK2_LINK=, JK2_WORK=, JK2_SRC=, JK2_TREE=keep, JK2_JOBS=N   (to build.sh)

## Is it the same game

`host_sum.sh` (2 November 2026; the build computer, `fixedtime 50`, seed 7, no input, sound mixed into nothing): the
same lines at every run, and from a tree built from a fresh clone. **The machine's program prints the same nine**
(`rvc_cpu`, 4,000 frames from an empty home, twice: before and after the game's constructors moved), and every one of
the first 273 frames of the game the same, one a line (`JK2_SUM=1`):

    jk2sum: frame 400 time 21250 entities 641 sum 3b52fdac draws e04cc09f
    jk2sum: frame 800 time 41250 entities 641 sum c811c3c7 draws 916e06bf
    jk2sum: frame 1200 time 61250 entities 646 sum d3779ccf draws f4ffe90b
    jk2sum: frame 1600 time 81250 entities 639 sum f41d6913 draws 3589dd3d
    jk2sum: frame 2000 time 101250 entities 639 sum b933d53b draws 3e76ff12
    jk2sum: frame 2400 time 121250 entities 639 sum 5f4c1192 draws 31ae8c67
    jk2sum: frame 2800 time 141250 entities 639 sum 5df47961 draws b27c64ec
    jk2sum: frame 3200 time 161250 entities 639 sum 559c85a7 draws 810d9e76
    jk2sum: frame 3600 time 181250 entities 639 sum 286e9b16 draws 274198c2

A line is a sum over every entity in use: what it is, its health, where it is and faces to an eighth of a unit, a
client's place, speed, weapon and animations, an NPC's behaviour, and `rand`'s state; `draws` is `Q_flrand`'s and
`Q_irand`'s state. `JK2_SUM_AT=N` lists every entity at game frame N (`jk2ent:`), to find what differs.

Three things made two runs on the build computer differ: the null sound's position went by the computer's clock, so a
voice ended in another frame (it goes by the client's time now); the game's seed was the clock (`JK2_SEED`); and the
home folder a run left behind (its `openjo_sp.cfg` and autosave) changed the next run's game from frame 400 on, so
`host_sum.sh` starts from an empty one, and so must the machine. Three more made the machine differ from it:

- **The order of a call's arguments.** `GunRackAddItem( ..., Q_flrand(-1,1) * 2, (i-1) * 9 + Q_flrand(-1,1) * 2, ... )`:
  GCC evaluates a call's arguments last to first for x86 and first to last for RISC-V, so the rack's blasters lay
  elsewhere from the first frame. `draws.py` finds every call whose arguments draw two random numbers (15 in the game
  and the client's game); the patch draws them into variables first, last to first as the 2002 game did (MSVC's x86),
  and `VectorSet`'s first to last (a macro in 2002, a function in OpenJK). `draws.py` must say 0 after a change.
- **The `.nav` file read with 64-bit `long`s on the build computer**: its header did not match, the demo's paths were
  made again from the level, and an NPC took another path at the sixth frame. The machine (and the 2002 game) read it.
- The client's game drawing from the game's random state (`cg_rand.c`, above).

The build computer is built with the machine's float flags (`-fsingle-precision-constant -ffp-contract=off
-fno-math-errno`); its libm and musl's may differ in a last bit, which the eighths are for: no such difference showed.

**The tests** (`tools/test_all.py jk2 jk2-sums`, tagged `long`, on an image with the demo's pak: an image without it
says `JK2-NO-DATA` and passes). `jk2-sums` is the run above (4,000 frames, the last two lines; 235 s on `rvc_cpu`).
`jk2` plays the start from the menu by a script of keys (`JK2_KEYS`): Enter on New, the pointer on Begin Game and a
click, the opening scene, then forward for 200 frames, a turn, forward again and a shot; the player dies after it and
the autosave is loaded. Its sums are the build computer's (2 November 2026, 151 s):

    jk2sum: frame 800 time 41250 entities 645 sum b7b63862 draws cf778cae
    jk2sum: frame 1000 time 51250 entities 651 sum 0610489c draws cb577a0c
    jk2sum: frame 1200 time 61250 entities 641 sum 5496b62c draws 8d82b4ce
    jk2sum: frame 1000 time 51250 entities 643 sum d946eeaa draws 7bf726e5     (after the load)

The player can move from about the game's frame 850 (a walk from client frame 1,300 moved the sums at game frame 900,
one from 1,000 did not). Two runs of the same keys played two games until `CL_KeyState` was made to take a key held in
a frame as held for all of it under `fixedtime` (the patch): it took the share of the computer's milliseconds the key
was down.

## On the machine (the plan's step 1)

Built 2 November 2026 in a private WSL home (`~/agents/jk2`, a copy of `~/nm`, the `building-the-system` skill) with
OpenTTD's GCC 13.3: `jk2.bin` is 4.49 MB flat. Run on `rvc_cpu` in a private image (`linux/nommu/image.py jk2 ROOT
KERNEL` over a copy of the built flat root, with the pak unchanged in `/usr/share/jk2/base`), one core, `fixedtime 50`,
seed 7, no input. `jk2stat:` lines give this core's instructions (`rdcycle`, which counts them on `rvc_cpu`):

| | |
|---|---|
| From the command to the level played (frames 1 to 400 of the client's, the load and the loading screen) | 28.56 billion instructions |
| A frame of the client's while playing (frames 2,000 to 4,000, 400 at a time) | 7.6 to 7.7 million |
| The same in the first 1,600 frames (NPCs waking, the autosave) | 5.8 to 9.3 million |
| Free memory at the shell / while playing | 245.9 MB / 78.9 MB: 167 MB taken |
| of which the pak's pages in the page cache (`Cached`) | 55.9 MB |
| so the program's own | about 111 MB |
| the zone in it, now and at most | 64.2 MB, 66.1 MB (12,788 blocks) |

At 2 MIPS that is a load of four hours and a frame every 3.8 s; on the calibrated card's 5.5 MIPS 1.4 hours and 1.4 s.
**The plan says to stop at a frame over about 400 thousand instructions or memory over about 150 MB, and say so**:
the frame is 19 times that, with rd-vanilla's front end, a null GL and the sound mixed on the CPU; the memory is under
it without the page cache and over it with. So this stops here, at the gate, for the owner's decision.

Where the load goes (`tools/prof.py` over the whole run to frame 360, 28.3 billion instructions, 96% the program's):

| | billion |
|---|---|
| pictures decoded by the CPU into the null GL (`R_FindImageFile`: JPEG's IDCTs and colour, `R_CreateImage`, mipmaps) | 11.5 |
| the weather's "outside" cache made (`COutside::Cache`: 13.5 million `CM_PointContents`), for the snow: "No cached weather file found, generating" | 9.9 |
| inflating the pak, its CRCs (`crc32` 0.6) | 1.9 |
| MP3 sounds decoded and resampled (`window`, `fdct32`, `ResampleSfx`, ...) | about 2 |
| the kernel | 1.1 |

With a picture the pictures go to GPU memory, decoded by libmedia on the workers (as Quake III's); the weather's cache
is the same level's every time and could be kept (but not in the image: a file made from the demo's level, see the
licence) or made on workers.

Where a frame goes (`prof.py` with `--count-from` at the console's `echo` at frame 800, to frame 1,600: 9.0 million
instructions a frame of `Com_Frame`, the busy part of the run):

| | a frame | |
|---|---|---|
| rd-vanilla's back end, tessellating into the null GL (`RB_ExecuteRenderCommands`; `RB_SurfaceGhoul` 0.86, `DotProduct` from it and culling) | 3.48 M | goes with our renderer |
| the client's game adding the scene (`CG_DrawActiveFrame`: players 1.8, of it their blob shadows' marks `CG_PlayerShadow` 1.15; impact marks) | 2.77 M | shadows and marks are left out at first, as Quake III's |
| the sound mixed and its MP3s decoded on the CPU (`S_Update`) | 1.19 M | the card's (step 4) |
| **the game** (`G_RunFrame`, at 20 a second: one a client frame here) | **1.05 M** | stays: traces 0.76 (40 a frame of 18.7 thousand, through patches), NPCs' behaviours 0.58, `ClientThink` and `Pmove` 0.34, Ghoul2 bolts the game asks for 0.41 |
| the rest of the server (`SV_Frame` but the game), the kernel | 0.1 M, 0.3 M | |

**So the game alone is about 21 million instructions a second of the game's time** (1.05 million each 50 ms), ten times a
core at 2 MIPS and four times the calibrated card's first core, before anything is drawn. Quake III's server on its
worker was 26 frames of the game a second at 25 passes each. Even with the drawing replaced, the sound on the card, and
the game on a worker of its own, the demo's fight would play at a tenth to a quarter of its speed unless the game's
traces, behaviours and bolts are made several times cheaper or shared out among workers (NPCs think one by one).

Not yet seen: whether the first scripted scene plays as it should. The opening crawl and film are skipped, and with no
picture there is nothing to look at; the level loads, its scripts run (ICARUS), the autosave is written, and 200 s of it
play without error.

## The picture (the plan's step 2)

Made 2 November 2026. The quickest way to a picture was taken first: rd-vanilla stays whole, front end and back end,
and its OpenGL 1.1 calls go to Half-Life's (`linux/halflife/gl_shaderemu.c`, included unchanged by `linux/jk2/gl_jk2.c`),
which makes them Quake III's GPU draws on `gles.c`. A renderer of our own in `tr_shaderemu`'s form is **not** written:
this is the picture to compare one with, and it shows what rd-vanilla's back end costs.

- **The window**: `GrNewWindowEx` of 640 x 480 when `GrOpen` finds a window system (`WIN_Init`), the GPU through
  `hlglOpen`; no window system (or `JK2_PICTURE=none`) is the null picture. A frame is swapped in `WIN_Present`.
- **What is drawn**: the BSP's faces and patches with every stage of their shaders as rd-vanilla draws them (no
  multitexture: a light map is a second pass that multiplies), MD3 and Ghoul2 models skinned by rd-vanilla on core 0,
  the menus, the loading screen and the 2D, the snow. Left out by the shim: depth ranges (the first person's weapon),
  clip planes (portals, mirrors), stencils (shadows), copies of the screen (the glow), line widths.
- **Textures**: 512 texels a side at most (`GL_MAX_TEXTURE_SIZE`: the menus' and the loading screen's pictures of 1,024
  were 12 MB), `r_picmip 2` by the starter, in a pool of 36 MB of RAM (`HL_POOL_MB`): the menus and the level take
  31.6 MB, 0 left out. With the 24 MB first tried, 105 to 401 textures found no room and their surfaces were not drawn.
  A JPEG is decoded at the size `r_picmip` asks for, by libjpeg's scaled IDCT (the fast one), not whole and then halved,
  and the levels under the first are not made (the GPU draws level 0 alone).
- `r_primitives 2` (the starter): triangles as `glDrawElements`. rd-vanilla's default with no compiled vertex arrays
  was strips of `glArrayElement`, about four thousand primitives a frame, each a draw of its own: 11.79 million
  instructions a frame against 11.35.
- `jk2gpu:` lines (with `JK2_STATS`) say the textures in GPU memory, how many were left out and the vertices drawn.

Captures (`rvc_harness --cpu --warp` through `tools\gpu_queue.ps1`, `--save-state`, `tools\snapshot_screen.py`), 2 November
2026: the main menu (New, Load, Controls, Setup, Exit; New's difficulties and Begin Game), the loading screen ("A long
time ago..."), the opening scene in the hangar (Kyle's ship coming in, a file of Stormtroopers, the light maps, the snow),
the level's courts with the Stormtroopers marching, and the level after the opening. Darker than the game on a PC
(no gamma ramp: the light maps are not brightened), the sky's boxes black in places. **Not seen**: the player's own
view with the HUD, hands and saber; at the captures' frames the opening scene was still playing or the player was dead.

| 2 November 2026, `rvc_cpu`, `fixedtime 50`, seed 7 | before (step 1, null GL) | null GL now | with the picture |
|---|---|---|---|
| The load, the client's frames 1 to 400 | 28.56 billion | 18.0 billion | 11.36 billion (`r_picmip 2`) |
| A frame of play, the client's frames 2,000 to 2,800 | 7.6 to 7.7 million | 7.8 million | 11.35 million |
| GPU memory for textures | | | 31.6 of 36 MB |

The load's two cuts: the snow's outside cache ("The load") and the pictures at `r_picmip`'s size. With the picture the
state sums at frames 400 to 2,400 are the null picture's (the renderer changes no game state).

Where the frame with the picture goes (`tools/prof.py` over 400 frames from frame 2,000 with strips, before
`r_primitives 2`, 11.4 million instructions a frame): rd-vanilla's back end 7.25 million (`RB_RenderDrawSurfList`),
of it the shim's conversion of vertices (`draw_vertices`, `array_vertex`, `jk2_glArrayElement`: 2.9 million), Ghoul2's
surfaces skinned (`RB_SurfaceGhoul`, 67 a frame: 2.39 million with what they call), the culling's `DotProduct` (1.07
million), marks clipped (`R_ChopPolyBehindPlane`, `R_MarkFragments`: 0.47 million); the client's game 3.05 million
(`CG_DrawActiveFrame`); the game 0.6 million. Half of the frame is rd-vanilla's way of feeding a GL: the next step is
the plan's renderer of our own, the level kept in GPU memory and drawn by a few kept draws (Quake III's way).

## Ghoul2 (the plan's step 3): the numbers

Measured on the frame above: 6 characters drawn a frame (`CG_Player`), 67 Ghoul2 surfaces, 2.39 million instructions
of core 0 to skin them (`RB_SurfaceGhoul`: weights, bones and normals, about 400 thousand a Stormtrooper of 2,843
vertices at LOD 0, some 140 instructions a vertex); their bones 0.25 million (`G2_TransformBone`,
`Multiply_3x4Matrix`); the game's bolts on its side 0.41 million ("On the machine"). The busiest view of the demo's
start has 14 Stormtroopers: about 5.6 million a frame to skin them.

- **On worker cores** (the plan's first way): 5.6 million shared out among workers, each model's vertices into GPU memory,
  a pass or more each way and the vertices' stores against a core's 6 KB a pass; it needs the back end to take a model's
  vertices from elsewhere than its tessellation buffer, which rd-vanilla's back end does not allow without the renderer
  of our own.
- **In the GPU's vertex shader** (the second): a model's 72 bones (72 x 12 floats) written a frame, about 2 thousand
  instructions of core 0 a model against 400 thousand: two hundred times fewer, and nothing for the workers. It needs a
  new draw kind (four weights a vertex, the bones in a table) in the GPU shader and Unity's copy, measured there.
- **Bones at a lower rate** (the third) helps either way only for the skinning; the game's own bolts must stay exact.

**Chosen on these numbers: the GPU's vertex shader**, with far models' skins at a lower rate where it changes no game
state. Not built yet: it goes with the renderer of our own, since rd-vanilla's back end skins into its tessellation
buffer on core 0 whatever the GL under it.

## Input, sound and the desktop (the plan's step 4)

- **Keys and the pointer** (`se_input` in `sys_shaderemu.cpp`, once a frame from `WIN_Present`), Quake III's way: the
  window's keys as the game's (`A_*`; a printable key is its character, F5 to F12 are not after F4 in OpenJK's table), the
  machine's input words for the pointer, the window system asked when they moved and four times a second. In a menu
  the pointer's move is the menu's cursor's; in play the left button held turns the view by the pointer's travel and
  the right button fires (`A_MOUSE1`, the demo's `+attack`). Ctrl fires too (the starter's `+bind CTRL "+attack"`; the
  demo binds Ctrl to use, which E is as well).
- **Sound**: the engine's own mixer, into a stream voice's ring of 8,192 stereo frames on the sound card
  (`se_card_init`): the card plays what the mix has written (`SNDDMA_Submit` tells it `s_paintedtime`) and its place
  is the DMA position. The CPU still mixes (about 1.2 million instructions a frame) and decodes the MP3s; voices of the
  card for each sound, and the music and films, are not done. With no card (`rvc_cpu` without `RVC_SOUND`) or
  `JK2_SOUND=mix` the mix goes nowhere at the client's pace, as before: a run on the card's pace plays another game
  (a voice that ends in another frame), so the sums are made without the card. Heard: not yet.
- **The desktop**: `Games/Jedi Outcast (demo)` in the Start menu (`nxapps.77-jk2`, put by `data.py` into an image with
  the demo only), the icon `linux/apps/icons/jk2.png` (a light sword drawn by `tools/draw_icons.py`, nothing of the
  game's), the game's number `GAME_JK2` 10 (`linux/apps/gameid.h`, `EmuGamepad.cs`: the shooters' controls; not
  tried in the world). No `ui_single_instance`, no holodeck seat.

## The load

- **The snow's outside cache** (`COutside::Cache`, 9.9 billion instructions: 13.5 million `CM_PointContents`, each also
  searching a 16-entry cache of points): `CM_PointContentsGrid` (`cm_test.cpp`, given to the renderer in `refimport_t`)
  answers a box of the grid's points at once while it lies on one side of every plane down the tree and, in its leaf,
  whole inside or apart from each brush of the asked contents; what lies within 0.25 units of a plane is cut in two
  down to single points, asked as `CM_PointContents` asks. `JK2_WEATHER_CHECK=1` asks every point too: **0 of 423,192
  words differ** (the demo's zone, 229 x 264 x 224 points, 155,330 outside; the build computer, 2 November 2026). The
  cache file the game wrote into its home (a file made from the level) is neither read nor written.
- **Pictures** at `r_picmip`'s size and no levels under the first ("The picture"). Not done: decoding on the workers
  by libmedia; rd-vanilla loads a picture at a time as its shaders are parsed, so that needs the loads gathered first.

## Memory

The zone (`zone_details` at frame 600, the build computer, 64-bit; the machine is 32-bit and will be somewhat less):

| | MB |
|---|---|
| The zone in all, with sound / at most | 76.3 / 84.1 |
| without sound (`JK2_SOUND=off`) / at most | 34.8 / 42.8 |
| sounds decoded (`SND_RAWDATA`), MP3 headers (`SND_MP3STREAMHDR`), music (`SND_DYNAMICMUSIC`) | 20.9, 14.5, 6.1 |
| the level's file whole (`BSP_DISKIMAGE`), its collision (`BSP`) | 9.0, 1.6 |
| the renderer's world and the rest of the old hunk (`HUNKALLOC`) | 10.4 |
| Ghoul2 animations (`MODEL_GLA`), meshes (`MODEL_GLM`), MD3 | 9.6, 2.6, 0.6 |
| the game (`G_ALLOC`), ICARUS, Ghoul2's vertex heap | 0.1, 0.05, 0.25 |

The SP engine has no hunk: everything is `calloc` a block (12,670 blocks). What this means for 256 MB with no MMU: the
sound is the card's (from the ROM, as Quake III's), and the level's file, the animations and the meshes can be read
where they lie in the ROM, not copied: about 12 to 15 MB would be left, and the pictures in GPU memory. Pictures are
not in the zone here (the null GL keeps nothing); on the machine they are GPU memory. The largest single blocks are
the level's file (9.5 MB) and `_humanoid.gla` (10 MB): both should be mapped from the ROM. **But** 1,989 of the pak's
1,990 files are deflated: a file can be read where it lies only if the pak is stored unpacked (as Quake III's
`quake3-pak0.pk3` is), and a pak packed again is arguably not "the original form" the licence asks for. Kept as it is,
the level and the animations are inflated into RAM (19 MB in two blocks), on the workers by libmedia.

**On the machine** (`zone_details` at frame 650, 32-bit): 64.2 MB, of which sound 38.0 (decoded 17.5, the MP3s'
decoder states 14.5: 569 of 26.6 KB, one a sound whether it plays or not, music 6.1), the old hunk 9.5, animations 9.6,
meshes 2.6, the weather's cache 1.6, collision 1.3; the level's file is gone (step 2's first change: freed once the
renderer has the world, 9.5 MB). The game's variables put back at a load now cost 30 KB, not 5.5 MB. Left for step 4:
the sound (with the card the decoded samples and the MP3 decoders' states are not in RAM: about 32 MB) and the music.

## On the build computer: where a frame goes

These are host times, not the machine's instructions (WSL has no perf or valgrind; `rvc_cpu` counts are step 1's).
An AMD Ryzen 9 5900X, 2 November 2026, `fixedtime 50`, seed 7, no input:

| | user CPU |
|---|---|
| the program's start, the level's load and 1,000 frames | 1.97 s |
| the same and 5,000 frames | 6.09 s |
| a frame (the client's, with the game at 20 a second), from the two | 1.03 ms (0.99 without sound) |

`gprof` over 5,000 frames (a `-pg` build): the load is mostly decoding JPEG pictures (`decode_mcu`, the IDCTs,
`ycc_rgb_convert`: 0.5 s of 4.5) and inflating the pak; a frame's largest are the renderer's front end (`DotProduct`
from culling, `R_SortDrawSurfs`), Ghoul2's surfaces made for drawing (`RB_SurfaceGhoul`, about 49 a frame) and bones
(`G2_TransformBone`, about 236 a frame; `Multiply_3x4Matrix` 880; `MC_UnCompressQuat` 390), the snow
(`CParticleCloud::Update`, 1,000 particles each tested by `COutside::PointOutside` every frame), traces
(`CM_TraceThroughTree`, `SV_AreaEntities_r`) and the sound's mixing. The renderer is replaced on the machine (step 2),
so of this only the game's, the traces' and Ghoul2's bones carry over.

## C++, threads, floats

- **No threads**: OpenJK's SP code has no `std::thread`, `pthread_create` or SDL thread (the sound's SDL callback was
  the only other one, and is gone).
- **Exceptions**: `Com_Error` throws an `int` and the frame catches it (`common.cpp`): OpenTTD's GCC with its unwinder
  (`ehframe.c`) is needed, not `rv32-c++`. No `dynamic_cast` or `typeid` in what is compiled; `std::vector`, `std::map`,
  `std::string` in 50 files.
- **Assembly**: only MSVC's `fld`/`fistp` in `q_math.c`, not compiled with gcc.
- **Doubles**: 109 uses in 57 files of what is compiled (the MP3 decoder's tables, `bg_panimate.cpp` 9, ICARUS 6,
  `cg_players.cpp` 5); to be looked at by the profile on the machine, not before.
- **64-bit**: none of note.

## What is left, in order

The owner chose on 2 November 2026 to go on past step 1's gate. Steps 2 and 4 have a first form, step 3 its choice:

1. **A renderer of our own** (step 2's real form): `tr_shaderemu`'s method behind `refexport_t`, the level kept in GPU
   memory as kept draws, keeping rd-vanilla's Ghoul2 (`G2_*.cpp`, `tr_ghoul2.cpp`'s bones: the game traces against
   them), models, skins and fonts. rd-vanilla's back end is 7.25 of a frame's 11.4 million ("The picture").
2. **Ghoul2 skinned by the GPU's vertex shader** ("Ghoul2"): a draw kind of four weights a vertex and a bone table, in
   `gpu.shader` and Unity's copy, measured on the card.
3. **The game's own 21 million instructions a second** ("On the machine"): traces through patches, NPCs' behaviours and
   Ghoul2 bolts made cheaper, or the game on a worker (step 5).
4. **Sound** as voices of the card (the decoded sounds and the MP3s' 14.5 MB of decoder states out of RAM), music, films.
5. **The player's own view checked**: HUD, hands and saber, in a capture after the opening scene with the player alive.
6. **In** (step 6): `image.py --jk2`/`JK2_IMAGE=1` calling `data.py`, `AGENTS.md`'s line.

**For the owner:** whether an image with the demo's pak may go into the world, and whether the pak may be baked
(converted) on the build computer at all, given the licence's "no modified files, no derivative works" (above).

## Not done

The renderer of our own, Ghoul2 on the GPU, sound by voices, music and films (skipped: a film says it could not be
opened, in red at the top of the picture), the HUD and the first person's weapon seen, depth ranges, mirrors and
portals, shadows, the glow; `ui_single_instance`, the holodeck's seat; nothing in the shared image (`image.py --jk2` is
not wired: a private image only); pictures decoded on workers; `RVC_RACES` (no workers are used: `HLGL_WORKERS=0`);
OpenJK's `alloc-dealloc-mismatch` in `strip.cpp` at shutdown (upstream's, seen under AddressSanitizer). The renderer's
variables are not put back at a `vid_restart` as the game's are.

## A private image

    wsl -- bash -c 'HOME=~/agents/X JK2_OUT=DIR bash linux/nommu/build.sh jk2/build.sh && python3 linux/nommu/flat.py DIR'
    cp -r build/nommu/flatroot ROOT                     (of the main checkout)
    JK2_PROGRAM=DIR python linux/jk2/data.py ROOT --accept-eula
    python linux/nommu/image.py jk2 ROOT build/nommu/Image

(A worktree needs `build\images\linux` and `rvc` of the main checkout, a junction each.)
