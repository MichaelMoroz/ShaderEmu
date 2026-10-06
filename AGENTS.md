# Agent notes

Rules for anyone (human or agent) running the emulator harness in this repo.

## Direction

Speed first. In order: a bare-metal machine with no MMU (no translation, TLBs or state for
them), Doom on it, then an emulated GPU with its own driver so drawing runs in parallel shader
passes instead of on the serial emulated CPU. x86 is dropped (`docs/original-x86-plan.md`).

## Show the live console whenever the emulator runs

Before starting `rvc_harness`, make sure the console viewer window is open so the developer can
watch the emulated machine:

    explorer.exe tools\watch_console.cmd

- It follows `logs\uart.log`, so every run must pass `--uart-log logs\uart.log` (`run_rvc.bat`
  does). Each run writes a `=== rvc_harness: ... ===` banner where it starts.
- One window serves all runs. Its pid is in `logs\console_viewer.pid`; if that process is alive,
  reuse it instead of opening another.
- Launch it through `explorer.exe`: `Start-Process pwsh` from an agent shell gets no window.

## Terminal mode and the memory view

- `bin\rvc_harness.exe` (D3D11 + FXC) and `bin\rvc_harness_dxc.exe` (D3D12 + DXC) are one program
  with a different default backend; `--dxc` / `--d3d11` switch. With no arguments either is the
  interactive terminal (raw console pass-through, memory view window, cold boot; `--resume`
  starts from the shell snapshot). Automated runs always pass arguments, which keeps them
  headless; never start the no-argument form from a script and leave it running.
- An image menu blocks at start when nothing says what to boot and stdin is a console. Scripted
  runs must pass one of `--image NAME`, `--payload`, `--ram`, `--load-state` or `--no-stdin`.
- `rvc_harness` runs 16,384 instructions per draw by default (about 20% faster than 2,048 on DXC).
  Every reference state hash was taken at 2,048: pass `--ticks 2048` when comparing
  (`perf_test.ps1` and `rvc_trace12` already use 2,048).
- Our programs live in `programs/`; `programs\build.bat` builds them with clang for rv32ima and
  the harness loads `programs\bin\<name>.bin` directly (no PNG step). They are integer-only C,
  so the same source compiled natively gives a bit-exact reference for what the emulator must
  produce: check framebuffers against that, not by eye.
- `raytrace` waits for five answers on the console before rendering. Script it with
  `--expect width --send ""` style pairs (one per prompt: width, height, bounces, side,
  shadows) and `--until "raytrace: done"`.
- `raycast` waits for a key at its title screen, then draws only when a key changes the view.
  `b` runs a fixed 354-frame walk and prints `raycast: demo done` (the benchmark: about 218k
  instructions per frame); `x` walks forever. Typed input reaches a guest at about 25 keys/s,
  so give scripted key strings enough time before snapshotting, or the last frame is half drawn.
- The GPU device (`docs/gpu.md`) is `experiments\rvc_opt\gpu.shader`: a real mesh draw into
  its own colour and depth target plus one small control zone, run after Commit when that file
  exists (`--no-gpu` leaves them out). Check it against `tools\gpu_reference.py`: pause the
  guest (`gears`: `--expect "frame 1024" --send p --until "paused at"`), pass `--save-state` and
  `--gpu-capture`, then run the model on both. Expect 99.9% of pixels away from triangle edges
  within 2 levels; the rest are one-pixel shifts at texel boundaries.
- A vertex shader reading a texture in D3D12 needs the NON_PIXEL_SHADER_RESOURCE state. With
  only PIXEL_SHADER_RESOURCE the pixel shader sees fresh data and the vertex shader stale data
  in the same draw, with no error. To find such a thing, write what each stage reads into the
  colour and capture it.
- `wfi` ends the CPU's tick pass early (a stall, cleared at commit). It is the way for a guest to
  wait for anything that changes between frames; an idle Linux now runs short frames too.
- `gpu.shader` compiles under FXC in 0.1 s and gears on D3D11 matches the software model as
  well as on D3D12. It ran at 78 frames/s there against 485 on D3D12/DXC; why is not known yet.
- Guest data the GPU reads a texel at a time (vertex buffers, uniform vectors) must be 16-byte
  aligned (`GPU_ALIGNED`); unaligned data renders garbage without any error.
- The display is RAM at fixed addresses (`docs/display.md`); the view window decodes it beside
  the memory image. The CPU shader has no display code and should not grow any.
- `--machine auto` (the default) compiles the smallest machine the image runs on: `NO_PAGING`
  for rvc's bare-metal images, `M_MODE_ONLY` for our own programs, full for Linux and snapshots.
  A smaller machine must not change emulation for a guest that stays within it: compare `state`
  and `instructions` with `--machine full` on a fixed-timestep run (`--fixed-dt 0.004 --frames N
  --bench 0`).
- The shader has no switches for old or rejected code paths any more. When removing a switch,
  dump the DXIL before and after (`rvc_trace12 --dxc --dxc-dump DIR`, with and without each
  remaining define) and require the files to be identical.
- `--dxc` implies `NO_DOUBLES` and, without `--rvc`, the `experiments\rvc_opt` shader (upstream
  does not compile with DXC). `--profile` and `--present` are D3D11 only.
- The D3D12 backend has its own memory view (`rvc_memview12.h`) sharing shader, window and text
  code with `memview.cpp`. D3D11 cannot open a 128-bit-per-texel texture shared from D3D12, so
  do not try to reuse the D3D11 view there.
