# The ports, one by one

Where each port's source comes from, its own files, the tricks worth copying, and how it is
checked. Paths are from the repository root; each game's doc is `docs/games/NAME.md`.

## OpenTTD (newest; the cleanest template)

- **Source**: OpenTTD 15.3 tag, `linux/openttd/build.sh`; open base sets (OpenGFX, OpenSFX,
  OpenMSX) fetched from cdn.openttd.org; zlib and liblzma built by the script.
- **Compiler**: `toolchain.sh` (GCC 13.3, full libstdc++, emulated TLS, libatomic, PIE) and
  `ehframe.c` (registers `.eh_frame` before any constructor). CMake with a toolchain file;
  host tools (strgen, settingsgen) built natively with `-DOPTION_TOOLS_ONLY=ON`;
  `CMAKE_DISABLE_FIND_PACKAGE_*` for every optional library.
- **Patch** (`openttd.patch`, under `WITH_SHADEREMU`): build takes the new drivers and
  `mixer_shaderemu.cpp`; `StartNewThread` refuses; null video driver prints `OPENTTD_SUM`;
  8-bit blitter default; no zoom closer than normal; 64x64 new map; no MD5 of base sets; sprite
  zoom levels made lazily; `RandomAccessFile` 8 KB aligned reads, seeks inside its buffer;
  sprite offsets read once; industry sort on names made once; river search marks a bit a
  tile; small title game (`make_title.sh`, 13.7 KB); world generation drawn once a second;
  `MachineCopy` scroll in `blitter/8bpp_base.cpp`. Start: 1.39 billion to 350 million.
- **Drivers**: `shaderemu_v.cpp` (window, paletted texture, `seglQuad`, `seglSwapEarly`/settle
  on lock, keys, pointer, `GrGetNextEventTimeout` wait, `OPENTTD_RATE`, `OPENTTD_SCROLL`);
  `shaderemu_s.cpp` + `mixer_shaderemu.cpp` (a channel a voice, sounds copied into a 1 MB pool
  once); `shaderemu_m.cpp` (MIDI on FM voices with DMXOPL `genmidi.op2`).
- **Workers**: a viewport's parts, each found, sorted and drawn by a core into a buffer of its own (`ViewportFlush`, `OPENTTD_WORKERS`);
  the game tick on a worker (`shaderemu_t.cpp`, `TickYieldPoint`, `OPENTTD_TICK=slices|whole`), the link graph's jobs, ships' paths ahead. Compiled whole with
  played counts (`openttd.profile.gz`, `counts.py`). `docs/games/openttd.md`.
- **Desktop**: starter with `XDG_DATA_HOME`/`XDG_CONFIG_HOME` to `/persistence/openttd`;
  `nxapps.73-openttd`; `nxopen.73-openttd` (`openttd sav OTT openttd -g %s`); icon
  `media/openttd.128.png`.
- **Checks**: `host_sum.sh` (native dedicated build, `test.cfg`, seed 1 500 ticks `9d6fc0a0`,
  seed 2 2,000 ticks `66880c87`); `test_all.py` case `openttd`.

## Doom (Microwindows' contrib Doom)

- **Source**: `$WORK/src/microwindows/src/contrib/doom`, built by `linux/nanox/doom.sh`; no patch
  file: `sed` edits into `*_ours.c` with `grep -q`/`grep -c` guards, `-DFoo=Foo_port` renames,
  `told()` inserting change notifications and checking their count. Shareware WAD fetched.
- **Files**: `linux/nanox/doom_video.c` (320x200 page in GPU memory, palette, GPU melt from the
  window's own pixels `seglWindowTexture`, clock word, `doom_idle`/`doom_wait` end the pass,
  `D_Display` settles); `doom_gl.c` (level kept with `seglKeep`, sky as `FRAGMENT_LAID`,
  textures stored on their side so a column is a row of bytes, view on up to four workers via
  `mcw_for_begin`, compiled apart); `doom_fast.c` (sight check order and loop, subsector by
  blockmap cell, line marks in their own arrays, things asleep, `doom_ahead` on a fifth worker);
  `doom_sound.c` (lumps played in the ROM, MUS to FM).
