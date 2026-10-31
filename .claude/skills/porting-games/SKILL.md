---
name: porting-games
description: Step-by-step tutorial for porting a game (or any large C/C++ program) to the ShaderX86 machine - RISC-V in a GPU shader, no-MMU Linux with bFLT flat programs, a Nano-X desktop, worker cores, GPU drawing through gles.c/segl, a sound card and ROM files. Distilled from the ports already in the tree (Doom, Quake, Quake III, Half-Life, Tiberian Dawn, Red Alert, the NES, ClassiCube, Micropolis, OpenTTD). Use when asked whether a game can run on the machine, when starting a new port under linux/NAME, when writing its build.sh, patch, video/sound/input drivers or state sums, when making a port fast enough at 2 to 3 MIPS a core, when moving its work to worker cores, or when putting it on the desktop, in the image and in tools/test_all.py. Triggers on: port a game, new game, linux/NAME/build.sh, flat program, bFLT, nommu build, seglSwapEarly, sound card driver, host_sum, state sum, test_all case, nxapps, nxopen, game too slow on the machine.
---

# Porting a game to ShaderX86

The machine runs about 5.5 million instructions a second on its first core at full passes on an RTX 5090
(calibrated), and 2 to 3 in the world's rounds under a VR frame, so design for 2 MIPS a core; it also has up to 255 worker cores that are free but
see each other's stores one pass (8,192 instructions) later. Everything below is how the existing ports got there. Read the newest port first: OpenTTD
(`linux/openttd/`, `docs/games/openttd.md`) is the cleanest template of the whole path.

Reference files beside this one:

- `ports.md` - every port: where its source comes from, its platform files, its tricks, its sums
  and test cases, its switches. Look up "how did X do Y" there.
- `hotspots.md` - what was slow in every port and the fix that was measured, the write-cache and
  texel rules, and the worker-core patterns and their checks.

Once it runs: the `optimizing-games` skill for speed, `worker-core-bugs` for workers, `debugging-the-machine` for crashes,
`building-the-system` for the build chain and private trees, `running-tests` for the test set.

Standing rules that apply to every step (from `AGENTS.md`):
every `rvc_harness` run (also `--cpu --warp`) goes through `pwsh tools\gpu_queue.ps1`; the shared image
is made only through `pwsh tools\make_image.ps1`; delete private images when done; commits carry no
attribution to any AI tool. Look at captures of what you changed: sums and counts pass on a blank window.

## Step 0: is it viable

Answer these before writing any code; `docs/plans/aoe-plan.md` is a worked example of the answer
(sources, licences, data, what has to change, numbered steps, "To find out first").