- Raw pass-through only engages when stdin is a real console. To test it without the physical
  keyboard, run the harness in its own hidden console and write key events into that console's
  input buffer (`WriteConsoleInput`), then read its screen buffer back.
- Input reaches the guest slowly (about 27 characters/s): each console poll costs the guest
  tens of thousands of instructions. `rvc_opt` accepts up to four characters per handshake
  (`_UartBurst`), which is what keeps escape sequences such as arrow keys in one piece. Pass
  `--uart-burst 1` when comparing against upstream, whose shader takes one.
- `--viz-capture FILE.bmp` saves the memory view at exit, for checking it by measurement.

## Do not cold-boot; resume from a snapshot

A cold boot to the `/ #` prompt takes about 80 s on an RTX 5090 with upstream on D3D11 (about
17 s with `--dxc`). Do it once, then resume:

    run_rvc.bat --no-stdin --until "/ # " --seconds 600 --save-state build\snapshots\rvc_shell.snap
    run_rvc.bat --load-state build\snapshots\rvc_shell.snap ...

- A snapshot is the whole 2048x4096 RGBA32UI state texture plus guest time and UART input tag
  (128 MB, under the ignored `build\`).
- Re-create it only when the shader, the payload PNGs or the snapshot format change. A snapshot
  from another shader build loads without error and misbehaves.
- Pass `--until "/ # "` from PowerShell or cmd, never Git Bash: MSYS rewrites an argument that
  starts with `/` into a Windows path, the text never matches, and the run idles to its limit.
- When a run looks slow or stuck, read `logs\uart.log` and its timestamp before concluding anything.
- Scripted checks: `--expect TEXT --send TEXT` pairs plus `--until TEXT`; exit code 0 = matched,
  3 = limit hit first.

## Measure speed with the perf test

    pwsh tools\perf_test.ps1                           # upstream rvc
    pwsh tools\perf_test.ps1 -Rvc experiments\rvc_opt  # a patched shader folder

- About 3 s. It resumes `build\snapshots\rvc_bench.snap` (a shell busy loop; created on first
  use from `rvc_shell.snap`), runs 500 frames on a fixed timestep and prints IPS.
- Runs are deterministic. If `instructions` and `state` match upstream, the change did not alter
  emulation; if they differ, it did, and that needs explaining before any speed claim.
- `rvc_harness --profile` (on a shader with `PROF()` counters, see `experiments\rvc_opt\README.md`)
  prints how often each instruction and memory/CSR/TLB path runs. Counts are per emulated
  instruction, not per pixel.
- `pwsh tools\gpu_trace.ps1` captures an Nsight GPU Trace (per-draw hardware counters) in about
  15 s. It runs `bin\rvc_trace12.exe`, the D3D12 twin of the harness, because Nsight does not
  attach to D3D11. Always with `--no-doubles`: NVIDIA's D3D12 path miscomputes the shader's
  double math and the guest diverges otherwise. Check that its BENCH `state` matches D3D11.
- New D3D12 code: test it with a dummy shader that writes position, uniforms and texture
  contents before trusting it with the emulator.
- Prototype shader edits with DXC first: about 5 s per try instead of 2-3 minutes.

      bin\rvc_trace12.exe --rvc experiments\rvc_opt --payload rvc\_Nix\rvc\data-net --dxc --no-doubles --frames 500 --bench 30

  The `state` hash is as trustworthy as FXC's, so use it to check that an edit keeps emulation
  identical. Speed is close to FXC only with these settings: shader model 6.6 (the default;
  6.0 is 10% slower) and `--no-doubles` (doubles cost 15% under DXIL). `-O1`..`-O3` make no
  difference, and `-O0`/`-Od` fail to compile. Even so DXC and FXC can disagree on how much a
  change helps (dispatch + fetch: 1.32x under FXC, 1.17x under DXC), and the first run after
  a compile is often slow, so run twice and confirm winners with FXC on D3D11
  (`tools\perf_test.ps1`), which is what VRChat runs.
- In a shader loop this size, how control flow joins matters more than instruction counts:
  every value both sides of a branch can modify becomes a phi (a register move, or a spill)
  wherever they rejoin. `python tools\dxil_path.py tick.ll 19` prints one loop iteration's path
  through the DXIL for an opcode, with its phi and instruction counts; get `tick.ll` from
  `rvc_trace12 --dxc --dxc-dump DIR` and `dxc -dumpbin`. Keep hot and cold paths in separate
  loops, and keep rarely changed state out of the hot one.
- `rvc_trace12 --cold` boots from power-on on D3D12, for checking early-boot paths under DXC.
- `rvc_harness --dxc --load-state build\snapshots\rvc_bench.snap --no-stdin --fixed-dt 0.004
  --ticks 2048 --frames 830 --bench 30` must print the same `state` as `rvc_trace12 --dxc --no-doubles
  --frames 800 --bench 30` (`231e365030389de0` for the current shader).
- A shader edit costs one FXC compile (90 s alone, about 3 min with three in parallel). Compile
  variants in parallel, then benchmark them one at a time.

## Timings to expect

- Harness build: about 5 s.
- First FXC compile of rvc's `CPUTick` pass: about 90 s; cached afterwards in `build\shadercache`.
- Speed: about 520k instructions/s upstream, 1,860k with `experiments\rvc_opt` on FXC
  (under DXC on D3D12: about 2.2M at 2,048 ticks per draw, 3.0M at 65,536). It barely depends on
  `--ticks`: frame time is the serial CPU loop, about 1.8 us per instruction upstream.
