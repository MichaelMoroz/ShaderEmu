# Agent notes

Rules for anyone (human or agent) running the emulator harness in this repo.

## Direction

Speed first, and by one rule: accelerate everything that can be accelerated. The emulated CPU
is serial and slow; whatever touches many bytes or pixels goes to a device that runs in shader
passes of its own (the GPU, window composition, parallel copies, pictures sampled from the
ROM). The machine boots Linux to a desktop and runs Doom on its GPU; the VRChat world
(`unity/ShaderEmu`) is the target. x86 is dropped (`docs/original-x86-plan.md`).

## Count instructions on the processor, look at pictures on the graphics card

`bin\rvc_cpu.exe` (`docs/cpu-harness.md`) is the same machine as an interpreter: no graphics
card, a hundred million instructions a second, a cold boot in a tenth of a second. Use it for
anything that is about instructions: a game's `...stat:` lines, its state sums, whether a
program still starts and runs after a change. Red Alert's 300-frame check is 5 s there and
90 s in `rvc_harness`.

    bin\rvc_cpu.exe --cores 4 --quiet --uart-log logs\ucpu_x.log --expect "/ # " --send "COMMANDS; echo LX-''DONE\n" --until "LX-DONE"

- `rvc_harness --cpu` is the same interpreter with the harness's window, display, GPU and
  input (the shader's passes, fed from the interpreter's RAM): for trying a thing by hand at
  that speed, or `--cpu-ips 3000000` for the world's.
- `rvc_cpu.exe` itself has no GPU, display or sound: a submitted list counts as drawn and nothing is drawn.
  Pictures, sound, `mctest`, anything about the shader machine's own limits (its write cache,
  one writer to 16 bytes among workers) and frame rates in real seconds are `rvc_harness`'s.
- Start it from PowerShell or cmd, as the harness: Git Bash rewrites `"/ # "`.
- Its guest clock is counted from instructions (`--ips`, `--pass-ms`), so a run repeats exactly.

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
- `rvc_harness` runs 32,768 instructions per draw by default (it was 16,384; `rvc_cpu` still is).
  Every reference state hash was taken at 2,048: pass `--ticks 2048` when comparing
  (`perf_test.ps1` and `rvc_trace12` already use 2,048).
- Our programs live in `programs/`; `programs\build.bat` builds them with clang for rv32ima and
  the harness loads `programs\bin\<name>.bin` directly (no PNG step). They are integer-only C,
  so the same source compiled natively gives a bit-exact reference for what the emulator must
  produce: check framebuffers against that, not by eye.
- `raytrace` waits for five answers on the console before rendering. Script it with
  `--expect width --send "
"` style pairs (one per prompt: width, height, bounces, side,
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
- The Linux image the harness boots for `linux-net` is `build\images\linux` when it exists
  (`python tools\make_linux_image.py`: upstream's root filesystem plus `programs\bin\glxgears`,
  and a device tree with RAM ending at 0x86000000). Without it, upstream's image boots and has
  no glxgears. `build\snapshots\rvc_shell.snap` is of our image; the old one is kept as
  `rvc_shell_upstream.snap`. Re-run the script and re-make the snapshot after rebuilding a
  Linux program.
- The kernel is ours when `build\images\linux\Image` exists: build it with
  `wsl -- bash /mnt/c/Development/ShaderX86/linux/kernel/build.sh` (84 s the first time,
  downloads included; everything goes to `~/shaderemu-linux`, nothing is installed), then run
  `make_linux_image.py`, which puts it at +4 MiB of the boot image, with nothing before it.
  It adds `/dev/gpu` (mmap, and a wait that runs `wfi` in the kernel),
  `/dev/fb0` and the evdev keyboard and pointer (`docs/input.md`).
- Our kernel's tick is 100 Hz (upstream's is 20 Hz, where any sleep takes at least 50 ms).
- Nano-X: `wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh` (run it from
  PowerShell: Git Bash rewrites the `/mnt/c` path), then `make_linux_image.py` and a new shell
  snapshot. `nx` starts it in the guest. To check its drawing, run the same scene with
  `NANOX_SOFTWARE=1` and compare the framebuffers in RAM (`docs/nanox.md`).
- The desktop's own programs (`linux\apps`, `docs/nanox.md`): `wsl -- bash
  /mnt/c/Development/ShaderX86/linux/apps/build.sh` after Nano-X's build, and
  `python tools\make_wallpaper.py [FOLDER ...]` for the desktop's pictures (needs the network
  for the Unsplash ones), then `make_linux_image.py` and a new shell snapshot.
- Start the server by hand as `nano-X -p &`: without `-p` it ends when its last program has
  gone, and what follows cannot reach it. Windows are placed in the order programs connect,
  so a test that clicks at fixed places must start its programs a few seconds apart.
- A PPM picture is drawn by the GPU from the file's bytes in the ROM (`GrDrawImageFromFile`).
  To check one, stretch the file over the same rectangle on the host, sampling at pixel
  centres, and compare with the window's buffer: all pixels match within one texel.
- A program's own timing does not include what it asked the Nano-X server to do. To time a
  drawing request, run the program ten times in a row (each waits for the server to be free)
  and subtract ten runs of one that draws nothing.
- Reading a file costs about 2,000 instructions a 4 KB page in Linux's page cache, whatever
  the copy costs: the ROM driver's reads go through the machine's parallel copy
  (`linux\kernel\phram_hook.py`) and a 5 MB cold read only went from 0.99 s to 0.89 s.
- The root is an overlay over the ROM's romfs: `fstatfs` says overlay, not romfs, for a file
  in the ROM. A file's number is still romfs's, the offset of its header in the ROM.
- To see where a guest spends instructions, pass `--pc-log FILE --ticks 2048` and run
  `python tools\pc_profile.py FILE kernel=vmlinux.nm server=nano-X.nm` (`nm -n` output; the
  unstripped binaries are in `~/shaderemu-linux`). Measure before optimising drawing: the
  first Nano-X profile was 57% system calls, not pixels.
- The harness reads the machine's control words (RAM from 0x87000000, 256 texels) back with
  row 0 every frame: `popRow()` returns them after the 64 texels of row 0. A guest program
  that owns the keyboard (`docs/input.md`) stops window keys from also reaching the console.
- A change to Microwindows itself goes into `linux\nanox\microwindows.patch`: edit a clean
  tree in `~/shaderemu-linux/src/microwindows` and save `git diff -- src/nanox src/engine`.
  The software-versus-GPU comparison cannot catch a mistake there (both runs share the
  engine): compare with frames from the build before the change.
- GPU memory is 0x86000000-0x87b00000 now (`/dev/gpu` maps from 0x86000000; the control words
  are still at 0x87000000) and the image's RAM ends at 0x86000000. A change to that layout
  touches `shaderemu_gpu.c`, `make_linux_image.py`, `scr_shaderemu.c`, `gl.c` and the
  `0x600000` texel bound in the shader's `gpu_writeback`.
- An ioctl number written as a constant must carry the argument's size (`_IOW` puts it in
  bits 16-29): a wrong one returns ENOTTY and the program carries on drawing nothing.
- To see what is on the guest's screen without opening a picture, print a region of the RAM
  framebuffer as characters, one per colour; that is how the terminal's problems were found.
- `linux\prebuilt` holds the kernel and the image's programs for people without a compiler;
  `make_linux_image.py` uses it only when `build\images\linux` has no `Image` or `root`.
  After a rebuild worth keeping, run `python tools\make_linux_image.py --save-prebuilt` and
  commit the result (about 7 MB each time, so not after every experiment).
- An `--until` text must not appear in the command typed with `--input`, or the echo matches
  it: write the marker as `echo DONE-''MARK` and wait for `DONE-MARK`.
- The harness window gets the developer's real pointer too. A test that posts pointer
  messages to it may see extra events; aim posted points with the window's client size.
- `--d3d11` with other arguments runs upstream's shader, which has no GPU device: pass
  `--rvc experiments\rvc_opt` to test the GPU on D3D11.
- Writing the GPU's picture back to RAM happens in the Commit pass, compiled with
  `GPU_DEVICE` whenever `gpu.shader` is loaded; `--no-gpu` builds the commit without it.
- The runtime's `memcpy` matters: a byte-at-a-time copy cost glxgears a third of its frame.
- On upstream's kernel Linux reaches GPU memory through an MTD device made at run time by writing
  `gpu,0x87000000,0xb00000` to `/sys/module/phram/parameters/phram`; the kernel has no /dev/mem.
- `programs\linux\build.bat` downloads glxgears.c and compiler-rt's builtins into
  `programs\build\fetch`; they are not kept in the repository.
- To check something inside Linux, resume the shell snapshot with `--input "commands\n"` and
  `--until`: about 10 s. Do not cold-boot for that, and avoid `$` in the commands (the
  PowerShell and guest shell quoting together is easy to get wrong).
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
  --frames 800 --bench 30` (`d38498f4259b6757` for the current shader). Pass the harness
  `--no-bands` for this: on D3D12 its commit skips the 4 MiB bands of RAM nothing wrote to and
  keeps their list in state texel (41,0), which changes the hash and nothing else.
- `--stats-after S` prints instructions/s, frames/s and the GPU time of each pass, measured
  from S seconds in. With `--ticks 2` it shows the fixed cost of a frame: tick 0.015 ms,
  commit and device 0.01 each.
- A static array in the tick shader is zeroed at the start of every pixel's run (the write
  cache's 4,096 words cost 0.035 ms a frame). A local array passed down as `inout` is not, and
  DXC makes no copies of it. Under DXC the harness defines `L1_LOCAL`, which does that for the
  write cache; `src/l1_local.h` lists the functions that take it. A new function that reaches
  `l1_cache` must be added there the same way, and an entry may only be read when its
  occupancy bit is set. FXC builds keep the static array. The second-level TLB's two arrays
  (`tlb2_tag`, `tlb2_pg`) go the same way, with `tlb2_occ` as their occupancy bitmap.
