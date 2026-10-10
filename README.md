# ShaderEmu

A computer emulated entirely in a pixel shader: a 32-bit RISC-V machine that boots Linux to a
desktop, with its own GPU, and runs Doom. The target is VRChat, which allows only Direct3D 11
graphics shaders (no compute shaders, no UAVs), so the whole machine lives in textures that
fragment shaders rewrite each frame.

> **Accelerate everything that can be accelerated.**

The emulated CPU runs a few million instructions a second, one after another, and no amount of
tuning changes what kind of machine that is. The shader around it is massively parallel. So
anything that touches many bytes or many pixels is given to hardware devices that run in their
own shader passes, and the CPU only says what it wants: drawing, window composition, text,
3D, copies and fills of memory, pictures sampled from the ROM where they lie. That is the
difference between a desktop picture that takes 6 seconds to appear and one that takes 15 ms,
and it is the only way this computer becomes usable.

## What it does today

- **The machine** is RV32IMA with supervisor mode and paging, derived from
  [pimaker's rvc](https://github.com/pimaker/rvc). Its processor state, RAM and the GPU's
  memory are one 2048x4096 texture; the root file system is a ROM in four more.
- **Linux** (5.17, our build of pimaker's fork) boots to a shell in under 3 seconds and
  10.5 million instructions, without firmware: the emulator answers the kernel's SBI calls
  itself (`docs/boot.md`). The shell and tools are a static busybox on musl.
- **A GPU device** (`docs/gpu.md`): the guest writes vertex buffers, textures and a command
  list into memory, and a mesh whose shaders read them is rasterised by the real graphics
  card, in up to eight passes with depth and blending. A small fixed-point OpenGL ES 1.x
  library sits on it (`programs/linux/gles.c`).
- **A desktop** (`docs/nanox.md`): Nano-X, with every window in a buffer of its own and the
  screen composed by the GPU. A bar with a Start menu and task buttons, a terminal with
  scroll-back, an editor, a file manager, a browser with simple CSS, paint, a picture viewer,
  settings, a system monitor, photographs for a desktop, and `glxgears`. The machine starts
  it by itself at boot.
- **Pages from the network** (`docs/fetch.md`): the machine has none of its own, so the browser
  asks the host, which fetches the page and writes it into memory in one shader pass.
- **Doom** (`docs/doom.md`): Microwindows' port, with the level drawn by the machine's GPU
  through OpenGL instead of by its CPU.
- **Red Alert** (`docs/ralert.md`): the same source tree's Red Alert with the 1996 demo's data
  (a Soviet and an Allied mission), on Tiberian Dawn's layer: the map on the GPU, sound and music.
- **Command & Conquer** (`docs/tdawn.md`): Vanilla Conquer's Tiberian Dawn with the 1995 demo's
  missions, drawn in software and shown by the GPU: it plays, at a half to two thirds of its speed.
- **Quake** (`docs/quake.md`): id's GLQuake with the shareware episode, everything drawn by the
  GPU, on float instructions the machine gained for it (`docs/fpu.md`): 2 to 8 frames a second.
- **ClassiCube** (`docs/classicube.md`): the Minecraft Classic client, a world of blocks to dig and
  build in, drawn by the GPU from chunks kept in its memory: 50 to 70 frames a second walking.
- **A C compiler in the machine** (`docs/cc.md`): TinyCC, with the C library, Nano-X and
  OpenGL ES, and two 3D examples it builds there.
- **Keyboard, pointer and wheel** as Linux input devices (`docs/input.md`), a display the guest
  describes in a few control words (`docs/display.md`).
- **A VRChat world** (`unity/ShaderEmu`): the same shaders run by UdonSharp scripts, with the
  display, a console, keyboards and a live view of memory on a wall, and a third screen that is a
  window onto a 3D program's scene, seen in depth from where one stands (`docs/volume.md`).

Speed on an RTX 5090, in emulated instructions per second:

| Shader | Compiler / API | Speed |
|---|---|---|
| upstream rvc | FXC, D3D11 | about 0.5M |
| `experiments/rvc_opt` | FXC, D3D11 (what VRChat runs) | 2.6M to 3.4M under Linux |
| `experiments/rvc_opt` | DXC, D3D12 | 3.4M to 4.5M under Linux |
| the VRChat world | Unity, D3D11 | 2.7M to 2.9M |

What that buys, in frames per second: `glxgears` in a window about 580 (DXC) and 420 (FXC);
Doom played in real time at the start of the first level about 31 (DXC) and 23 (FXC).
`experiments/rvc_opt/README.md` lists the changes to the CPU shader and what each measured.

The D3D11 build no longer needs FXC: [fxc2](https://github.com/MichaelMoroz/FXC2) compiles the
CPU shader in 9 seconds instead of 9 minutes and, because it can compile what FXC cannot (the
write cache and TLB as local arrays, MULH as one instruction), its build is faster too: on an
RTX 5070 laptop 1,970k against 1,528k instructions per second at 2,048 instructions a frame, and
a Linux cold boot in 22.6 s against 30.2 s, with the same emulated state. `docs/fxc2.md` has the
how and the numbers.

## Try it (Windows)

Needs CMake 3.20+ and Visual Studio 2022 (MSVC) with the Windows SDK, Python with Pillow, and
upstream rvc cloned next to the sources for its boot images:

    git clone https://github.com/pimaker/rvc rvc
    build.bat                          # the harness: bin\rvc_harness.exe, bin\rvc_harness_dxc.exe
    python tools\make_linux_image.py   # our Linux image, from linux\prebuilt (no compiler needed)

Then double-click `bin\rvc_harness_dxc.exe` (D3D12 + DXC: starts in seconds) or
`bin\rvc_harness.exe` (D3D11 + FXC: the first start compiles the shader for several minutes;
with fxc2's `d3dcompiler_47.dll` next to it, seconds: `docs/fxc2.md`).
A menu asks what to boot; `linux-net` is Linux with the desktop.

Two windows appear. The console is the machine's serial terminal: every key goes to the guest,
Ctrl+C and the arrows included, and **Ctrl+]** quits. The other window is the machine's screen:
the display on the left, where the window's keys and pointer are the machine's keyboard and
mouse, and all of its memory on the right, with what is being written glowing.
`--no-desktop` boots to the shell only; `--resume` starts from a saved shell prompt.

| image | what it is |
|---|---|
| `linux-net` | Linux with our kernel, tools and the desktop |
| `gears` | three lit, textured gears drawn by the GPU device, bare metal (`programs/gears`) |
| `blend`, `rects` | GPU test cards: passes and blending; 3,600 rectangles copied back to RAM |
| `raycast` | our Wolfenstein-style raycaster: w/a/s/d or the arrows, `x` walks on its own |
| `raytrace` | our C raytracer: asks for resolution, bounces and shadows on the console |
| `linux`, `micropython`, `rust`, `rvc-raytrace`, `bare` | upstream rvc's images |

The bare-metal images run on smaller machines compiled without paging, or with machine mode
only, which are faster and end in the same state; `--machine` overrides the choice.
`rvc_harness --help` lists every option, and `AGENTS.md` holds the working notes: how to script
runs, measure speed, and what has been tried.

To rebuild the guest software (WSL, no root; everything goes to `~/shaderemu-linux`):

    wsl -- bash /mnt/c/.../linux/kernel/build.sh       # toolchain and kernel
    wsl -- bash /mnt/c/.../linux/userland/build.sh     # init
    wsl -- bash /mnt/c/.../linux/userland/busybox.sh   # shell and tools
    wsl -- bash /mnt/c/.../linux/nanox/build.sh        # Nano-X and its programs
    wsl -- bash /mnt/c/.../linux/apps/build.sh         # the desktop's own programs
    wsl -- bash /mnt/c/.../linux/nanox/doom.sh         # Doom
    wsl -- bash /mnt/c/.../linux/tcc/build.sh          # the C compiler for the guest, its examples
    python tools\make_wallpaper.py                     # desktop pictures (fetched, not kept here)
    python tools\make_linux_image.py

## Layout

- `experiments/rvc_opt/` – the machine: the CPU's tick and commit passes (`main.shader`,
  `src/`) and the GPU device (`gpu.shader`). A patched copy of rvc's shader (MIT, see its
  `LICENSE`).
- `harness/` – runs the shaders outside Unity. `apps/rvc_harness.cpp` is the frontend over two
  backends: D3D11 with FXC bytecode, as VRChat runs it, and D3D12 with DXC. `core/` is the
  ShaderLab parser, compiler cache, materials by reflection, readback and the memory view.
- `linux/kernel/` – build script for the kernel and our drivers: the GPU (`/dev/gpu`), the
  display (`/dev/fb0`), keyboard and pointer, and hooks that send large copies and fills to
  the machine's parallel copy.
- `linux/userland/` – the musl toolchain, the image's init and busybox.
- `linux/nanox/` – Nano-X: its screen, keyboard and pointer drivers for this machine, our
  patch to Microwindows, the bar, and Doom's video and OpenGL renderer.
- `linux/apps/` – the desktop's programs: editor, files, browser, paint, viewer, settings, monitor.
- `linux/prebuilt/` – the kernel and programs already built (sources and licences in its
  README).
- `programs/` – bare-metal programs in C, and `programs/linux`: the OpenGL library and
  `glxgears`.
- `unity/ShaderEmu/` – the VRChat world: UdonSharp scripts, shaders and the editor scripts
  that build the scene.
- `tools/` – the image builder, a software model of the GPU (`gpu_reference.py`), profilers
  (`pc_profile.py`, `boot_profile.py`, `dxil_path.py`), the speed test and the console viewer.
- `docs/` – one file per subject: `boot`, `gpu`, `display`, `input`, `fetch`, `volume`, `nanox`, `cc`, `doom`, `tdawn`, `ralert`, `console`, `gamepad`.
- `rvc/` – a clone of upstream rvc (not part of this repository): its boot images and the
  reference shader.

## Not done yet

- A real network: pages reach the browser through the host (`docs/fetch.md`), nothing else does.
- `linux/prebuilt` lags behind the sources between refreshes.
- The Unity world runs one machine configuration (Linux); the bare-metal images run only in
  the harness.

An earlier plan targeted an 8086 PC running MS-DOS and Windows 3.0. It was dropped as too slow;
the design notes are kept in `docs/original-x86-plan.md`.

## Built on

[rvc](https://github.com/pimaker/rvc) and [linux-rvc](https://github.com/pimaker/linux-rvc) by
pimaker; Linux; musl; BusyBox; GCC and LLVM; [Microwindows](https://github.com/ghaerr/microwindows)
by Greg Haerr and contributors, with its port of Doom; TinyCC and jrrk2's riscv32 port of it; DOOM and [Quake](https://github.com/id-Software/Quake) by id Software; [Vanilla Conquer](https://github.com/TheAssemblyArmada/Vanilla-Conquer)
and Command & Conquer by Westwood Studios (source released by Electronic Arts);
[ClassiCube](https://github.com/ClassiCube/ClassiCube) by UnknownShadow200 and contributors; [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut and contributors; [Nofrendo](https://github.com/espressif/esp32-nesemu) by Matthew Conte, with [Thwaite](https://github.com/pinobatch/thwaite-nes) by Damian Yerrick and [Nova the Squirrel](https://github.com/NovaSquirrel/NovaTheSquirrel) by NovaSquirrel; [minimp3](https://github.com/lieff/minimp3) by lieff and [dr_flac](https://github.com/mackron/dr_libs) by David Reid in the music player; glxgears from the
Mesa demos; photographs from Unsplash; furniture, plants and surfaces of the VRChat room from
[Poly Haven](https://polyhaven.com) (CC0); [LTCGI](https://github.com/PiMaker/ltcgi) by pimaker,
[VRC Light Volumes](https://github.com/REDSIM/VRCLightVolumes) by RED_SIM and
[Mochie's shaders](https://github.com/MochiesCode/Mochies-Unity-Shaders) for the room's light and rain; recordings from [Freesound](https://freesound.org) given to the public domain: a computer by squashy555, a keyboard by harrisonlace, thunder by Fission9, bastipictures and Kinoton; VRChat's SDK and UdonSharp;
[ShaderAudio](https://gitlab.com/lox9973/ShaderAudio) by lox9973, whose way of getting sound out of
a shader the sound card follows.

## Licence

MIT (`LICENSE`). `experiments/rvc_opt` is derived from pimaker's rvc and keeps its own MIT
licence file. The kernel drivers in `linux/kernel` are GPL-2.0, as they are built into Linux.
`linux/prebuilt` holds binaries built from other projects' sources; its README lists them
with their licences.
