# ShaderEmu

A computer emulated entirely in a pixel shader, as fast as it can be made to go. The target
platform is VRChat, which allows only Direct3D 11 graphics shaders (no compute shaders, no
UAVs), so the whole machine lives in a render texture that a fragment shader rewrites each frame.

## Where it stands

The machine today is a RISC-V (RV32IMA) computer derived from
[pimaker's rvc](https://github.com/pimaker/rvc), run outside Unity by a headless harness:

| Shader | Compiler / API | Instructions per second (RTX 5090) |
|---|---|---|
| upstream rvc | FXC, D3D11 | about 520k |
| `experiments/rvc_opt`, changes 1-17 | FXC, D3D11 | about 1,860k, bit-identical to upstream |
| `experiments/rvc_opt`, all changes | DXC, D3D12 | 2.2M at 2,048 instructions per draw, 3.0M at 65,536 |

It boots Linux to a shell in about 17 s (DXC), and runs MicroPython and bare-metal C and Rust
payloads. `experiments/rvc_opt/README.md` lists every change and what was measured.

## Where it is going

Speed is the priority, in this order:

1. **Bare-metal mode with no MMU.** Address translation, its TLBs and the state that carries
   them sit on the serial dependency chain of every emulated instruction. A machine that runs
   bare-metal programs needs none of it.
2. **Doom**, running bare metal on that machine.
3. **An emulated GPU with its own driver.** The emulated CPU is one serial chain per frame; a
   GPU's strength is running every pixel at once. Drawing moves out of the emulated CPU into
   shader passes that work in parallel, and the guest talks to them through a driver.

An earlier plan targeted an 8086 PC running MS-DOS and Windows 3.0. It was dropped as too slow;
the design notes are kept in `docs/original-x86-plan.md`.

## Layout

- `harness/core/` – reusable D3D11 harness: ShaderLab parser, FXC compile + bytecode cache,
  Unity-style materials (uniforms/textures by name via reflection), a double-buffered
  Custom Render Texture with update zones, pipelined readback, WIC image loading.
- `harness/unity_include/` – minimal stand-ins for Unity's built-in `.cginc` files.
- `harness/apps/rvc_harness.cpp` – runs rvc's shader with its UART on the console. It is a
  frontend over two backends: `rvc_backend11.cpp` (D3D11, FXC bytecode, what VRChat runs) and
  `rvc_backend12.cpp` (D3D12, DXC-compiled DXIL). `bin\rvc_harness.exe` defaults to the first,
  `bin\rvc_harness_dxc.exe` to the second; `--dxc` / `--d3d11` switch either.
- `experiments/rvc_opt/` – patched copy of rvc's shader (MIT, see its `LICENSE`).
- `harness/apps/rvc_trace12.cpp` – the same two draws on D3D12, for benchmarks and
  `tools/gpu_trace.ps1` (Nsight GPU Trace hardware counters per draw).
- `programs/` – our own bare-metal programs in C (`programs\build.bat`, needs LLVM with the
  RISC-V target). The built images in `programs/bin/` are checked in, so running them needs no
  compiler.
- `tools/perf_test.ps1` – 3-second speed benchmark; `tools/watch_console.cmd` – live console.
- `rvc/` – clone of upstream rvc (not part of this repo): the payload images and the reference shader.

## Build (Windows)

Needs CMake 3.20+ and Visual Studio 2022 (MSVC) with the Windows SDK, and upstream rvc
cloned next to the sources for its payload images:

    git clone https://github.com/pimaker/rvc rvc

    build.bat            # cmake -S . -B build && cmake --build build --config Release

## Use it as a terminal

Double-click `bin\rvc_harness_dxc.exe` (D3D12 + DXC: starts in seconds, about 2.4M instructions/s)
or `bin\rvc_harness.exe` (D3D11 + FXC: the first start compiles for minutes). A menu asks what to
boot:

| image | what it is |
|---|---|
| `linux-net` | Linux with networking and a romfs root; about 17 s to the shell on DXC |
| `linux` | Linux with a built-in initramfs (slow to unpack) |
| `micropython` | MicroPython REPL on OpenSBI |
| `rust` | Rust test payload on OpenSBI |
| `raytrace` | our C raytracer: asks for resolution, bounces, rays per pixel and shadows on the console, then draws to the display (`programs/raytrace`) |
| `rvc-raytrace` | rvc's Rust raytracer, which fills raw memory instead |
| `bare` | C bare-metal self test, no firmware |

The menu appears whenever the command line does not say what to boot (`--image NAME`,
`--payload`, `--ram`, `--load-state`) and input is a console; scripts with redirected input or
`--no-stdin` get `linux-net`. `--image list` prints the table.

The bare-metal images run on a machine built without paging (`NO_PAGING`: `satp` hardwired
to 0, no translation, no TLBs), which is 14-17% faster and ends in the same state as the full
machine. Linux needs the MMU and gets the full build. `--paging on|off|auto` or `m` in the menu
overrides the choice.

The console then becomes the emulated machine's terminal: every key goes to the guest, including
Ctrl+C, arrows and Tab, and the guest does its own echo. **Ctrl+]** quits. It runs the fastest
shader (`experiments/rvc_opt`) and boots from power-on. `--resume` starts at the Linux shell
prompt from `build\snapshots\rvc_shell.snap` instead.

A second window shows memory live: RAM as colour in two strips (low addresses top left), texels
written since the previous screen frame glowing and fading. To its right is the machine's
display: a framebuffer the guest keeps in RAM at fixed addresses, with a mode and resolution it
sets itself (`docs/display.md`). A bar underneath, clear of the memory
image, shows IPS, frames/s, frame time, GPU time of the tick and commit draws, uptime, guest
clock and totals, with the 64x64 CPU state area magnified at its right end. Closing it quits the harness; `--no-viz` skips it, and `--viz`
adds it to any other invocation.

## Run rvc

    run_rvc.bat                                  # boots Linux on the GPU; type at the console
    run_rvc.bat --warp                           # software rasterizer (no GPU, no TDR, slow)
    run_rvc.bat --until "/ # " --seconds 900     # exits 0 once the shell prompt appears
    run_rvc.bat --no-stdin --until "/ # " --save-state build\snapshots\rvc_shell.snap
    run_rvc.bat --load-state build\snapshots\rvc_shell.snap   # resume at the prompt in ~1 s
    tools\watch_console.cmd                      # window that follows logs\uart.log live

The first run compiles rvc's shader with FXC, which can take several minutes; the bytecode is
cached in `build/shadercache/`. UART output is also appended to `logs/uart.log`.
`--skip-opt` compiles much faster at some runtime cost. `rvc_harness --help` lists all options.
