# Plan: Star Wars Jedi Knight II: Jedi Outcast, the 2002 demo

Written 1 November 2026. Nothing has been built, and the demo has not been downloaded or opened. It comes from reading
`docs/games/quake3.md`, `docs/plans/aoe-plan.md` and the listing of OpenJK's tree, and from web listings of the demo. What these
did not settle is under "To find out first". When a page `docs/games/jk2.md` exists and it differs from this plan, the page is right.

| | upstream | commit | licence |
|---|---|---|---|
| The game | `github.com/JACoders/OpenJK` (`code/`, `codeJK2/`, `shared/`) | `260c59c` (29 Sep 2026) | GPL-2.0, over Raven's release |
| The demo | the official PC demo of 10 May 2002: `archive.org/download/jk2demo/jk2demo.exe`, 69,680,101 bytes, md5 `7d9b901df28f2f4b11f1b00d5d2a0ca5`, sha256 `d3f8e180...4b80bfb` (`linux/jk2/data.py`) | | its `license.txt`: copies only in the original form and for no money; no modified or added files, no derivative works (`docs/games/jk2.md`) |

**2 November 2026:** step 0 is done on the build computer, and the answers below are in place of the questions. The
page `docs/games/jk2.md` has the details; where it and this plan differ, the page is right.

## Which code

- **OpenJK** is maintained, builds with CMake on Linux and has removed the original's x86 assembly paths as far as its other
  platforms needed (to check). Its Jedi Outcast is single player only: the engine in `code/` with `codeJK2/` as the game
  (`codeJK2/game` is 3.6 MB of source). Its renderer is `code/rd-vanilla`, which we replace, and its Ghoul2 is `code/ghoul2`.
- **`github.com/TriForceX/JK2SourceCode`** is the original source of versions 1.02 to 1.04, single and multiplayer, for Visual
  Studio. It is the starting point only if OpenJK cannot read the demo.
- **JK2MV** is multiplayer. The official demo is single player only by its listings, so it has nothing to run here. It is not
  considered.

So the plan is OpenJK's single player. This is not the cheap way in that a multiplayer map with bots would have been.

## What Quake III gives, and what it does not

Quake III on the desktop is 79 frames a second at 8.4 passes a frame, on the card and on the calibrated clock
(`docs/games/quake3.md`, "Speed"). That is one level, a bot and much work to make it so:

| From `linux/quake3` | Here |
|---|---|
| The level kept in GPU memory, in draws that stay in every frame's list | Should carry over: the BSP format is a descendant, its differences to be read in `tr_bsp` |
| Textures decoded into GPU memory when registered; light maps in the ROM | Carries over, in a data file baked like `quake3-se.dat` (`bake.py`) |
| Game, client's game and menus linked in, no DLLs or virtual machine | The same work on `codeJK2` |
| `tr_shaderemu.c`, a renderer of our own behind `refexport_t` | A new one in the same form: OpenJK's renderer interface is a different one, C++ and larger |
| Models as poses in the ROM, moved by the GPU (`0x10000`) | **Does not apply to Ghoul2**: below |
| The server's frame on a worker, the client's game a frame ahead | To be earned again: single player has no separate clients' snapshots to speak of, and NPCs' scripts are in the game's frame |
| `SE_Who()`, `RVC_RACES`, state sums, `wanted.txt`, the prebuilt pak baked for one level | The same method |

## Ghoul2

The game's characters are skeletal: `.glm` meshes and `.gla` animations, bones blended, weights per vertex. It is the largest new
piece, and what decides the frame rate in a fight. Three ways, to be measured in this order on one Stormtrooper:

1. **On worker cores:** each model's bones and vertices made by a worker into memory the GPU list reads, a frame's models shared out.
   It needs no change to the GPU shader. Its cost is the vertices' stores against 6 KB of texels a pass, so it may need the lanes.
2. **In the GPU's vertex shader:** a new draw kind, bones in a table, four weights a vertex in the ROM. Cheapest for the cores, and
   the shader may be changed, but the GPU's own time and Unity's copy must be measured, and the draw must hold under VRChat's limits.
