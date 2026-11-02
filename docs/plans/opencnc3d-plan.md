# Plan: OpenCNC3D on the machine

Written 2 November 2026, from a reading of OpenCNC3D's tree at its current commit (`github.com/freschism3000/OpenCNC3D`,
GPL-3.0): its README, `BUILDING.md`, `NOTICE.md`, `brain/README.md`, `tools/bakery/README.md`, the data folders' READMEs, and
the shape of `game/cnc_eyes.cpp`. Nothing of it has been built yet. It replaces the plan for a 3D view of our own, whose work was
taken out of main (the reverts of 7cf591ff, beb16a45 and 21547f03): the owner wants OpenCNC3D itself here, not a recreation of it.

## What OpenCNC3D is

A PC version of the Nintendo 64's Command & Conquer, in three parts it names itself:

- **The brain**: EA's GPL Tiberian Dawn logic, Vanilla Conquer at **ce83b59**, the same commit as `linux/tdawn/build.sh`'s, with
  `brain/patches/vanilla-opencnc3d.patch` (766 KB, 80 files, Red Alert's included). It is built as a library behind the Remaster's
  flat `CNC_*` C ABI (`dllinterface.cpp`) and runs the missions of the cartridge (the N64's INIs, not the DOS ones).
- **The skin**: the cartridge's own models, textures, terrain and camera, extracted by `tools/romdump/` and baked into one pack a
  mission (`CNC3DPKC`, version 12) by `tools/bakery/` and the `game/bake_*.py` slices. The 2D (sidebar, cursors, infantry, fonts,
  palette) and all sound and films come from the 1995 DOS discs' MIX files.
- **The eyes**: `game/cnc_eyes.cpp`, one file of about 47,600 lines: OpenGL 1.1 for the most part (glBegin and glVertex, 15
  glDrawElements), SDL3 for the window, input and sound, and shaders only in the desktop's optional chain (bloom, shadows, a CRT),
  which is off by default. Its "tier 1" (`docs/tier1-gap.md`) is the contract for a 3dfx Voodoo 2 under Windows 98: fixed
  function, no render targets, 640x480, 16-bit colour. That tier is what this machine can be.

It also has menus, a sidebar and films of its own (`menu/`, `sidebar/`, `video/`), a launcher, multiplayer over CnCNet, skirmish
AI, and a Red Alert in 3D in progress (RA3D). This plan is Tiberian Dawn's campaign; the rest is after it.

## What it needs that we cannot ship

- **A dump of the European cartridge** (`cnc_eu.z64`, 32 MB, big-endian): every offset in its `docs/re-findings.md` is the EU
  one's. A US dump does not line up.
- **The 1995 DOS data**: `CONQUER.MIX`, `LOCAL.MIX`, `GENERAL.MIX`, `TEMPERAT.MIX`, `DESERT.MIX`, `SOUNDS.MIX`, `SPEECH.MIX`,
  `SCORES.MIX`, `AUD.MIX`, `ZOUNDS.MIX`, and the films. EA made the Gold discs freeware in 2007, so these can be fetched as the
  demo is now; the cartridge cannot.
- So it goes the way of Age of Empires and Jedi Outcast (`AGENTS.md`): out of `ALL` in `linux/nommu/build.sh`, out of
  `linux/prebuilt` and the shared image, into a private image only (`--opencnc3d` on `image.py`), from a cartridge the owner puts
  at a path outside the tree (`OPENCNC3D_ROM`). Nothing made from the cartridge is committed: no pack, no picture of a model in
  the docs, no sum that names its bytes beyond a checksum.

## The first blocker: the pack baker

OpenCNC3D's own README says it: `bake5.py` begins with `import pk4mod`, which loads `bake_pk4.pyc`, compiled Python whose source
was lost, and neither is in the published tree. So a fresh clone cannot make the packs the renderer loads. What is there:
`tools/romdump/` (archive, `.IMG` decoding, scene graphs), the `game/bake_*.py` slices, and `packinspect.py`, which reads every
section of a pack. The renderer's loader in `cnc_eyes.cpp` says the format too.

Step 0 is therefore a new baker, ours, under the GPL beside the port: the cartridge in, packs out, checked by `packinspect.py`
section by section. If the project publishes its baker meanwhile, or its built downloads carry the packs, that is cheaper, and the
owner says which. This step can fail; it is the gate. (2 November 2026: it did not fail. Only the lost module and the terrain
resolver had to be written; OpenCNC3D's `bake5.py` runs unchanged over them, step 0 below.)

## How it is put together here

- `linux/opencnc3d/`, like `linux/tdawn/`: `build.sh` fetches OpenCNC3D at a pinned commit and puts the tree back from
  `opencnc3d.patch` on every build. Its code is never copied into the repository; the patch, the host and our files are GPL-3.0,
  as `vanilla-conquer.patch` is under EA's GPL. Nothing of it goes into an MIT part (`gles.c`, the tools, the host libraries).
- **The brain** is linked into one flat program, not loaded: no `dlopen` here. It is built with what `linux/tdawn` learnt
  (`-fno-lifetime-dse`, no libstdc++ but `cxxrt.cpp`, the C++ headers over musl), and its 32-bit pointer casts are at home: the
  machine is RV32.
- **The eyes** go to Half-Life's GL shim, as Jedi Outcast's rd-vanilla did (`linux/jk2/gl_jk2.c`): glBegin and glVertex, the
  matrix stack and textures over `gles.c`. Tier 1's paths are taken and the shader chain left out at compile time.
- **SDL3** goes to a host of our own over Nano-X, as `shaderemu.cpp` and `host.c` are Vanilla Conquer's: a window, keys and the
  pointer, a sound card stream.
- Our Tiberian Dawn port stays as it is, in 2D; it is the one with the demo's data and the worker cores. What it learnt about the
  logic's speed (the scan table, path searches on a worker, counts) may later be carried to the brain here, which is the same
  code.

## Steps, each with what it must show

0. **The data on the build computer.** The cartridge's checksum checked, the packs baked (the blocker above), the DOS data
   fetched. OpenCNC3D built natively in WSL with its own SDL renderer, and a mission played there: our reference, never shipped.
   - *Done 2 November 2026 (`docs/games/opencnc3d.md`, "The data"), but for the renderer.* The EU dump (SHA-1
     `0d5211e2…`, 33,554,432 bytes) is checked and extracted by OpenCNC3D's `archive.py` (1,624 records, 0 failures)
     into a cache outside the tree. **The baker is ours**: `linux/opencnc3d/bake/pk4mod.py` in place of the lost
     `bake_pk4.pyc` and `n64_terrain.py` in place of the unpublished resolver, under OpenCNC3D's unchanged `bake5.py`.
     `SCG01EA` and `SCB01EA` bake (`CNC3DPKG` v16, 8.2 MB, 2.1 s each, the same bytes twice), pass every equality check
     of `bake5.py` (15 lit meshes, 21 PRIM-only triangles, 153 translucent faces over 20 lists, the parts and sections
     of every model, 18 vehicle shadows, the tail) and OpenCNC3D's `packinspect.py summary` (339 textures, 264 meshes,
     11,234 triangles, 592 types, 4,096 cells, 75 strips, 20 infantry, the PK8/PKA/PKC tail OK). The DOS data: 8 MIX files,
     9.8 MB, by range from the Gold GDI disc against SHA-256 (TEMPERAT and DESERT are OpenCNC3D's sums). **Not done:
     the native renderer**: it builds with macOS's clang or MinGW and wants SDL, GL headers and libcurl, none of which
     WSL here has (no sudo), so no pack has been drawn by OpenCNC3D's own eyes.
1. **The brain on the machine, no picture.** One flat program, `SCG01EA` loaded through the `CNC_*` ABI: 60 of 60 objects as the
   cartridge's INI says (its own `brain/README.md` check), 3,000 ticks, and state sums the same on the build computer and
   `rvc_cpu`. Measured: instructions a tick and to load. A gate: if a tick is far over budget (Tiberian Dawn's 2D logic is about
   80 thousand instructions of core 0 a frame), stop and say so before the eyes.
   - *Done 2 November 2026 (`docs/games/opencnc3d.md`, "The brain"); the gate is not met.* `linux/opencnc3d/build.sh`
     makes one flat program (1.49 MB) of the brain and a host of ours; **`SCG01EA` 60 of 60 and `SCB01EA` 73 of 73**
     objects as their INIs say; 3,000 ticks with the **same 11 sums on the build computer and `rvc_cpu`** for both missions
     (`SCG01EA` ends `7ed86914`, `SCB01EA` `1a15acc0`). Core 0: the load **331 million** instructions; a tick **503
     thousand** (515 thousand for `SCB01EA`), 6.3 times the budget, once the 1995 view the brain draws at every tick for
     the Remaster is off (4.97 million with it; the sums are the same either way). Of a tick, **427 thousand are
     `MapClass::Clean`**, the Remaster's check of all 16,384 MEGAMAP cells at every tick, and **71 thousand the game's
     logic** (`LogicClass::AI`), about Tiberian Dawn's. The next step is that scan, with the sums, before the eyes.
2. **The eyes.** `cnc_eyes.cpp` over the GL shim in a Nano-X window: the mission's terrain, water, models and the 2D seen in
   captures under `rvc_harness --cpu --warp` against the native build's pictures. Its packs and textures held in RAM within a
   budget measured here; models and terrain go to the ROM if they must.
3. **Input, sound, the menus, the sidebar, films.** The pointer and keys through the game's own input; sound on the card; the
   sidebar and menus of OpenCNC3D; films through libmedia's VQA. A mission played by script to its end, as a test case of
   `tools/test_all.py` tagged `long` that says `OPENCNC3D-NO-DATA` on an image without the cartridge.
4. **Speed.** `tools/prof.py` on a frame of play; what bounds it goes on the worker cores by the rules of `AGENTS.md` (what a
   worker stores is its core's, `RVC_RACES` 0 undone), the frame drawn a frame behind where it pays; then the card's frames a
   second with `tools/game_par.py --card`.
5. **The desktop.** A Start menu entry only in an image with the cartridge, an icon, a game id for the gamepad, the holodeck's seat.

## Licences

OpenCNC3D is GPL-3.0 as a whole and its brain carries EA's GPL with Section 7 terms (`brain/vanilla/License.txt`): the port is
built from it and stays GPL, kept in its own folder, as Tiberian Dawn's patch is. Its `NOTICE.md` lists what it changed; ours
says the same of our patch. The cartridge and the DOS data are never in the repository or a shared image.

## What the owner decides

- Whether there is a European cartridge dump to use, and where it lies on this computer.
- At step 0: a baker of our own, or the project's packs if they become available.
- Whether Red Alert in 3D follows, after its Phase 2 there is done.