1. **Source.** Released source, a portable fork (Vanilla Conquer, xash3d-fwgs, ioquake3,
   esp32-nesemu's Nofrendo) or a decompilation. Pick the fork that already left Win32/DOS: its
   platform layer is one place (SDL backend, `video_sdl2.cpp`) that you replace.
2. **Licences, code and data separately.** Data that is not ours to pass on is fetched by the
   build (Doom's shareware WAD, Quake's pak, the C&C demos) and never committed. Half-Life's
   Uplink data is fetched only when an image is made (`linux/halflife/data.py`, md5-checked)
   and never goes into `linux/prebuilt`. Open data sets replace the original's where they exist
   (OpenTTD's OpenGFX/OpenSFX/OpenMSX). Unclear licence: the developer decides, and until then
   the image with it is a private one.
3. **Language and runtime.**
   - C: `rv32-cc` (musl, static, `rv32imaf`, ilp32 with floats in integer registers).
   - C++ without exceptions/RTTI: `rv32-c++` (made by `linux/tdawn/build.sh`) over musl with
     libstdc++'s headers only; `linux/tdawn/cxxrt.cpp` is the runtime, fetch `tree.cc`/`list.cc`
     from libstdc++ for `std::map`/`std::list` (Micropolis).
   - Full C++ (exceptions, RTTI, `std::filesystem`, threads' types): OpenTTD's own GCC 13.3 for
     `riscv32-linux-musl` (`linux/openttd/toolchain.sh`, ~10 min once): PIE by default, emulated
     TLS (a flat loader sets up none), libatomic (no 64-bit atomics on RV32), and `ehframe.c`
     registering `.eh_frame` (no `PT_GNU_EH_FRAME` without auxv). Drivers `ottd-cc`/`ottd-c++`.
4. **Threads.** Run single-threaded: OpenTTD's `StartNewThread` says no and the video driver sets
   `is_game_threaded = false`. Real parallelism is worker cores (Step 7), not pthreads.
5. **Floats.** Single precision is hardware; a double add is a ~100-instruction call, a divide
   200+. A double-heavy frame needs rework (`docs/games/quake.md`, "No doubles in a frame").
6. **Dynamic loading.** None. Link game modules into one program: Quake III's three modules are
   each one object with its names kept inside and their variables reset at each "load"
   (`linux/quake3/sys_shaderemu.c`); Half-Life links client and server in (`XASH_STATIC_LIBS`).
7. **Memory.** ~240 MB free at the shell, but a no-MMU program takes all its data, bss, stack and every
   anonymous mapping at start, and each mapping needs one contiguous run of pages. Size big pools
   to what is used (Quake III's hunk 8 MB on no-MMU, set by `sed` in `linux/nommu/build.sh`) and
   map them, do not `calloc` them. `python linux/nommu/memory.py` checks that all big programs
   still fit after each other.
8. **Speed budget.** Build the game natively first, with no window, and count what a frame and
   the start cost (AoE plan step 4). At 2 MIPS, 15 fps is ~130k instructions a frame on core 0.
   Old 2D games at 320x200 and 386-era logic got there after several rounds; a 3D game needs its
   level kept in GPU memory and its server or drawing on workers. Starts of 100-300 million
   instructions are a minute or more at the world's speed and are worth attacking too.

## Step 1: the build script, reproducible

`linux/NAME/build.sh`, run from the repository's folder through the flat drivers:
`wsl -- bash linux/nommu/build.sh NAME/build.sh`. Copy the
shape of `linux/quake/build.sh` (C) or `linux/openttd/build.sh` (CMake, C++):

- `set -euo pipefail`; `HERE`/`REPO`; `source "$HERE/../userland/toolchain.sh"` (gives `$WORK`,
  `rv32-cc`, `SOURCE_DATE_EPOCH` so unchanged programs are the same bytes).
- Guard prerequisites with a message: `[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }`.
- **Pinned upstream**: `COMMIT=...` (or a tag), cloned once into `$WORK/src/NAME`. Large repos:
  `--filter=blob:none` and `sparse-checkout` (Micropolis).
- **The patch**: `NAME.patch` is `git diff` of the tree at that commit. Every build puts the tree
  back and applies it (`git checkout -q -f $COMMIT && git clean -q -fd && git apply`), unless
  `NAME_TREE=keep`, which is how you edit the tree and then write the patch again. Faster forms:
  Half-Life's `tree()` re-applies only when HEAD or `git diff` differ from the patch; OpenTTD
  writes only files whose patched text changed (`put() { cmp -s "$1" "$2" || cp "$1" "$2"; }`):
  a touched header otherwise rebuilt everything, 210 s against 10.
- **Small edits without a patch** (Doom, `linux/nanox/doom.sh`): `sed` into `FILE_ours.c` and
  check the edit happened, failing loudly, e.g.
  `grep -q 'doom_line_mark\[ld - lines\]' p_maputl_ours.c || { echo "p_maputl.c: ... not found"; exit 1; }`
  or count matches (`told()` stops if the source has another number of writes).
- **Replace a function, keep the original as fallback**: compile the upstream file with
  `-DFoo=Foo_port` (or `sed 's/^Foo$/Foo_port/'`) and write your `Foo` beside it; a switch
  (`DOOM_PORT=sight,look,...`) brings the port's code back for comparison.
- **Incremental objects**: `xargs -P "$(nproc)"` over sources with a `-nt` test against the
  source, the patch, `build.sh` and shared headers; keep the flags in a file and rebuild all
  when they change (`doom.sh`'s `flags`).
- **Install** into `OUT=${NAME_OUT:-$REPO/build/images/linux/root}` (the `_OUT` override builds
  into a private image's overlay, `tools/private_image.py`). Program as `/usr/bin/NAME.bin`
  (stripped), data in `/usr/share/...`, a starter script `/usr/bin/NAME`. Keep the unstripped ELF
  and `riscv32-linux-nm -n` output in `$WORK/src` for `tools/prof.py` and `pc_profile.py`.
- **No-MMU wiring** (forgetting these cost rounds):
  - add the script to `ALL` in `linux/nommu/build.sh` (dependency order: after nanox, libmedia,
    tdawn if it uses `rv32-c++`);
  - add the binary to `BIG` in `linux/nommu/flat.py` if it needs a 1 MB stack (default 256 KB):
    every game is there;
  - then `bash linux/nommu/root.sh` (makes every program flat into `build/nommu/flatroot`) and
    `python linux/nommu/image.py`, or `pwsh tools\make_image.ps1`. Without `root.sh` the image
    keeps the old program.
  - Under the flat drivers `-fno-pie` is dropped and everything is `-fPIE`; `fork` is `vfork`
    (`nommu_fork.h` is force-included in C; OpenTTD's C++ keeps `fork` and only uses it for a
    browser). A program built by another driver must be linked position-independent or
    `flat.py` leaves it as it is and reports it.

### Compiler flags that mattered

| Flag | Why (port) |
|---|---|
| `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops` | 3-6% fewer instructions than `-O2` (Doom, Quake) |
| `-flto`, with C++ `-flto-partition=1to1 -fno-toplevel-reorder` | a fifth of a C&C frame; global constructors must keep source order or the start fails "out of memory" |
| hidden globals (`nommu_hidden.h` in the drivers) | a global without a GOT load, as the flat build is PIE |
| `-fsigned-char`, `-fno-strict-aliasing` | x86-era code (Doom's tables end with -1; Quake reads floats as ints) |
| `-fno-math-errno -fsingle-precision-constant` | `sqrt` one instruction; `x * 0.5` not a double call (Quake, Half-Life) |
| `-fwrapv` | sums that overflow were compiled differently on host and guest (Micropolis) |
| `-fno-lifetime-dse` | a custom `operator new` that marks objects before construction (C&C: missions with no walls without it) |
| `-fno-common -fdata-sections` + `objcopy --set-section-alignment .data*=16 ...` + `zz_pad` objects | what a worker writes lies in its own 16-byte texels (Doom's `APART`, Quake's `ALIGN16`) |
| PGO (`TDAWN_PROFILE=count`, `linux/tdawn/profile.py`; `linux/userland/pgo/gcov.c` for any program) | 2-9% of a C&C frame; counters printed to the console because the toolchain's libgcov is another float ABI. Late: `optimizing-games`, section 7 |

## Step 2: the platform layer

Write the machine's side as new files beside the patch (`*_shaderemu.c/.cpp`, `shaderemu_v.cpp`,
`host.c`). Keep upstream's code paths and hide yours behind a define (`WITH_SHADEREMU`,
`SHADEREMU`, `PLAT_SHADEREMU`) so the same tree builds natively for the sums (Step 5).

### Window and picture

The canonical 2D driver is `linux/openttd/shaderemu_v.cpp` (and Doom's `doom_video.c`,
`linux/tdawn/host.c`, the NES's `osd_shaderemu.c`):

1. `GrOpen()`, `GrNewWindowEx(GR_WM_PROPS_APPWINDOW, ...)`, `GrSelectEvents`, `GrMapWindow`,
   `GrSetFocus`, then `seglInit(window)` (fails: no GPU).
2. A game that works for seconds before its first picture draws "Loading NAME..." with `GrText`
   at each exposure of the first second (OpenTTD) or calls `ui_framed()` after mapping
   (`docs/system/nanox.md`, "A window before the work"), or it stands unframed in the corner.
3. The screen is an 8-bit page in GPU memory: `seglMemory(w*h)`, `glGenTextures`,
   `seglTexturePointer(ptr, w, h, GL_COLOR_INDEX8_EXT)`; point the game's screen buffer at it
   (no copy). The palette is 256 words via `seglPalette()` (0x00RRGGBB): a palette effect is 256
   stores. RGBA textures are words 0xTTRRGGBB.
4. A frame is one `seglQuad(xyz, uv, texture, 255, 255, 0)` over the window with constant
   matrices (2D in units of the whole view, not pixels: 16.16 cannot hold 2/320), then
   **`seglSwapEarly()`**, and **`seglSettle()` before the next write through a pointer** to
   anything the list reads (the page, the palette, kept vertices). OpenTTD settles in
   `LockVideoBuffer`, `Paint` and `Resize`; Doom in `D_Display`. Keep a `NAME_SWAP=wait` switch.
5. Window resized: on `GR_UPDATE_SIZE` call `seglWindowChanged()`; map the pointer with
   `seglPicturePoint()`. Use whole multiples of the game's screen by default (non-integer scale
   made texels of the atlas neighbour show at quad edges: C&C added gutters).
6. Many small draws of different textures: `seglQuad` (~150 instructions) or `seglSprite(s)`;
   a tile map is one `SEGL_TILES` layer (Micropolis); a video frame `SEGL_YUV420`.
7. 3D: keep the level in GPU memory once (`seglKeep`, `seglKeptQuads`; Doom, Quake, Quake III,
   Half-Life `gl_kept.c`) and have the game *tell* you what changed instead of scanning (Doom's
   `doom_sector_changed`, `doom_thing_changed` inserted by `doom.sh`). A GL 1.1 program gets a
   shim to `gles.c` (Quake's `qgl.c` with `include/GL/gl.h` renaming every call; Half-Life's
   `gl_shaderemu.c`); build `gles.c` with `-DSEGL_FLOAT` for float games, fixed point otherwise.
   ClassiCube writes the GPU's command lists directly (`Graphics_ShaderEmu.c`).
   `docs/machine/gpu.md` lists every `segl*` call.
8. `fps_caption.h` (`programs/linux`) writes frames a second into the caption for free.

`gles.c` is shared: a change to it moves `CC_BYTES_NOMMU` in `tools/test_all.py`, needs
`linux/tcc/build.sh` (`AGENTS.md`), and must keep every other game's checks.

### Input

- Keys and buttons from Nano-X events; map `MWKEY_*` and modifiers to the game's codes (OpenTTD's
  `KeyOf`). Track held keys yourself for keys the game polls (`_dirkeys`).
- The pointer's place from the machine's words at `0x81000020` (two loads: `pointer_read()` in
  `linux/tdawn/host.c`, `ReadPointer` in ClassiCube), not from motion events; ask the window
  system for events only when the button word or key count changed and a few times a second
  (`docs/machine/input.md`, "A program's own reading"): Red Alert went 10 to 17 fps with it.
- Wheel: a button-down with `GR_BUTTON_SCROLLUP/DN`. 3D games turn the view while the left
  button drags, a quarter degree a pixel; the right button fires.
- For tests, let lines typed at the starting terminal drive the game (ClassiCube's
  `down KEY`/`turn DX DY`/`hold`, Quake's console commands), or replay presses by frame
  (`NES_PRESS="20 start 80 a"`, `TDAWN_AUTO=1` presses Return through menus).

### Time and waiting

- Read the machine's clock word (`0x81000034`, `regs[0x34/4]`) instead of a system call (~400
  instructions each) and divide in 32 bits (64-bit division is a library call): 45k
  instructions a frame in Tiberian Dawn; Doom's `I_GetTime`, Quake's `Sys_Milliseconds`.
- Never spin on the clock: the clock moves between passes, so a spin burns the whole pass. End
  the pass: `GrGetNextEventTimeout` until the next tick (OpenTTD's `MainLoop`), `mcw_rest()` on a
  program with workers, the pause hint otherwise (Doom's `doom_idle`/`doom_wait`: 177 of 402
  million instructions of a demo were the wait).
- Do not refuse frames for being too short unless needed (Quake's `Host_FilterTime` at 10 ms
  instead of 13.9 took passes used from 89.7% to 98.2%).

### Sound and music

`#include "shaderemu_sound.h"` (`linux/userland`; registers in `programs/common/sound.h`,
`docs/machine/sound.md`). No CPU mixing in any port:

- `snd_open()`, `snd_claim()` a voice per channel, `snd_memory(bytes, &phys)` one pool,
  `snd_play(voice, kind, phys, length-1, loop, snd_step(rate), SND_VOLUME(l, r))`,
  `snd_playing`, `snd_stop`. Addresses are physical. 8-bit samples are unsigned (`^ 0x80`).
  OpenTTD's `mixer_shaderemu.cpp` maps a mixer channel to a voice and copies a sound into the
  pool on first play.
- **Play from the ROM where the file lies**: `snd_rom(fd, first_bytes, n)` gives a ROM file's
  physical address (Quake's WAVs inside the pak, Doom's WAD lumps, Micropolis's `sounds.bin`).
- **Pack at build time** into what the card plays: IMA ADPCM with a checkpoint every 32 samples
  (`tools/make_tdawn_sound.py`, ClassiCube's `sound/adpcm_encode.c`, Half-Life's `repack.py`),
  or unsigned bytes decoded by libmedia's host build (`$WORK/src/libmedia/media_test-host`).
- Music: MIDI/MUS as FM tracks with a GENMIDI OPL bank (`linux/nanox/doom_sound.c`;
  OpenTTD's `shaderemu_m.cpp` with DMXOPL's `genmidi.op2`); or an MP3 decoded on
  workers by libmedia into a stream voice's ring (ClassiCube); NES channels as voices playing
  waves at a step and volume the emulator updates (`linux/nes/apu_shaderemu.c`).
- `rvc_cpu` has no card unless `RVC_SOUND=1`; check samples with `--fixed-dt 0.004
  --sound-capture F.wav --save-state F.snap` and `tools\sound_reference.py ... --rom`.

## Step 3: data in the ROM

- Files go under `/usr/share` (or `/usr/share/NAME/`; the image builder makes missing folders
  and turns `NAME.romlink` into a hard link). The ROM is read by parallel copies: each read
  costs ~8,000 instructions however small, a misaligned read is copied a byte at a time, and a
  page of a mapped file brings 32 pages round it. So: read in large aligned blocks (OpenTTD's
  `RandomAccessFile`: 8 KB, from 8-byte boundaries, stdio unbuffered under it), and map whole
  archives instead of reading them into the heap (`ShaderEmu_Mix_Map`; a worker must not meet a
  page that is not there, so map with all pages present).
- Run the program from the ROM where it lies, from a `/tmp/NAME` folder of links to the data
  (C&C: `/usr/lib/tdawn/game`); never copy big files to `/tmp` (no-MMU tmpfs is RAM that stays).
- Bake the data down at build or image time to what the game asks for (Quake III: `bake.py` and
  `wanted.txt` from a `QUAKE3_WANTED=1` run; ClassiCube: `store_zip.py` stores zip entries
  uncompressed). Smaller is not faster: every byte saved is decoding work (Half-Life's repack
  saved 45 MB and no load time). Big files libmedia inflates should be "deflate in segments"
  (`docs/system/media.md`), decoded on every core.
- Fix file searches: case-insensitive walks from `/` list folders (reading the ROM) thousands of
  times (Tiberian Dawn: 212M to 70M instructions; later a folder's names read once,
  `ShaderEmu_Folder_Of`). Drop integrity hashes of data at load (Red Alert's SHA-1 of 24 MB,
  ~500M instructions; OpenTTD's MD5 of 20 MB).
- The starter script points the game's home at `/persistence/NAME`, else `/tmp`:
  `P=/persistence; mkdir -p $P/NAME 2>/dev/null || P=/tmp; XDG_DATA_HOME=$P XDG_CONFIG_HOME=$P exec NAME.bin "$@"`
  (OpenTTD), or `cd` there with `HOME` (Doom), or a link (Quake's `id1`). The folder holds
  ~96,000 packed bytes for everything: settings and small saves only.

## Step 4: first light on rvc_cpu

`bin\rvc_cpu.exe` is the machine as an interpreter (100-200M instructions a second, no card;
instructions are the speed). From PowerShell, not Git Bash (it turns `"/ # "` into a path;
a worktree has no `bin\` or `build\`: `running-tests`):

```
bin\rvc_cpu.exe --quiet --uart-log logs\NAME.log --wall 120 --expect "/ # " ^
  --send "nano-X -p & sleep 2; NAME_FRAMES=300 NAME; echo LX-''DONE\n" --until "LX-DONE"
```

The `''` keeps the echoed command from matching `--until`. `--image build/images/PRIVATE` runs a
private image (`python tools/private_image.py PRIVATE` after building with `NAME_OUT`).
`--pass-model` with `tools/card_model.json` gives a pass the card's time (the `calibrated-measurements` skill);
`--ips 2000000` holds it to the world's speed; `--count-from TEXT` counts every core from a mark the game prints. Read the log before concluding anything about a run. Pictures: `rvc_harness
--cpu --warp` (through the queue) with `--save-state` and `tools\snapshot_screen.py`, or
`--gpu-capture` and `tools\gpu_reference.py SNAP BMP --size WxH` against the software model.

Give the game these switches from the start; every port has them and every later step needs them:

| Switch (convention) | Does | Examples |
|---|---|---|
| `NAME_SEED` | fixed random seed | `TDAWN_SEED=7`, `CLASSICUBE_SEED`, `-G 1` |
| `NAME_FRAMES=N` / `NAME_STOP_FRAME` | end or hold after N frames, print a line | `NES_FRAMES`, `RALERT_FRAMES`, `DOOM_STOP_TIC` |
| `NAME_SUM` / `NAME_HASH_*` | print a sum of the game state | `DOOM_HASH_TICS=100`, `OPENTTD_SUM=1`, `NES_SUM=1` |
| `NAME_STATS_MS` / `NAME_RATE` | periodic `namestat:` line: frames, ticks, instructions a frame | `doomstat:`, `rastat:`, `OPENTTD_RATE=1` |
| `NAME_NO_DELAY`, `NAME_AUTO` | no real-time pacing; scripted input | `TDAWN_NO_DELAY=1`, `TDAWN_AUTO=1` |
| `NAME_SWAP=wait`, `NAME_HOLD=N`, `NAME_HOLD_LATE=1` | swap discipline check (Step 6) | `QUAKE_SWAP`, `NES_HOLD_LATE` |
| `NAME_MARK=FROM,TO` | marks for `--count-from`/`--until` | `HL_MARK`, `NES_MARK` |
| `NAME_X=inline` / `=worker` / `=check` | the same work on core 0 or a worker, or both compared | `QUAKE_SERVER=inline`, `HL_ROTATION=check` |

## Step 5: is it the same game

The machine must give the native build's sums; this is what catches compiler, UB and port bugs.

- A sum over the whole state after a fixed run: tiles, units, random state, date, money
  (OpenTTD's in `null_v.cpp` under `-v null:ticks=N -s null -m null -g -G SEED -c test.cfg`);
  every thing's place/state/health (Doom); 6502 RAM every frame (the NES: RAM is the sum to
  trust, the last picture once hid a fault).
- The same tree built for the build computer without a window: `linux/openttd/host_sum.sh`
  (native `-DOPTION_DEDICATED=ON`), Micropolis's `micropolis-sum` built by its `build.sh`,
  C&C's native build with a `host.c` that writes the page to a file. Run with a config copy:
  OpenTTD writes its cfg back on exit.
- A mismatch is a bug to find, not noise: Micropolis's `voteProblems` read past its table, its
  overflowing sums needed `-fwrapv`, `Quad` had to be 32 bits.
- Add a case to `tools/test_all.py` with the doc it comes from:
  `case('NAME', 'games', 'CMD; CMD', workers, sums=[(r'regex (hex)', ['sum1', 'sum2'])])`;
  also `has=`, `never=`, `count=`, `digest=` (md5 of matched lines for long lists), `same=`
  (another case's lines), `until=`, `wall=`, `more=['--send', ...]`, `env={'RVC_SOUND': '1'}`.
  `X = 'nano-X -p & sleep 2; '` prefixes a windowed run. Put the sum table in the doc too.
- Float games' sums on the card differ from `rvc_cpu`'s by design; judge them by agreement
  with themselves.

## Step 6: make it fast enough

The whole of this is the `optimizing-games` skill. In short: measure passes a frame and calibrated frames a second
(`python tools/game_par.py GAME --calibrated`), not only core 0's instructions; a change that is faster but adds worker
instructions is kept ("faster beats fewer instructions").

- `python tools/prof.py run OUT -- --image build/images/nommu --quiet --expect "/ # " --send "echo FL-''GO; ...; echo LX-''DONE\n" --count-from FL-GO --until LX-DONE`,
  then `prof.py report OUT --elf ~/nm/work/src/.../NAME` (the unstripped build the image has;
  a rebuilt ELF is refused) or `prof.py auto` (verdicts on which loops and calls a worker could
  take). `docs/machine/profiling.md`. Profile without test sums switched on.
- `python tools/game_par.py` (core 0 and all cores a frame), `pwsh tools\tick_util.ps1` (what
  ended passes early), `RVC_CALLS=FILE` + `tools/mc_candidates.py` (calls worth a job).
- A start is measured as a one-tick run (`openttd -v null:ticks=1 -g` under `prof.py`).
- The card only to validate the metric (`game_par.py GAME --card`, through `tools\gpu_queue.ps1`), after checking for the
  developer's own `rvc_harness.exe` with no arguments; it refuses while VRChat or SteamVR runs.

`hotspots.md` has the list of what was slow before. The four that every port hit: the clock
and 64-bit division; file searches and data hashes at start; redrawing what did not change
(status bars, messages, whole screens: draw only on change); and the write cache - one core
keeps about 5.25 KB of distinct 16-byte texels a pass (a large worker 2.6 KB, a small one 0.66), so stores 64 bytes apart,
column-major copies and wide structures end passes (lay hot structures out by texel; Quake's
`particle_t` is three texels). Large moves go to the machine's copy (CSRs `0x0b1` src,
`0x0b2` dst, `0x0b3` bytes, write 1 to `0x0b0`; 256 KB a pass; OpenTTD's `MachineCopy` for
scrolling, behind `OPENTTD_MEMOP=0|check`).

**Swap discipline check**: `NAME_HOLD=N NAME_HOLD_LATE=1` with `--gpu-capture`, once with
`NAME_SWAP=wait` and once without: the two pictures must be identical. A plain hold stops
before the next frame writes and cannot show a store made too early.

## Step 7: worker cores

Read `docs/machine/multicore.md` ("Using the workers from a program", "The second layer",
"Races", "Where workers pay"). What paid in the ports: work that needs no answer this frame -
a server on its own worker (Quake, Quake III, Half-Life), drawing a frame behind from copies
(Doom's view, Quake's picture, C&C's scene), decoding and loading (movies, PNGs, MP3, inflate),
work begun a frame ahead (Doom's sight checks, Red Alert's LCW). What does not: small jobs (a
pass each way, ~2 passes), splitting a frame that is ten passes long, game logic that the
drawing reads and marks.

- Use `programs/mc/mcwx.h` (`mcw_for`, `mcw_for_begin/ready/wait`, `mcw_call`/`mcw_join`,
  `mcw_print`, `mcw_defer`, `mcw_scratch`, `mcw_fail`) over `mcw.h`; no hand-rolled loops.
  Link `mcw.c` and `mcwx.c` (compiled apart, 16-byte aligned sections).
- `mcw_open(n)`: ask only for what is needed; 0 workers is the common case (D3D12, profiles)
  and the program must do the work itself then.
- **Sixteen bytes, one writer a pass**: give a worker whole texels; globals it writes
  `aligned(16)` and padded, or compiled apart with `-fdata-sections` and section alignment.
- **First core stores first**: memory a worker will store to (GPU memory and mailboxes too)
  wants a store by core 0 first; `mcw_touch()` buffers; code a worker starts in is read first
  (`mcw_touch_code` from `_start`'s page, never from `__executable_start`, which is 0).
- A job that waits ends its pass with `mc_end_pass()`; core 0 waiting calls `mcw_rest()`.
- No `malloc`/stdio on a worker while core 0 uses them; a worker cannot open a packed file
  (Half-Life's level change became a command for core 0). A frame drawn on a worker is
  swapped with `seglSwapHand()` and submitted by core 0's `seglSwapTake()`.
- Keep a `NAME_X=inline` switch whose sums must equal the worker's, and a `NAME_CHECK=1` that
  recomputes what the worker gave and prints `0 of them wrong` (Doom's `DOOM_VIEW_CHECK`,
  C&C's `TDAWN_CHECK_PATHS`, Half-Life's `HL_ROTATION=check`).
- Run once with `RVC_RACES=FILE` and `RVC_RACES_NEAR=4` on `rvc_cpu`: **0 undone** (reads "as it was" are fine) and the
  same sums; `python tools/test_all.py --races`. Walk the game, too: standing still plays little of it. A switch
  that races stays off by default. The `worker-core-bugs` skill reads its output.
- `mctest 3 120000` must PASS before and after the game in one boot, also after
  `killall -9` mid-run (the NES's `nes-kill` case).

## Step 8: the desktop

All written by the game's `build.sh` into `$OUT`:

- Start menu: `echo "Games/Label=command" > "$OUT/usr/share/nxapps.NN-name"` (NN orders the
  files; 50 Doom, 70 Quake, 72 Micropolis, 73 OpenTTD). The desktop's shortcut comes from it.
- Files opened with it: `/usr/share/nxopen.NN-name`, a line a kind: `kind endings first-bytes
  command %s` (`-` for none, `\xNN` bytes). Doom opens WADs by `IWAD`/`PWAD`, OpenTTD `.sav`.
  Add a row to the table in `docs/system/open.md`.
- Icon: a PNG in `linux/apps/icons/NAME.png` (the game's own, credited in
  `linux/apps/icons/README.md`, or drawn by `tools/draw_icons.py` when the logo is not ours),
  the line `name,name.bin  NAME` in both `linux/apps/icons.txt` and `icons7.txt`, then
  `python tools\make_icons.py`.
- Settings the desktop shares: registry keys `NAME.key` through `linux/apps/registry.h`
  (`reg_get`, `reg_set`); the game's own config and saves in `/persistence/NAME` (row in
  `docs/system/persist.md`'s table).
- Optional, for the VRChat world: a number in `linux/apps/gameid.h` and
  `unity/ShaderEmu/Scripts/EmuGamepad.cs` for the controller's mapping; `seglWorldEye`,
  `seglHostState` and a `NAME_SEAT` switch for the holodeck's seat (`docs/world/holodeck.md`).

## Step 9: in the image, tested, documented

1. `wsl -- bash linux/nommu/build.sh NAME/build.sh` (prints one line, exit code and seconds; the log is
   in `~/nm/logs`), `wsl -- bash linux/nommu/root.sh`, then `pwsh tools\make_image.ps1`
   (`--save-prebuilt` keeps `linux/prebuilt`, never with data we may not pass on).
2. `python tools/test_all.py` (two minutes): every case passes, yours included; `--races` if
   it uses workers; `python linux/nommu/memory.py` if it is large (add it to its `BIG`).
3. Run it on the desktop under `rvc_harness --cpu --cpu-ips 2000000` through the queue; capture
   with `--save-state` and `snapshot_screen.py`, and look at the pictures.
4. `docs/games/NAME.md` in the docs' own style (plain sentences, numbers with their date and
   how they were measured, calibrated for speed), in the sections the others have: the commands,
   what the build is made of / what the patch changes, how it is drawn, sound, speed, "Is it the
   same game" / "Checking it" (the sums), "Not done". Add it to `docs/README.md`, and to `README.md`'s table.
5. `AGENTS.md`: one dense line only for an invariant a later change could break (what must
   still say `0 of them wrong`, what must be rebuilt after what), as the existing lines do.
6. Commit in the repository's style (the change and its numbers), no attribution; `merging-agent-work` for branches.

## Pitfalls that cost rounds before

- Git Bash path mangling of `"/ # "` and of `/mnt/...` paths: run `rvc_cpu` from PowerShell.
- In WSL `$NAME` is the computer's name: never name a build variable `NAME`.
- Python edits of repo files: open `rb`/`wb` (text mode writes CRLF). A Bash-tool heredoc loses
  a level of backslashes: write such scripts with the Write tool.
- A build that rewrites every patched file rebuilds everything (write only changed text).
- A no-MMU busybox `setsid` run by a session leader re-execs as the wrong applet: do not
  `exec setsid` in scripts (`linux/nanox/nx`, `nxoff`).
- `tools/private_image.py` reads the shared root each time: a "before" image made again after a
  rebuild is not before; `linux/prebuilt/root` of the last commit is.
- `rvc_cpu` with threads counts other instruction totals from run to run for programs whose
  workers wait on each other: compare means of several runs, never one; the sums do not move.
- A progress picture drawn too often is the cost (OpenTTD's world generation: four a second
  were a third of the making; now one).
- Defaults sized for the machine: OpenTTD's new map 64x64 (256x256 takes ~16 min to make at
  2 MIPS), Tiberian Dawn's game speed 20 steps, ClassiCube's low-memory world.
- A window that dies with no error is usually the developer closing it; run headless.