- **Flags**: `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops -flto -fno-pie`,
  `-fsigned-char`; `APART` objects with 16-byte section alignment and `zz_pad`.
- **Desktop**: starter in `/persistence/doom` with `HOME` there; `nxapps.50-doom`;
  `nxopen.50-doom` (IWAD, PWAD by first bytes).
- **Checks**: `DOOM_HASH_TICS=100 doom -1 -timedemo demoN` state sums; `DOOM_VIEW=told
  DOOM_VIEW_CHECK=1` (0 not as told); `DOOM_PORT=rest`; `DOOM_SUM` order-free sum with
  `DOOM_STOP_TIC`; `doomstat:` lines, `DOOM_TOTALS=1`. Cases `doom-1..3`, `doom-told`, `doom-rest`.

## Quake (GLQuake)

- **Source**: id's Quake at `bf4ac42` + `quake.patch` (`git diff -- WinQuake`), `QUAKE_TREE=keep`;
  shareware 1.06 pak fetched (an LHA inside a zip, opened by lhasa built in the script), md5 checked.
- **Files**: `qgl.c` (GL 1.1 to `gles.c`'s float build; `include/GL/gl.h` renames calls to
  `qgl*`; texture pool, poses, light tables), `world_shaderemu.c` (level kept, PVS leaves),
  `mesh_shaderemu.c`, `fmath.h`/`fmath_shaderemu.c` (float trig), `vid_shaderemu.c`,
  `sys_shaderemu.c`, `snd_shaderemu.c` (voices at WAVs in the pak in the ROM),
  `server_shaderemu.c` (server frame on a worker), `scene_shaderemu.c` (picture on a worker),
  `gpu_shaderemu.c` (gles.c plus draws made again).
- **Tricks**: clock in whole ms, no doubles a frame; particle 3 texels; `seglSwapEarly` with
  settles where the list's memory is written; `seglMemorySpare` for 3.9 MB more GPU memory;
  GPU memory as a cache of textures. Starter links `id1` to `/persistence/quake`.
- **Checks**: `QUAKE_SERVER=inline|late|wait QUAKE_SUM=10 quake +map e1m5 +host_framerate 0.05`
  (same digest in all modes), `QUAKE_PARTICLE_SUM`, `QUAKE_LIGHTPOINT=check`, `QUAKE_TRACK` walk,
  `QUAKE_TEST_EXIT`; `QUAKE_SWAP=wait`, `QUAKE_HOLD`/`QUAKE_HOLD_LATE`.

## Quake III Arena (ioquake3, q3dm17)

- **Source**: ioquake3 at a commit + `quake3.patch` (`git diff -- code`; a tree already at the
  patch is left alone); demo pak fetched; `bake.py` writes `quake3-pak0.pk3` (stored) and
  `quake3-se.dat` (what the GPU reads in the ROM) from `wanted.txt` (`QUAKE3_WANTED=1` run).
- **Files**: `tr_shaderemu.c` renderer, `gpu_shaderemu.c`, `snd_shaderemu.c`,
  `server_shaderemu.c`, `client_shaderemu.c`, `sys_shaderemu.c` (modules linked as objects,
  variables reset at load), `jpgraw.c`, `q3pack.c`.
- **Tricks**: pools as one mapping each (`SE_Pool`), smaller on no-MMU via `sed` in
  `linux/nommu/build.sh` and `+set com_hunkMegs 8 +set com_zoneMegs 4` in the starter;
  pictures decoded into GPU memory at level load; light maps and poses read by the GPU from
  the ROM; server, client game and scene on workers via `mcw_call`; `SE_OnWorker()` for state
  both write; core 0 polls job helpers while it waits (`helpers_poll`).
- **Checks**: `QUAKE3_SUM=1` with `QUAKE3_SERVER/CLIENT/SCENE/SIDE=inline|worker` (cases
  `quake3-*`), seeded bots `QUAKE3_SEED=7`.

