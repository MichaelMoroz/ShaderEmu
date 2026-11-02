# OpenCNC3D: Command & Conquer of the Nintendo 64, on the PC's code

Begun 2 November 2026, by `docs/plans/opencnc3d-plan.md` (its steps 0, 1 and the start of 2). OpenCNC3D
(`github.com/freschism3000/OpenCNC3D`, GPL-3.0, pinned at its v0.9.0, commit `248233af`) is a PC version of the N64
Command & Conquer: EA's GPL Tiberian Dawn logic as a library behind the Remaster's `CNC_*` interface (its "brain"),
the cartridge's models and terrain baked into one pack a mission, and an OpenGL renderer (its "eyes"). What there is
here:

- **Step 0, the data.** The owner's European cartridge is checked and read where it lies or from a cache outside the
  tree; **its packs bake**, with a baker of ours in place of OpenCNC3D's lost one, and every pack passes OpenCNC3D's
  own checks; the 1995 DOS data of the freeware Gold disc is fetched by range and checked.
- **Step 1, the brain on the machine, no picture.** One flat program, `SCG01EA` loaded through `CNC_*`: 60 of 60
  objects as the cartridge's INI says (73 of 73 for `SCB01EA`), 3,000 ticks with the **same sums on the build computer
  and `rvc_cpu`**. A tick was 503 thousand instructions of core 0 and the load 331 million, over the plan's gate
  (Tiberian Dawn's 2D logic is about 80 thousand a frame); with `MapClass::Clean` on the cells that hold something and
  `linux/tdawn`'s file search, **a tick is 74 thousand and the load 54 million**, the sums as they were ("The gate").
- **Step 2, OpenCNC3D's own program, under way.** Its app and renderer (`app/opencnc3d.cpp`, `game/cnc_eyes.cpp` and
  their C files, unchanged) built by OpenTTD's C++ compiler with all of libstdc++, the brain linked in, SDL2 replaced by a
  layer of ours over Nano-X (`sdl_nanox.c`) and its GL by Half-Life's shim (`gl_ocnc.c`): **it compiles and links**
  (milestone 1, "The game").

Nothing of the cartridge or the DOS data is in the repository, `linux/prebuilt` or the shared image, and the port is
not in `ALL` of `linux/nommu/build.sh`: a plain build downloads nothing of it and changes no byte.

    wsl -- python3 linux/opencnc3d/data.py CACHE/dos                         the DOS data, by range from GDI95.ISO
    wsl -- bash linux/nommu/build.sh opencnc3d/build.sh                      the brain for the machine (fetches the sources)
    wsl -- python3 linux/opencnc3d/bake.py --tree $WORK/src/opencnc3d --cache CACHE --rom ROM SCG01EA
                                                                             the cartridge checked, extracted, the packs baked
    wsl -- bash linux/opencnc3d/host_sum.sh CACHE SCG01EA 3000 300           the build computer's sums, and the INI check
    wsl -- python3 linux/opencnc3d/overlay.py ROOT CACHE PROGRAM             program and data into a private image's root
    python linux\nommu\image.py ocnc ROOT KERNEL                             the private image (HALFLIFE_DATA=0)

