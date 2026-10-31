# ShaderEmu

A computer emulated entirely in a pixel shader: a 32-bit RISC-V machine that boots Linux to a
desktop, with its own GPU, and runs Half-Life. The target is VRChat, which allows only Direct3D 11
graphics shaders (no compute shaders, no UAVs), so the whole machine lives in textures that
fragment shaders rewrite each frame.

> **Accelerate everything that can be accelerated.**

The emulated CPU runs a few million instructions a second, one after another, and no amount of
tuning changes what kind of machine that is. The shader around it is massively parallel. So
anything that touches many bytes or many pixels is given to hardware devices that run in their
own shader passes, and the CPU only says what it wants: drawing, window composition, text,
3D, copies and fills of memory, pictures sampled from the ROM where they lie. That is the only
way this computer becomes usable.

## What it does today

- **The machine** is RV32IMAF with supervisor mode, derived from
  [pimaker's rvc](https://github.com/pimaker/rvc). Its processor state, RAM and the GPU's
  memory are one 2048 x 9280 texture (256 MB of RAM, 0x82000000 to 0x92000000, the devices' 32 MB
  below it); the root file system is a ROM in four more.
- **256 cores** (`docs/machine/multicore.md`): Linux runs on core 0, and programs borrow worker cores
  for their loops: decoding pictures, sound and films, physics, path tracing, the browser's
  strips, a game's server or its view. A worker's pixels can run lanes of their own
  (`docs/machine/lanes.md`).
- **Linux without an MMU** (`docs/system/nommu.md`): 5.17, our build of pimaker's fork, built without
  `CONFIG_MMU`, every program a flat one, and the machine with no page walk at all. It boots without
  firmware: the emulator answers the kernel's SBI calls itself (`docs/machine/boot.md`). The shell and
  tools are a static busybox on musl. Linux with the MMU is no longer supported.
- **A GPU device** (`docs/machine/gpu.md`): the guest writes vertex buffers, textures and a command
  list into memory, and a mesh whose shaders read them is rasterised by the real graphics
  card, in up to eight passes with depth and blending. A small fixed-point OpenGL ES 1.x
  library sits on it (`programs/linux/gles.c`).
- **Devices**: keyboard, pointer and wheel as Linux input devices (`docs/machine/input.md`), a display
  the guest describes in a few control words (`docs/machine/display.md`), a sound card
  (`docs/machine/sound.md`), a console (`docs/machine/console.md`), float instructions
  (`docs/machine/fpu.md`).
- **A desktop** (`docs/system/nanox.md`): Nano-X, with every window in a buffer of its own and the
  screen composed by the GPU, a bar with a Start menu, pinned programs, task buttons, a calendar and
  the volume. The machine starts it by itself at boot. Two looks, chosen in Settings: Windows 7
  (Oxygen icons, glass frames, the Selawik font, `docs/system/fonts.md`) and classic. A program that
  fails to start is reported in a window, and a file opens with the program for its kind
  (`docs/system/open.md`).
- **Pictures, sound and films** (`docs/system/media.md`): one library, libmedia, decodes and encodes
  them on the worker cores for every program, and inflates one deflate stream on many cores.
- **A network between machines** (`docs/system/lan.md`): TCP/IP with Linux's own stack, each machine a
  host of one network; the harnesses and the players of the VRChat world are its hosts.
- **Pages from the web** (`docs/system/fetch.md`): the machine has no way out of its own, so the
  browser asks the host, which fetches the page and writes it into memory in one shader pass. An
  `http` address on the machines' own network it opens over its own sockets (`docs/apps/web.md`).
- **A folder that persists** (`docs/system/persist.md`): `/persistence`, which the host keeps between
  sessions when asked (in the world, as the player's PlayerData), with a registry of settings all
  programs share.
- **A VRChat world** (`unity/ShaderEmu`, `docs/world/world.md`): the same shaders run by UdonSharp
  scripts, a machine for every player, in a den of the 1990s. On its wall the display, a
  console, keyboards and a live view of memory, and a screen that is a window onto a 3D
  program's scene, seen in depth from where one stands (`docs/world/volume.md`). Players can put each
  other's machines on their screens (`docs/world/share.md`), a classroom's computers show everyone's
  (`docs/world/stations.md`), and in the holodecks a 3D game's frame is a world round one's seat
  (`docs/world/holodeck.md`). The Control panel sets the speed and the instructions a tick, and
  shows how much of a tick the machine reaches.

## What is in the image

Every program below is in `build/images/nommu`, and flat (`ls /usr/bin` in the machine). The Start
menu lists those with a window; the machine's own reader has these docs at `/usr/share/doc/shaderemu`.

### Games

| In the Start menu | What it is | Doc |
|---|---|---|
| Half-Life: Uplink (`halflife`) | Xash3D FWGS with the HLSDK, its 3D on a worker core a frame behind; Valve's demo data is fetched from archive.org when an image is made, never committed | `docs/games/halflife.md` |
| Quake (`quake`) | id's GLQuake with the shareware episode, everything drawn by the GPU, the server on a worker | `docs/games/quake.md` |
| Quake III Arena (`quake3`) | ioquake3 with the free demo's The Longest Yard and its bots, the server's frame on a worker | `docs/games/quake3.md` |
| Doom (`doom`) | Microwindows' port, the level drawn by the GPU, the view made on up to four workers | `docs/games/doom.md` |
| Tiberian Dawn (`tdawn`) | Vanilla Conquer with the 1995 Command & Conquer demo's missions, the map on the GPU | `docs/games/tdawn.md` |
| Red Alert (`ralert`) | the same source tree with the 1996 demo's data (a Soviet and an Allied mission), sound and music | `docs/games/ralert.md` |
| OpenTTD (`openttd`) | OpenTTD 15.3 with the open base sets, the picture on the GPU, viewports on the workers, the game's tick on a worker | `docs/games/openttd.md` |
| ClassiCube (`classicube`) | the Minecraft Classic client, a world of blocks to dig and build in, chunks kept in GPU memory | `docs/games/classicube.md` |
| Micropolis (`nxmicropolis`) | the city simulator of 1989 (MicropolisCore's engine) with its cities and scenarios, the map on the GPU | `docs/games/micropolis.md` |
| Thwaite, Nova the Squirrel (`nes`) | a NES: Nofrendo with two free games, or any `.nes` of a mapper it has | `docs/games/nes.md` |
| Pinball (`nxpinball`) | a table of our own | `docs/apps/pinball.md` |
| Tetris, Mines (`nxtetris`, `nxmine`) | small games on the desktop | `docs/system/nanox.md` |
| Network (`nxgame`) | Quake, Quake III, OpenTTD and Doom hosted and joined between machines | `docs/system/lan.md` |

### The desktop's programs

| Program | What it is | Doc |
|---|---|---|
| `nxterm` | a terminal, xterm-like | `docs/system/nanox.md` |
| `nxfiles` | the file manager and the desktop itself | `docs/system/nanox.md` |
| `nxedit` | a tabbed editor, with a reader for Markdown | `docs/system/nanox.md` |
| `nxweb` | a browser: HTML, CSS, forms, cookies, no scripts; `http` over the machine's own sockets, the rest through the host | `docs/apps/web.md` |
| `nxpaint`, `nxview` | paint; a photo viewer | `docs/system/nanox.md` |
| `nxplay` | a music player with a playlist (MP3, FLAC, Ogg, WAV, MOD) | `docs/apps/play.md` |
| `nxvideo` | a video player (MPEG-1, VQA, Motion JPEG, GIF) | `docs/apps/video.md` |
| `nxcalc`, `nxclock`, `nxmon` | Windows 7's calculator and clock; Task Manager | `docs/system/nanox.md` |
| `nxsettings`, `nxreg`, `nxpersist` | settings; the registry; what `/persistence` keeps | `docs/system/nanox.md`, `docs/system/persist.md` |
| `nxnet` | the machines of the network and their shared folders | `docs/system/lan.md` |
| `open`, `unarc` | a file with the program for its kind; zip, tar and `.gz` unpacked on the workers, eight to ten times busybox's speed | `docs/system/open.md`, `docs/system/media.md` |
| `nxphys` | rigid boxes on every core | `docs/apps/physics.md` |
| `nxray`, `nxpath` | rays on every core; a path tracer and viewer of meshes | `docs/apps/raytrace.md` |
| `imdemo` | Dear ImGui over the machine's OpenGL | `docs/apps/imgui.md` |
| `glxgears`, `nxeyes`, `nxev`, `demo-*` | Mesa's gears; Nano-X's eyes, event viewer and demos | `docs/system/nanox.md` |

The bar (`nxbar`), questions (`nxask`), the shutdown (`nxoff`), program icons (`nxicon`) and typed keys
for tests (`nxkey`) are the desktop's parts: `docs/system/nanox.md`.

### Developer tools

| Program | What it is | Doc |
|---|---|---|
| `nxide` | an IDE: a project's tree, coloured text, errors marked as you type, Build and Run, a debugger for C and Python, JavaScript and HLSL too | `docs/apps/ide.md` |
| `cc`, `tcc`, `elf2flt`, `example` | TinyCC with the C library, Nano-X, OpenGL ES and the worker cores, and examples it builds there | `docs/system/cc.md` |
| `hlslc`, `hlslview` | HLSL compiled in the machine to steps on the lanes | `docs/system/hlsl.md` |
| `qjs`, `qjsc`, `micropython` | QuickJS and MicroPython | `docs/system/scripts.md` |
| `nano`, `mg`, `mc`, `htop`, `dialog`, `file`, `sl` | upstream's terminal programs, built flat | `docs/system/terminal.md` |
| `mctest`, `lanetest`, `steptest`, `fptest`, `sndtest`, `media_test`, `hlsl_bench`, `nxbench`, `geotune`, `handoff`, `printf_check` | the machine's own tests and benchmarks | `docs/machine/multicore.md`, `docs/machine/lanes.md`, `docs/machine/cpu-harness.md` |

### Ports

Upstream's network and system programs, built flat (`docs/system/ports.md`): Dropbear's `ssh`, `scp`,
`dbclient` and server, OpenSSH's `sftp`, `curl` with TLS, `socat` with `filan` and `procan`, `thttpd` with
`makeweb` and `htpasswd`, `pcregrep` and `pcretest`, `dotlockfile` and lockfile-progs, `at` and `atd`, and
iproute2's `ss`, `bridge`, `tc`, `genl`, `ifstat`, `lnstat`, `nstat`, `rtacct` and `rtmon`. BusyBox has the
rest (`docs/system/nommu.md`).

## How fast

The games, frames a second by the guest's clock: `python tools/game_par.py GAME --calibrated` on the image
of 31 October 2026, `rvc_cpu`'s clock fitted to an RTX 5090 (calibrated, 31 October 2026, good to about 7%:
`docs/machine/speed.md`, "Calibrated"), beside that card's own run where the fit has one. Core 0 alone at
full passes is about 5.5 million instructions a second.

| Game, the load | RTX 5090 | calibrated |
|---|---|---|
| Doom, `-timedemo demo1` | 145.4 | 145.9 |
| Quake, `+timedemo demo1` | | 109.7 |
| Quake III, q3dm17 with a bot, frames 300 to 700 | 79.4 | 77.4 |
| Tiberian Dawn, mission 3, frames 301 to 1200 (it wants 20) | 60.4 | 60.8 |
| Red Alert, the Soviet mission, frames 301 to 1200 | 86.2 | 86.8 |
| ClassiCube, walking into a hill, 854 x 480 | | 176.2 |
| the NES, Thwaite, its frames 100 to 1000 (it wants 60) | | 226.8 |

Half-Life and OpenTTD have their own figures, from 31 October 2026: Half-Life's fight, calibrated,
24.3 frames a second and walking 50.5 (`docs/games/halflife.md`, "Speed"); OpenTTD on the card under a
VR frame, a played round 10.3 to 11.2 pictures and 2.6 to 2.9 game ticks a second
(`docs/games/openttd.md`, "Speed on 31 October").

The D3D11 build needs no FXC: [fxc2](https://github.com/MichaelMoroz/FXC2) compiles the CPU shader
in seconds where FXC takes minutes (`docs/machine/fxc2.md`). `experiments/rvc_opt/README.md` says how the
CPU shader is built and why.

## Try it (Windows)

Needs CMake 3.20+ and Visual Studio 2022 (MSVC) with the Windows SDK, Python with Pillow, and
upstream rvc cloned next to the sources: the image is made on its root file system.

    git clone https://github.com/pimaker/rvc rvc
    build.bat                          # the harness: bin\rvc_harness.exe, bin\rvc_harness_dxc.exe, bin\rvc_cpu.exe
    pwsh tools\make_image.ps1          # the Linux image, from linux\prebuilt (no compiler needed)

Then double-click `bin\rvc_harness.exe` (D3D11, as VRChat runs it, with the worker cores; the
build puts fxc2's `d3dcompiler_47.dll` next to it, so the shader compiles in seconds, where FXC
takes minutes: `docs/machine/fxc2.md`) or `bin\rvc_harness_dxc.exe` (D3D12 + DXC, one core only).
A menu asks what to boot; `linux-net` is Linux with the desktop (`build\images\nommu`). `i` in the
menu, or `--isa base|zb`, chooses the instruction set. `bin\rvc_cpu.exe` is the machine on the processor,
no card needed (`docs/machine/cpu-harness.md`).

Two windows appear. The console is the machine's serial terminal: every key goes to the guest,
Ctrl+C and the arrows included, and **Ctrl+]** quits. The other window is the machine's screen:
the display on the left, where the window's keys and pointer are the machine's keyboard and
mouse, and all of its memory on the right, with what is being written glowing.
`--no-desktop` boots to the shell only; `--resume` starts from a saved shell prompt.

| image | what it is |
|---|---|
| `linux-net` | Linux with our kernel, tools and the desktop (`build\images\nommu`) |
| `gears` | three lit, textured gears drawn by the GPU device, bare metal (`programs/gears`) |
| `blend`, `rects` | GPU test cards: passes and blending; 3,600 rectangles copied back to RAM |
| `raycast` | our Wolfenstein-style raycaster: w/a/s/d or the arrows, `x` walks on its own |
| `raytrace` | our C raytracer: asks for resolution, bounces and shadows on the console |
| `sound`, `classes`, `fpcheck` | a sound test card; loops of one kind of instruction; float instructions' sum |
| `micropython`, `rust`, `rvc-raytrace`, `bare` | upstream rvc's images |

The bare-metal images run on smaller machines compiled without paging, or with machine mode
only, which are faster and end in the same state; `--machine` overrides the choice.
One harness on the card runs alone: it refuses to start beside another or beside any
`--cpu` one, which run together (`docs/machine/cpu-harness.md`, "One card, many processors").
`rvc_harness --help` lists every option, and `AGENTS.md` holds the rules of the work: how to script
runs, measure speed, and what each subsystem's change must keep.

To rebuild the guest software (WSL with rsync, no root; from the repository's folder, after
`make_image.ps1` above; everything goes to `~/shaderemu-linux` and `~/nm`). `docs/system/nommu.md`,
"Building it", has each step:

    wsl -- bash linux/kernel/build.sh      # once: the cross compiler and the kernel's tree
    wsl -- bash linux/userland/build.sh    # once: the C library, init
    wsl -- bash linux/nommu/setup.sh       # once: compiler drivers for flat programs, ~/nm
    pwsh linux\nommu\all.ps1 -Kernel       # the kernel, every program, the root, the image, the test set

`all.ps1` is `linux/nommu/kernel.sh`, `build.sh` (one script a game or program; names after it build
only those), `root.sh`, the icons and fonts, `image.py` and `test.py`. A change to one program:

    wsl -- bash linux/nommu/build.sh quake/build.sh
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1

WSL's `python3` wants numpy (Quake III's level, ClassiCube's sound) and `ffmpeg` is wanted once, for
ClassiCube's tune, and for Half-Life's data. The address the path tracer's teapot is fetched from no
longer answers: before the first build, `mkdir -p ~/nm/work/src/pathtrace-data` and copy
`linux/prebuilt/root/root/Models/teapot.obj` there. Desktop pictures are `python tools\make_wallpaper.py`
(fetched, not kept here).

`python tools\test_all.py` is the test set (152 cases on `rvc_cpu`, about two minutes; `--list` names them).
`pwsh tools\make_image.ps1 --save-prebuilt` keeps a build in `linux\prebuilt`.

## Layout

- `experiments/rvc_opt/` – the machine: the CPU's tick and commit passes (`main.shader`,
  `src/`) and the GPU device (`gpu.shader`). A patched copy of rvc's shader (MIT, see its
  `LICENSE`). The world's own tick is `experiments/rvc_lean`'s (`MachineTickLean`).
