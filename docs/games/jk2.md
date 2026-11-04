# Star Wars Jedi Knight II: Jedi Outcast, the 2002 demo

Begun 2 November 2026, by `docs/plans/jk2-plan.md`. What there is: OpenJK's single player of Jedi Outcast built as
one program by `linux/jk2/build.sh`, with the game and the renderer linked in and a platform layer of our own. The
demo's level loads and plays on the build computer and **on the machine**, and the same runs give the same sums on
both. Since the plan's step 2 (2 November 2026) it is **drawn on the machine's GPU** in a Nano-X window, through
rd-vanilla over Half-Life's OpenGL ("The picture"); keys and the pointer drive it, its sound goes to the card, and a
script of keys plays its start from the menu as a test. **It is far too slow to play** ("Speed", 3 November 2026): on
the card's calibrated clock the menu runs at 7.1 frames a second (1.2 before) and a frame of play takes 1 to 1.4 s (3 to
4 before), and the load to the level is 11.5 billion instructions. What bounds it is the game itself on the first
core ("What bounds it"). It goes into every image (`pwsh tools\make_image.ps1`, since 4 November 2026; `JK2_IMAGE=0`
makes a private test image without it).

    wsl -- python3 linux/jk2/data.py host ~/jk2-host/data --accept-eula   (the demo, fetched once into a cache; its licence)
    wsl -- bash linux/jk2/host_sum.sh                                     (builds the build computer's program, plays, sums)
    wsl -- bash linux/jk2/program.sh OUT PRIVATE_HOME                     (the machine's program, flat; data.py OVERLAY does it)
    python linux/jk2/survey.py PAK                                        (what the demo's pak holds)
    python linux/jk2/draws.py TREE                                        (calls drawing two random numbers: must say 0)

In the guest (a private image with the installer `/usr/share/jk2/jk2demo.exe` and the program, "A private image"; the
starter unpacks the pak first, "The demo in the machine"): `nano-X -p &
jk2` for the menu, `jk2 +map demo` for the level; with no window system it draws nothing (the sums' runs): `JK2_FRAMES=4000
JK2_SUM=400 JK2_SEED=7 jk2 +set fixedtime 50 +set com_maxfps 0 +map demo`.

(`JK2_CACHE=DIR` puts the cache elsewhere: `~/.cache/shaderemu/jk2/demo` of whoever runs it, WSL's or Windows'.)

## The demo's data

Nothing of it is in the repository. `linux/jk2/data.py` fetches the installer into `~/.cache/shaderemu/jk2/demo` and
checks it. **An image gets the installer itself, byte for byte** (`/usr/share/jk2/jk2demo.exe`, since 2 November 2026):
the machine shows its licence, read out of it, and unpacks the pak into memory once it is accepted there ("The demo in
the machine", `docs/system/install.md`). Only for the build computer's own program (`data.py fetch` and `host`, with
`--accept-eula`) are the pak and licence taken out of it on this side (the installer is a Microsoft cabinet behind a
program: data.py reads the cabinet's MSZIP blocks with Python's zlib, and nothing of it is run).

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

- The owner's decision (2 November 2026): an image holds the installer unchanged, the demo in its original form, and
  it is unpacked in the machine after the licence is accepted there. (Before, an image held the pak taken out of it.)
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
    JK2_MARK=F0,F1      a measured window: jk2mark: lines at frames F0 and F1 (its frames and milliseconds), then the end
    JK2_MUSIC=engine    the music decoded and mixed by this core, as before ("Music on the card"); JK2_MUSIC_LOG=1 says
                        each stream of the card's; JK2_WORKERS=N the workers asked for (16)
    JK2_GL=plain        rd-vanilla's triangles through Half-Life's shim as before ("Speed"); =passes the GPU's passes as
                        before ("What the menu was missing"); =nomemo no large draw kept; JK2_GL_LOG=F frame F's draws
    JK2_SHADOWS=made    every blob shadow made anew each frame ("Fewer instructions")
    JK2_RENDER=core     the drawing on core 0 ("The drawing on a worker"); JK2_SKIN=drawing Ghoul2 skinned by the drawing alone;
                        JK2_MIX=core the mix painted on core 0 ("The mix on a worker")
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

**The tests** (`tools/test_all.py jk2 jk2-sums`, tagged `long`, on an image with the demo's installer, its pak unpacked first with the licence answered on the terminal: an image without it
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
  the menu's cursor (the saber) is put where the pointer is in the window (`se_menu_pointer`, `_UI_MouseTo` in
  `jk2.patch`'s `ui_main.cpp`), stretched with the picture to 640 x 480, and a click lands where it was pressed. Until
  3 November 2026 a menu was told the pointer's travel instead: the cursor stopped at the menu's edges while the
  pointer went on, a move outside the window was lost and a moved window kept the old offset, so the saber and the
  desktop's pointer went apart and clicks missed (the owner's photo of the main menu in the world). `JK2_POINTER_LOG=1`
  prints each place given; `test_all.py jk2-pointer` (on an image with the demo) moves the pointer round over the
  menu: on a window at 43,46 the display's 303,326 is the menu's 260,280. In play the left button held turns the view by the pointer's travel and
  the right button fires (`A_MOUSE1`, the demo's `+attack`). Ctrl fires too (the starter's `+bind CTRL "+attack"`; the
  demo binds Ctrl to use, which E is as well).
- **The window's size**: a second after the window's size last changed, the renderer is restarted at that size
  (`r_mode -1`, `r_customwidth` and `r_customheight`, `vid_restart`; 320 x 240 to 1,024 x 768 in whole 8s), so a larger window
  is drawn at its own pixels, not 640 x 480 stretched; the menus stay the game's 640 x 480, stretched, and the pointer is mapped
  through it. `JK2_RESIZE=stretch` is the way before. `test_all.py jk2-resize` (an image with the demo) maximises the window at
  the main menu: "the window is 794x545: the renderer restarted at 792x544", then New and a click where the stretched menu has
  Begin Game (the display's 623,542 to the menu's 499,454) loads the level. A restart reloads what the renderer holds: tens of
  seconds of the guest's at this machine's speed. Until 3 November 2026 the restart kept 640 x 480 ("MODE: -1, 640 x 480"), drawn in
  the window's corner with old pixels round it: rd-vanilla calls `WIN_Init` only while `glConfig.vidWidth` is 0 and its shutdown
  leaves it, so `WIN_Shutdown` now clears it ("MODE: -1, 792 x 544"). The log lines alone had passed; a capture showed it
  (`rvc_harness --cpu`, `--save-state`, `tools/snapshot_screen.py`).
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
- **Pictures** at `r_picmip`'s size and no levels under the first ("The picture").
- **A picture's JPEG decoded by libmedia on the workers** (`SE_PictureJPG` in `sys_shaderemu.cpp`, asked first by
  rd-common's `LoadJPG`; `JK2_PICTURES=engine` is libjpeg's): one picture at a time as rd-vanilla parses its shaders,
  each over the workers ("Without markers" in `docs/system/media.md`; one under 16,384 pixels on this core), at
  `r_picmip`'s size by `m_jpeg_decode_shrunk`, whose sides are libjpeg's scaled ones. The pixels are not libjpeg's:
  `JK2_PICTURES_CHECK=1` decodes each both ways and prints how far apart: over the load's 414 pictures (`r_picmip 2`) a
  mean of 0.69 levels a channel, the most 124 (small glows a quarter of their size); at `r_picmip 0` 0.51 and 67 (the
  R5 droid's body), the fast IDCT and fancy upsampling against libmedia's. The same run as the sounds' below: **872,552
  to 715,088 passes**, core 0 6.78 to 5.34 billion; the sums to frame 450 the same.
- **No CRC-32 of a pak's file** read whole (`se_pak_crc` in minizip's `unzip.c`; `JK2_PAK_CRC=1` checks again): the
  inflate already fails on a broken stream. Core 0 from the desktop to the level played (`client state 7`, the
  installer's files unpacked first, `fixedtime 50`), `rvc_cpu`: 9.05 to 8.43 billion (3 November 2026).
- **A sound's MP3 decoded by libmedia on the workers** (`SE_SoundMP3` in `sys_shaderemu.cpp`, called from
  `S_LoadSound_Actual`; `JK2_SOUNDS=engine` is the engine's decoder, 16 workers or `JK2_WORKERS`): the level's 220 or
  so sound effects were decoded one after another on core 0. libmedia's frame table leaves out a frame that decodes to
  nothing and the engine counts it (two in three sounds differ by one frame of 1,152 samples, either way): the length
  stays the engine's, as ICARUS waits for a sound's end, a frame missing is silence and one more cut off. The same run:
  **1,067,800 to 872,552 passes** (-18%), core 0 8.43 to 6.78 billion, the workers 3.3 billion. The game's sums to
  frame 450 (`JK2_SUM=50`) are the same both ways; `test_all.py jk2-sums` (1,200 s) was not run.

### Side by side (3 November 2026)

The load's work went to the workers a file a worker, not a file over all of them: rd-vanilla and the sound system ask
for one picture or sound at a time, and nothing waits for its pixels or samples until something is drawn or played.
The same run as above (`rvc_cpu`, 16 workers, the desktop to `client state 7`), each step the tree against the one before:

| Step | Passes | Core 0 |
|---|---|---|
| the pictures' JPEGs over all workers, one at a time (above) | 715,088 | 5.34 billion |
| animation tables found by an index (`GetIDForString`); large pak entries by libmedia | 696,550 | 4.93 |
| pictures side by side | 564,052 | 3.86 |
| what `Upload32` makes of them before the upload, on their worker | 546,499 | 3.65 |
| the map's MD4 on a worker | 537,297 | 3.54 |
| the GPU's words made on the worker | 531,386 | 3.47 |
| libmedia's split inflate given rings it can map | 506,078 | 3.26 |
| sounds side by side | 397,211 | 2.38 |
| the music's sizes kept; the autosave's screenshot by libmedia | 365,474 | 2.12 |
| a picture's size from more than its first 4 KB, a TGA's from 32 bytes | 367,757 | 2.03 |
| a pak entry's header in one read | 359,206 | 1.96 |
| a picture with no free worker waits a pass (`mcw_rest`), not made here | 355,260 | 1.91 |
| `strtod` exact in doubles (`--wrap=strtod`), configstrings copied as strings, `SkipBracedSection` in place | 348,409 | 1.86 |
| the pictures out at once 20 MB at most, the sounds 6 MB | 351,309 | 1.86 |
| the loading screen drawn at most every 250 ms (`JK2_LOADING_MS`): 256 pictures of the load to 176 | 348,110 | 1.83 |
| a deflated WAV read packed, its head's 4 KB inflated here, the rest by its sound's worker (`se_wav_packed_read`) | 336,590 | 1.78 |
| an MP3's unpacked size counted from its frames' headers (`se_mp3_count`, `cl_mp3.cpp`; `JK2_MP3_CHECK=1`: 0 of the load's differ) | 327,574 | 1.71 |
| 32 workers opened (`JK2_WORKERS`), libmedia's teams kept at 16 (`JK2_TEAM`) but the large entries' split inflate on all: play with 32 not measured | 313,864 | 1.66 |
| the string tables' `GetLine` (`strip.cpp`) with its tests inline and no 8 KB cleared a line (`JK2_PARSE_CHECK=1`: 0 differ) | 308,967 | 1.65 |
| the game's `G_FindConfigstringIndex` searching the configstrings where they lie (`SV_SE_FindConfigstring`) | 306,791 | 1.63 |
| `mcw_wait` serving only workers with something out (`programs/mc/mcw.c`: every program's; 96 million of `serve` before) | 307,014 | 1.57 |
| the pak read where it lies: mapped once read only and shared (the page cache itself without an MMU, as `cabinst` wrote it in one piece), minizip's file functions over the map (`unzSE_OpenMapped`), the pictures', sounds' and large entries' packed bytes a pointer into it (`unzSE_PackedAt`, `FS_SE_FreeRaw`); `JK2_PAK_MAP=0` the file reads (303,013, 1.534) | 284,171 | 1.37 |
| core 0 rests while the large entries' split inflate runs, not taking a part (`m_inflate_stream_rest`; `JK2_PAK_REST=0`) | | 1.274 |
| a picture's head inflated by zlib, which stops when the room is full, not libmedia's whole first block (`SE_PictureHead`; `JK2_HEAD=media`) | | 1.218 |
| a folder's files read ahead by idle workers once two of one folder are read in a row (`SE_FileGive`/`SE_FileJoin` in `files.cpp`, a 512 KB room of their own, the entry found by its local header, `unzSE_EntryAt`; every libmedia team reaps them first; `JK2_FILES_AHEAD=0`, `JK2_AHEAD_LOG=1`) | | 1.167 |
| the shader files' text kept from the first of the load's three scans (`tr_stl.cpp`; `JK2_SHADERS_KEEP=0`) | | 1.140 |
| the UI's scripts parsed without `va` and `Q_strcat` (the same bytes); the navigation file read whole, not in 43 thousand reads (`g_navigator.cpp`; `JK2_NAV_WHOLE=0`) | 257,852 | 1.111 |
| a JPEG's head inflated in steps until its frame header (`SE_PictureHeadUntil`; `JK2_HEAD_UNTIL=0`); folder listings kept for the search path's misses, listed again after any write, rename, removal or new folder (`se_dir_lacks`; `JK2_DIRS_LISTED=0`); libmedia's JPEG encoder asks for 105 bytes a block first, 420 only when an interval overflows (the autosave's 5 MB room had failed and libjpeg ran here) | | 1.028 |
| libmedia's inflate asks for no room over 1 MB in one piece (`ONE_PIECE`: the rings apart); a kept MP3's 26.6 KB decoder state made when it first plays (`SE_MP3Header`; `JK2_MP3_LAZY=0`, `JK2_MP3_LAZY_CHECK=1`) | | 1.013 |
| the colour cleared at a world view nothing else clears (`RB_BeginDrawingView`; `JK2_CLEAR=0`): the sky's boxes are black where they are not drawn, not the GPU memory's old pictures | 248,497 | 1.008 |
| a loading screen's frame waits only for the pictures it binds (`R_SE_PictureNeeded`; `JK2_PICTURES_BIND=0`); the dissolve's and the autosave's resample in rows on the workers, in a room on 16 bytes, core 0 resting (`JK2_RESAMPLE=core`) | 165,094 | 0.876 |

The last nine rows (3 November 2026, evening) give the same sums to frame 450, no failed allocation over the load (the
20, 10.6 and 5 MB tries before), the zone at most 56.4 MB at the load's end (71.5 before) and 43.8 MB free (28.5).
`RVC_RACES` with `RVC_RACES_NEAR=4` over the load: one store undone, libmedia's (`second_jobs`), at a part's edge,
which core 0 writes again from the part's own copy (`m_inflate.c`, "the texels two parts share"). The resample with core 0
taking a share, or into a buffer not on 16 bytes, had a texel undone: hence the room and the rest. On core 0 at
1.008 billion (three profiled windows, before the last row, which took much of the waiting): the sounds' registration (about 140 million: their inflate, MP3 sizes and decoders), waiting
for workers (about 90: `mcw_wait`, `serve`, 75 thousand `REST`s, the timer), the UI parsed twice (82; 540 thousand
`Q_stricmp`), the entities' spawn (86, the NPCs' animation files 40), the effects' parse through `std::istream` (29),
the string packages (26), the collision's patches (24), the shader files (25) and the autosave (23).

More workers lose: 32 gave 339,814 passes and 1.82 billion, 64 gave 342,598 and 1.94, 255 did not reach the level in
3 million passes; 64 opened with libmedia's teams kept at 16 (`JK2_TEAM`) and only the large entries' split inflate on
all 64 did not either (2.7 million passes, 6.2 billion). The workers are idle four passes in five already: what is left on
core 0 is mostly the engine's own work, a call at a time.

- **Pictures** (`R_FindImageFile`, `tr_image.cpp`): the `image_t` is made at once as `R_CreateImage` makes it (its
  texture's number, its name in `AllocatedImages`), the file's bytes as they lie in the pak (`FS_SE_ReadRaw`,
  `unzSE_ReadPacked`) go to a worker with nothing to do (`mcw_try_call`, `SE_PictureGive`), which inflates
  (`media_inflate_one`), decodes (`m_jpeg_decode_alone_shrunk`, new in libmedia, or `SE_TGAPixels`, `LoadTGA`'s own
  loops), halves and scans the pixels as `Upload32` would (`se_prepare`) and makes the GPU's words
  (`hlglWordsInPlace`, `GL_SE_WORDS` in the shim). This core uploads it when it is through, and every picture before
  anything is drawn (`RE_BeginFrame`) or an image let go (`R_SE_PicturesFinish`). A picture is drawn only after its
  upload and its size read only where it is drawn. `JK2_PICTURES_AHEAD=0` is the way before; `HLGL_TEX_SUM=1` prints
  every texture's words (661 the same both ways), `JK2_PICTURES_SUM=1` the pixels given to the GL.
- **Sounds** (`S_LoadSound_Actual`, `snd_mem.cpp`): the `sfx_t` gets its length and memory at once, a worker decodes an
  MP3 (`media_mp3_decode_alone`, new in libmedia) and runs `ResampleSfx`'s loop; the samples and `fVolRange` are copied
  in at the next sound loaded and before any sound is started, mixed or freed (`S_SE_SoundsFinish`). A stereo MP3 (the
  engine makes it mono) and a decode under half the engine's length (a file of 10 frames, which minimp3 made silent from
  the start) are made as before. `JK2_SOUNDS_AHEAD=0` is the way before; `JK2_SOUNDS_SUM=1` prints every sound's
  samples' sum at the level's end (269 the same both ways).
- **The map's MD4** (`CM_LoadMap_Actual`) on a worker while this core loads the lumps (`SE_ChecksumGive`, a copy of MD4
  with its state on the worker's stack: md4.cpp keeps it in a static); `JK2_MD4_CHECK=1` makes both (`e0d5b5c7`).
- **Large pak entries** (64 KB packed or more) by libmedia's split inflate as a PNG's stream (`SE_PakInflate`, no CRC),
  whose room the machine without an MMU could not map in one piece for the 10 MB animation file: its rings are halved
  until it can (`m_inflate.c`). Smaller ones stay zlib's, which is faster on one core than libmedia's.
- **`strtod`** (every `atof` too, through the link's `--wrap=strtod`, `__wrap_strtod` in `sys_shaderemu.cpp`): musl's
  makes each number in long doubles, software on this machine; a mantissa up to 2^53 and a power of ten up to 22 is one
  rounded double operation, strtod's own value, the rest musl's. `JK2_STRTOD_CHECK=1` makes both: 0 differ over the load.
- **`SV_GetConfigstring`** copies the string and its end, not `strncpy`'s zeros to 1,024 bytes (the game's
  `G_FindConfigstringIndex` reads every configstring of a set for each name: 70 thousand reads).
- **`SkipBracedSection`** walks `COM_ParseExt`'s tokens where they lie (the shader text's 200 thousand at the start):
  `JK2_PARSE_CHECK=1` walks both, 0 differ (place, lines, token line, last token).
- Memory: one run of four of the sums' (play past the load) was killed for want of memory as play began; the pictures'
  memory out at once is capped at 20 MB and the sounds' at 6 MB since, and two runs since went through. 30 MB are free
  after the load.
- Checks on this build: the sums to frame 450 as before, `RVC_RACES` 0 changes undone over the load.
- Tried and taken out: **the start's renderer and UI kept** at a map of the command line (`CL_FlushMemory` shuts both
  down and `CL_StartHunkUsers` makes them again, every shader file and menu read twice): kept, the level did not come
  in 1.2 million passes; not made at the start, the program crashed before the map.
- Tried and taken out: **a pak's files read ahead**, after each entry `FS_ReadFile` read the next 32 of its folder and
  ending inflated by idle workers. 299 of 1,329 reads found theirs ready, but a call keeps its worker busy until it is
  joined, and the pictures, sounds and libmedia's teams then had fewer: core 0 1.96 to 3.15 billion.
- **Left** (1.86 billion; per frame, the load's 256): frame 4 (the server's spawn, 0.44 billion: core 0's share of the
  animations' split inflate, 0.1, `Q_stricmp`, the game's configstring searches), frames 1 and 2 (0.22), frame 174
  (the world, 0.19), frame 255 (0.15: the screen's dissolve resampled, the music's start), and some 240 frames of 0.3
  to 2 million each (small files inflated by zlib one at a time, the kept MP3s' decoders made, the frame itself).

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

## Speed

Asked for on 3 November 2026: at least 15 frames a second in the menu and in play. **Not reached, and not reachable
in play with this game's structure** ("What bounds it"). What was done, each measured on `rvc_cpu` with the card's
calibrated clock (`--pass-model`, `tools/card_model.json`: 5.49 million instructions a second of core 0, a pass 1.49
ms), the same image pair, the same keys (test_all's `jk2` script, `fixedtime 50`: a frame is one 50 ms frame of the
game, so these are the speeds of a game slowed to them, not of the real-time game, which runs up to four game frames
a slow frame):

| | before (650f94c3) | after | |
|---|---|---|---|
| The main menu (frames 100 to 300, `game_par.py jk2-menu`) | 1.16 frames a second, 4.62 million instructions of core 0 a frame | 7.1 frames a second, 0.63 million (with the logo, the title and the saber it was missing drawn) | |
| The walk (frames 1,500 to 1,700, `jk2-walk`) | 0.31 frames a second, 3.27 s a frame (about 2,200 passes), about 17 million | 1.1 frames a second, 655 passes a frame; core 0 4.7 million, the drawing's worker 2.6 (1.0 and 704 passes with the mix on core 0; 0.64 and 1,046 with the drawing too) | |
| The opening scene's march, the busiest stretch of the start (1,250 to 1,450, `jk2-march`) | 0.28, 3.61 s a frame | 0.7, 1,008 passes a frame; core 0 5.9 million, the drawing's worker 3.6 (0.53 with the drawing on core 0) | |
| The walk on after the turn (1,750 to 1,900) | 0.48, 2.09 s | 0.96, 1.04 s (with the drawing on core 0) | |

The test's shot at frame 1,900 kills the player and the autosave loads from 1,950: the start has no fight to measure
past that; the march is its most crowded stretch (14 Stormtroopers in view).

### Fewer instructions

What each step gave (the walk's instructions of core 0 a frame, from `tools/prof.py` over frames 1,500 to 1,650):

| Step | Saved a frame of the walk | The menu |
|---|---|---|
| The music on the card ("Music on the card") | 2.9 million (and the starved music's gaps) | 3.0 of its 4.6 million |
| The small vector functions inline (`q_math.h` under `SHADEREMU`; an out-of-line `DotProduct` was 7% of the frame) | about 1.5 million | |
| rd-vanilla's triangles made once a vertex in `gl_jk2.c` (an index a copy of its vertex's four words; it was two copies of the whole vertex an index, 165 instructions an index) | about 1.8 million | 0.13 million |
| The client's game's traces against entities: a box (and a brush model not turned) apart from the move's extent is not traced (`cg_predict.cpp`; the effects' particles traced against every solid entity) | 1.8 million | |
| `R_ChopPolyBehindPlane`'s two arrays not cleared (`tr_marks.cpp`, OpenJK's `= { 0 }`: 0.66 million in `memset`) | 0.66 million | |
| A Ghoul2 model found again by its handle, not its name (`G2_SetupModelPointers`, 317 lookups a frame) | 0.3 million | |
| A blob shadow from the same place, turning and size kept (`cg_players.cpp`: its trace and mark are of the world) | 0.5 to 1.1 million (standing people) | |
| A patch tested against its bounds before its facets (`cm_patch.cpp`: Quake III 1.32's test, which Raven's code lacked) | about 0.4 million | |
| A model's frame not between two frames kept (`tr_surface.cpp`), the mix's transfer from locals (`snd_mix.cpp`) | small | 0.76 to 0.68 million, the menu's whole picture now drawn |
| A large draw as it was drawn from its words kept in GPU memory (`gl_jk2.c`'s memo: the logo's first stage) | | 0.68 to 0.63 million |

Every one of these leaves the game as it was: `host_sum.sh`'s nine lines and `test_all.py jk2`, `jk2-sums` and
`jk2-install` the same; captures of the level at frame 900 (`+map demo`) and of the walk at frame 1,650 of the keys'
start (`JK2_SOUND=mix`, so that the card's pace does not move the game) are the same pixels before and after (0 of
307,680 differ). `RVC_RACES` with `RVC_RACES_NEAR=4` (`game_par.py`'s runs of the menu and the walk): 0 undone.

Seen on the way and fixed:

- **The music starved.** The engine filled its raw buffer 0.74 s ahead of the card's clock once a frame; a frame of 1
  to 3 s left the music silent most of the time. On the card's stream voices it is decoded up to 1.5 s ahead by the
  workers, whatever the frame takes.
- **The main menu was missing its picture** (the turning Jedi Knight logo, the Star Wars title, the saber and its
  halos: only the background and the buttons were drawn). Two causes in what the GPU does otherwise than OpenGL
  ("What the menu was missing"); seen in captures before and after.

### Music on the card

`SE_MusicGive` (`sys_shaderemu.cpp`) takes what `S_UpdateBackgroundTrack_Actual` (`snd_dma.cpp`) gave the mixer: the
engine keeps deciding which track plays, from where, how loud, and its intro, loop, dynamic states and cross-fades, as
far ahead of the card's clock as before (`se_music_end`, its raw buffer's end); a track's file is in memory as the
engine holds it (a non-dynamic track is read whole too, `qbInMemory`). A **stream** is a track played from a frame on:
the engine's next give at the frame where the last ended is the same stream, any other a new one (a rewind, a seek, the
fader's copy going on with the old track while the current one rewinds). A stream has its own card voice and a ring of
32,768 frames in sound memory; libmedia (`media_audio_piece`, at half its rate: 22,050 a second, the mix's) decodes
its pieces of a quarter of a second on the workers, four at a time, up to a ring ahead of what is heard. Its voice
starts when the card's clock reaches the time the engine meant its first frames for, so an intro's tail and its loop do
not overlap, and it is let go once all that was given is heard. A seek is only a place (`SeekTo`), and the engine's
time left in a track (`iBytesDecodedTotal`) is the place given. The engine still decodes and mixes the music where
there is no card (the sums' runs: `JK2_SOUND=mix` or `rvc_cpu` without `RVC_SOUND`), no workers, or with
`JK2_MUSIC=engine`. The menu's track is 5,224,320 frames (237 s); the workers spend about 0.5 million instructions a
frame of the menu on it.

### What the menu was missing

Captured with `rvc_harness --cpu --warp` (`JK2_AT=F:echo ...` as the mark): before, the main menu was the background,
the buttons and nothing else. `JK2_GL_LOG=F` prints frame F's draws (`gl_jk2.c`); it showed:

- rd-vanilla's 2D is drawn at depth 0 (the near plane), mostly depth-tested, and a menu's 3D model after it in a scene
  of its own whose depth OpenGL clears first. The machine's GPU has one depth buffer a frame (a clear only clears
  colour) and draws its passes in order whatever the list's order: the background, tested in pass 1, came after the
  logo's opaque pass 0 and over it. Now 2D before the frame's first 3D draw goes to the depth-tested passes at the far
  plane (its z made -1 under rd-vanilla's ortho of 0 to 1), so what is drawn after it in 3D is in front; 2D after it
  goes to the passes without depth, on top, as in the list. `JK2_GL=passes` is the way before.
- The GPU's added pass scales by alpha, which `GL_ONE, GL_ONE` does not: the saber halos' alpha is a wave from -1 to 1
  (`alphaGen wave sin`), so they were dim or gone. Their alpha is made 1.

### The drawing on a worker

Since 3 November 2026 (`JK2_RENDER=core` is the way before). At a frame's end (`R_IssueRenderCommands` from
`RE_EndFrame`, in play: `SE_InPlay`) rd-vanilla's commands go to a worker of their own (`gl_jk2.c`, `jk2RenderGive`: one
of the first 16 cores, whose write caches are the larger, on a stack of 256 KB of its own), which runs
`RB_ExecuteRenderCommands` while core 0 makes the next frame into a second `backEndData`. The picture is a frame later;
a key still reaches the frame of the game it did (the window's part of the swap, `SE_WindowFrame`, is core 0's at the
give; the GPU's list is swapped when the job is done, at the next give or any settle).

- **Ghoul2's bones** were evaluated lazily by the drawing into the model's bone cache, which the game's and the client's
  bolts set up anew meanwhile: at the give core 0 evaluates the bones each of the frame's surfaces names into a set of
  its own (`SE_RSBones`, two sets) and the drawing reads those.
- **Ghoul2's skinning** (`se_skin`, `RB_SurfaceGhoul`'s own loop) is shared out at the give by vertices to up to eight
  helper jobs (`SE_SkinGive`, `mcw_try_call`); the drawing copies a surface's vertices once its helper is done (it is
  posted first and waits for `jk2RenderGo`, so that no helper takes its worker). `JK2_SKIN=drawing`: without helpers.
- **Settles**: any GL call core 0 makes while the job is out (a texture uploaded, a sync of the renderer's, a resize)
  waits for it first (`SETTLE()` in `gl_jk2.c`); `jk2render:` lines (with `JK2_STATS`) count gives, settles and the passes
  waited.
- **Apart**: `R_RotateForEntity`'s scratch matrix is one for each side and `glState.finishCalled` the drawing's, which
  `RVC_RACES` found stored by both cores; every variable's section starts a texel (`build.sh`'s objcopy); no malloc and
  no clock on the worker (the kept model frames and the memo's memory are made on core 0).

Checked: captures (`rvc_harness --cpu`) drawn on the worker and on core 0 a frame earlier are the same pixels (the level
at frame 900, the march at 1,400, the walk at 1,650 with Kyle in view: 0 of 307,680 differ); `RVC_RACES` with
`RVC_RACES_NEAR=4` 0 undone on the walk, the march and the menu; with no window (the sums' runs) nothing changes.

### The mix on a worker

Since 3 November 2026 (`JK2_MIX=core` is the way before). `S_Update_`'s painting, the card's submit and the lip syncs go at
the end of `S_Update` to a worker of their own (one of the first 16 cores other than the drawing's) and are settled at the
start of `CL_Frame`, before the client's game starts or places a sound, and before the sound system is stopped, cleared,
shut down or registers anew: the mix overlaps the server's frame, which reads of the sound only the voices' loudness
(`gi.VoiceVolume`, a frame old now). Only with the card: the sums' runs mix on core 0 as before. The walk 704 to 655 passes
a frame (core 0 5.24 to 4.67 million); the march is bound by the drawing's worker and did not move.

### Ghoul2 on the GPU

Since 4 November 2026 (`JK2_SKIN=helpers` is the way before, the helpers of "The drawing on a worker"; `JK2_SKIN=drawing`
the drawing's worker alone). A Ghoul2 surface is not skinned into rd-vanilla's tessellation buffer: `RB_SurfaceGhoul`
asks `SE_GhoulOnGPU` (`tr_shade.cpp`) first, which draws each of the surface's stages as one GPU draw of the model as it
lies in memory (`jk2glSkinned` in `gl_jk2.c`, the GPU's `VERTEX_BONES` in `shader_vm/src/gpu.h`, `docs/machine/gpu.md`,
"A skinned model"): the model's own triangles as the indices, its 32-byte vertices (on 4 bytes in the file: the GPU
reads three texels and shifts, the same for all of a draw), its texture coordinates, and copied into the frame's memory
only the surface's bone references' matrices (`SE_RSBones`' set, or the bone cache's with `JK2_RENDER=core`) and the
entity's light. The stage's colour is `ComputeColors`' for one vertex, the state (texture, blend, pass) the GL's as
`fast_triangles` takes it; `rgbGen lightingDiffuse` and `lightingDiffuseEntity` are made by the GPU from the skinned
normal as `RB_CalcDiffuseColor` and `RB_CalcDiffuseEntityColor` make them (the entity's multipliers given as the CPU's
floats). The GPU does `RB_SurfaceGhoul`'s operations in its order, unfused (`precise`), and the texture coordinates in
1,024ths as a compact vertex has them: the picture is the CPU drawing's to the pixel. `linux/jk2/skintest.c`
(`JK2_SKINTEST=1 linux/jk2/build.sh`, not in the image) draws one mesh both ways, the vertices at all four
misalignments, lit, unlit and tinted, one to four weights: under `rvc_harness --cpu --warp`, `SKINTEST=cpu` and
`SKINTEST=gpu` are the same pixels (0 differ, 4 November 2026). A surface goes the CPU's way when its shader has
deforms, a polygon offset, fog, dynamic lights, multitexture, surface sprites, texture coordinates made or moved, or
colours of a vertex's own, or its entity a disintegration, distortion or forced alpha; with no window (the sums' runs)
nothing changes. `JK2_SKIN_LOG=1` prints every 100 frames how many surfaces went which way and why: in the demo's
start every one goes to the GPU. The helpers skin nothing while the GPU is asked.

What it gave (`rvc_cpu`, the card's calibrated clock, `game_par.py`'s workloads in calls of under a minute from
`--save`s; the shared image of 4 November 2026 against a private one with this program; `RVC_RACES` with
`RVC_RACES_NEAR=4` 0 undone on the walk and the march; `test_all.py jk2 jk2-sums jk2-install jk2-cleanup jk2-pointer
jk2-resize jk2-seat --long` pass on the image, in sections; a run's passes move with its threads, so each pair is one
setting, `RVC_THREADS=16` to 32):

| | before | after |
|---|---|---|
| The walk (1,500 to 1,700) | 607 passes a frame, 1.12 frames a second; core 0 3.18 million, the drawing's worker 2.76, all cores 9.27 | 472, 1.43; core 0 3.18, the drawing's worker 1.81, all cores 6.74 |
| The march (1,250 to 1,450) | 714, 0.96; core 0 3.87, the drawing's worker 3.31, all cores 10.73 | 544, 1.24; core 0 3.85, the drawing's worker 2.46, all cores 8.33 |
| The load (`+map demo`, the game's start to `client state 7`) | 173,966 passes, core 0 0.882 billion | 173,880, 0.881 (nothing of it changed) |
| The menu (100 to 300; its drawing is core 0's) | 106.0 passes a frame, 7.2 frames a second | 106.1, 7.2 |

Before it the drawing's worker was the walk's chain: core 0 rested 36% of the passes waiting for it, whose passes ended
mostly on a full write cache (`RB_SurfaceGhoul`'s copy of the helpers' vertices into tess, 35 thousand passes in 200
frames, `fast_triangles`' copy of a vertex an index, 17 thousand). Now the walk's 472 passes are near core 0's own
(3.18 million at 5.49 million a second is 389 passes): what is left is core 0's chain ("What bounds it").

### The autosave's memory

On the image of 4 November 2026 the walk died at the autosave (frame 1,460 or so, the guest's 00:20:19): `nommu:
Allocation of length 3928064 ... failed` and `std::bad_alloc`. The saved game's level chunk (`LVLC`) is 1,962,740 bytes;
`SavedGame::compress` sized its output at twice its input, and the vector's doubling made the chunk's own buffer 2.5 MB:
without an MMU a block over 2 MB is a 4 MB block of contiguous memory, which a long game no longer has (an earlier image
had it by chance). Now (`ojk_saved_game.cpp`, `SE_SAVE_ROOM`) the buffer grows to 2 MB less a page at once past 1 MB, the
compress's room grows as it writes (a step writes at most 128 bytes; the bytes are the same), and both are given back
when the file is closed (they held 6.4 MB of the game's memory after the autosave). The walk then plays on to its
death and the autosave's load.

### The players on workers (4 November 2026, work in progress)

`shared/qcommon/se_slot.h`: eight job slots, each a stack of its own (`SE_Slot()` tells a slot by its stack).

- **The players made ahead** (`cg_ents.cpp`, `CG_SE_PlayersAhead`). Before the entity loop, every `ET_PLAYER` entity's
  `CG_AddCEntity` runs on a slot.
  - What it asks of the engine is answered where it runs if it only reads (traces, point contents, marks, `LerpTag`,
    lighting, bolts). It is recorded if it adds to the scene (`cl_cgame.cpp`, `SE_RecBegin`, `SE_RecReplay`), and the
    loop replays the recordings in the entities' order.
  - What cannot be done there ends the job with `SE_SlotAbort`: an allocation, a print, an effect, a spawn, a random
    number of the game's. The entity is then put back from its copies and made in its turn.
  - A player draws random numbers from a state of its own (`cg_rand.c`, `SE_CGRandFor`).
  - Per-slot state: the trace memo, the marks' counts, `calcedMp`, the shadows' cache, `worldMatrix`, and
    `CM_PointContents`'s leaf cache.
  - `JK2_PLAYERS=core` is the way before, `=inline` makes them ahead on core 0, `JK2_PLAYERS_LOG=N` counts them.
  - In the walk, 4,830 of 4,840 players were made ahead.
- **The bones at the give** (`SE_RSBones`, `tr_ghoul2.cpp`) are evaluated on the slots, a bone cache always by the same
  slot. `JK2_RSBONES=core` is the way before.
- **The front end split** (`tr_main.cpp`, `R_SE_RenderViewSplit`, `RC_SE_VIEW`, `R_SE_FrontView`). Core 0 makes a
  view's Ghoul2 surfaces; the drawing's worker makes the rest of the view and draws it.
  - A view that may see the level's portal surface is made whole as before (`R_SE_PortalMaybeSeen`).
  - **Off by default** (`JK2_FRONT=worker` turns it on). `JK2_FRONT=check` said 1,005 of 1,200 views' sorted lists were
    not as the view made whole: not yet explained (the check makes each view twice).
- **The server**:
  - `SV_AreaEntities_r` tests the bounds kept in `svEntity_t` at the link. `JK2_AREA_CHECK=1` printed no difference in
    one walk.
  - The stealth loop skips empty words of the in-use bits.
- **Measured** (the walk, frames 1,500 to 1,700, calibrated, from a save):

  | Change | Core 0 a frame | Passes a frame | fps |
  |---|---|---|---|
  | Before | 3.01 million | 467 | 1.45 |
  | Players made ahead | 2.11 million | 398 | 1.71 |
  | And the bones on the slots | 1.89 million | 392 | 1.74 |
  | And the front end split | 1.66 million | 407 | 1.69 |

  With the front end split, the drawing's worker (2.15 million a frame) bounds the frame.
- **Checked**: `RVC_RACES` with `RVC_RACES_NEAR=4` said 0 undone with the players and the bones.
- **Not checked**: the sums (`host_sum.sh`, `test_all.py jk2 jk2-sums`), captures, the walk with the area and stealth
  changes.
- **Left for 5 frames a second**: the back end shared out to several workers, and the server overlapped with the client
  through copies of the entities and of their Ghoul2 instances.

### What bounds it

After both rounds (3 November 2026, calibrated, `game_par.py`): the walk 655 passes a frame (1.1 frames a second), the
march 1,004 (0.7), the menu 96.5 (7.1). 15 frames a second is 44 passes, about 366 thousand instructions of core 0.

- **Core 0** (the walk, `prof.py`, 4.67 million a frame): the game's frame 1.48 million (`G_RunFrame`: NPCs' behaviours,
  `ST_Commander` and `NPC_FindCombatPoint` 0.5, the game's traces 0.8 through `SV_Trace`, Ghoul2 bolts 0.4); the client's
  game 2.67 million (`CG_AddPacketEntities` 1.42 of which the players' blob shadows 0.71, the effects 0.56, the renderer's
  front end `R_RenderView` 0.48); the kernel, the window and the rest about 0.5.
- **The drawing's worker** (the march, 3.56 million a frame): it stores every vertex several times (tess, the shim's
  words: 20 thousand vertices a frame of the GPU's words alone) and a worker's write cache ends its passes: 563
  thousand worker passes ended full over a march run against 46 thousand of core 0's; core 0 waits about 420 passes a
  frame of the march for it.
- **The game's frame cannot simply go to a worker.** Jedi Outcast's single player has no client and server apart: the
  client's game reads the game's entities directly (`cent->gent`, its client, its NPC state, `ghoul2`) and makes the
  game's Ghoul2 instances' bones and bolts (`G2API_GetBoltMatrix` on the shared caches); about 640 entities are in use. A
  game frame beside the client's would need copies of all that at each handover (about 1 MB of entities and the Ghoul2
  instances' vectors), or a check of every read: not done.
- **Since Ghoul2 on the GPU** (4 November 2026) the walk is 472 passes a frame against core 0's 389 busy ones, the
  march 544 against 471: core 0's chain is now most of the frame, and what pays is off core 0 (its game, its client's
  game, its front end), not the drawing.
- **Even with the game's frame and the front end off core 0 at no cost**, the client's game (about 2.2 million) would stay
  on core 0's chain: about 270 passes, at most 2 frames a second; and the drawing's worker would have to come down from
  1,000 passes to 44, which its stores alone forbid while every vertex's words are made by a core (Ghoul2 skinned by the
  GPU's vertex shader, "Ghoul2", and the level kept in GPU memory are what would change that).

So 15 frames a second in play is not reachable with this game's structure on this machine; the menu is at 7.1.

## What is left, in order

The owner chose on 2 November 2026 to go on past step 1's gate. Steps 2 and 4 have a first form, step 3 its choice:

1. **A renderer of our own** (step 2's real form): `tr_shaderemu`'s method behind `refexport_t`, the level kept in GPU
   memory as kept draws, keeping rd-vanilla's Ghoul2 (`G2_*.cpp`, `tr_ghoul2.cpp`'s bones: the game traces against
   them), models, skins and fonts (the back end is on a worker since 3 November, "The drawing on a worker").
2. **Ghoul2 skinned by the GPU's vertex shader**: done on 4 November 2026 ("Ghoul2 on the GPU"), measured on
   `rvc_cpu`'s calibrated clock; on the card and in the world not yet (Unity's `gpu.cginc` takes it at "Sync shader
   sources").
3. **The game's own 30 million instructions a second** ("What bounds it"): NPCs' behaviours, traces and Ghoul2 bolts
   made cheaper; it cannot simply go to a worker.
4. **Sound effects** as voices of the card (the decoded sounds and the MP3s' 14.5 MB of decoder states out of RAM), films.
   The music is the card's ("Music on the card").
5. **The player's own view checked**: HUD, hands and saber, in a capture after the opening scene with the player alive.
6. **In** (step 6, done 3 November 2026): `image.py --jk2`/`JK2_IMAGE=1` calling `data.py`, `AGENTS.md`'s line.

**For the owner:** whether the pak may be baked (converted) on the build computer at all, given the licence's "no
modified files, no derivative works" (above). An image holds the installer unchanged since 2 November 2026 (below).

## The demo in the machine

The starter runs `cabinst` (`docs/system/install.md`) before the game: `Disk1\Install\license.txt` read out of the
installer and shown in a window (on the terminal without a window system), Accept only once it has been scrolled to its
end, the registry's `jk2.eula` keeping an acceptance; then the pak, 64,001,989 bytes, unpacked on the workers into a
tmpfs file mapped in place, `/tmp/jk2/base/assets0.pk3`, at every start, and the game started with `fs_basepath /tmp/jk2`; the starter deletes `/tmp/jk2` when it ends.
`jk2 --unpack` does only that. On the card (2 November 2026): the licence 0.89 s, the pak 6.69 s (the folder's 74.2 MB
inflated, 255 workers), `jk2 --unpack` from the command to its end 12.3 s. Its CRC-32 is 7-Zip's, `239abc19`.

**Does it play with the pak in RAM?** Yes, measured: `jk2-sums`' run at frame 4,000 (`jk2stat`) had 69,360 KB free with
64,036 KB cached (the pak, which tmpfs cannot drop), where the pak in the ROM left 79,144 KB free and 54,220 KB cached
(the pak's pages read, which the kernel can drop); 183,228 KB were free at the shell after the unpack. The nine sums of
`host_sum.sh` are the same, and `jk2` (the keys' script) and `jk2-sums` pass, the licence answered on the terminal. So
the pak is not read by blocks out of the installer: that way (each block's start and the windows to start from kept a
session) was not needed. `test_all.py jk2-install` is the first start on the desktop: Accept refused, the text scrolled
to its end, accepted, the bar to the end, the pak's CRC-32.

## Not done

The renderer of our own, sound by voices, music and films (skipped: a film says it could not be
opened, in red at the top of the picture), the HUD and the first person's weapon seen, depth ranges, mirrors and
portals, shadows, the glow; `ui_single_instance`, the holodeck's seat; pictures decoded on workers; `RVC_RACES` (no workers are used: `HLGL_WORKERS=0`);
OpenJK's `alloc-dealloc-mismatch` in `strip.cpp` at shutdown (upstream's, seen under AddressSanitizer). The renderer's
variables are not put back at a `vid_restart` as the game's are.

## A private image

    wsl -- bash -c 'HOME=~/agents/X JK2_OUT=DIR bash linux/nommu/build.sh jk2/build.sh && python3 linux/nommu/flat.py DIR'
    cp -r build/nommu/flatroot ROOT                     (of the main checkout)
    JK2_PROGRAM=DIR python linux/jk2/data.py ROOT                   (the installer, unchanged, and the program)
    python linux/nommu/image.py jk2 ROOT build/nommu/Image

(A worktree needs `build\images\linux` and `rvc` of the main checkout, a junction each.)
