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
- **Step 2, begun: a picture, not OpenCNC3D's eyes.** `eyes.c` draws `SCG01EA`'s terrain with its heights, the sea,
  the cartridge's models of every building, vehicle and tree, and the infantry's strips, from the pack, over Half-Life's
  GL shim in a Nano-X window: 1.05 million instructions of core 0 a frame (89 thousand the game, 965 thousand the
  drawing). OpenCNC3D's own renderer, the sidebar and the menus are not here ("The picture", "What is left").

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

`linux/opencnc3d/data.py` reads the ISO9660 directory of `GDI95.ISO` (archive.org item `gdi-95`, EA's 2007 freeware
Gold) by HTTP range and fetches each file by its extent: `TEMPERAT.MIX`, `DESERT.MIX`, `WINTER.MIX`, `CONQUER.MIX`,
`GENERAL.MIX`, `SOUNDS.MIX`, `AUD.MIX` and `INSTALL/CCLOCAL.MIX`, 9.8 MB, each checked against its SHA-256 (the
first two are OpenCNC3D's `data/dosdata/README.md`'s and matched; the rest pinned at the first fetch). Not fetched
yet: `MOVIES.MIX` (449 MB), `SCORES.MIX` (39 MB), and `LOCAL.MIX`, `SPEECH.MIX`, `ZOUNDS.MIX`, which this disc keeps
inside `INSTALL/SETUP.Z`. The brain needs only a theater's palette.

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

## The picture (step 2, begun)

**OpenCNC3D's own renderer is far more than a day's work here, and is not ported.** `game/cnc_eyes.cpp` is 47,617 lines
and includes 128 headers of its own (110 thousand lines more: effects, remastered art, the editor, Red Alert in 3D,
the sidebar, the menus, audio, network), uses `std::map`, `std::string` and `std::unordered_map` (all of libstdc++, which
the flat C++ driver here has not: OpenTTD's toolchain would be needed), zlib, `dlopen` of the brain, SDL2 for its
window, events and sound, and a dozen packs of its own beside the mission's (`dossidebar.pack`, `cameos.pack`, the
DOS art packs the DOS data's bakers make). So the smallest picture was made instead, as the coordinator's brief
allowed: `linux/opencnc3d/eyes.c`, ours, about 400 lines, which reads the mission's pack and the brain's objects and
draws them with `cnc_eyes.cpp`'s conventions (a cell is a unit, a model unit 1/1024 of a cell, a height step 1/64, a
facing turning a model by `-face * 2 pi / 256`, the cartridge's 50 degrees of field and a camera pitched down 55
degrees at 16 cells). It is OpenCNC3D's data and the cartridge's models; it is not OpenCNC3D's picture.

What it draws: the terrain's cells in the view (the atlas, keyed where the cartridge punched the sea out) on the PK9
heights; under the cells with holes the sea floor (`BOTTOM`) and the sea (`WATER1` at 170/255); every building,
vehicle, aircraft and tree by its pack mesh (solid and cutout first, then the shadows and translucent faces blended),
a turret turned about its pivot by its own facing; every soldier as his set's `STAND` frame for his facing in his
house's palette, upright to the camera. The GL is Half-Life's shim (`linux/halflife/gl_shaderemu.c` over
`linux/quake3/gpu_shaderemu.c` on `gles.c`), included as `linux/jk2/gl_jk2.c` does, in a 640 x 480 Nano-X window;
`build.sh` links it with the brain into `/usr/bin/opencnc3d` (1.65 MB). `OPENCNC3D_LOOK=X,Y`, `OPENCNC3D_DIST`,
`OPENCNC3D_PITCH` move the camera; `OPENCNC3D_TICKS=N` plays N ticks before the first picture.

Looked at (captures under `rvc_harness --cpu --warp` through `tools\gpu_queue.ps1`, 2 November 2026, kept in no
repository): GDI 1's grass, roads, the cliff with its height, the beach and the sea with its rocks, the trees with
their blended shadows, the MCV, the boat, a Nod gun turret and four GDI riflemen in tan where they stand. Wrong or
missing in them: GDI's models are drawn in the Nod/neutral colours (the pack's GDI variants are not used yet), no
building's or tree's animation plays (PKB), the tree clumps `TC01`..`TC05` have no mesh (the cartridge has none, as
OpenCNC3D's `unit_models.json` says) and are not drawn, the soldiers stand (no `RUN` or `DETH1`), no vehicle shadow
quads (PKD), no shroud, no terrain shade (the console's per-corner light and the CM tint), no sidebar, no cursor.

Measured on `rvc_cpu` (90 frames of `SCG01EA`, the camera on the MCV, the game a tick a frame):

| | instructions of core 0 |
|---|---:|
| a frame | 1,050,949 to 1,062,200 |
| the game's tick in it | 87,912 to 91,312 |
| the drawing | 961,824 to 974,288, for 6,081 vertices and 33 objects |
| the load (`CNC_*` start) | 54,312,086 |
| the pack read and its textures put in the GPU's pool | 38,143,637 |

Memory: the pack (8.2 MB) is read whole and freed once its 339 textures are in the GPU's pool, which holds them in
5,749 KB of a 12 MB pool (`HL_POOL_MB=12`, 0 left out; the soldiers' strips among them, the GDI variants not
put there); 1,002 KB of meshes, cells and heights stay in the heap. The drawing is a vertex at a time through
`glBegin` (`s_glVertex3f` 47 instructions, the shim's `draw_vertices` 2,576 a call over 7,380 calls in 60 frames, and
`main`'s own loops 29%): the terrain is three quarters of the vertices and does not change, so it is the first thing
to keep in the GPU's memory (`seglKeep`, as Doom and Quake do).

## What is left

- OpenCNC3D's renderer itself, at tier 1: a port of `cnc_eyes.cpp` and its modules needs OpenTTD's full C++ toolchain
  (or the STL replaced), SDL's window, events, timer and sound replaced by a host of ours, its `dlopen` of the brain
  turned into a link, its packs of DOS art baked (`game/bake_dos*.py` over the DOS data), and the optional shader chain
  left out at compile time. Its tier-1 contract (`docs/tier1-gap.md`) says which paths a fixed-function GL keeps.
- The sidebar and the menus, which come from the DOS data's SHP art through those packs.
- In `eyes.c`, if it stays: the GDI variants, animation, the vehicle shadows, the terrain's shade and tint, the
  shroud, and the terrain kept in the GPU's memory.

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
- The movies, music and the DOS data inside `SETUP.Z`.