- `harness/` – runs the shaders outside Unity. `apps/rvc_harness.cpp` is the frontend over two
  backends: D3D11 with FXC bytecode, as VRChat runs it, and D3D12 with DXC; `apps/rvc_cpu.cpp`
  is the machine as an interpreter on the processor (`docs/machine/cpu-harness.md`). `core/` is the
  ShaderLab parser, compiler cache, materials by reflection, readback and the memory view.
- `linux/kernel/` – the kernel's tree and our drivers, which `linux/nommu/kernel.sh` builds: the GPU (`/dev/gpu`), the
  display (`/dev/fb0`), keyboard and pointer, and hooks that send large copies and fills to
  the machine's parallel copy.
- `linux/nommu/` – the machine's Linux: its kernel's patches, the flat programs' compiler
  drivers and loader format, the image, its test set and memory test.
- `linux/userland/` – the musl toolchain, the image's init and busybox.
- `linux/libmedia/` – pictures, sound and films for every program, on the worker cores.
- `linux/nanox/` – Nano-X: its screen, keyboard and pointer drivers for this machine, our
  patch to Microwindows, the bar, and Doom's video and OpenGL renderer.
- `linux/apps/` – the desktop's programs: editor, files, browser, paint, viewer, settings, monitor.
- `linux/quake/`, `linux/quake3/`, `linux/halflife/`, `linux/tdawn/`, `linux/ralert/`, `linux/openttd/`,
  `linux/classicube/`, `linux/micropolis/`, `linux/nes/`, `linux/physics/`, `linux/ports/` and the rest –
  the ports and programs that have a folder of their own.
