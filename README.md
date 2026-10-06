# ShaderX86

An 8086 PC (MS-DOS, Windows 3.0 real mode) emulated in a VRChat pixel shader. See the design
handoff for the plan; this repo currently holds the headless D3D11 harness.

## Layout

- `harness/core/` – reusable D3D11 harness: ShaderLab parser, FXC compile + bytecode cache,
  Unity-style materials (uniforms/textures by name via reflection), a double-buffered
  Custom Render Texture with update zones, pipelined readback, WIC image loading.
- `harness/unity_include/` – minimal stand-ins for Unity's built-in `.cginc` files.
- `harness/apps/rvc_harness.cpp` – runs pimaker's rvc (RISC-V Linux) unmodified, with its UART
  on the console. This proves the harness reproduces Unity's CRT behaviour. It is a frontend over
  two backends: `rvc_backend11.cpp` (D3D11, FXC bytecode, what VRChat runs) and
  `rvc_backend12.cpp` (D3D12, DXC-compiled DXIL). `bin\rvc_harness.exe` defaults to the first,
  `bin\rvc_harness_dxc.exe` to the second; `--dxc` / `--d3d11` switch either.
- `experiments/rvc_opt/` – patched copy of rvc's shader, 3x faster with bit-identical emulation.
- `harness/apps/rvc_trace12.cpp` – the same two draws on D3D12, for `tools/gpu_trace.ps1`
  (Nsight GPU Trace hardware counters per draw).
- `tools/perf_test.ps1` – 3-second speed benchmark; `tools/watch_console.cmd` – live console.
- `rvc/` – upstream clone of https://github.com/pimaker/rvc (not part of this repo; ignored).

## Build (Windows)

Needs CMake 3.20+ and Visual Studio 2022 (MSVC) with the Windows SDK.

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
| `raytrace` | Rust raytracer, no firmware |
| `bare` | C bare-metal self test, no firmware |

The menu appears whenever the command line does not say what to boot (`--image NAME`,
`--payload`, `--ram`, `--load-state`) and input is a console; scripts with redirected input or
`--no-stdin` get `linux-net`. `--image list` prints the table.

The console then becomes the emulated machine's terminal: every key goes to the guest, including
Ctrl+C, arrows and Tab, and the guest does its own echo. **Ctrl+]** quits. It runs the fastest
shader (`experiments/rvc_opt`) and boots from power-on. `--resume` starts at the Linux shell
prompt from `build\snapshots\rvc_shell.snap` instead.

A second window shows memory live: RAM as colour in two strips (low addresses top left), texels
written since the previous screen frame glowing and fading. A bar underneath, clear of the memory
image, shows IPS, frames/s, frame time, GPU time of the tick and commit draws, uptime, guest
clock and totals, with the 64x64 CPU state area magnified at its right end. Closing it leaves the emulator running; `--no-viz` skips it, and `--viz`
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