## Half-Life: Uplink (xash3d-fwgs + hlsdk-portable + mainui), no-MMU only

- **Source**: three trees at commits, three patches; `tree()` re-applies only when needed;
  `XASH_STATIC_LIBS` links client and server into one program; `XASH_LOW_MEMORY`.
- **Data**: `data.py` fetches the installer when an image is made (md5-checked), REWise unpacks
  (`data.sh`), `repack.py`: sounds IMA ADPCM with a `ckpt` chunk played in the ROM, deflate in
  segments for maps/models/sprites, textures once in a WAD, 24-bit pictures as JPEG. Never in
  `linux/prebuilt`; `HALFLIFE_DATA=0` builds without it.
- **Files**: `gl_shaderemu.c` (GL 1.1), `gl_kept.c` (level kept, bones ahead on workers),
  `gl_studio_job.c`, `rnd_shaderemu.c` (the 3D drawn a frame behind on a worker, the default; `HL_RENDER=core|wait`),
  `sv_shaderemu.c` (paced server on a worker), `vid_shaderemu.c` (window, splash, seat),
  `avi_shaderemu.c` (films), `snd_shaderemu.c`, `img_media.c`/`snd_media.c` (libmedia).
- **Tricks**: a 28 MB RAM pool the GPU reads directly (no MMU: a pointer is its address,
  `HL_POOL_MB`); spillable textures; text drawn again from captured quads; workers cannot open
  packed files, so a level change goes to core 0.
- **Checks**: `HL_ROTATION=check` (0 wrong), `HL_PREDICT=check` (0 over 0.1), `HLGL_PICSUM=1`, `HL_MARK=FROM,TO` for counting; walked with `HL_SCRIPT` under `RVC_RACES`.

## Tiberian Dawn and Red Alert (Vanilla Conquer)

- **Source**: Vanilla Conquer at a commit; `linux/tdawn/vanilla-conquer.patch`; Red Alert applies
  that patch then its own (`linux/ralert/vanilla-conquer.patch`) on a copy of its own
  (`rsync -c`, files keep their times unless their bytes change: 2-3 s no-op builds).
- **Compiler**: makes `rv32-c++`: libstdc++ headers only over musl, `cxxrt.cpp`,
  `cxx/bits/c++locale.h`; `-O3 -flto` with raised inline limits, `-flto-partition=1to1
  -fno-toplevel-reorder`, `-fno-lifetime-dse`; PGO from played games (`tdawn.profile`,
  `ralert.profile`, `profile.py`).
- **Files**: `host.c` (Nano-X window, two 8-bit pages and a pointer picture in GPU memory,
  events, `pointer_read`, `host_picture` for movies, PGO counters), `shaderemu.cpp` (the game's
  side of SDL's video and keyboard; `Catch_Up` defers the page copy), `gl.cpp` (map on the GPU,
  atlas with gutters), `soundio_shaderemu.cpp` (ADPCM pack from `tools/make_tdawn_sound.py`),
  `linux/ralert/workers.c` (VQA rows on workers, LCW a frame ahead, `workers_alloc`).
- **Tricks**: clock word; folder names cached, files run where they lie in the ROM
  (`/usr/lib/tdawn/game`), mix files mapped (`ShaderEmu_Mix_Map`); bits a cell for target scans;
  scene a frame behind on a worker; path searches on a worker (`TDAWN_CHECK_PATHS=1`: 0 wrong);
  map dragged by the right button (no edge scroll for beams).
- **Checks**: `TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=N TDAWN_FRAMES=N tdawn
  FROMINSTALL` state sum (agrees with an x86-64 build); `RALERT_SIDE`, `RALERT_SEED`,
  `RALERT_FRAMES`; `TDAWN_SUMS`/`TDAWN_REFRESH` and `tools/tdawn_sums.py`; `TDAWN_CHECK_GPU`;
  `RALERT_MOVIE_CHECK`. Cases `tdawn-*`, `ralert-*`.