- `linux/prebuilt/` – the kernels and programs already built: a shared root, and in `nommu/` the
  machine's kernel and flat programs (sources and licences in its README).
- `programs/` – bare-metal programs in C, `programs/linux`: the OpenGL library and
  `glxgears`, `programs/mc`: the worker cores' library (`mcw.c`, `mcwx.h`) and its tests,
  `programs/hlsl`: the HLSL compiler, `programs/wl`: the machine's workloads piece by piece.
- `experiments/` – beside `rvc_opt`: `rvc_lean` (the same processor written again, a shader a draw), and the
  studies `rvc_compute`,
  `isa_proto` and `udon_aot`.
- `unity/ShaderEmu/` – the VRChat world: UdonSharp scripts, shaders and the editor scripts
  that build the scene.
- `world/` – the Blender scripts that model the room, its computers and the city (`docs/world/world.md`).
- `tools/` – the image builder, the test set (`test_all.py`), games' speeds (`game_par.py`,
  `calibrate.py`), a software model of the GPU (`gpu_reference.py`), profilers (`prof.py`: `docs/machine/profiling.md`; `pc_profile.py`,
  `boot_profile.py`, `tick_util.ps1`), the
  queue every run on the card goes through (`gpu_queue.ps1`), the speed test and the console
  viewer.