- The D3D11 compiler is fxc2 (`docs/fxc2.md`, `tools\fxc2`): the build puts its
  `d3dcompiler_47.dll` beside the harness, the tick compiles in about 6 s (FXC: 3 to 9
  minutes), and the shader switches on `L1_LOCAL` and the one-instruction MULH when `__FXC2__`
  is defined. The banner says which compiler was loaded, and the cache keeps their blobs apart.
  Delete `bin\d3dcompiler_47.dll` to compile with Microsoft's FXC again.
- In D3D11 bytecode a function's return value is a flag that is written, copied and tested.
  In the fast loop, leave with `break` from the place that finds a thing out instead of
  returning a bool to test (`fast_run` in `cpu.h`): that was 18% of the tick pass.
- `RVC12_DXBC=1` with `--dxc` runs the D3D12 backend on bytecode from `d3dcompiler_47.dll`
  (fxc2) instead of DXC's. Its timings repeat within 0.002 ms where D3D11's wander by 0.015,
  so compare compiler changes there first. The same bytecode is about 10% slower on D3D11.
- Running the next instruction in the same trip of the fast loop when it matches the one
  just run was tried for `lw` after `lw` and `sw` after `sw` off one base register on one
  page (16% of the kernel's instructions start such a pair; the window already holds the
  word, and the second shares the translation). Same state hash, and the tick pass was 1%
  slower at 2,048 instructions a frame, level at 16,384: the test is a branch on every load
  and store, and what a pair saves (fetch, decode, the loop) is the cheap part of a memory
  instruction. `tools\pc_ngrams.py` prints which sequences a guest runs, `tools\pc_hot.py`
  how few places its instructions are in (sample with `--ticks 29 --fixed-dt 0.00005664`).
- The same was tried again on the cache of texels, where it should cost least: the next
  `lw` served from the RAM texel the first one read, the next `sw` written straight into the
  cache entry the first one used, up to three of them, no lookup at all. It runs for 4.3% of
  the instructions of a Linux boot (2.3% stores, 2.0% loads) and the tick pass is 2% slower
  at 2,048 instructions a frame, 0.6% at 16,384; as a loop instead of nested tests, 3.6%
  slower. The test runs on every load and store (28% of instructions) and costs about what
  the pairs it finds save. A sequence has to be known without looking for it at run time.
- The number of state texels (the pixels that run the whole tick) does not set the tick's
  speed: 1,093 and 901 of them (`TLB2_N=128`) take the same time.
- Both backends commit in bands now; a state hash taken without them needs `--no-bands`.
- D3D11 starts a frame's GPU work at `Flush` or at the readback's `Map`, whichever comes
  first. The harness flushes at the end of `frame()`; without it the GPU idled through the
  host's own work, 7% at 2,048 instructions a frame. Anything that draws and then waits
  should do the same.
- The D3D11 build's timer value comes from doubles in the shader, D3D12's from the host: at
  65,536 instructions a frame their state hashes differ unless D3D11 runs with `--no-doubles`.
- An array of scalars declared one component wide in the bytecode (as FXC does) ran 3.5%
  slower than four wide; fxc2 keeps four.
- Upstream rvc's own shader compiles with the fxc2 in `tools\fxc2` (older builds had a syntax
  error on its `4294967296.0l`), but runs at 60% of FXC's speed there; why is not known.
- fxc2 before 8 October 2026 computed `float - uint` wrongly (it negated the unsigned value
  first: 25.18 - 25u came out as 4294967296). The harness's state hashes do not see such a
  thing in a shader that only draws; `Terminal.shader` had one, and keeps its explicit cast.
- Unity compiles with fxc2 through an overlay copy of the editor (FXC2's
  `scripts\unity-overlay.ps1`, with `-Dll bin\unity\D3DCompiler_47.dll` and
  `fxc2_d3dcompiler.dll` copied beside it). Unity preprocesses shaders itself, so
  `MachineTick.shader` defines `__FXC2__` by hand: a stock editor needs that line out. The
  stock compiler's cache is `Library\ShaderCache.fxc`.
- The Unity editor must compile with fxc2 (here it is in the install itself: the stub
  `D3DCompiler_47.dll` and `fxc2_d3dcompiler.dll` in the editor's `Data\Tools`). An editor with
  Microsoft's compiler fails on `mulhi` and Unity keeps that failure in
  `Library\ShaderCache\shader\MachineTick*` (files of about 1 KB), after which no editor
  compiles the tick and the machine runs no instructions: delete those files and re-import
  `MachineTick.shader` (5 s). A shader made in editor code that calls `mulhi` tells which
  compiler is there.
- In Unity the commit and the control pass draw the 64 state rows and the bands of RAM that
  changed, not the whole texture: a geometry shader in `MachineBlit.cginc` puts those rectangles
  in place of Blit's two triangles (no tessellation; if one is ever added, its factor stays at 8
  or under). Each pass also draws what the pass before it changed, because the texture it draws
  into lacks that: the commit takes the control pass's bands 28 and 29, the control pass the
  commit's, from state texel (41,0).b. A round went from 2.22 ms to 2.03 ms, the harness's.
- To check that no band is missed, pause the machine (`paused`) and compare `current` with
  `other` in a shader made in editor code (`any(a != b)` into an ARGB32 target, read with
  `ReadPixels`; a state texture cannot be read back itself): no texel may differ outside
  bands 28 and 29. An error logged in play mode pauses the editor ("Error Pause"): the
  machine then runs nothing until `EditorApplication.isPaused` is false again.
- In Unity a frame's rounds are read back together: each round's Blit writes its 448 words
  into its own row of the 448 x 32 readback texture (`_Row`), and one request a frame fetches
  them; only the console's output needs every round, the rest is the last row's. It took 0.03
  ms a round off the main thread and nothing off the GPU.
- The harness can do the like across frames (`--readback-batch N`: N frames' rows in one
  staging texture, one Map): the same state hash, typed input no slower, and a gain inside the
  noise (4% of a 2-instruction frame, 1 to 3% at 8,192 with N = 16), for output N frames
  later. It is 1 unless asked for.
- The Unity machine has the harness's 64 cores and the guest's geometry
  (`docs/multicore.md`, "In Unity"): `CORES` in `MachineBlit.cginc`, a tick texture 2048 x 16
  (core 0's 64 x 16 and the workers' band), the state copied into it before each tick, the
  commit of every core. Where its tick and commit deal with cores they are `main.shader`'s
  lines: a change there is made in both. A change to the band's size moves `TickState`'s (the
  builder, and the asset in `Generated`) with it. To test: `mctest 3 120000` and `mctest 15
  60000 shape 6,6,6,5,5,4,4,4` at the console must end in `PASS`, and `nxray` must print one
  sum with `RAY_WORKERS=0` and without; `mctest 3 400000 bench 2` keeps four busy for a sweep.
- A field added to an Udon behaviour that VRChat copies for every player (`EmuShare`) is
  empty in objects made from a template older than the field: an array there has no
  elements. Whoever reads it sizes it first (`Roster` in `EmuShareHub`). Never end `mctest` with Ctrl+C: its workers stay running
  and the next one finds none, until the machine is switched off and on.
- What each core ran is among the control words (0x87000380, `MC_STATS`): `nxmon`, the
  harness's `STATS workers` line and the Unity panel all read it there. The Unity readback is
  768 words a row: state row 0, the first 64 control texels and 64 texels of the network's row,
  where cores 16 and up are counted (`MC_STATS_MORE`, 0x876b8280).
- To time rounds in play mode, take the frame cap off (`Application.targetFrameRate = -1`), set
  `rounds` to 8, 16 and 32 with a busy guest (`while :; do :; done` typed at the console
  keyboard) and read frames and `totalInstructions` from an `EditorApplication.update` callback
  that writes a file: the slope is a round, and what is left is Unity's own frame.
- Skipping pixels in the commit with `discard` saves little: rasterising the whole texture is
  0.06 ms even when every pixel is discarded. Its vertex shader (`commit_vert`) reads the band
  list from the state instead and draws one quad per band; the harness binds the state texture
  to that stage and draws 33 quads.
- A guest ends its frame from user mode with `pause` (`0x0100000f`); `wfi` traps there.
- The GPU draws a list in up to eight passes (`docs/gpu.md`); `programs\blend` is their test
  card and must match `tools\gpu_reference.py` on both backends. `pass` is a reserved word
  under FXC: a shader that names a variable so compiles with DXC and fails on D3D11.
- Re-make `rvc_shell.snap` after every `make_linux_image.py`, not only when a program changes:
  the snapshot's kernel has the old image's file layout cached and reads garbage from a new
  one (a missing WAD lump, a crash in a program that worked).
- Doom (`docs/doom.md`): `linux\nanox\doom.sh`. To compare two ways of drawing it, hold both
  at one game tic with `DOOM_STOP_TIC=N` and compare the windows; a check against the buffer
  the GPU reads only proves the copy.
- Text with a backslash in it (`\n` in a C string, a Windows path) does not survive a bash
  heredoc into Python here: write it with the edit tools.
- Our Linux image has no firmware in it (`docs/boot.md`): the harness defines `SBI_HLE` for it
  and for snapshots, and the machine answers the kernel's SBI calls. `rvc_trace12` needs
  `--define SBI_HLE --define L1_LOCAL` to match the harness. Only upstream's images
  (`--payload rvc\_Nix\rvc\data-net`, `micropython`, `rust`) still boot their own OpenSBI.
  The state hashes above were taken with firmware and have not been re-taken.
- To check a shader change keeps emulation the same, cold-boot twice with `--fixed-dt 0.004
  --frames 2600 --bench 0 --save-state`, before and after, and compare with a state diff: RAM
  and registers must be equal (leftover write-cache texels may differ).
- In the tick shader a `switch` costs about 4 ns per case, an array read 10 to 15 ns, a branch
  4 ns and arithmetic 0.7 ns (`docs/boot.md`). Do not add a switch to the fast path.
- Resuming a snapshot of our image needs `--image linux-net` as well as `--load-state`:
  without it the root filesystem is upstream's and none of our programs are found.
