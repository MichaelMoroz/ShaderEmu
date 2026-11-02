# Star Wars Jedi Knight II: Jedi Outcast, the 2002 demo

Begun 2 November 2026, by `docs/plans/jk2-plan.md`. **It does not run on the machine yet.** What there is: OpenJK's
single player of Jedi Outcast built as one program by `linux/jk2/build.sh`, with the game and the renderer linked in
and a platform layer of our own, and the demo's one level loaded and played in it on the build computer (WSL), with
no window, no picture and sound mixed into nothing, 200 s of the game the same at every run. The build for the
machine is written in the same script but has not been compiled: the guest's toolchain was in use by another build
that day (see "What is left").

    wsl -- python3 linux/jk2/data.py host ~/jk2-host/data --accept-eula   (the demo, fetched once into a cache; its licence)
    wsl -- bash linux/jk2/host_sum.sh                                     (builds the build computer's program, plays, sums)
    python linux/jk2/survey.py PAK                                        (what the demo's pak holds)

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
- `sys_shaderemu.cpp` is what `shared/sdl` was: no window (a 640 x 480 "mode"), no input but the terminal's console
  lines, sound mixed into a buffer nobody hears at the pace of the client's time, and the end (`SE_Exit`: the C
  library's `exit` ran a null destructor of a linked-in module).
- `nullgl.c` is the 71 OpenGL 1.1 calls the renderer makes, doing nothing: everything the renderer does but draw still
  happens (the level, shaders, pictures decoded, Ghoul2 bones and vertices made, the scene sorted).
- `se_rand.c` is the game's `rand`: glibc's and musl's differ, MSVC's is what the game was written against.
- `survey.py` reads the pak; `data.py` fetches it; `host_sum.sh` is the reference sums.

What the patch changes besides the demo's three things: the engine without SDL (`sys_main.cpp`, `sys_loadlib.h`);
`fixedtime` not a cheat (a map without cheats put it back to 0); `JK2_SEED` for the game's seed (`sv_game.cpp`);
films not played unless `JK2_CINEMATICS=play` (an in-game film pauses the game until it ends or a key is pressed);
`SE_ZoneBytes` (`z_memman_pc.cpp`); the state sum (`g_main.cpp`).

    JK2_FRAMES=N        quit after N frames, with a jk2stat: line
    JK2_STATS=N         a jk2stat: line every N frames: the frames' host time, the zone's bytes and blocks, its peak
    JK2_LAST=CMD        a console command before quitting (zone_details: the zone by kind)
    JK2_SUM=N           a jk2sum: line every N frames of the game ("Is it the same game")
    JK2_SEED=N          the game's random numbers from N, not the clock
    JK2_SOUND=off       no sound at all (the game then loads none, and a save of the game crashed before nullgl's fix)
    JK2_CINEMATICS=play the films played (the default skips them)
    +set com_demo 1     the demo (the starter's)
    +set fixedtime 50   each frame 50 ms of the game's time, whatever it took
    JK2_CFLAGS=, JK2_LINK=, JK2_WORK=, JK2_SRC=, JK2_TREE=keep, JK2_JOBS=N   (to build.sh)

## Is it the same game

`host_sum.sh` (2 November 2026; the build computer, `fixedtime 50`, seed 7, no input, sound mixed into nothing): the
same lines at every run, and from a tree built from a fresh clone. The machine's program must print them.

    jk2sum: frame 400 time 21250 entities 641 sum d649d252
    jk2sum: frame 800 time 41250 entities 641 sum 28600e75
    jk2sum: frame 1200 time 61250 entities 643 sum 0c8026fc
    jk2sum: frame 1600 time 81250 entities 639 sum 44e04d11
    jk2sum: frame 2000 time 101250 entities 639 sum 505e07aa
    jk2sum: frame 2400 time 121250 entities 639 sum 23a3e335
    jk2sum: frame 2800 time 141250 entities 639 sum d45b607d
    jk2sum: frame 3200 time 161250 entities 639 sum 5bd8fada
    jk2sum: frame 3600 time 181250 entities 639 sum 7cd10745

A line is a sum over every entity in use: what it is, its health, where it is and faces to an eighth of a unit, a
client's place, speed, weapon and animations, an NPC's behaviour, and the game's random state. Three things made two
runs differ before: the null sound's position went by the computer's clock, so a voice ended in another frame (it goes
by the client's time now); the game's seed was the clock (`JK2_SEED`); and the home folder a run left behind (its
`openjo_sp.cfg` and autosave) changed the next run's game from frame 400 on, so `host_sum.sh` starts from an empty one.
On the machine the run must start from an empty `fs_homepath` too. The build computer is built with the
machine's float flags (`-fsingle-precision-constant -ffp-contract=off -fno-math-errno`); its libm and musl's may still
differ in a last bit, which the eighths are for. Whether they suffice is step 1's to see.

Not yet seen: whether the first scripted scene plays as it should. The opening crawl and film are skipped, and with no
picture there is nothing to look at; the level loads, its scripts run (ICARUS), the autosave is written, and 200 s of it
play without error.

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
the level's file (9.5 MB) and `_humanoid.gla` (10 MB): both should be mapped from the ROM.

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

Each needs the guest's toolchain (`~/nm`, `linux/openttd/toolchain.sh`), a private WSL home by the
`building-the-system` skill, or the owner.

1. **The machine's program with no picture** (the plan's step 1). `bash linux/jk2/build.sh` with OpenTTD's compiler:
   the platform layer is the same file; to add: `jk2` in `BIG` of `linux/nommu/flat.py` (1 MB stack), a starter
   (`/usr/bin/jk2`: `cd` to a folder with `base` linked to `/usr/share/jk2/base`, `+set fs_homepath /persistence/jk2
   +set com_demo 1`), the clock word for `Sys_Milliseconds` (not `gettimeofday`), and `data.py OVERLAY` with
   `JK2_PROGRAM`. *Checked by:* `rvc_cpu` with `JK2_FRAMES=4000 JK2_SUM=400 JK2_SEED=7 jk2 +set fixedtime 50 +map
   demo` printing `host_sum.sh`'s lines; the peak of the zone and the program's memory written down (`python
   linux/nommu/memory.py`); the load's and a frame's instructions (`--count-from`), against the plan's 400 thousand and
   150 MB. A first guess from the host's times and the zone: the frame is far over 400 thousand with rd-vanilla's
   front end, and the memory is over 150 MB until sound and the large files stay in the ROM.
2. **Memory to fit**: the level's file, `_humanoid.gla` and the meshes read where they lie in the ROM (`CM_LoadMap`'s
   `gpvCachedMapDiskImage`, `R_LoadMDXA`, `R_LoadMDXM`), sounds not decoded into the zone (step 4's sound card);
   *checked by* `zone_details` on `rvc_cpu` and `memory.py`.
3. **The picture** (step 2): a renderer of our own in `tr_shaderemu`'s form behind `refexport_t`, keeping rd-vanilla's
   Ghoul2 (`G2_*.cpp`, `tr_ghoul2.cpp`'s bones: the game traces against them), models, skins and fonts, and replacing
   its drawing; or first rd-vanilla over Half-Life's GL 1.1 shim (`linux/halflife/gl_shaderemu.c` on `gles.c`) for a
   picture to compare with. The pictures are decoded at load from the pak as it is (the licence), on workers by
   libmedia. *Checked by* a flight through the level on `rvc_harness --cpu --warp` (through the queue), the calibrated
   clock against Quake III's 79 frames a second.
4. **Ghoul2** (step 3): the three ways of the plan on one Stormtrooper (2,843 vertices, 72 bones, four weights).
5. **Input, sound, the desktop** (step 4); **speed** (step 5); **in** (step 6: `image.py --jk2`/`JK2_IMAGE=1` calling
   `data.py`, `test_all.py jk2 --image`, `AGENTS.md`'s line). Each as the plan says.

**For the owner:** whether an image with the demo's pak may go into the world, and whether the pak may be baked
(converted) on the build computer at all, given the licence's "no modified files, no derivative works" (above).

## Not done

No picture, sound, input or window; nothing built or run on the machine; films; the second load of the game after a
saved game is restored (the game's module is not loaded afresh: its statics keep their values, as Quake III's
`sys_shaderemu.c` had to put right); OpenJK's `alloc-dealloc-mismatch` in `strip.cpp` at shutdown (upstream's, seen
under AddressSanitizer).