`overlay.py ROOT CACHE OUT` takes `build.sh`'s `OPENCNC3D_OUT` folder: both programs, the MIX files, the missions and the
packs. In the guest: `opencnc3d-brain /usr/share/opencnc3d/ /usr/share/opencnc3d/ SCG01EA 3000 300` (content folder,
mission folder, mission, ticks, a sum every that many ticks, and a seed: 0, the game's own, by default), and with the
window system up `opencnc3d /usr/share/opencnc3d/ /usr/share/opencnc3d/ SCG01EA /usr/share/opencnc3d/SCG01EA.pack
[FRAMES]` for the picture. `ROM` is the dump, or
`OPENCNC3D_ROM`; `CACHE` a folder outside the tree (here `~/agents/ocnc/cache` in WSL).

## The data (step 0)

### The cartridge

`D:\ROMS\Command & Conquer (Europe) (En,Fr).z64`, 33,554,432 bytes, SHA-1 `0d5211e2…288ce305`, MD5
`42da4c7d040f9e7cd046a42ec3e68027`: the MD5 OpenCNC3D's `docs/re-findings.md` gives for the EU dump its offsets are
quoted against. `bake.py` refuses any other file, copies it into the cache, and extracts it with OpenCNC3D's own
`tools/romdump/archive.py`: 1,624 records, all decompressed, 0 codec failures. Laid out by type
(`CACHE/share/assets/extracted/<EXT>/`) as OpenCNC3D's asset store expects, and pointed at by links in the fetched tree
(`data/rom`, `tools/bakery/sharecopy/rom`, `tools/bakery/support/share/OpenCNC3D`): nothing of it lies in this
repository.

The brain's mission files are made from it too: the `.INI` verbatim, and the `.MAP` (64 x 64 big-endian tile ids)
as a PC `.BIN` by OpenCNC3D's `game/missions/n64map_to_bin.py` through the theater's `TL4`/`TL8` tables. Both
`SCG01EA` and `SCB01EA` come out **byte for byte** the copies OpenCNC3D's tree carries (`bake.py` compares them).

### The pack baker, ours

OpenCNC3D's `bake5.py` begins `import pk4mod; B = pk4mod.load()`, and that module loaded `bake_pk4.pyc`, bytecode
whose source was lost and which the published tree does not carry; its terrain resolver `n64_terrain.py` is not
published either. Everything else of the bake is (`bake5.py`, 3,061 lines, its slices and tables, `tools/romdump/`).
So two files of ours, GPL-3.0, written from what the tree says of them, are copied beside `bake5.py` at each run and
OpenCNC3D's own `bake5.py` runs unchanged:

- `linux/opencnc3d/bake/pk4mod.py` is the module `B`: a walker of F3DEX2 display lists with the RDP's state (tiles,
  TMEM by address, the TLUT, PRIM, the combiner and the render mode; a `G_DL` call shares its caller's state),
  `decode_tex` (CI4/CI8 through the list's TLUT or the house table `GLOBAL_TLUT`, RGBA16/32, I4/I8, IA4/8/16; rows at
  the tile's width for a block load, at the image's from its corner for a tile load), `bake_mesh` (uv over the
  texture's used size by the S10.5 rule of `re-findings` M5, wrap bits as `cnc_eyes.cpp`'s `wrap_mode` reads them,
  shadow mode for I textures, cutout for any clear texel, a black untextured face takes PRIM, the decal combiner
  `PRIM, TEXEL0_A` takes PRIM's colour), a `TexBank` padded to powers of two, the infantry strips through
  `tools/romdump/soldier.py` (sets E1..DP, STAND/RUN/DETH1 as the renderer's `SprAnim`, palettes 0 GDI and 1 Nod),
  and the writers. Which the walk's model of the cartridge got right was decided by `bake5.py`'s own assertions, not by
  looking: two rules (the vertex alpha slot, and PRIM's colour on the decal combiner) came from a first run that its
  `flatten_lit_meshes` equality check refused.
- `linux/opencnc3d/bake/n64_terrain.py` makes `terrain_<SCEN>.json` and the theater's atlas: 24 x 24 tiles, 32 a
  row at a 26-texel pitch with an edge-repeated gutter, the 4bpp bank (`DA4` through the `ND4`-chosen 16 colours of
  `PA4`) then the 8bpp bank (`DA8` through `PA8`), a black texel clear (the sea's hole), a clear cell drawing CLEAR1's
  icon `(x&3)|((y&3)<<2)` (OpenCNC3D's `tools/gen_terrain_tiles.py` and `tools/heightmap-viewer/export.py` say all of
  it). TEMPERAT has 315 + 443 tiles (242 with holes), DESERT 945 + 28 (381).

What proves a pack, all run on every bake (2 November 2026, both `SCG01EA` and `SCB01EA`):

- `bake5.py`'s own equality checks pass: the 15 all-packed-normal meshes it expects and no other, the PRIM-only
  combiner on exactly its 21 triangles over 16 meshes, the translucent faces on exactly its 153 faces over 20 display
  lists (6 whitened), the turret and rotor parts of its 10 models, every construction-section count against the
  display lists' own batches, 18 vehicle shadows on 5 textures, the cursor flipbooks, texture books, damage art, the
  nuke, the ion cannon and the rally flag, and `check_pk8_tail` reading the tail back at the renderer's offsets.
- OpenCNC3D's `packinspect.py summary` reads every section back: `CNC3DPKG v16 SCG01EA/TEMPERAT 8.2 MB, textures 339
  (86 with a GDI variant), meshes 264, tris 11234, types 592, cells 4096, sprites 75, infantry 20, animated meshes 24,
  terrain_tex=0 water=(331, 332) seabed=333 trailing=0, heights: 4225 corners min=28 max=130, cm tint: 248 of 4225
  corners non-black, PK8/PKA/PKC tail OK`. `SCB01EA/DESERT` gives the same counts with its own terrain.
- Looked at, privately (no picture of the cartridge's art is kept): the terrain cells laid out from the pack draw GDI
  1's coast, roads, cliff and beach with the sea in the holes; the first 64 bank textures and four models (the medium
  tank, the harvester, the construction yard with its fans, the jeep) rasterised from the pack are whole and textured
  where they should be.

Not the same as OpenCNC3D's own packs, which nobody here has: 1,250 of the 11,234 triangles are untextured where a
comment in `cnc_eyes.cpp` counts 1,282 "in a mission pack"; the second terrain atlas of the DOS art (`PKG`'s
`terrain_tex_dos`) is written as -1, as the format allows for a theater without one; the pack is 8.2 MB, not "most of
twelve". A bake of one mission takes 2.1 s in WSL once the cartridge is extracted, and two bakes give the same
bytes (SHA-256 `5364a9fd…` for `SCG01EA.pack`, `40825df2…` for `SCB01EA.pack`: sums of cartridge-made files, kept
only to tell a bake that moved).

**The native reference build was not made.** OpenCNC3D's renderer builds with macOS's clang and Homebrew's or a
universal SDL, or MinGW for Windows (`game/build.sh`, `tools/win/`), and wants SDL2/SDL3, OpenGL headers, libcurl and a
dozen other baked packs (`dossidebar.pack`, `cameos.pack`, …) beside the mission's; WSL here has no SDL, no GL headers
and no sudo to install them. So the packs are proven by `packinspect.py` and `bake5.py`'s checks, not by the renderer
drawing one. The renderer's loader (`load_pack`, `cnc_eyes.cpp`) was read against the writer: every field
`packinspect.py` checks is one it reads, in that order.

### The DOS data

`linux/opencnc3d/data.py` reads the ISO9660 folders of two disc images on archive.org, the two OpenCNC3D's own launcher
reads by byte range (its `tools/media/media-sources.txt`), and fetches each file by its extent, checked against its
SHA-256: from `gdi-95/GDI95.ISO` (EA's 2007 freeware Gold) the three theaters and `AUD.MIX` (`TEMPERAT.MIX` and
`DESERT.MIX` are OpenCNC3D's `data/dosdata/README.md`'s and matched); from `cc_worldwide_warfare/COVERT.iso` (The Covert
Operations, the MS-DOS disc, in English) `INSTALL/LOCAL.MIX`, `CONQUER.MIX`, `GENERAL.MIX`, `SOUNDS.MIX`,
`INSTALL/SPEECH.MIX` and `INSTALL/TRANSIT.MIX`: 19.4 MB in all. Gold is the Windows 95 game at 640 x 400, and its
fonts and sidebar are drawn for that (its `6POINT.FNT` is 16 rows high, the DOS one 8): OpenCNC3D's menu and sidebar
draw the DOS game's 320 x 200 art, and with Gold's fonts every label of the menu fell below its button. Gold's English
`CCLOCAL.MIX` also has no `TITLE.CPS`, whose palette the menu's bake takes. Not fetched: `MOVIES.MIX`, `SCORES.MIX`
and `ZOUNDS.MIX` (on neither disc).

## The brain (step 1)

`linux/opencnc3d/build.sh` fetches OpenCNC3D (a sparse clone of what is used: its tree is a gigabyte) and Vanilla
Conquer at `ce83b59` (the commit of its `brain/patches/UPSTREAM.txt`, and of `linux/tdawn`), applies OpenCNC3D's
`brain/patches/vanilla-opencnc3d.patch` and copies its two files as its `tools/check-brain-checkout.sh` does, then
`opencnc3d.patch`, ours. The tree is built where OpenCNC3D keeps it (`brain/vanilla`, beside its `fair_ai.h` and
`game/sidebar_build_potential.h`, which its patch includes by relative path). The library is the Remaster target
(`BUILD_REMASTERTD`), static, with `brain_shaderemu.cpp` in it, and linked with `host.cpp` and `linux/tdawn/cxxrt.cpp`
into one program: no `dlopen`. 1,422,412 bytes of text, 44,304 of data, 311,392 of bss; 1.49 MB flat.

Our patch (`linux/opencnc3d/opencnc3d.patch`, 4 files): the library static under `OPENCNC3D_STATIC`, the game's
`main` renamed for the host's (`SHADEREMU_BRAIN`), `EventClass`'s 22-byte assertion left out (RISC-V GCC has no
`ms_struct` to lay its bitfields out with, and there is no network), and `Legacy_Render_Enabled` asking the host.
GCC wants three Microsoft words and `-fpermissive` for `dllinterface.h` (`msvc_gcc.h`, force-included), where
OpenCNC3D builds with Clang's `-fdeclspec`. `-fno-lifetime-dse` as for Tiberian Dawn.

`host.cpp` starts the mission with the rules of OpenCNC3D's `brain/host/run1.cpp`, prints the objects of the engine's
heaps (`OpenCNC3D_Dump_Objects`), plays TICKS ticks through `CNC_Advance_Instance` with no input, and prints `ocnc:`
lines: a sum every EVERY ticks and the instructions a tick (`rdinstret`). The sum (`ShaderEmu_State_Sum`) is FNV-1a
over the game's numbers only: the frame, both random generators' seeds, every live infantry, unit, building,
aircraft, terrain and bullet (heap id, kind, type, owner, coordinate, strength, limbo, mission, target, facing,
destination), every anim's place and type, and each house's credits, tiberium and defeat. No pointer goes in, so the
build computer's 64-bit program and the machine's must agree.

**The seed.** The brain seeds its `Seed` from the clock at a mission's start, but `Clear_Scenario` starts the game's
generator at `RandomClass(0)` whatever it is: a mission plays the same every time, which is OpenCNC3D's "deterministic
across runs". Seed 0, the default, is that game; another seed restarts both generators after the load (seed 7 gives
other sums, the same on both machines' builds).

**The 1995 view.** `CNC_Advance_Instance` draws the DOS game's view into the hidden page at every tick for the
Remaster's legacy picture. Nothing here reads that page, and OpenCNC3D's eyes draw the mission themselves; drawing it
was 86% of a tick on the machine (4.97 million instructions a tick with it). The host turns it off unless
`OPENCNC3D_RENDER=1`; **the sums are the same with and without it**, over 3,000 ticks of both missions on the build
computer and 600 ticks of `SCG01EA` on `rvc_cpu`.

## Is it the same game

`host_sum.sh` (the build computer: WSL, `g++` 15, x86-64, `-O2 -ffp-contract=off`) and the machine (`rvc_cpu`,
`opencnc3d-brain` built by the flat drivers, `-O2`) print the same lines, 2 November 2026:

| tick | SCG01EA | live inf/unit/bld/terrain | SCB01EA | live inf/unit/bld/terrain |
|---:|---|---|---|---|
| 0 | `f3addff9` | 14 2 3 41 | `e3d0f0ef` | 32 3 13 25 |
| 300 | `ca748fdc` | 18 3 2 41 | `7843c1be` | 39 3 13 25 |
| 600 | `7c22f60d` | 20 3 2 41 | `e3ef83b1` | 39 3 13 25 |
| 900 | `4a35b216` | 18 4 2 41 | `943e3571` | 39 3 13 25 |
| 1200 | `e003e97e` | 18 3 2 41 | `b0663376` | 39 3 13 25 |
| 1500 | `2ab0ed51` | 19 3 2 41 | `48705288` | 37 3 13 25 |
| 1800 | `8161c7e2` | 19 3 2 41 | `3261f308` | 36 3 13 25 |
| 2100 | `b57507b3` | 19 3 2 41 | `b9ea5dd6` | 36 3 13 25 |
| 2400 | `c10135b1` | 18 3 1 41 | `33cde0c1` | 36 3 13 25 |
| 2700 | `726945c6` | 18 2 1 41 | `6d13083e` | 36 3 13 25 |
| 3000 | `7ed86914` | 18 3 0 41 | `1a15acc0` | 36 3 13 25 |

The game moves: GDI 1's reinforcements land and the Nod gun turrets fall (3 buildings to 0), and Nod 1's village
fills with infantry. At the start, `check_objects.py` matches the engine's objects to the mission's INI by kind, type,
owner, cell and health (`Fixed_To_Cardinal` of the INI's 0..256): **`SCG01EA` 60 of 60 and `SCB01EA` 73 of 73**, on
both machines, as OpenCNC3D's `brain/README.md` says.

## The gate: what a tick costs (`rvc_cpu`, 2 November 2026)

| | SCG01EA | SCB01EA |
|---|---:|---|
| the load (`CNC_Init`, the rules, `CNC_Start_Custom_Instance`) | 331,359,550 | 336,116,522 |
| a tick, mean of 3,000 | 503,226 | 515,421 |
| a tick, fewest and most in ticks 2701-3000 | 470,803 and 742,642 | 480,530 and 523,945 |
| a tick with the 1995 view drawn, mean of 600 | 4,972,422 | |

Instructions of core 0 (`rdinstret`, the kernel's interrupts in them, about 3.8%). Where a tick goes (`tools/prof.py`
on 1,000 ticks of `SCG01EA`, 504,637 a tick inside `CNC_Advance_Instance`):

- **`MapClass::Clean`, 427,551 a tick (85%)**: a safety pass the Remaster added (`dllinterface.cpp`, "We can afford the
  CPU time to just clean them up", 2020) that walks every cell of the 128 x 128 MEGAMAP, 16,384 of them, at every tick,
  calling `Cell_Occupier` on each (16.5 million calls in 1,000 ticks) and clearing overlappers that are no longer live.
- **`LogicClass::AI`, 70,679 a tick (14%)**: the game, infantry and vehicles' AI, paths, threat. About what Tiberian
  Dawn's 2D logic costs (`docs/games/tdawn.md`).
- The rest, 6 thousand: the input, the layer's sort.

So the brain was about 6.3 times the plan's budget of 80 thousand, and nearly all of the excess was one scan; the game
itself is within it.

### Clean on the cells that hold something

`Clean` (the Remaster's, `map.cpp`) changes a cell only through a non-null pointer: an occupier that is no longer live
(`Cell_Occupier` then clears it), or an overlapper no longer live or in limbo. A cell is given a pointer in four places
only: `Occupy_Down`, `Occupy_Up` (its next), `Overlap_Down`, and a saved game's `Decode_Pointers`. Our patch marks the
cell there in a bitmap of 16,384 bits (and a bitmap of its 512 words), and `Clean` walks the marked cells in the same
ascending order as before, since its `active_fail` and `limbo_fail` carry from one cell to the next; a cell it finds
with no pointer left is unmarked. Whatever it does is then what the full walk did. `OPENCNC3D_CLEAN=full` is the
Remaster's walk, and `=check` the marked walk with a count, before each `Clean`, of unmarked cells that have a
pointer: **0 in 3,000 ticks of both missions**. The lowest set bit is found by a multiply (de Bruijn): the machine has
no count of trailing zeros, and GCC's `__ctzsi2` was 8% of a tick.

### The file search

`linux/tdawn`'s answer, taken whole from its `vanilla-conquer.patch` (`common/file.cpp`, `file_posix.cpp`,
`rawfile.cpp`, `rawfile.h`, under `SHADEREMU_BRAIN` here): a file's own name looked for in any case but not each folder
above it, a folder's names read once and kept, a name with no wildcard compared rather than `fnmatch`ed, and an open of
a name the folder does not have answered without the kernel.

### Before and after (`rvc_cpu`, 2 November 2026)

| | SCG01EA before | after | SCB01EA before | after |
|---|---:|---:|---:|---:|
| the load | 331,359,550 | 53,978,630 | 336,116,522 | 58,759,921 |
| a tick, mean of 3,000 | 503,226 | 74,346 | 515,421 | 83,762 |
| the 11 sums | | the same | | the same |

Both missions' 11 sums are the build computer's and the table's above, 60 of 60 and 73 of 73 objects, with `Clean`
marked, full and checked. The tick's next items now (`tools/prof.py`, 3,000 ticks of `SCG01EA`, 71,179 a tick in
`CNC_Advance_Instance`): `LogicClass::AI` 52,254 (infantry's AI 1,039 a call, 18 a tick; units' 5,096, 3 a tick; the
`Mark`s of moving objects 2,291 a call), `Clean` 13,452 (the cells with trees, walls and buildings stay marked),
`HouseClass::Adjust_Threat` 4,835 and `HouseClass::As_Pointer` 4,707 (259,164 calls in 3,000 ticks), `MapClass::Logic`
3,940, `LayerClass::Sort` 2,292.

The load's 331 million: 43% is case-insensitive file searches (`Resolve_File` lists a folder and `fnmatch`es every
name, 3,822 times, 37 thousand instructions each, 42.7% of it in the kernel's directory reads), 20% the scenario
descriptions (`Read_Scenario_Descriptions`, 68 million), the same two things `docs/games/tdawn.md` found in Tiberian
Dawn's start and cut there ("A folder's names read once").

## The game (step 2): OpenCNC3D's own program

The owner wants OpenCNC3D here, not a recreation of it: this is its own app (`app/opencnc3d.cpp`: the DOS menus, the
enhanced menu, the campaign and the state machine between menu and game) and its own renderer (`game/cnc_eyes.cpp`,
compiled as a library with `-DOPENCNC3D_NO_MAIN` as its Mac and Windows builds do), built for the machine by
`linux/opencnc3d/game.sh`, which `build.sh` runs after the brain. Their sources are compiled as they are; what the
machine lacks is supplied beside them, as OpenCNC3D's own `compat/win` does for Windows.

| Their need | Here |
|---|---|
| C++ with all of libstdc++ (`std::map`, `std::string`, `std::unordered_map`, exceptions) | OpenTTD's GCC 13.3 for `riscv32-linux-musl` (`linux/openttd/toolchain.sh`, `ottd-cc` and `ottd-c++`) |
| zlib | OpenTTD's (`$WORK/src/openttd-deps`) |
| libcurl (the internet game list, `launcher/lnet.c`) | the image's own port (`linux/ports/curl.sh`, with mbedTLS) |
| SDL2 | the headers of SDL 2.30.9 (zlib licence, fetched against their SHA-256; none of SDL's code is built) and `sdl_nanox.c`, ours: the 91 functions the program calls |
| OpenGL 1.x | `compat/OpenGL/gl.h` (SDL's copy of Mesa's `gl.h` and `glext.h`) and `gl_ocnc.c`: the program's 91 `gl` calls, renamed `ocnc_gl` in its objects so that `gles.c`'s own never meet them, onto Half-Life's shim (`linux/halflife/gl_shaderemu.c`) over Quake III's draws, as `linux/jk2/gl_jk2.c` does |
| `dlopen` of the brain | `brain_link.c`: `dlopen`, `dlsym`, `dlclose` and `dlerror` answer from a table of the brain linked in (weak, so the five functions only Red Alert's brain has are 0, as their `dlsym` would be) |
| `__int64`, `__declspec` | `msvc_gcc.h` with `-fms-extensions -fpermissive`, as for the brain (their Windows build's flags) |

**The SDL functions** (`sdl_nanox.c`): start and end (`SDL_Init`, `_InitSubSystem`, `_Quit`, `_SetHint`, `_GetVersion`,
`_GetPlatform`, `_GetCurrentVideoDriver`, `_getenv`, `_free`, `_memset`, `_GetError`, `_ShowSimpleMessageBox`,
`_GetPrefPath` (`/persistence/opencnc3d/`, else `/tmp`), `_GetBasePath`); time (`_GetTicks`, `_GetPerformanceCounter`,
`_GetPerformanceFrequency`, `_Delay`: the clock, never a wait in place); threads, run to their end when they are made
(`_CreateThread`, `_WaitThread`, `_DetachThread`), `_CreateMutex`, `_LockMutex`, `_UnlockMutex`, `_AtomicSet`,
`_AtomicGet`; the one window (`_CreateWindow` as a desktop window, its size held under the screen less the caption and
the bar, `_DestroyWindow`, `_GetWindowFlags`, `_GetWindowSize`, `_SetWindowSize`, `_GetWindowPosition`,
`_SetWindowPosition`, `_SetWindowResizable`, `_SetWindowBordered`, `_SetWindowFullscreen` (windowed only),
`_SetWindowDisplayMode`, `_GetWindowDisplayMode`, `_RaiseWindow`, `_SetWindowInputFocus`, `_SetWindowMouseGrab`,
`_GetWindowDisplayIndex`, `_GetWindowFromID`, `_GetKeyboardFocus`, `_SetWindowData`, `_GetWindowData`); the screen
(`_GetNumVideoDisplays`, `_GetDesktopDisplayMode`, `_GetCurrentDisplayMode`, `_GetNumDisplayModes`, `_GetDisplayMode`,
`_GetClosestDisplayMode`, `_GetDisplayBounds`, `_GetDisplayUsableBounds`); GL (`_GL_SetAttribute`, `_GL_GetAttribute`,
`_GL_CreateContext`, `_GL_DeleteContext`, `_GL_MakeCurrent`, `_GL_GetCurrentContext`, `_GL_GetCurrentWindow`,
`_GL_GetDrawableSize`, `_GL_SetSwapInterval`, `_GL_GetSwapInterval`, `_GL_SwapWindow`, `_GL_GetProcAddress`); input
(`_PumpEvents`, `_PollEvent`, `_PeepEvents`, `_PushEvent`, `_FlushEvent`, `_GetKeyboardState`, `_GetModState`,
`_StartTextInput`, `_StopTextInput`, `_GetMouseState`, `_GetGlobalMouseState`, `_ShowCursor`, `_SetRelativeMouseMode`,
`_WarpMouseInWindow`, `_WarpMouseGlobal`, `_GetScancodeFromKey`, `_GetKeyName`, `_GetKeyFromName`: Nano-X's keys,
buttons, wheel, pointer, close and resize as SDL's events); files (`_RWFromFile`, `_LoadWAV_RW`, `_FreeWAV`); sound
(`_OpenAudioDevice`, `_CloseAudioDevice`, `_PauseAudioDevice`, `_LockAudioDevice`, `_UnlockAudioDevice`: no device yet,
which the program takes as a machine with none).

**The GL calls the shim does not draw** (`gl_ocnc.c`, `OPENCNC3D_GL_LOG=1` names each the first time):
`glClipPlane`, `glColorMask`, `glCopyTexSubImage2D`, `glGetTexImage`, `glPushAttrib` (and so `glPopAttrib`),
`glReadPixels` (zeros), `glScissor`; depth state (`glDepthFunc`, `glDepthMask`, `glClearDepth`: the GPU's depth test is
fixed), culling, normals, the texture matrix, and every texture unit past the first (`glActiveTexture` and the
`glMultiTexCoord` of other units keep nothing). `SDL_GL_GetProcAddress` gives `glActiveTexture`,
`glClientActiveTexture`, `glMultiTexCoord2f` and `3f`, and 0 for anything else: no shaders, no framebuffers, which is
tier 1.

### Milestone 1: it compiles and links (2 November 2026)

`bash linux/nommu/build.sh opencnc3d/build.sh` (a private WSL home) makes `/usr/bin/opencnc3d`: 9,135,012 bytes of
text, 183,384 of data and 17,784,460 of bss (`riscv32-linux-size`), 9.3 MB stripped. The brain inside it is the one
whose sums are above, built as before.

### The play folder

`python3 linux/opencnc3d/play.py SRC CACHE` makes `CACHE/play`, what OpenCNC3D's `game/make-build.sh` stages beside its
program, with OpenCNC3D's own bake scripts run in their own folders of the fetched tree: `menu/bake_dosmenu.py`
(`dosmenu.pack`, the title, the 1995 fonts and the dialog frames), `game/bake_campaign.py`, `bake_dossidebar.py`,
`tools/sidebar_redesign/bake_pack.py` (`hud640.pack`, the 640 x 480 sidebar; the `hud640_layout.h` it writes is the
committed one, byte for byte), `bake_dosinfantry.py`, `bake_efx.py`, `bake_logos.py`, `bake_n64infantry.py`,
`bake_dostiberium.py`, `bake_verdict.py`, `bake_smudges.py` and `bake_doscrate.py`; beside them the scenario packs
of `bake.py`, `missions/` and `dosdata/` (TRANSIT.MIX in `dosdata/transit/`). The bakers look for the 1995 data in
three places of their tree (`data/dosdata`, `playable/dosdata`, `dosart/` and `dosart/cd1/`), each made a link to
the cache. 12 packs, 52 MB with the two scenarios. `overlay.py` copies it to `/usr/share/opencnc3d/play`, and
`ocnc-play` makes `/tmp/ocnc` of links to it (and to the theaters, which the brain opens where it runs): the program
runs there and writes its settings there. Not baked: the Enhanced art (the remastered textures, `sky.pack`,
`tib3d.pack`, `cameos.pack`), which a GL with no shaders does not draw.

### Milestone 2: it starts and reaches its first frame; milestone 3: the menu (2 November 2026)

`nano-X -p & ocnc-play; cd /tmp/ocnc; opencnc3d` on `rvc_cpu`: GL 1.1 from the shim, 24-bit depth, no sound device
(the program says so and runs silent), the menu's nine buttons from `dosmenu.pack` (`DMBTN` lines), `APP|state|MENU`.
A capture under `rvc_harness --cpu --warp` (through `tools\gpu_queue.ps1`) shows OpenCNC3D's own DOS menu as its
build draws it: the title plate with the explosion, the green 1995 buttons (Special Ops, User Maps, Start New Game,
Load Mission, Skirmish, Multiplayer, Intro & Sneak Peek greyed with no movie, Visuals, Exit Game), its version line,
the OpenCNC 3D wordmark and the legal lines, in an 800 x 545 window on the desktop. The first capture, with Gold's
fonts, had every label below its button: hence the DOS disc ("The DOS data").

Two things the program needed of the machine's side: GL_ARB_multitexture in `GL_EXTENSIONS` (the calls are taken,
and only unit 0's texture and coordinates are drawn; a texture bound on another unit is still filled and set), and
`-Wl,-u` for each of the brain's 76 `CNC_`/`OpenCNC3D_` entry points at the link: `brain_link.c`'s names are weak,
and a weak reference pulls no member out of `TiberianDawn.a`, so the first `--flowtest` found no `CNC_Init` (text
9,135,012 to 10,392,512 bytes with the brain in).

`opencnc3d --classic --flowtest N` (OpenCNC3D's own autopilot: Start New Game, GDI, the briefing, N ticks, a won
mission) then plays GDI mission 1: the brain is found, `STATICCELLS|verdict=AGREE`, the pack of SCG01EA (339
textures, 264 meshes), `dosinfantry`, `n64infantry`, effects, verdict, tiberium, crates, smudges, the 640 x 480 HUD
(39 assets) and the DOS sidebar (72 shapes, 3 fonts), the N64 camera on the scenario's start view, and the mission's
end, score, fame and map selection. On the way it built, ahead of time, what Enhanced would want (the water field:
2,165 s of the guest's clock on `rvc_cpu`), though a GL with no shaders never shows Enhanced: our one change to
OpenCNC3D's program (`game/gmwipe_mod.h`, in `opencnc3d.patch` under `opencnc3d/`) makes nothing ahead when the
chain cannot boot.

### Milestone 4: GDI mission 1 drawn, with the sidebar (2 November 2026)

`opencnc3d --classic --flowtest 2000` under `rvc_harness --cpu --warp` (through `tools\gpu_queue.ps1`), captured in
play: the N64 camera over the GDI beach landing, the green shore, cliffs and rocks of the cartridge's terrain on its
heights, the sea, the gunboat firing (the cartridge's particle effects), the landed riflemen and the MCV, the sandbag
wall, the shroud's soft edge, and OpenCNC3D's 640 x 480 sidebar on the right (credits 2000, the GDI eagle in the radar,
Repair, Sell and Map, the empty build columns: no construction yard yet in this mission). Two things of the shim were
needed for the terrain, which was black at first (`gl_ocnc.c`):

- **Draws past 4,096 vertices.** The shim drops a primitive bigger than its buffer (`MAX_PRIMITIVE`); `glDrawArrays`,
  `glDrawElements` and a `glBegin` are cut into pieces of whole triangles, quads or lines.
- **The terrain's two texture stages** (unit 0 `GL_COMBINE` subtracting the tint, unit 1 modulating by the primary
  alpha, the N64 combiner's form): while they are set, a vertex colour (tint, light) is given as (light, light, light,
  1), in `glColor` and in colour arrays. The tint's subtraction is left out (it is 0 on all but 248 of SCG01EA's 4,225
  corners); the sea draws its first water texture alone (unit 1's second is not drawn).

**What a frame costs core 0** (`OPENCNC3D_FRAMEPROF=1` with `OPENCNC3D_CLOCK=instret`, which makes SDL's performance
counter core 0's retired instructions at "10^9 a second", so OpenCNC3D's own frame profile reads in millions of
instructions; the harness's `--cpu --warp`, a window of 80 frames in play): **63.13 million instructions a frame**,
of which **47.44 million** are OpenCNC3D's "events + tick + refresh" (the brain's ticks due and the renderer taking the
brain's state, `OBJ|` text it prints and the renderer reads back) and **15.69 million the drawing**: water 5.93,
terrain depth 1.78, the HUD and present 1.78, rain and shroud 1.64, relief 1.08, objects 1.02, seabed 0.78, shroud
corners 0.69, trees 0.47, infantry 0.22, effects 0.14. The windows before: 67.99 (51.83 logic) over 74 frames, and
107.10 over 47 frames with one frame of 1,935.7 million (1,919.0 in the tick and refresh). At 2 to 3 MIPS a frame
is 21 to 32 s. **Memory**: `free` says 160,652 kB used of 256,060 with the mission running (the kernel, Nano-X and
the program; the program's own 10.4 MB of text, 0.2 MB of data and 18.1 MB of bss, the 64 MB GPU pool and the 8.2 MB
scenario pack among it), 68,496 kB free.

**The game logic's sums**: the brain in the program is `TiberianDawn.a` from the same build as `opencnc3d-brain`, whose
bytes are as at the sums above (MD5 `6a5b3221...` before and after this work), so its 11 sums of both missions stand;
nothing of the brain was changed for the program.

## On the desktop (step 5, begun 2 November 2026)

`overlay.py` puts the Start menu's entry `Games/OpenCNC3D` (`/usr/share/nxapps.78-opencnc3d`) and its starter
`opencnc3d-start` (`ocnc-play`, then `opencnc3d` in `/tmp/ocnc`) into a private image only when it has both the program
and the cartridge's play folder: the shared image has neither. The icon is ours, three green crystals on a tilted tile
(`tools/draw_icons.py`, `linux/apps/icons/opencnc3d.png`), on the program's name `opencnc3d` in both looks' lists. A
private image made so (`build/images/ocnc3`), its desktop started by `nx` and `opencnc3d-start` run from the shell,
captured under `rvc_harness --cpu --warp`: OpenCNC3D's DOS menu in an 806 x 574 window, the crystals in its caption and
on the bar's button. The Start menu itself was not opened in that capture.

## Licences

OpenCNC3D is GPL-3.0 and its brain carries EA's GPL with the Section 7 terms of `brain/vanilla/License.txt`; its code
is fetched at a pinned commit and never copied here. Everything in `linux/opencnc3d/` is ours and GPL-3.0 (the baker's
two files were written for this port and read no bytecode). The cartridge, anything extracted or baked from it, and
the DOS data stay in a cache outside the tree and in private images; `bake.py` checks the cartridge by its SHA-1.

## Not done

- The native reference build of OpenCNC3D's renderer, and a pack drawn by it ("The pack baker, ours").
- A `--opencnc3d` switch on `linux/nommu/image.py` and a `tools/test_all.py` case: the private image was made by hand
  (`overlay.py`, then `image.py ocnc ROOT KERNEL` on a copy of the shared root), and the sums are in this document.
- No card run: `rvc_cpu` and `rvc_harness --cpu --warp` only, no worker cores, no frames a second on the card.
- The movies and the music; no sound (`SDL_OpenAudioDevice` says there is no device).
- Input by hand: the captures are of OpenCNC3D's `--flowtest` autopilot; keys and the pointer go through
  `sdl_nanox.c` but were not played with.
- The second texture unit (the sea's second water texture, the tint), the depth state, `glScissor`, `glClipPlane`,
  `glReadPixels`, `glCopyTexSubImage2D`: listed above, not drawn.
- The picture was not compared with OpenCNC3D's native build on the build computer.
- Speed: 63 million instructions a frame on core 0, three quarters of it the tick and the taking of the brain's state;
  nothing on worker cores yet.