## NES (Nofrendo from esp32-nesemu)

- **Source**: esp32-nesemu at a commit + `nofrendo.patch` (edit a clean clone, save `git diff`);
  Thwaite and Nova the Squirrel (GPL-3) fetched. `NES_OUT` for a private overlay.
- **Files**: `osd_shaderemu.c` (window, pad, 256x240 bytes paletted, frame pacing),
  `rc6502.h` (recompiled 6502), `apu_shaderemu.c` (five channels as voices with waves in sound
  memory; DMC decode on a worker via `mcw_call`), `exact_rom.py` (own test cartridge).
- **Checks**: `NES_SUM=1 NES_PPU=inline NES_FRAMES=120 nes thwaite` RAM and picture sums, the same
  with `NES_LIST=worker` and every `NES_*` mode; `NES_CELLS_CHECK`; `NES_HOLD_LATE` with
  `NES_SWAP=wait`; `nes-kill` (mctest PASS after `killall -9`).

## ClassiCube

- **Source**: commit + `classicube.patch`, every file compiled with `PLAT_SHADEREMU` (each leaves
  itself out by the platform's defines); low-memory build, no launcher, no FreeType.
- **Files**: `Graphics_ShaderEmu.c` (on the GPU device directly: command lists, matrix blocks),
  `Window_ShaderEmu.c` (window, keys, pointer words, terminal lines drive the game),
  `Math_ShaderEmu.c` (float trig), `png_shaderemu.c` (PNG pack decoded on workers),
  `Audio_ShaderEmu.c` (ADPCM pack in the ROM; MP3 tune decoded on workers into a stream ring),
  `store_zip.py`, `sound/make_sound.py` (Mojang's sounds by SHA-1).
- **Tricks**: chunk meshes built by a second builder on workers; `mcw_touch` on the builder's
  data (a worker otherwise stopped at every untouched page: 3M instructions in 20 s).
- **Checks**: `CLASSICUBE_MESH=inline|worker CLASSICUBE_MESH_SUM=1 CLASSICUBE_SEED=7`
  (`singleplayerphysics=false` in `options.txt`), `CLASSICUBE_TEXTURE_SUM`, `CLASSICUBE_CHECK_KEPT`,
  `CLASSICUBE_HOLD_LATE` + `CLASSICUBE_SWAP=wait`, seat cases with `rvc_cpu --host`.

## Micropolis (MicropolisCore's engine)

- **Source**: sparse checkout at a commit (engine and content only) + `micropolis.patch` (five
  fixes, one found by a host/guest sum mismatch); upstream's callback replaced by
  `engine.cpp` and a queue (`mp.h`); front end `nxmicropolis.c` is new.
- **Build**: `rv32-c++` with `cxxrt.cpp` and libstdc++'s `tree.cc`/`list.cc`; `-fwrapv`; the
  engine also built for the host into `micropolis-sum`, run at the end of the build.
- **Data**: `pack.py`: map as a `SEGL_TILES` layer in a 64x64 ring of cells (a scroll writes only
  new cells), sprites as a second layer via `seglSprite`, overview colours; sounds decoded by
  `media_test-host` into `sounds.bin`, played from the ROM on four voices.
- **Desktop**: registry keys `micropolis.speed/.autobudget/.disasters`; cities in
  `/persistence/micropolis`; `nxopen.72-micropolis` for `.cty`.
- **Checks**: `MICROPOLIS_SUM=CITY|SCENARIO,TICKS[,SEED]` (case `micropolis`);
  `MICROPOLIS_REPORT=1` prints window places for `nxkey` scripts.

## The desktop's own games and apps

`linux/apps/nxpinball.c` (`PINBALL_DEMO=1800` sum, case `nxpinball`), `nxtetris.c`, `nxmine.c`;
`linux/physics/nxphys.c` (255 workers, `PHYS_COUNT=1` must say 0 pages brought) and
`linux/raytrace` - written for the machine, useful as small examples of workers and `segl`.