3. **Bones at a lower rate:** far models' bones at every second or fourth frame, whichever way is taken. It applies only where it
   changes no game state: the game itself asks for bolts and traces against the bones (saber against body), so these must keep
   their exact answer or a state sum moves.

The saber's trace against Ghoul2 models runs on the same bones, and is not a drawing cost: profile it apart.

## The data

Nothing of the demo is committed, as with Age of Empires (`AGENTS.md`):

- `linux/jk2/data.py` fetches the demo against its checksum into a cache outside the tree, and the image's build accepts its EULA
  (`--accept-eula`; written and tried 2 November 2026).
- The licence forbids modified files and derivative works: "baked into one file" below waits for the owner (see "To find out first"); until
  then the pak goes in unchanged (64 MB) and is converted at load time.
- It stays out of `ALL` in `linux/nommu/build.sh` and out of `linux/prebuilt`, and goes into an image only by `--jk2` or `JK2_IMAGE=1`
  on `tools/make_image.ps1` or `image.py`. A plain build downloads nothing and changes no byte.
- The demo's level goes into the ROM in one baked file with what `JK2_WANTED=1` says the game asks for, as `wanted.txt` does for Quake III.
- Saved games go to `/persistence/jk2` (`docs/system/persist.md`). Their size is to be measured: 96,000 packed bytes are all.

## To find out first (answered 2 November 2026)

- **What the EULA of the demo allows**, above all whether a build may unpack it, and whether an image with it may go into the world. This is
  the developer's call. Until then an image with it is a private one.
  *Answer:* `license.txt` allows copies "only in its original form" given at no charge, and forbids modifying, adding or deleting files and
  derivative works. Unpacking the installer to use the demo is using it; the owner accepted the licence for the cache ("Yes to everything",
  2 November). **A baked file as Quake III's is a modified, derived file**: by the licence's words not allowed, even privately. So the port reads
  the pak as it is and converts at load time in the machine, until the owner decides otherwise. Whether an image with the unchanged pak may go
  into the world stays the owner's call.
