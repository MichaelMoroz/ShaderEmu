# gvm_gpu

gpu_vm machines on the card: the tick (`gpu_vm/tick/steps/x17.hlsl`; its interface `gpu_vm/tick/STATE.md`) runs the
cores in passes on D3D11, and this host loads the program, gives each pass its TIME, reads the console and the cores'
state back, makes the semihosted calls and, with `--check`, runs the reference (`gpu_vm/ref/gvm_ref.h`) beside the
card bundle for bundle. gvm_cpu's twin (`gpu_vm/cpu/README.md`): the same options, devices, semihosting and exit codes.

## Build and run

    gpu_vm\gpu\build.bat                       bin\gvm_gpu.exe (MSVC through vswhere; fxc2's d3dcompiler_47.dll beside it)
    pwsh tools\gpu_queue.ps1 bin\gvm_gpu.exe [options] PROGRAM.elf [ARGS...]
    pwsh tools\gpu_queue.ps1 bin\gvm_gpu.exe --dtb gvm.dtb vmlinux

Every run goes through `tools\gpu_queue.ps1` (one card user at a time). Compiled shaders are kept in `bin\gvmcache`.

gvm_cpu's options: `--ram MB`, `--rom FILE`, `--dtb FILE`, `--bin FILE --at ADDR [--entry ADDR]`, `--budget N` (bundles
a pass, 8192), `--pass-time N` (TIME's ticks a pass, 20), `--cores N` (core 0 runs, the others are parked: halted),
`--max-bundles N` (checked after each batch, so it stops a little past N), `--strace`, `--stats`, `--no-console`,
`--no-stdin`. Not here: `--trace` (use `--check`'s report, or gvm_cpu). The card's:

| option | |
|---|---|
| `--shader FILE` | the tick (default `gpu_vm\tick\steps\x17.hlsl` from the exe's `..`) |
| `--warp` | D3D11's WARP instead of the card |
| `--batch N` | passes at most between two readbacks (32) |
| `--op-rounds N` | the operations' rounds a pass (2) |
| `--check` | the reference beside the card; exit 4 at the first difference |
| `--check-ram N` | with `--check`: all of RAM compared every N passes, at a batch's end with no operation in flight (4096; and at the end) |
| `--until TEXT` | exit 0 when the console's output has TEXT, with the wall time |
| `--trace-pass` | each pass on stderr: core 0's pc, bundles, why it ended, flags, operations in flight |
| `--profile` | `--stats` and a pass's GPU time by its draws (tick, commit, operations, readback copies) |

Exit codes: the program's exit code; 1 usage; 2 an unhandled trap, every core halted, or every core waiting for
nothing; 3 `--max-bundles`; 4 `--check` found a difference; 5 D3D11 failed.

## How it runs

- **Memory**: the RAM texture (8192 texels wide, RAM's end / 16 / 8192 rows) is the image from address 0: the boot area,
  the device tree at 0x1000, the device window and RAM. The ROM is a texture of its own.
- **Passes, batches**: a pass is the tick, the commit's four draws and `--op-rounds` rounds of the operations
  (`STATE.md`); TIME is the pass's in the cbuffer, `--pass-time` more each pass. After each pass its state is copied
  into a staging texture (inside a batch texels 18 and 19 alone, the flags and counters; the batch's last pass every
  texel) and the console's 18 texels; after a batch they are mapped. A batch is one pass after anything the host made
  (a call answered, input delivered), then twice the last, to `--batch`. Readbacks cost about 10% of the card's time at
  one pass a batch, 1% from 16 up.
- **Semihosting**: a trap while TVEC is 0 sends the tick to address 0, where the host puts a stub `{sync ; j 0}`: the
  core spins there (two bundles a pass) until the batch ends. The host then makes the call as gvm_cpu does (number in
  r12, arguments r5-r10, guest memory read and written through RAM's texels), writes r5, sets pc to EPC + 4 and puts
  STATUS, EPC, CAUSE and TVAL back as the last pass before the trap left them. Any other cause is reported (exit 2).
  The kernel's power-off (`exit_group` with TVEC 0) ends the run with its code.
- **Console** (0x016b8280): every pass's ring is printed; input (up to four characters) is written into the host's
  texel by UpdateSubresource after a batch when taken == delivered, as gvm_cpu does.
- **Waiting**: when every running core is in a supervisor's `wait` at a batch's end, TIME jumps to the earliest timer
  that can wake one (later than gvm_cpu, which jumps at once, so the guest sees other times).

## --check

The reference machine gets the same image, start state and TIME; after each pass of the card, each core of the
reference runs exactly the bundles the card's ran (19.z, stopped by `host_step`) and is compared: the 63 registers, pc,
the SRs (STATUS, EPC, CAUSE, TVAL, TVEC, SCRATCH, IEN, TCMP), the wait and software-interrupt flags, RANDOM's state,
CYCLE and OPS, and the words the pass stored (the card's write cache entries against the reference's write log). Where
the card ended a pass inside a bundle, before an op (an operation in flight held an access, or no room for a store),
the reference's last bundle is run again with only the ops before that one. A pass made again (flag 16) runs nothing
on the reference. The semihosted calls and the console's input are made on both. All of RAM is compared every
`--check-ram` passes. Operations are made whole by the reference's commit and over rounds by the card, so a pass's
RAM is compared only with none in flight. The first difference prints the pass, the core, each differing value, the
reference's last 24 bundles disassembled and the card's registers.

Expected differences: the special functions (`fsin`, `fcos`, `fexp2`, `flog2`, ...) are the card's approximations, so
a program using them (`exec/fastmath`) diverges under `--check` though it passes its own loose checks.

## Tests

    bin\gvm_cpu_mk.exe DIR
    pwsh tools\gpu_queue.ps1 "C:\Program Files\Git\bin\sh.exe" gpu_vm/gpu/tests/run.sh bin/gvm_gpu.exe DIR [--check]
    pwsh tools\gpu_queue.ps1 "C:\Program Files\Git\bin\sh.exe" gpu_vm/gpu/tests/exec.sh bin/gvm_gpu.exe [--check]

`tests/run.sh` is gvm_cpu's 19 cases (`--trace` replaced by `--strace --stats`); `tests/exec.sh` the code generator's
execution tests built in `build/gvm_exec` (`gpu_vm/llvm/test/exec/run.sh`).

## State (22 October 2026, the committed x17, RTX 5070 Laptop)

- gvm_cpu's 19 tests pass on the card, with `--check` too (RAM the same). The 56 execution tests pass; with `--check`
  55, `fastmath-O0` diverging as expected.
- CoreMark (`build/gvm_bench/coremark.elf`, -O2, 20 iterations, 149k bundles an iteration): correct, 3.01 M bundles in
  0.53 s, **5.7 M bundles/s, 38 iterations/s**, 1,240 passes/s; a pass: tick 760 us, commit 4, operations 8, readback 7;
  passes ended by budget 260, full write cache 362, sync 36 (gvm_cpu on the host: 23 M bundles/s).
- Linux (`~/gpu_vm/linux-build/vmlinux` of 13:01, `gvm.dtb`): boots on the card to `Run /init as init process` and
  init's line in **3.0 s** wall, 6.1 M bundles, 14,824 passes (2.0 M bundles/s; gvm_cpu 0.24 s). A pass there is 414
  bundles on average: 99.7% end on a full write cache (130 words), and the tick costs 170 us a pass (0.41 us a bundle,
  CoreMark's 0.17). A line typed comes back (the tty's echo and init's). `--check` from the boot through init, the
  typed line and idle: no difference (46,431 passes, RAM too, to 8 M bundles).
- The tick agent's x17 in progress (paths 1 and 2 in `step12`), with its `run()` choosing `step12` or `general` by
  `if`/`else` (as `?:` fxc made both calls): the same tests pass with `--check`, the boot is the reference's (RAM too),
  and Linux reaches init in **2.1 s** (a pass's tick 112 us); CoreMark is unchanged.
