# Agent notes

Rules for anyone (human or agent) running the emulator harness in this repo.

## Show the live console whenever the emulator runs

Before starting `rvc_harness`, make sure the console viewer window is open so the developer can
watch the emulated machine:

    explorer.exe tools\watch_console.cmd

- It follows `logs\uart.log`, so every run must pass `--uart-log logs\uart.log` (`run_rvc.bat`
  does). Each run writes a `=== rvc_harness: ... ===` banner where it starts.
- One window serves all runs. Its pid is in `logs\console_viewer.pid`; if that process is alive,
  reuse it instead of opening another.
- Launch it through `explorer.exe`: `Start-Process pwsh` from an agent shell gets no window.

## Do not cold-boot; resume from a snapshot

A cold boot to the `/ #` prompt takes about 80 s on an RTX 5090. Do it once, then resume:

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
- A shader edit costs one FXC compile (90 s alone, about 3 min with three in parallel). Compile
  variants in parallel, then benchmark them one at a time.

## Timings to expect

- Harness build: about 5 s.
- First FXC compile of rvc's `CPUTick` pass: about 90 s; cached afterwards in `build\shadercache`.
- Speed: about 520k instructions/s upstream, 1,050k with `experiments\rvc_opt`. It barely depends on
  `--ticks`: frame time is the serial CPU loop, about 1.8 us per instruction upstream.