- **Whether OpenJK reads the demo's data.** *Answer: yes.* The pak's checksum is 1431467275, the one the 1.03 and 1.04 engines require of the
  demo: it is the final game's data. Its level `demo` loads and plays in OpenJK's JK2 code with four changes (new game starts `map demo`,
  the crawl `tc_demo`, the first-map loading screen, and a white picture for Jedi Academy's snowflake the demo lacks). The original source is not needed.
- **What the demo holds:** *Answer* (`linux/jk2/survey.py`): one level `maps/demo.bsp` (9.5 MB, 22,128 triangles of faces and 423 patches, 19
  light maps), 76 NPCs placed (49 Stormtroopers, 15 Imperials, 6 Reborn), 95 scripts; 19 Ghoul2 meshes (people: 72 bones, 2,300 to 3,300
  vertices at LOD 0) and `_humanoid.gla` (17,278 frames, 10 MB); 914 sounds (823 of them MP3), 12 MP3 songs, 4 RoQ films of 512 x 512; 648
  pictures (72 million texels); 108.3 MB unpacked, 64.0 MB packed.
- **A source to fetch from.** *Answer:* the Internet Archive's item `jk2demo` (the official `jk2demo.exe`; md5 as the Archive lists it). The
  pak is a file of the installer's cabinet, taken out by `data.py` with no tool but Python.
- **OpenJK's memory:** *Answer:* the SP engine has no hunk; the zone is a `calloc` a block, 12,670 blocks at the level. 76 MB in all on the
  64-bit build computer (84 MB at most), of which 41.5 MB is sound and music decoded, 9.0 MB the level's file whole, 9.6 MB the animations;
  35 MB without sound. With sound on the card and the large files read where they lie in the ROM, about 12 to 15 MB.
- **C++ and threads in OpenJK**: *Answer:* no threads at all. `Com_Error` is a C++ exception, so OpenTTD's compiler with its unwinder, not
  `rv32-c++`; no RTTI in what is compiled; no assembly but MSVC's.

## Steps

Each ends with what it must show. If a gate fails, stop and report before going on.

0. **The demo, on the host (no machine).** Fetch it, read the EULA, unpack, list the files. Build OpenJK natively for Linux and try the demo's level with a
   null renderer. *Gate: the level loads and its first scripted scene plays, or what stands in the way is known and sized. The developer has read the EULA.*
   *Done 2 November 2026* (`JK2_HOST=1 linux/jk2/build.sh`, `host_sum.sh`): the level loads and 200 s of it play the same at every run with no
   input; the films are skipped and nothing was seen (no picture), so "its first scripted scene plays" is not yet seen. A frame of the client is
   1.0 ms of a Ryzen 9 5900X with rd-vanilla's front end and a null GL. The EULA was read; the owner accepted it for the cache.
1. **The game with no picture on the machine.** `linux/jk2/build.sh` with the C++ toolchain, the game linked in, null renderer, null sound, `rvc_cpu`.
   A state sum for the same run under the native build, as in `linux/openttd/host_sum.sh`. *Gate: the same sum on both; peak memory, the level's load in instructions
   and a game frame's instructions are written down. **If the frame alone is over about 400 thousand instructions on core 0, or memory over about 150 MB, stop and say so.**
   (About 5 frames a second at 2 MIPS, leaving nothing for the rest. A first figure to be argued with, not a measure.)*
   *2 November 2026: the same sums on both (nine lines over 4,000 frames, after three causes of difference were put
   right: arguments drawn in another order, the `.nav` file read with 64-bit longs, the client's game's draws). **The gate
   stops it**: a frame of core 0 is 7.6 to 7.7 million instructions (19 times 400 thousand) with rd-vanilla's front end,
   a null GL and sound mixed on the CPU; the load 28.6 billion (11.5 of them pictures decoded, 9.9 the snow's "outside"
   cache); 167 MB taken of 246 free, 56 of it the pak's page cache, about 111 the program's (`docs/games/jk2.md`, "On the
   machine"). Of a busy frame's 9.0 million, the game itself is 1.05 million each 50 ms of the game's time: about 21
   million instructions a second before anything is drawn, against Quake III's server, which a worker kept at 26 frames
   a second. Waiting for the owner.*
2. **The level drawn, no characters.** `tr_shaderemu`'s method behind OpenJK's renderer interface: the BSP, its shaders' first stages, light maps, the 2D of the HUD and the menus. *Gate: a flight through the
   level with a picture you would recognise, and a frame rate on the calibrated clock with Quake III's as its comparison.*
3. **Ghoul2.** The three ways above on one character, then the most the demo's busiest room has, on the calibrated clock and the card. *Gate: one way chosen on numbers; the first person's hands and saber are drawn right.*
4. **Input, sound, the desktop.** The window, keys and pointer as Quake III's. Sound through the existing layer, music and videos left out at first. An icon, `nxapps`, `gameid.h`. *Gate: the demo played from its start by a person, with a script of keys that holds as a test.*
5. **Speed.** By the `optimizing-games` skill: profile with `tools/prof.py`, the game's frame on a worker where it can be, Ghoul2 on workers if it is there, the script's and NPCs' costs, LTO and PGO. *Gate: the demo's busiest fight at the frame rate agreed at step 3, with `RVC_RACES` and `RVC_RACES_NEAR=4` at 0 undone, walked, not standing, and state sums held.*
6. **In.** `tools/test_all.py jk2 --image` (the sum, on an image made so), `docs/games/jk2.md`, `AGENTS.md`'s note, and the world if the EULA allows.

## Not planned

The full game's data (the developer's own copy would work on a private image, but the ROM is about 290 MB, and nothing is tuned for it). Multiplayer. Dynamic lights, marks and shadows at first: Quake III's list of what it does
not draw is the starting point, and a lightsaber's glow and blade are the one exception that must look right.

## The risks, largest first

1. **The demo's data does not fit OpenJK**, and the older source is a harder start (step 0).
2. **A fight's frame:** Ghoul2, saber traces and NPC scripts together are far more than Quake III's bots (steps 1, 3, 5).
3. **The EULA** does not allow the demo in the world: then this is for private images (step 0).
4. **Memory in one piece** on a machine with no MMU, as OpenTTD's saves found: large blocks are taken at the start, once.