- `tools\boot_profile.py` with `rvc_harness --frame-log FILE`: boot time, the median rate and
  why frames ended early.
- The tick's state is one 64 x 16 rectangle at the top of the state block and the tick pass
  draws only that (`STATE_ROWS`, the layout at the top of `src/types.h`; `docs/multicore.md`
  has why: 5 to 9% for one core, and a second core that does not cost a second pass). The
  write cache is at texel 44, the TLBs after it, the float registers after those, and the CSR
  area from texel 1024. A snapshot from before this layout does not load (re-make
  `rvc_shell.snap`), and upstream's shader and `experiments\rvc_compute` keep the old one:
  the harness draws 64 rows for them (`BackendOptions::tickRows`).
- A shader edit costs one FXC compile (now 140 s to 290 s for the tick). Compile variants in
  parallel, then benchmark them one at a time. Never compile the tick with FXC's "avoid flow
  control" flag (`--fxc-flags 9200`): the compiler grew past 34 GB of memory.
- The second-level TLB (256 entries a mode) and the megapage TLB are kept between passes in
  state texels after the write cache's (`TLB_STATE_AT`). The
  second level's arrays still start each pass empty: an entry they lack is read from its texel
  on the miss (loading all of them at the start of every pass cost small programs 2%).
- Doom's frame rate falls faster than the emulator's speed: about 0.9M instructions a second go
  to its 35 game tics whatever the frame rate, so a 1.3x slower machine draws 1.5x fewer frames.
