# ShaderX86

An 8086 PC (MS-DOS, Windows 3.0 real mode) emulated in a VRChat pixel shader. See the design
handoff for the plan; this repo currently holds the headless D3D11 harness.

## Layout

- `harness/core/` – reusable D3D11 harness: ShaderLab parser, FXC compile + bytecode cache,
  Unity-style materials (uniforms/textures by name via reflection), a double-buffered
  Custom Render Texture with update zones, pipelined readback, WIC image loading.
- `harness/unity_include/` – minimal stand-ins for Unity's built-in `.cginc` files.
- `harness/apps/rvc_harness.cpp` – runs pimaker's rvc (RISC-V Linux) unmodified, with its UART
  on the console. This proves the harness reproduces Unity's CRT behaviour.
- `rvc/` – upstream clone of https://github.com/pimaker/rvc (not part of this repo; ignored).

## Build (Windows)

Needs CMake 3.20+ and Visual Studio 2022 (MSVC) with the Windows SDK.

    build.bat            # cmake -S . -B build && cmake --build build --config Release

## Run rvc

    run_rvc.bat                                  # boots Linux on the GPU; type at the console
    run_rvc.bat --warp                           # software rasterizer (no GPU, no TDR, slow)
    run_rvc.bat --until "login:" --seconds 900   # exits 0 once the login prompt appears

The first run compiles rvc's shader with FXC, which can take several minutes; the bytecode is
cached in `build/shadercache/`. UART output is also appended to `logs/uart.log`.
`--skip-opt` compiles much faster at some runtime cost. `rvc_harness --help` lists all options.