- `docs/` – one file per subject, in folders (`docs/README.md`): `machine/` (the processor, its
  devices and harnesses), `system/` (Linux, Nano-X, the compiler, libmedia, the network), `apps/`
  (the desktop's programs), `games/`, `world/` (the VRChat room) and `plans/` (what is not built).
- `rvc/` – a clone of upstream rvc (not part of this repository): its boot images and the
  reference shader.

## Not done yet

- A way out to the internet: the machines reach each other (`docs/system/lan.md`), and pages reach the
  browser through the host (`docs/system/fetch.md`); nothing else leaves.
- `linux/prebuilt` lags behind the sources between refreshes.
- The bare-metal images run only in the harness.

An earlier plan targeted an 8086 PC running MS-DOS and Windows 3.0. It was dropped as too slow;
the design notes are kept in `docs/plans/original-x86-plan.md`.

## Built on

[rvc](https://github.com/pimaker/rvc) and [linux-rvc](https://github.com/pimaker/linux-rvc) by
pimaker; Linux; musl; BusyBox; GCC and LLVM; [Microwindows](https://github.com/ghaerr/microwindows)
by Greg Haerr and contributors, with its port of Doom; [Xash3D FWGS](https://github.com/FWGS/xash3d-fwgs) and [hlsdk-portable](https://github.com/FWGS/hlsdk-portable) by FWGS, after Valve's Half-Life SDK; [QuickJS](https://bellard.org/quickjs/) by Fabrice Bellard; [MicroPython](https://micropython.org); nano, htop, Midnight Commander, mg, dialog, file and sl, and Dropbear, OpenSSH's sftp, curl with Mbed TLS, socat, thttpd, at and iproute2, with Monocypher (their versions in `docs/system/terminal.md` and `docs/system/ports.md`); TinyCC and jrrk2's riscv32 port of it; DOOM, [Quake](https://github.com/id-Software/Quake) and Quake III Arena by id Software, with [ioquake3](https://github.com/ioquake/ioq3); [MicropolisCore](https://github.com/SimHacker/MicropolisCore) by Don Hopkins, from SimCity by Will Wright and Maxis (source released by Electronic Arts); [Vanilla Conquer](https://github.com/TheAssemblyArmada/Vanilla-Conquer)
and Command & Conquer by Westwood Studios (source released by Electronic Arts);
[ClassiCube](https://github.com/ClassiCube/ClassiCube) by UnknownShadow200 and contributors; [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut and contributors; [Nofrendo](https://github.com/espressif/esp32-nesemu) by Matthew Conte, with [Thwaite](https://github.com/pinobatch/thwaite-nes) by Damian Yerrick and [Nova the Squirrel](https://github.com/NovaSquirrel/NovaTheSquirrel) by NovaSquirrel; [minimp3](https://github.com/lieff/minimp3) by lieff and [dr_flac](https://github.com/mackron/dr_libs) by David Reid in the music player; glxgears from the
Mesa demos; [pl_mpeg](https://github.com/phoboslab/pl_mpeg) by Dominic Szablewski; the
[Chicago95](https://github.com/grassmunk/Chicago95) and [Oxygen](https://github.com/KDE/oxygen-icons) (KDE) icons; the [Selawik](https://github.com/microsoft/Selawik) font by Microsoft; the fonts' distance fields made by [msdf-atlas-gen](https://github.com/Chlumsky/msdf-atlas-gen) 1.4 by Viktor Chlumsky (MIT); Big Buck Bunny's trailer ((c) copyright 2008, Blender
Foundation, CC BY 3.0); Musopen's public-domain recordings; photographs from Unsplash, NASA's and two old
paintings from Wikimedia Commons; furniture, plants and surfaces of the VRChat room from
[Poly Haven](https://polyhaven.com) (CC0); [LTCGI](https://github.com/PiMaker/ltcgi) by pimaker,
[VRC Light Volumes](https://github.com/REDSIM/VRCLightVolumes) by RED_SIM and
[Mochie's shaders](https://github.com/MochiesCode/Mochies-Unity-Shaders) for the room's light, and [Fake Glass](https://gitlab.com/s-ilent/fake-glass) by Silent (CC BY-SA 4.0, its rain's textures by deepspacebanana) for the rain on the window; the path tracer's models are the Utah teapot (Martin Newell) and the Stanford Bunny (Stanford Computer Graphics Laboratory); recordings from [Freesound](https://freesound.org) given to the public domain: a computer by squashy555, a keyboard by harrisonlace, thunder by Fission9, bastipictures and Kinoton; [QvPen](https://github.com/ureishi/QvPen) by ureishi for the room's pens; VRChat's SDK and UdonSharp;
[ShaderAudio](https://gitlab.com/lox9973/ShaderAudio) by lox9973, whose way of getting sound out of
a shader the sound card follows.

## Licence

MIT (`LICENSE`). `experiments/rvc_opt` is derived from pimaker's rvc and keeps its own MIT
licence file. The kernel drivers in `linux/kernel` are GPL-2.0, as they are built into Linux.
`linux/prebuilt` holds binaries built from other projects' sources; its README lists them
with their licences.