- To try a change to the fast loop under FXC in seconds, compile the tick without its general
  path (a copy of the shader folder, `--rvc`, the loop's entry flags set by hand) and run it
  from a snapshot of a loop that never leaves the fast path. The driver, not FXC, decides how
  that bytecode runs, and decides differently for the small shader: confirm in the full one.
- The same FXC bytecode loop ran at half the speed in a small shader: the driver runs branches
  that hide texture reads as "both sides, then select". A one-trip loop around such a branch
  stops it there, and made the full shader slower.
- The guest starts its desktop at boot when the host flags say so (`docs/nanox.md`): the
  no-argument terminal mode does, a run with arguments does not unless it passes `--desktop`.
  A test must not rely on either default: pass `--desktop` or start `nano-X -p` itself.
- The image's shell and tools are our own static busybox (`wsl -- bash
  /mnt/c/Development/ShaderX86/linux/userland/busybox.sh`, 13 s after the first time), which
  the image builder puts in place of upstream's. Upstream's is linked against glibc at run
  time: each command it started took 175 ms, against 75 ms now. `HOME` is `/root`, because this
  shell writes the home directory as `~` and everything here waits for the prompt `/ # `.
- A file in `build\images\linux\root` with the name of one upstream's image has replaces it
  (the old one stays in the ROM under a name starting with a dot).
- Two tests that start programs a second apart can see them stacked in either order: the
  second to start may map its window first. Start them three or more seconds apart.
- The tick rate is not worth changing: 20 Hz against 100 Hz gained 2 to 3%, and a timer for
  input polling of its own cost nothing measurable either way.
- Starting a program costs about 60 ms even when it does nothing: about 100 parallel copies,
  75 fills and 13 instruction-cache flushes, each of which ends the frame.
- 98% of Doom's instructions take the fast step (`--profile` on D3D11). Each kind of
  instruction is 1.2 to 1.45 times slower under FXC than under DXC, none stands out.

## Timings to expect

- Harness build: about 5 s.
- First FXC compile of rvc's `CPUTick` pass: about 90 s; cached afterwards in `build\shadercache`.
- Speed: about 520k instructions/s upstream, 2,240k with `experiments\rvc_opt` on FXC
  (under DXC on D3D12: about 2.8M at 2,048 ticks per draw, 3.1M at 65,536; 4.0M while glxgears
  runs under Nano-X). It barely depends on
  `--ticks`: frame time is the serial CPU loop, about 1.8 us per instruction upstream.

## The VRChat world (Unity)

The machine also runs in a Unity 2022.3 / VRChat Worlds SDK project, `C:/Development/VRChat/ShaderEmu`
(not a git repository). Its sources live here in `unity/ShaderEmu` and are copied to that
project's `Assets/ShaderEmu`; after changing anything there, copy it back here.

- Menu `ShaderEmu`: "Sync shader sources" copies `experiments/rvc_opt` in (the `src/*.h` files
  become `.cginc`: Unity never recompiled a changed `.h`), "Import boot images" turns
  `build/images/linux/*.bin` into textures, "Create Udon program assets", then "Build world"
  makes every asset and the scene `Assets/ShaderEmu/ShaderEmuWorld.unity` from nothing.
- `EmuMachine.cs` runs the machine with `VRCGraphics.Blit`, several rounds per Unity frame:
  CPUTick into a 64x64 texture, Commit (which reads the CPU's state from it), a readback,
  `Camera.Render()` of the GPU's mesh, GPUControl. One round a frame through a Custom Render
  Texture was capped by the frame rate (0.7M instructions/s, glxgears 80 frames/s); in rounds
  it is 2.7M to 2.9M and glxgears 250 to 340.
- One shader permutation only: full machine with `SBI_HLE` (our Linux image). The CPUTick pass
  takes about four minutes to compile in Unity, with the editor frozen (`editor_sync_compilation`).
- `_HostFlags` on the machine's material is 1 there: the guest starts its desktop at boot.
- Unity's shader preprocessor splits macro arguments at commas inside braces and does not
  process `#ifdef` inside a macro argument. FXC and DXC accept both; write neither in `DEF()`.
- A material's `Int` is a float: 32-bit values go in as two 16-bit halves (`_UartInLo/Hi`,
  `_HostMsLo/Hi`, the RTC words) and Machine.shader puts them together.
- The GPU there is a mesh of 65,536 points, one per triangle, and a geometry shader
  (`GpuDrawPass.cginc`): it finds the command once per triangle (`gpu_command` in `gpu.h`) and
  emits nothing for a triangle no command claims whole. Its target is 1280x720 (`GPU_TARGET_W`,
  `GPU_TARGET_H`; the harness keeps 2048x2048), so the guest's picture is 720p at most.
- That shader writes clip positions itself: flip z for Unity's reversed depth, and emit nothing
  unless the camera is the GPU's (orthographic, a target of that size), or every other camera
  draws the guest's picture over the room.
- CPUTick is a shader file of its own (`MachineTick.shader`, its own material): Unity compiles
  a whole shader again when any file it includes changes, and the tick takes four minutes.
  "Sync shader sources" writes only the files that changed, for the same reason.
- To test in the editor: enter play mode (ClientSim), then read Udon's variables with
  `UdonBehaviour.GetProgramVariable` (the C# proxy's fields are not the running values) and type
  by writing into `EmuKeyboard`'s `queue` and `tail`.
- Input in the world is `EmuPointer`'s own beams (`docs/input.md`), not VRChat's laser: that
  is one hand at a time and never says where it points. A collider in front of a canvas
  also blocks it, which sending UI events by hand in the editor does not show.
- To test the beams in play mode: `TeleportTo` in front of the target (the desktop beam is the
  head's), then `RunInputEvent("_inputUse", new UdonInputEventArgs(true, HandType.RIGHT))` on
  the "Beams" behaviour. ClientSim may scale "DestkopTrackingData"; set it to 1 first.
- The guest has a C compiler (`docs/cc.md`): `wsl -- bash /mnt/c/Development/ShaderX86/linux/tcc/build.sh`
  (7 s) after Nano-X's build, then the image and snapshot. To check it, resume the snapshot and
  run `cc` on a file, or `example cube` under `nano-X -p`. A library TinyCC must find after the
  C library goes into its `libtcc1.a`; the toolchain's default `libgcc.a` is hard-float and
  gives a program built with it an illegal instruction.
- Text of a built-in font is one GPU command a string (`CMD_TEXT`, `gd_drawstring`): 374 to 73
  instructions a character. A drawing request still costs 1,000 to 2,000 whatever it draws.
  `nxbench` prints instructions as well as time for each of its phases.
- A page for the guest's browser comes from the host (`docs/fetch.md`): control words at
  0x87000100 and 256 KB at 0x876c0000, written by the control pass. The harness prints
  `the guest asks for` and `answered with` on stderr. `linux/apps/web/sites.txt` is both the
  browser's home page and the only addresses the VRChat world can load: rebuild both after editing.
- Any other address, and each picture, a visitor of the world hands over by copy and paste
  (four slots, a button by the display: `docs/fetch.md`). In play mode, paste from editor code:
  `typedUrls[i].SetUrl(new VRCUrl(wantedLinks[i].text))`. ClientSim loads pictures too.
- The browser's style sheets are `linux/apps/css.h` (`docs/fetch.md`): compare a change with
  the build before it on the local pages (the old binary beside the new one in the image), and
  expect only what the change is for to move. A page's sheets are request kind 2.
- A driver hook that draws for the engine (`gd_drawpicture`) gets one rectangle of the clip
  region a call and must stay inside it: without that a picture scrolled up painted over the
  title bar. The caption is 22 pixels high and its buttons 18 (`nanowm.h`), for VR's beams.
- Doom's speed is counted in instructions, not seconds (`docs/doom.md`): its `doomstat:` lines
  give a tic and a frame, and frames/s = (instructions/s - 35 x a tic) / a frame. Seconds lie
  whenever the developer's GPU is busy (VRChat open: the harness ran at 0.6M to 2M/s instead
  of 3.3M): run `nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader` before
  believing a frame rate, and measure again later if it says more than about 20%.
- A change to Doom's game code must leave a demo playing the same: `DOOM_HASH_TICS=500 doom
  -timedemo demo1 -nodraw` prints sums of the game's state, `DOOM_PORT=sight,look,think`
  brings the port's code back to compare with, and `DOOM_PORT=view` the older way of drawing
  (hold a frame with `DOOM_STOP_TIC=N` and compare the windows).
- `rdcycle` costs a user program one instruction and counts the machine's instructions: put
  two around anything to know what it costs. `gettimeofday` is 416; the clock word at
  0x87000034 (milliseconds, a machine frame old at most) is a load.
- The write cache holds RAM texels (four words), not words: 384 of them in buckets of three,
  two tables of 64 buckets with different hashes, a texel going to the second table only
  when its bucket in the first is full (the same block in five headers; `l1_find` and
  `mem_set_ram` in `mem.h`, `l1_state_find` in `types.h` for the commit). An entry is made
  from the texel as RAM has it, so a store that changes nothing costs none. Frames a full
  cache ended, desktop with glxgears, 12,000 frames: 8,237 with upstream's word cache, 8,065
  with its set from xor-ed address bits, 1,324 with this. Tag 0 is a free entry and a
  texel's tag its number plus one: the word cache's commit took a free entry for address 0
  and wrote 0 over the first word of RAM.
- `L1_TABLE_BITS` and `L1_WAYS` are the cache's size: 6 and 3 by default (384 texels in an
  array of 512; 4,101 frames of that desktop run end on a full cache), 6 and 4 for 512 texels
  (1,324 such frames), 7 and 3 for 768 (39). The smallest is the default because it is 5%
  more instructions a second on D3D11 in spite of its stalls, for two reasons that were
  found by adding pixels and arrays that do nothing: four entries a bucket cost every load
  and store about 3.5% whatever the size, and every state texel is a pixel that runs the
  whole tick, of which more than about 1,000 cost every instruction (965: 0.486 ms for 2,048
  instructions; 1,029: 0.512; 1,093: 0.54). The declared size of an array costs nothing, nor
  does one more array. Count the pixels before adding state texels: 44 + the cache's + the
  TLBs' 409 + the float registers' 8 is 973 now.
- Tried on the fast path after that and not kept (tick pass for 2,048 instructions of the
  Linux bench, fxc2's bytecode on D3D12, 0.451 ms as it is): the cache's occupancy bits in
  four registers instead of an array of four, 0.454; no read-ahead of the next instruction
  texel, 0.471 (the read-ahead is worth 4.6%: a texture read is slow to arrive and cheap to
  have asked for early); the second source register read only by instructions that have one,
  0.460; the 32 registers as eight vectors chosen by conditional moves instead of an array,
  0.645; the first-level TLBs in two arrays instead of 24 registers, 0.457; `csrrci`,
  `csrrsi`, `csrrs` and `csrrc` on sstatus done in the fast loop when only SIE changes (they
  are 60% of the general ticks of a desktop), 0.460 to 0.464 in two designs and 3 to 5%
  slower on a shell workload, though they did leave the general path. A value the loop
  carries and changes in some rare branch costs about 0.2% of every instruction (8, 16 and
  32 of them: 0.460, 0.466, 0.485).
- Hot traces of the kernel translated ahead of time into the tick (`tools/rv_trace.py`: a
  straight-line trace with side exits, registers in locals) emulate correctly, same frames
  and instructions, and are a loss: one 74-instruction trace that is 8% of a shell workload
  made the run 11 to 17% slower, in the fast loop or after it. The trace's code being there
  costs every instruction (0.545 ms against 0.451 for 2,048 on a workload that never runs
  it; four tests of pc at the top of the loop alone, 0.485), while 14,000 lines of
  unrelated dead code cost nothing and the compiler's register allocation changes nothing.
- One test instead of two at the end of a fast instruction ("not ours" folded into the
  loop's end test, the writes made conditional): 0.460 against 0.453.
- A general tick costs about 560 ns where a fast one costs 170, not the microseconds older
  notes say: (0.158 - 0.015) ms for 256 of them with the fast run switched off. They are
  1.4% of a Linux boot's instructions, 0.65% of a desktop's and 0.2% of a shell loop's.
- The D3D11 harness's wall time a frame is 0.25 ms more than its three passes' GPU time at
  16,384 instructions a frame (0.06 ms at 2,048). It is not the readback's `Map` waiting:
  asking without waiting in a loop changes nothing.
- A feature for few instructions must not sit in the path of all of them. The F extension
  did (operand registers chosen between two sets for every instruction, flw and fsw inside
  the integer load and store, a float destination in the common register write) and every
  instruction was 8% slower for it. All of it is one branch of the fast path's chain now,
  after the jumps, and costs 3%. `fpcheck` (`--image fpcheck`, bare metal, the full machine)
  prints a sum over 20,000 float operations, loads and stores: it must not change
  (`cf9c7697`), and `fptest` in the guest checks the values themselves.
- To study a change to it, record a run with `--l1-log FILE` (D3D12) and `--ticks 32
  --fixed-dt 0.0000625`, and run `python tools\l1_sim.py FILE` (texels) or `tools\l1_study.py`
  (words); then count "write cache full" frames with `--frame-log` and
  `tools\boot_profile.py`. Stalls move, so state hashes change: check the guest's own sums
  (Tiberian Dawn's, file checksums in Linux) and the final RAM of a bare-metal program, and
  re-make the snapshots.
- A 64-bit division is a library call of some 400 instructions (`__divdi3`); a 32-bit one is
  an instruction. In the fast path of anything, make the operands fit.
- musl's `qsort` with a comparison function cost 1,500 instructions an element on 2,500
  elements. Count into buckets when the key allows.
- The toolchain's default is position-independent code, which the guest's TinyCC needs of
  every library it links (`R_RISCV_HI20` and friends are "Unknown relocation type for got"
  there). A program that wants speed asks for `-fno-pie` itself, as Doom does.
- A C string with `\n` written through a bash heredoc into Python arrives with a real line
  break in it. Write the editing script with the Write tool and run that file.
- The wheel is key-ring events 0x3fe (up) and 0x3ff (down): `WM_MOUSEWHEEL` in the harness
  window, `EmuMachine.Wheel` in Unity. A program sees a button-down with a scroll bit set.
- The image builder adds files to folders the ROM already has and cannot make a folder: new
  data goes into `/usr/share` under a prefix (`web-*.html`).
- Asking the file system about every name in a large folder takes seconds (each lookup scans
  the ROM's directory and each read ends a frame): `nxfiles` asks only for the tiles in view.
- A page's bold in `nxweb` is the text drawn twice a pixel apart; headings are that underlined.
- The wall shows `DisplayPicture`: `EmuMachine` draws `Display.shader` into it a pixel a texel
  each frame, with mipmaps, and `DisplayShow.shader` samples it shifted towards texel centres
  by `fwidth` (sharp close up, filtered far away). The hand rays are untested in a headset.
- The room's light is baked: run "ShaderEmu/Bake lighting" after every "Build world" and wait
  for "lighting baked, lamps off, scene saved" in the console before uploading. It runs on the
  CPU lightmapper (the GPU one ran out of memory and stalled beside a headset). The lamps are
  enabled only for the bake: a scene without baked data draws "baked" lights in real time,
  with shadows, and a build made then is very slow in VR.
- The machine starts off; in play mode press the panel's Power button
  (`Button.onClick.Invoke()` from editor code).
- The volume display (`docs/volume.md`): a third screen, on the left wall by the console, that draws a 3D
  program's last frame behind itself from the visitor's eyes. It reads the frame's list from
  0x87000300, which both OpenGL libraries publish; a program that builds frames in place must
  alternate between two places. To test it in play mode, type `glxgears &` or `doom &` at the
  console (Doom's own demo starts after its title) and render a camera aimed at "Volume
  display", with "Volume content" on and off. From the side glxgears shows nothing until the
  slider (`_Plane`) is near 0.84: with the screen at its near plane the gears are 20 m behind.
- Outside the window is real geometry (`City()` in `ShaderEmuDecor.cs`: two meshes, an unlit
  shader, haze in vertex alpha) and a panoramic skybox made by `NightSky()`. The wall is four
  boxes round the opening, with a collider in it.
- Labels are TextMeshPro and UI images use VRChat's super-sampled UI material, which is what
  the SDK's build panel asks for. Two same-facing faces in one plane flicker: after adding
  props, compare renderer bounds pairwise for coincident faces that overlap.
- The board on the left wall (`About()` in `ShaderEmuDecor.cs`) tells visitors what the machine
  is, what they can do and whose work it uses. When the image gains a program or the project a
  dependency, add it there and to the README's "Built on"; then "Build world" and bake.
- Players see each other's console and display (`docs/share.md`): `EmuShare` is a player
  object, `EmuShareHub` ("Share" in the scene) packs and unpacks. To test with one player, set
  the hub's `selfTest` in play mode, or `ClientSimMain.SpawnRemotePlayer`, write the new
  player's `EmuShare` variables and send it `_onDeserialization`. ClientSim sends a packet a
  second where the hub means to send six or seven: watch a change to the stream at both rates (an editor hook
  that clears the hub's own `EmuShare`'s `sending` each frame gives VRChat's). With two real clients the hub's `netTest` makes them test
  themselves and log `[ShareTest]` lines to VRChat's output logs. "ShaderEmu/Add sharing to the open scene" puts it into a
  scene without building the world again.
- A rewrite of the display stream (October 2026: 8x8 cells, a DCT for what holds still, a
  palette for what moves, whole frames) was dropped: watched in the world, the stream as
  committed looked better. dB against a model of it did not say so. Judge the stream by
  watching a game through it.
- The stream's fine level (`docs/share.md`) is checked against a numpy model of its bytes:
  dump the hub's `storeBytes`, `fineBefore` and `remotePicture` in play mode once `work` is
  false; every quarter must be the model's bytes (but for a few rounding ties) and the
  decoder within a level of the model's picture.
- A shader that packs bytes for Udon uses `SV_Position` and `Load` on both sides (target rows
  are then the readback's rows and `LoadRawTextureData`'s); only where it meets
  `DisplayPicture` does it use the `uv`, as `Display.shader` does. `sample` is a reserved word.
- After `make_linux_image.py`, run "Import boot images" in Unity: the world boots the copy in
  `Assets/ShaderEmu/Images`, not the files in `build`. To check it, decode the four PNGs of a
  part (texel 0 is the top row) and compare with the `.bin` byte for byte.
- Tiberian Dawn (`docs/tdawn.md`): `wsl -- bash /mnt/c/Development/ShaderX86/linux/tdawn/build.sh`
  (9 s) after Nano-X's build, then the image and snapshot. To measure it, resume the snapshot
  with `nano-X -p &` and `TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_SCEN=10 TDAWN_FRAMES=300 tdawn FROMINSTALL`
  and wait for `tdawn: done`: 30 s to start, then `tdstat:` lines with a frame's instructions.
- A change to the game must leave its `state` sum the same (`TDAWN_STOP_FRAME=N` with
  `TDAWN_NO_DELAY=1` prints it): `f6e24c6c`, `c63742a4` and `f7650705` for missions 1, 3 and 10
  at frame 300 with seed 7, the same as an x86-64 build's.
- Its changes to Vanilla Conquer are `linux/tdawn/vanilla-conquer.patch`: edit a clean clone at
  the commit `build.sh` names and save `git diff`. The build puts the tree back each time.
- C++ for the guest is `rv32-c++` (made by that build): libstdc++'s headers over musl, with
  `-fno-exceptions -fno-rtti` and `cxxrt.cpp` in place of the library. A program that needs
  more of the library than the string class fails to link.
- Old C++ that sets fields in `operator new` needs `-fno-lifetime-dse`: without it the
  compiler drops those stores at some optimisation levels, and nothing reports it.
- A program that looks for its files in any spelling lists whole folders: listing `/` or
  `/usr/bin` reads the ROM each time. Give such a program a small folder of its own.
- Nano-X's screen is 800x600 unless `NANOX_SIZE` says otherwise (`linux/nanox/config`; it was
  1280x720, and 640x480 before). The world's display and console are 4:3 for it. A change to that file needs Nano-X's build, the image, a new snapshot and Unity's
  "Import boot images". To see the guest's screen as a picture, compose the display's layers
  from a snapshot: the table at the address in `0x8700000c` is a count, three unused words,
  then eight words a layer (x, y, width, height, address).
- The sound card (`docs/sound.md`) is `experiments\rvc_opt\sound.shader` and `sound_control` in the
  GPU device's control pass. Check it by measurement, never by ear: run with `--fixed-dt 0.004
  --sound-capture F.wav --save-state F.snap` and `python tools\sound_reference.py F.wav F.snap`
  (`--rom build\images\linux\rootfs.bin` for Linux, `--stream-test` for `--image sound`): every
  sample and every device word must match, on both backends.
- Samples the model reads from a snapshot must not have changed during the run: two programs
  one after the other reuse sound memory, so start them together (`a & b`).
- A scripted run is silent unless it passes `--sound`; the card still mixes for the guest.
  `--volume P` (10 by default), and Ctrl+F9 / F11 / F12 in the window.
- The card's control row is 256 texels now (RAM 0x87000000 to 0x87000fff): the GPU's control
  zone and the harness's readback both cover it; Unity's readback is still the first 48.
- A voice's samples are at a physical address: sound memory (`/dev/sound`'s ioctl) or a file
  in the ROM (`snd_rom` in `linux\userland\shaderemu_sound.h`), never a program's own memory.
- After a change to `linux\tdawn\soundio_shaderemu.cpp` or the pack's format, the game's state
  sums must still be the ones above; `tools\make_tdawn_sound.py --check` tests the pack itself.
- In Unity the audio thread runs `EmuSoundOut._onAudioFilterRead` (`docs/sound.md`). Play mode
  that is paused, or paused by an error, leaves the audio clock running: the card's clock
  jumps when it resumes. "ShaderEmu/Add sound to the open scene" installs it without a bake.
- The world's speed has two modes (the panel's button by "Speed", `EmuMachine.SpeedMode`): fixed,
  so many instructions a frame, and steady, so many a second. Steady counts a round for what
  rounds really ran lately (no less than a quarter of 8,192: an idle guest gets no more), and
  lowers the most rounds a frame may have while frames come slower than `minFrameRate` (45).
  To check it, read `totalInstructions` twice in play mode, and `roundCap` with the slider at 7.
- The memory screen has three pictures (`EmuScope`, `Scope.shader`): RAM with a legend of whose
  each part is (the colours are `owner()` in `MemView.shader`, the names a canvas over the
  screen), the ROM, and the sound ring as two traces. The CPU's 64 x 64 state texels have a
  small screen of their own to its right, always on.
  "ShaderEmu/Add the memory screen's views and speed modes to the open scene" installs them
  without a bake. A change to the memory map moves `owner()` and the names in `ShaderEmuScope.cs`.
- The desktop's programs share a list, a slider and a file chooser (`linux/apps/ui.h`,
  `ui_files.h`: `ui_list_draw` / `ui_list_event`, `ui_choose_file`). A list's program selects
  mouse motion events if its scroll bar is to be dragged.
- Settings (`nxsettings`) changes the screen's size by writing `/tmp/nxsize` and running
  `nx restart` detached (`setsid`): the window system and every program on it go, Settings too.
  The host's flags word (0x8700003c) says the largest screen it shows, in 16s of pixels (bits
  8-15 width, 16-23 height): 1280 x 720 in the world, 2048 x 2048 in the harness. Settings lists
  only what fits and the window system falls back to its default for anything larger.
  To check it without a pointer: `nx & sleep 8; nxsettings size 800x600`, then the display's
  words in a snapshot; `NXSETTINGS_TAB=N nxsettings` opens on a tab, `nxsettings volume 30`
  must leave 76 at 0x87000224. Its settings last until power-off: nothing is written to the ROM.
- The sound card's master volume is set to full once, when the driver starts, not at every
  open: it is the system's volume, which Settings changes.
- Doom's window is the largest of 1x to 3x the display has room for (`doom_window_scale` in
  `doom_video.c`); a test that compares its pictures with older ones passes `-1`.
- Tiberian Dawn sizes scroll steps by time and cycles its palette by the clock (and leaves
  frames undrawn when late, with `TDAWN_SKIP=N`); none of it happens with `TDAWN_NO_DELAY=1`
  or `TDAWN_FRAMES`, which is what makes those runs and their pictures repeat. `TDAWN_SCROLL=96 TDAWN_CHECK_REDRAW=1` with a held frame must
  print `0 of 64000 pixels differ from a full redraw`.
- Tiberian Dawn's map is drawn by the GPU (`linux/tdawn/gl.cpp`, `docs/tdawn.md`); the game's
  own drawing is `TDAWN_RENDER=soft`, and what the native reference build does. To check a
  change, hold both at one frame and compare the pictures: only shadows and the shroud's
  edges may differ, by a few levels (`>32` levels: the pointer and nothing else).
- A texture coordinate of a compact vertex is a 1,024th of the texture: a rectangle of part of
  a texture is exact only where its edges are whole 1,024ths. Make atlases 1,024 wide, and
  give a texture a height of 256 or 512 even when fewer rows are used.
- GPU memory left to one program is 3.25 MB (`TEXTURES_AT` in `gles.c`). Tiberian Dawn uses
  2.9 MB of it; a build with MEGAMAPS has 16,384 cells, and only 4,096 get a kept quad.
- The full machine has float instructions (`docs/fpu.md`: the F extension, define `FPU`, which
  the harness sets for it and `MachineTick.shader` has by hand), and the toolchain's flags
  make every program and the C library use them (`-march=rv32imaf -mabi=ilp32
  -ffp-contract=off`; floats still travel in integer registers). After a change to those
  flags build everything from clean objects: the scripts only look at dates. The kernel keeps
  the registers per task (`linux/kernel/fpu_hook.py`); `fptest & fptest` checks that, and
  `fptest` alone every instruction against the runtime's integer routines. A state hash says
  nothing about a guest that uses floats (divide and square root are as exact as the card
  makes them).
- An instruction costs the machine the same whether it adds integers or floats: floats pay
  where they replace a library call, not in a program that was integers already. Doom's tic
  and frame did not move when it was rebuilt for them.
- Linux tests `sstatus`'s summary bit (SD, bit 31) before it looks at FS: a machine that
  reports FS dirty without it never has its float registers saved.
- fxc2 gave `(float)` of a signed integer back as the integer's bits, and `(uint)` of a float
  stopped at 2^31 (8 October 2026; `fp_exec` in `emu.h` words both another way). When a
  shader converts between floats and integers, test the conversion on D3D11 with values on
  both sides of zero and of 2^31.
- A card divides by multiplying with a reciprocal: a quotient of two floats near the largest
  came out zero. Where that can matter, move the exponents towards the middle first.
- Quake (`docs/quake.md`): `wsl -- bash /mnt/c/Development/ShaderX86/linux/quake/build.sh` (3 s)
  after Nano-X's build, then the image and snapshot. To measure it, resume the snapshot with
  `nano-X -p &` and `QUAKE_HOLD=120 quake +map e1m1` and wait for `quake: holding`: about a
  minute, with `quakestat:` lines that split a frame's instructions by what they were for.
  Lines typed at its terminal are console commands.
- Quake's server runs on a worker core when the machine has one (`docs/quake.md`, "The server
  on a worker core"): after a change to the server's side or to the workers, run
  `QUAKE_SUM=10 QUAKE_HOLD=130 quake +host_framerate 0.05 +map e1m5` with `--cores 2 --fixed-dt
  0.004` three times, with `QUAKE_SERVER=inline`, `late` and `wait`: the sums must be the
  same. Server code that a worker runs must not print, run commands, set cvars or use the C
  library's `rand` except through what `server_shaderemu.c` gives it.
- A sum over a game's state must leave out pointers and what depends on them: an offset from
  the heap to a global moved with the environment's size (one more variable set), and two
  runs that did the same looked different from their first frame.
- `linux/prebuilt` is the full image and is kept up to date: every program the build scripts
  make and the games' data, so that the machine runs from a clone with no compiler. After a
  change to the kernel or to a program of the image that is worth committing, run
  `python tools\make_linux_image.py --save-prebuilt` and commit it with the change. On a
  machine that has not built everything, build the rest first (`linux/userland/busybox.sh`,
  `linux/apps/build.sh`, `linux/nanox/doom.sh`, `linux/tcc/build.sh`,
  `linux/classicube/build.sh`, `linux/imgui/build.sh`, `tools/make_wallpaper.py`): the save
  replaces the folder with what `build/images/linux/root` has, and what is missing there
  goes from the repository. A new program or data file needs a line in its `README.md`.
- Before giving a worker a piece of a frame, read "Where workers pay, and where they do not" in
  `docs/multicore.md`: a job waited for costs about two passes, so only work that needs no
  answer within the frame gains. The window system has none to give (it waits for the GPU).
- Builds carry one date (`SOURCE_DATE_EPOCH`, `KBUILD_BUILD_TIMESTAMP`): do not put the time of
  a build into a program, or `linux/prebuilt` grows by it at every save.
- A change to how Quake's server and client take turns is tested with play, not only with the
  sums of a player standing still: `QUAKE_TRACK` and `QUAKE_TEST_EXIT` (`docs/quake.md`). The
  first version passed every sum and crashed at the first level's exit.
- A harness run's `--uart-log` is added to, not replaced: give a run a new name, or lines of
  the run before are read as this one's.
- The two Command & Conquer games give a mission's scene to a worker core (`linux/tdawn/host.c`,
  `docs/tdawn.md`): anything added to `host.c` that calls the GPU's library must call
  `scene_finish()` first. `TDAWN_SCENE=inline` is the way it was, for comparing: the state
  sums and a held frame's picture (`--gpu-capture`; the pointer differs) must be the same.
- The GPU weighs four texels for a texture with fragment flag `0x400` (`docs/gpu.md`; in the
  library, `GL_TEXTURE_MAG_FILTER` `GL_LINEAR`). Quake uses it; a change to it is checked with
  `QUAKE_HOLD=60 quake +map e1m1`, `--save-state`, `--gpu-capture` and `tools\gpu_reference.py
  SNAP BMP --size 640x480` (every pixel within 2 levels), and needs "Sync shader sources" in
  Unity, whose GPU shares `gpu.h`.
- `rvc_harness --profile` on a snapshot of a program says how many of its instructions leave the
  fast step (`fast_step` under 0.98 of `tick` is worth a look). Quake's were 10%: `flw` and
  `fsw` have opcode bit 2, as the jumps have, and the fast step's test for those came first and
  sent every one to the general path, at three times the cost. With that mended Quake went
  from 16.7 to 22 frames a second and a float test from 3.3 to 4.8 million instructions a
  second. A new kind of instruction in the fast step: check with the profile that it gets there.
- One program at a time has the GPU's memory for programs (`SHADEREMU_GPU_DRAW`; `seglInit`,
  `gl.c` and ClassiCube's window ask): a test that starts a second OpenGL program gets "the GPU
  is drawing for ..." and exit code 1 from it.
- `glDeleteTextures` lowers the top of texture memory to the highest texture still used, and
  counts only textures inside that memory: a texture of a window's own pixels
  (`seglWindowTexture`) is below it. Counted, it held the top for good, and Doom ran out of
  texture memory eight levels after its first screen wipe. `DOOM_GL_DEBUG=1` prints what is
  left at each level's start.
- `nx restart` is one at a time (`/tmp/nxrestart.pid`), and Settings asks for it once.
- A crash of a game in the world comes with the kernel's register dump on the console: `epc`,
  `ra` and `badaddr` are enough. Red Alert's (October 2026) was `badaddr` at the end of the
  GPU's memory map (0xb00000 long), a2 a small negative number in `memset`: the game's
  `Buffer_Fill_Rect` given a right edge left of its left edge by `gl.cpp` (something drawn over
  the sidebar), which filled from the page to the end of the GPU's memory and took the
  window system's command list with it. A rectangle from the game's own arithmetic is
  clipped and checked before it is filled.
- One window system at a time: `nano-X` holds a lock (`/tmp/nano-X.lock`) and a second one ends
  with "a window system is running already"; `nx` says so and starts nothing.
- Never more than three harness runs at once: four took 264 s where one takes 70.
- Its changes to id's source are `linux/quake/quake.patch`: edit a clean clone at the commit
  `build.sh` names and save `git diff -- WinQuake`. The build puts the tree back each time.
- A window's own pixels are in a snapshot: the display's layer table (address in
  `0x8700000c`) has the window's buffer, frame and caption included. `gpu_reference.py` takes
  `--size WIDTHxHEIGHT` for a list drawn into a window, whose picture is the window's size.
- In C a float times `0.5` is a double multiplication. For a program full of floats built for
  the float instructions, `-fsingle-precision-constant` keeps such lines out of the runtime's
  double routines.
- ClassiCube (`docs/classicube.md`): `wsl -- bash /mnt/c/Development/ShaderX86/linux/classicube/build.sh`
  (6 s) after Nano-X's build, then the image and snapshot. To measure it, resume the snapshot
  with `nano-X -p &` and `CLASSICUBE_SEED=7 classicube` and wait for `classicube: steady`
  (35 s), then read the `ccstat:` lines. Lines typed at its terminal press keys, turn the view
  and hold a frame (`down W`, `turn 400 60`, `hold`): `--expect "classicube: steady" --send`.
- Its changes to the game are `linux/classicube/classicube.patch`: edit the clone in
  `~/shaderemu-linux/src/ClassiCube` (its files have CRLF line ends: keep them) and save
  `git diff`. The build puts the tree back each time.
- Its graphics are on the GPU device itself, not on `gles.c`: a change to the device's
  commands or vertex kinds touches `Graphics_ShaderEmu.c` too.
- A program for a machine with float instructions and no doubles: look for `__muldf3` and
  `__adddf3` in a profile first. ClassiCube's map generator and sine were 45% of its start.
- `tools\gpu_reference.py` cuts triangles at the near plane as the card does. A triangle with
  a corner behind the eye used to be left out of the model whole.
- `gles.c` takes its frame's size from defines (`SET_SIZE`, `UNIFORMS_IN`, `VERTICES_IN`,
  `MAX_COMMANDS`, `MAX_TEXTURES`); Quake changes only `MAX_TEXTURES`. A program whose textures
  do not fit keeps them in ordinary memory and GPU memory as a cache of the ones being drawn
  (`qgl.c`), and may have the display's own framebuffer too (`seglMemorySpare`, 0x87010000 to
  0x87400000: nothing reads it while the desktop is layers; one program at a time).
- A name added to `gpu.h` must not be one Unity's own shader files use (`gpu_point` is a
  struct in `GpuDrawPass.cginc`): the harness compiles and Unity's GPU draws nothing, with a
  black display. After "Sync shader sources", ask `ShaderUtil.GetShaderMessages` for every
  shader under `Assets/ShaderEmu` once play mode has compiled them.
- To see where a Quake scene's instructions go, save a snapshot at a `quakestat:` line
  (`--until "particles and weapon"`) and resume it with `--pc-log` for a quarter of a minute.
  Count calls before optimising one: a fight had 11 models and 110 draws, not hundreds.
- A vertex format the GPU takes as the program's data already is (`docs/gpu.md`: packed,
  tagged with a table, points) beats any loop that writes vertices: Quake's models went from
  244k to 93k instructions a fight frame and its particles from 150 each to 70.
- A bash heredoc does not carry `\n` in a C string into a file even when quoted `'EOF'`.
- Red Alert (`docs/ralert.md`): `linux\ralert\build.sh` after Tiberian Dawn's (20 s), then the
  image and snapshot. To measure it: `TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7
  RALERT_FRAMES=300 ralert` and wait for `ralert: done` (two minutes); `rastat:` lines, and
  the state sums `01e0dd60` (Soviet) and `ea72a5d1` (Allied) at frame 300, which a change to
  the game must leave. It is built from `linux\tdawn`'s shared files: after a change to
  `host.c`, `shaderemu.cpp` or `soundio_shaderemu.cpp` build and check both games.
- Its patch is a diff over Tiberian Dawn's: apply that to a clean tree, `git add -A`, edit,
  and save `git diff` as `linux\ralert\vanilla-conquer.patch`.
  (`linux\ralert\build.sh` puts the tree back without that `git add`: a `git diff` saved after
  a build has Tiberian Dawn's patch in it too, and the next build fails on it.)
- A pointer that moves is the common case in the world and was never in a harness run:
  `--pointer-sweep 0` moves it. A game must not select `GR_EVENT_MASK_MOUSE_POSITION` nor ask
  the window system for events because the pointer's words changed: it reads the pointer
  from `0x87000020` (`docs/input.md`, "A program's own reading"). Red Alert was at half its
  frame rate with the pointer moving before that.
- Kernel time compared with `jiffies` begins five minutes before zero: a static `unsigned
  long` stamp of 0 is "long ago" only after those five minutes (`pointer_at` in
  `shaderemu_input.c` was that for a build: no pointer reports at all after boot).
- The workers' number and sizes are the guest's to set (`docs/multicore.md`, "The geometry"):
  `mctest 15 60000 shape 6,6,6,5,5,4,4,4` must pass for any shape that fits. The tick's
  geometry shader (`tick_geom`) draws core 0's rectangle and a strip for each worker that
  runs; the host only copies the two zones in before the pass and back after, and knows
  nothing of the layout. Anything that reads a worker's state (the control pass, the
  harness's `CORE` lines) finds its strip from the geometry texel. A worker's pixels must
  stay whole 8 x 8 tiles.
- The D3D11 harness draws the tick into eight targets, eight state texels a pixel
  (`docs/multicore.md`, "Eight texels a pixel"; `--no-mrt` for one), with a write cache twice
  the shader's default (`L1_TABLE_BITS=7`) and 32,768 instructions a pass. Snapshots from
  before are of another state layout. D3D12 and Unity have one target still.
- The harness's machine has up to 64 cores (`--cores 64`; `docs/multicore.md`, "More than
  sixteen cores"): `RAY_SIZE=640x480 RAY_SHAPE=3,3,... nxray` with 63 threes is its measure
  (1.35 s a picture, sum `fd92a71e`), and `mctest 63 60000 shape 3,3,...` its test (a large
  limit makes the float check slow: core 0 works every worker's sum out again). A shader for
- The control row has no free texel: control words to 0x3ff, the display palette from 0x400,
  the sound card's voices from 0x800. The geometry of workers 25 and up is in the mailbox
  page (`MC_GEOMETRY_MORE`, 0x86c00f00) and what cores 16 and up ran in the network's row after
  the guest's window (`MC_STATS_MORE`, 0x876b8280, 13 texels). They were at 0x3e0 and 0x3f0,
  which are the network's.
  another number of cores is another compile. `rvc_cpu` and Unity have 16 at most.
- A program that takes every worker asks the library how many there can be (`mcw_most`,
  `mcw_fit`, `mcw_rows`, `mcw_room` in `programs/mc/mcw.h`): no 15 or 16 written into it.
  `nxray`, `nxpath` and `nxview` (a JPEG: one worker for its codes and one a strip of 16 rows
  at most) do; Quake, the two Command & Conquer games and the NES ask for the few they use.
- The D3D11 harness commits what the cores stored as points (`docs/multicore.md`, "The commit as
  points"; `--define NO_SCATTER` for whole bands, to compare). Anything new that writes RAM
  in the commit and is not a cache entry, a core's last store or a copy must add its band to
  `commit_bands_changed_core` or its own point to `commit_geom`, or the write is not drawn.
  With `--stats-after` (the timed frames) a guest's own times came out twice as long with
  the points: take a program's times from a run without it.
- Pixels of two cores that the card runs as one group cost what both cost, unless the cores
  run the same instructions: measure anything about the workers' pixels with `nxray` (every
  core its own work), never with `mctest`'s bench (the same primes on all of them).
- A store the write cache has no room for is still made, by itself, when the pass ends
  (`ram_l1_last_addr`). Anything the machine writes as several stores that another core
  reads as one record must not let its last word be that store: a worker's fault record was
  read by core 0 as a new count with zeros ("a worker core stopped, cause 0, at pc 0"),
  now and then, with the smallest caches (`MC_FAULT` in `src/mc.h`).
- `nxray` and `nxpath` (`docs/raytrace.md`) are the programs that use every core: after a change
  to the workers or the geometry, `RAY_FRAMES=1 RAY_STILL=1 nxray` with `RAY_WORKERS=0` and
  without must print the same sum, and so must `NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SAMPLES=2
  nxpath` with `NXPATH_CORES=3` and `0`.
- A program that uses the worker cores links `programs/mc/mcw.c` and posts its own functions
  as jobs (`docs/multicore.md`, "Using the workers from a program"): a worker is a thread of
  the program in user mode, on its page table. There is one kind of worker; do not add
  another, and do not copy the start and the mailboxes into a program. A job makes no system
  call and writes only 16-byte pieces of memory that nobody else writes. `mctest` is the
  library's test: it must print `mctest: PASS` before and after any other program that used
  the workers in the same boot, and after one that was killed (`kill -9`) while they ran.
- A worker's start needs the kernel's `SHADEREMU_GPU_WORKERS` ioctl (`linux/kernel/shaderemu_gpu.c`),
  which hands the cores out between programs: with a kernel from before it `mcw_open()` gives
  0 and everything runs on core 0. A program asks for the workers it uses and no more.
- To see where a guest's leaf functions are called from, `--ra-log FILE` and
  `python tools\pc_callers.py FILE game=prog.nm`; where inside a function, `tools\pc_hot_in.py`.
- Red Alert uses the machine's worker cores for its movies when there are any (`--cores 4`;
  `docs/ralert.md`, `docs/multicore.md`): `RALERT_MOVIE_CHECK=1` must say `0 of N shared frames
  differ`, and the state sums above hold with and without them. Run a timing both ways: the
  workers cost the mission itself 4 to 8% while they have nothing to do there.
- In Red Alert a cell is "mapped" when any of it shows and "visible" when all of it does; in
  Tiberian Dawn it is the other way round. A renderer carried over with the names swapped
  draws the shroud's edge solid black: compare with the game's own drawing at a held frame.
- When a part of a frame is neither logic nor the map, count the game's own shape drawing by
  window and size before guessing: Red Alert's 160k was one 160 x 141 plate drawn again for
  every unit that moved, and a million a frame was a tune being looked for that the demo lacks.
- The ROM is 168 MB now (2048 x 5123 texels); Unity's importer takes textures up to 8,192 high.
- The console is four terminals in the world (`docs/console.md`): `emumux` in the guest, started
  by `emuinit` when the host flags' bit 1 is set (`--tabs` in the harness; a run without it is
  as before). A test of the world's console reads `EmuTerminal`'s `grid` by terminal, and must
  import the boot images first: tabs with an image that has no `emumux` type `0x1e` at a shell.
- The volume display draws the frame's commands in four passes, as the GPU blends them
  (`GpuVolumePass.cginc`, `docs/volume.md`). To check it, run Quake in play mode, render a
  camera 1.4 m before "Volume display" with "Volume content" on and off, and compare the
  changed pixels' mean colour with the game's own picture: light maps drawn solid are grey.
- The game controllers (`docs/gamepad.md`, two `EmuGamepad`s, no menu) are tested in play mode with
  `RunInputEvent` and the display keyboard's `queue`. Its station and its fit in a hand are
  untried in a headset. The power button is a small sign on the tower ("Power sign").
- A new UdonSharp behaviour's program asset must exist and be compiled before an editor script
  can fill the behaviour in ("outdated script version"): run such a menu command twice.
- A program built on `linux\tdawn\host.c` and Quake both take the display's spare framebuffer
  for GPU memory (`seglMemorySpare`): only one of them can run at a time.
- Quake's sound (`linux/quake/snd_shaderemu.c`) is checked like any of the card's: a timedemo
  with `--fixed-dt 0.004 --sound-capture` and `tools\sound_reference.py ... --rom`. Measure the
  capture's clipped samples too: at the game's own level a fight clipped 1.5% of them.
- The desktop is worked from the keyboard too (`docs/nanox.md`, "The keyboard"). To test it
  without one, `nxkey` presses keys in the guest: `nx; sleep 8; nxkey meta down enter; sleep 2`,
  then compose the screen from a snapshot's layers: the window with the keyboard has the
  coloured caption and is the last layer. Alt+Tab and Alt+F4 never reach the harness window
  (the host takes them): the same keys work with Ctrl.
- The guest shell's `sleep` ends early when a background job finishes: `nx & sleep 25` slept
  two seconds. Run `nx` in the foreground (it returns once the desktop's programs are started).
- Dear ImGui (`docs/imgui.md`): `linux\imgui\build.sh` after Tiberian Dawn's (12 s the first
  time), then the image and snapshot. To measure it, `nx; sleep 8; imdemo & sleep 9; sleep 9`
  and read the `imstat:` lines; `nxkey down down down down down down space` opens ImGui's own
  demo window. A click from `nxkey` does not press an ImGui button (it acts on release).
- A window's frame is painted when the window is made; its later repaints (the caption's
  colour when it gains the keyboard) still wait for its program to ask for events.
- The Start menu's lines are `Folder/Label=command` (`nxbar.c`, `docs/nanox.md`); a new program's
  build names its folder. `NXBAR_SHOW=Games nxbar` opens it there for a test, and the menu's
  window is the layer 150 wide in a snapshot, 20 pixels a row plus 4.
- The room's models are made by scripts (`docs/world.md`): `python world\textures.py`, then
  Blender with `-b --python world\build.py` (5 s), copy `unity/ShaderEmu` over, and
  "ShaderEmu/Put the modelled room into the open scene", then bake. Geometry is written in
  Unity's coordinates; a screen moved in the builder must be moved in `world/computer.py` too.
- The room needs three shader packages in the Unity project (`world\install_packages.py`):
  without them the builder does not compile. LTCGI's own adapter for Light Volumes is left
  out: it is written for version 3 of them, and with 2.1.3 UdonSharp refuses it and compiles
  nothing else either.
- A Poly Haven model may be a kit laid out for show (pens in a row, four trees side by side):
  list its parts and sizes before placing it (`parts=` in `b.asset`), and never cut its
  triangles unless it is a box: chairs and leaves came apart.
- The room's light is baked on the graphics card, once, after everything else is in: a bake
  on the processor was at 49% after most of half an hour.
- The builder's boxes for the room are colliders only now (`StripBoxes` in
  `ShaderEmuModels.cs`): a new prop needs a model in `world/`, and a box in the builder only if
  it must stop a player.
- The holodeck (`docs/holodeck.md`) replaces the volume display's wall screen: through the den's
  left wall, drawn by `GpuHolodeck.shader` wherever the room's own faces are in view. The
  corridor is behind the back wall and is where a visitor arrives. Both are `world/annex.py`
  and `Annex()` in `ShaderEmuHolodeck.cs`; a doorway moved in one must move in the other and in
  `world/room.py`.
- The classroom's eight computers (`docs/stations.md`) show their owners' displays: a place is
  a synced number in a visitor's own `EmuShare`. `EmuStations` is a new Udon behaviour: after a
  change to its fields run "Create Udon program assets" and the room's menu command twice.
- A model's own objects cannot be moved out of its instance in the scene: copy them and put the
  model's away (`Annex()` does that for the holodeck's mask and control).
- A panel's lettering is part of its plate (`Plate.shader`, `docs/world.md`): run "Restyle the
  panels" after anything that makes or changes a panel, or its labels are unlit canvas text.
- Udon's variables are empty outside play mode (`GetProgramVariable` gives null): read a
  behaviour's fields from its C# proxy in the editor, and from Udon while playing.
- A classroom place is a terminal of its owner's machine (`docs/stations.md`): its keyboard and
  tube answer only the owner's beams. `world/bake_pc.py`'s low shell must be of one material:
  a second one makes its texture a target of the bake, cleared to black for every model using it.
- The classroom's computers are modelled to the standards' sizes (`world/pc.py`'s header): a
  29 inch presentation monitor, 5.25 and 3.5 inch drive faces, card slots 20.32 mm apart, a
  PS/2 power supply's back. Their lettering is decals in `Details.png` (`pc_lettering` in
  `world/textures.py`), which is 2048 square now.
- A beam stops at the first collider on layer 0 before what it points at (`Aim` in
  `EmuPointer.cs`): it reached through a classroom's monitor to the wall's display. A new prop
  between the chairs and the wall's screens needs no collider unless it should stop beams.
- "Look at the room without a bake" takes each reflection probe's picture with a camera
  (`ProbePictures`) and lets 16 pixel lights light a surface; "Bake lighting" undoes both.
- The room's light is baked by Bakery when the Unity project has it (`ShaderEmuBakery.cs`,
  `BAKERY_INCLUDED`): "ShaderEmu/Bake lighting" is one Bakery render (lightmap, light volumes,
  light probes in L1 mode), then Unity's own bake of each reflection probe into a file the probe
  uses as a Custom texture (a Baked probe's picture is in the lighting data, which every Bakery
  render replaces). Never copy a script into the project or refresh it while a bake runs: the
  reload drops what is left of the bake. Unity's GPU lightmapper hung on a 4096 page here.
- A sound of the world is cut from a recording (`world/sounds.py`, `SOURCES`), not computed:
  thunder and the computer's sounds were tried computed three times and were wrong each time.
- A panel (`Plate.shader`) is shown mostly as drawn (`_Flat`), and its far-off lettering takes
  the glyph's own ink: the kept picture of the lettering is taken in the room's light, and a
  line seen aslant changed colour along its length, which read as a sheen.
- The NES (`docs/nes.md`): `wsl -- bash /mnt/c/Development/ShaderX86/linux/nes/build.sh` (2 s) after
  Nano-X's build, then the image. A change to the emulator must leave the sums the doc lists
  (`NES_SUM=1 NES_PPU=inline NES_FRAMES=120 nes thwaite` under `rvc_cpu`, two seconds), with each
  of `NES_CPU=interp` and `NES_PLAIN=1`, `2`, `3` too; its speed is `rvc_harness --cores 4`
  and the `nesstat:` lines. Its changes to Nofrendo are `linux/nes/nofrendo.patch`; the 6502
  recompiled is `linux/nes/rc6502.h`.
- The GPU has a texture that is a layer of tiles (`docs/gpu.md`, fragment mode 5; `SEGL_TILES` in
  the OpenGL library): the NES's picture is quads of it (`docs/nes.md`). A change to it is checked
  with a held NES frame against `tools\gpu_reference.py` and against the first core's own
  drawing of the same frame, and needs "Sync shader sources" in Unity when that is allowed again.
- The kernel's sleeps are whole hundredths of a second: a program that paces frames sleeps the
  long part and ends passes (`pause`) for the rest, by the clock word (`frame_paced` in the NES).
- A sum of a program's last picture can stay the same through a mistake: sum its state at every
  frame. The NES's pictures matched while its RAM had gone another way for a while.
- Code a program writes for itself to run (the NES's recompiler) is fetched as it was until the
  machine's pass ends: `fence.i` after writing it, a user instruction here, ends the pass.
- A function with a hundred locals saves a dozen registers on its way in: a path through it that
  is taken every scanline and needs none of them belongs in a small function in front of it.
- Two static programs built `-fno-pie` are at the same addresses: `pc_profile.py` given both
  `.nm` files charges one program's instructions to the other. Name only the one that is busy.
- `open` (`docs/open.md`) starts the program registered for a file's kind; the kinds are the
  lines of `/usr/share/nxopen.*`, and a program's build adds its own file beside its
  `nxapps.*` one. To test what it decides: `open --dry FILE` under `rvc_cpu` (0.7 s). The whole
  way from the host: `rvc_harness ... --desktop --open "file://C:\path\x.png" --open-after "GPU
  drawing" --until "is nes"` (14 s from power-on).
- The host tells the guest to open something through control words at 0x87000240 and the
  desktop's bar starts `open`; the fetch has kind 3 (a file), an offset for answers over
  256 KB, and a file may travel inside a PNG (`tools\file_to_png.py`, `docs/fetch.md`).
- An end marker for `--until` typed from PowerShell inside a single-quoted string is
  `echo LX-''''DONE` (the guest then gets `LX-''DONE`). With `''` alone the guest's shell gets
  one quote and waits for the other for ever, and the run sits until its limit.
- A panel added to a baked scene is styled by itself (`RetroNewPanels` in `ShaderEmuRetro.cs`,
  which every "Add ... to the open scene" command that makes one calls): "Restyle the panels"
  makes every plate again, and a plate made again has lost its place in the lightmap.
- The kernel upstream's image is built from has networking in it already (`NET: Registered
  PF_INET`, a device `rvcnet`): look there before adding a stack.
- A screen in the program's world showing only a game's flat drawing was built and taken out
  (`docs/holodeck.md` has why). The frame's size is still the fourth word at 0x87000300, and
  `gpu_vertex_sized` in `gpu.h` still places a vertex for a picture of a given size.
- The host's words about the room are at 0x87000360 (two texels, `HOST_STATE`), written by the
  control pass when the host defines `HOST_STATE_WORDS(n)`: Unity does, the harness not yet.
- A lamp of a model that should follow a machine must not be in a baked picture of light:
  `world/bake_pc.py` bakes the set's lamps dark and `PowerLamps` in `ShaderEmuStations.cs`
  puts lit ones before them.
- A clock's hands are objects of their own turned by `ClockHand.shader` (`docs/world.md`); a
  mesh a shader turns in its own space must not be static, or batching moves it to the world's.
- `world/bake_pc.py` (53 s) writes the keyboard too: a change to anything of `pc.py`'s set
  needs it before `build.py`, not only a change to the shells.
- `KeyAt` of a classroom keyboard takes the nearest key anywhere on the plate; 3% of the plate
  (between the blocks of keys) had answered no key when the search stopped at half a key.
- The network (`docs/lan.md`): `--net PORT --net-id N` on `rvc_harness` (D3D11) and `rvc_cpu`;
  machine N is `10.0.0.N` on UDP port PORT + N; `--net-loss P --net-delay MS` are the VRChat
  link's part. The guest configures nothing: `lan0` takes its address from the host's number
  (0x870003f8). In the world the number is the visitor's player number and packets travel in
  `EmuShare.net`.
- The kernel has had TCP/IP all along; the fork's `rvcnet` is left out of the build and
  `shaderemu_net.c` is the device: control texels 0x3e and 0x3f, a row of RAM at 0x876b8000
  (window at 0, ring at 0x400). Nano-X's `DATA_END` ends below it. No control texel is free now.
- The guest sends one 640-byte window per readback round trip, several small packets to a
  window. One packet per ack made Doom run at 2 tics a second.
- `nc -l -p PORT < /dev/null > FILE` ends with the connection; without `< /dev/null` it waits
  for the console. `nc` and `telnetd` are scripts over BusyBox; `ping` is upstream's.
- Two machines at once: give each its own `--uart-log`; two shader harnesses run at half speed
  each. For a long game start put one side on `rvc_cpu`, which with `--net` runs in real time.
- Quake over the network: `quake -listen 4 +map e1m1` and `quake +connect NAME` (found by
  broadcast; a server's name is its address unless `+hostname`). The client must start after
  the server has its level. `QUAKE_NET=1` prints packet costs. `QUAKE_SUM` leaves the players'
  names out: sums noted before 10 October 2026 are not this build's.
- Doom's network game is the port's own `i_net.c`: `doom -net 1 .10.0.0.2`, `doom -net 2
  .10.0.0.1`. Doom needs the sound card: under `rvc_cpu` it crashes in its music code.
- `build.bat` through `cmd /c` from an agent's PowerShell exits at once: build the harness with
  `cmake --build build --config Release` (12 s).
- Quake draws all round while the host's words say its owner sits in a holodeck (`se_all_round`
  in `vid_shaderemu.c`, `R_CullBox`; `--holodeck` in the harness): a held frame of e1m1 costs
  the same there, 52k against 50k "in view".
  With tracked hands (bit 5 of the host's first word; `--holodeck-aim DEGREES` in the harness)
  the right hand's rise is the aim's; a head is not aimed with (the developer: "why do you
  have hands?").
- "Build & Test" from editor code: show the SDK's panel in one call and ask its builder in the
  next; asked in the same call it says "Open the SDK panel". The hub's `lanTest` makes two
  clients ping each other and then play Doom over the network, and log `[LanTest]` lines.
- A colour times a coloured texture can be black (a blue tint on the den's red carpet): give a
  new surface a neutral texture, or one of its own.
- Idle Linux runs about 0.2M instructions a second: a test that waits for "enough instructions"
  to know the guest has booted waits for ever. The prompt is there after 10.7M.
- In Unity the tick and the commit's points are cameras' passes (`docs/multicore.md`, "In
  Unity"): a camera each, a mesh each on a layer of its own (23 and 24), rendered from
  `EmuMachine`. Udon has `Camera.SetTargetBuffers` and `RenderTexture.colorBuffer`. A shader on
  such a mesh must be a file with one pass (a renderer draws every pass of its shader), and
  must draw nothing for any camera but its own: it looks at `_ScreenParams`.
- A material that commits must have the ROM's textures (`ApplyImage` in `EmuMachine.cs`): a copy
  out of the ROM is a commit's. Without them Linux panics with "Requested init /emuinit failed
  (error -8)", which looks like a bad image.
- A round in Unity costs about a millisecond besides the tick (the script and three cameras):
  measure a change there with passes of 2 instructions as well as full ones, and set the pass
  length on the tick's material (`_Ticks`), not only in the script's `ticks`.
- After a pull that changes `main.shader`'s tick or commit, the same change is owed in
  `MachineTick.shader`, `MachineCommit.cginc` and `MachineCommitPoints.shader`.
- A model's skin must be closed where it can be seen into: `blender -b build/world.blend --python
  world/check_watertight.py -- NAME` lists the loops of edges with one face. The classroom's
  shell (`pc_station`) must have none; a sheet of its own (the glass) needs a closed face behind it.
