# gpu_vm against the lean RISC-V tick on real work

The user's requirement: gpu_vm's tick 2-3x the lean RISC-V tick (`experiments/rvc_lean`) in effective work a second,
on real programs as on the bench's loops (x17 there: 2.22-2.81x, `gpu_vm/tick/steps/README.md`). The same C sources,
the same clang 23.1.4 at -O2, the same card (RTX 5070 Laptop, D3D11, fxc2), each machine's GPU harness, each program
under its machine's Linux.

| | |
|---|---|
| `build.sh [rv] [gvm] [bare] [kernel]` | (WSL) CoreMark and zlib's `minigzip` for both machines, CoreMark bare-metal, gpu_vm's kernel with them in its initramfs -> `build/bench_real` |
| `coremark/` | CoreMark's port for a program under Linux (both machines): `CLOCK_MONOTONIC`, static data, `coremark 0x0 0x0 0x66 N` |
| `init.c` | gpu_vm's `/init`: runs each program between `BENCH-START name` and `BENCH-END name` |
| `stamp.py` | runs a harness and prints each console line with this computer's seconds |
| `run_lean.ps1` | the RISC-V machine: `rvc_harness --d3d11 --rvc experiments\rvc_lean --image build\images\lean_bench` of the main checkout, the bench at the prompt |
| `run_gvm.ps1` | gpu_vm: `gvm_gpu` (tick x17) bare-metal CoreMark at both counts, then Linux; `-Trace`: `--trace-pass` and `passes.py`'s table |
| `passes.py` | passes, bundles a pass and why passes ended, for the boot and each program of a `--trace-pass` run |

## How it was built and measured (22 October 2026)

- **RISC-V**: clang `--target=riscv32-unknown-linux-musl -march=rv32imaf -mabi=ilp32 -fPIE -ffp-contract=off` (rv32-cc's
  flags; rv32-cc wraps GCC 13.3), linked by `rv32-cc` (the shader machine's musl, flatcrt), `linux/nommu/elf2bflt.py`.
  The private image `build/images/lean_bench` of the main checkout: `lean_isaloops`' kernel and overlay, the two programs
  in `/usr/bin` and the input in `/usr/share/bench` (`python tools/private_image.py lean_bench`).
- **gpu_vm**: `gpuvm-linux-cc -O2` (musl 1.2.5 for gpuvm, bFLT), in the initramfs of a kernel of its own
  (`~/gpu_vm/linux-build-bench`); `init.c` is `/init`. Bare-metal: `gpu_vm/llvm/test/bench/run.sh` (the exec runtime).
- The input of gzip: zlib's own C sources joined, the first 256 KB; both machines make the same 58,167 bytes and get it
  back. CoreMark's CRCs are the same on both (list e714, matrix 1fd7, state 8e3a; final a14c at 60, 25b5 at 180).
- **Time is this computer's wall clock with the card to itself** (no other harness, no build). The lean harness's guest
  clock is real time (CoreMark's own figure agrees with the stamps to 0.3%), gvm_gpu's guest clock is TIME (0.2 ms a
  pass), not real: so every figure below is wall time between console lines (`stamp.py`; a line comes back within a
  batch, under 25 ms). CoreMark is the difference of a 180- and a 60-iteration run (start-up and loading cancel);
  gzip and gunzip from the line before to the line after (program loading included on both).
- Instructions: the RISC-V machine's by `rvc_cpu --count-from` on the same image (CoreMark by the same difference);
  gpu_vm's bundles by `passes.py` over `--trace-pass`, bare-metal by `gvm_cpu --stats`.
- Two rounds (lean, gvm, gvm, lean): every figure within 0.3% of the other round.

## Results

| | lean RISC-V | gpu_vm x17 | gpu_vm / lean |
|---|---|---|---|
| CoreMark, iterations a second (Linux) | 24.2 (24.20, 24.24) | 38.4 (38.44, 38.38) | **1.59** |
| CoreMark bare-metal on gpu_vm | | 38.7 (5.76 M bundles/s) | 1.60 |
| gzip of 256 KB, s | 6.70 (6.72, 6.69) | 4.01 (4.010, 4.011) | **1.67** |
| gunzip, s | 0.814 (0.815, 0.813) | 0.495 (0.495, 0.495) | **1.64** |
| boot to init, s (different kernels, see below) | 1.04 (guest clock, real) | 2.49 | 0.42 |

Work a second and what a unit costs:

| | RISC-V instructions | lean: M instr/s, ns an instr | gpu_vm bundles | gpu_vm: M bundles/s, ns a bundle | RISC-V instr a bundle | gpu_vm in M RISC-V instr/s |
|---|---|---|---|---|---|---|
| CoreMark, an iteration | 270.6 k | 6.55, 153 | 149.8 k (194 k ops) | 5.75, 174 | 1.81 | 10.4 |
| gzip | 39.9 M | 5.95, 168 | 20.2 M | 5.04, 198 | 1.97 | 9.95 |
| gunzip | 4.69 M | 5.76, 174 | 2.35 M | 4.75, 210 | 1.99 | 9.47 |
| boot to init | 9.15 M | 8.8 | 10.6 M | 4.27, 234 | | |

**gpu_vm is 1.6-1.7x the lean tick on real code, not 2-3x.** On the bench's loops it is 2.2-2.8x: there a bundle
carries more RISC-V work (hand-scheduled, two ops and a fused branch) at a cheaper fixed cost.

## What limits gpu_vm on real code

- **Bundles carry too little.** Compiled CoreMark is 1.30 ops a bundle (`profile.sh`: 18.6% of bundles have no op, a
  branch alone; 32.9% one op; 48.5% two), 1.81 RISC-V instructions a bundle with the branches in control words; gzip
  and gunzip 2.0. The hot code is chains (`lw` ;; `lhu` from it ;; `beq` on it in `core_bench_list`, `lbu` ;; `beqi`
  in `core_state_transition`): a branch compares registers as the bundle found them, so a load and the branch on it
  are two bundles, and a load's result is not an operand in its own bundle.
- **A bundle costs more than a lean instruction**: 174 ns against 153 on CoreMark (passes ended by budget, 7,800
  bundles a pass, the commit, operations and readbacks 19 us of a 1.36 ms pass), 198 against 168 on gzip. The tick's
  fixed cost a bundle on this code is above the bench loops'.
- **The write cache**: gzip ends 96% of its passes on a full cache (2,700 bundles a pass), gunzip 71%, the boot 86%
  (2,118 a pass). A pass's fixed draws (commit 3.9 us, operations 8.4, readback copies 7.0) are about 4% of gzip's
  535 us passes, so the direct cost is small; the cost of a fuller cache inside the tick is not measured apart.
- **Path 0**: 3.8% of CoreMark's bundles are the general step's (cc 0: calls, returns, the control word's add).
- For 2x on CoreMark (48.4 iterations a second) gpu_vm needs 138 ns a bundle at today's code (21% cheaper), or 120 k
  bundles an iteration at today's cost (2.26 RISC-V instructions a bundle, 25% denser), or part of each.

## Caveats

- The boot is not like for like: gpu_vm's 5.17.11 `gpuvm_defconfig` with an initramfs of 650 KB (this bench's programs
  and input, unpacked into RAM) against the shader machine's own kernel with its root on the ROM (`Run /emuinit` by its
  printk, a real-time clock). gpu_vm's boot with the plain initramfs is 1.71 s (`gpu_vm/tick/steps/README.md`).
- The lean harness is the windowed one (frames of passes, the GPU device's and sound's passes, 255 workers laid out and
  idle), as its isaloops baseline was measured; gvm_gpu has one core and no devices on the card.
- gpu_vm's userland under Linux is `init.c` spawning each program (posix_spawn), not busybox's shell (its rootfs was
  not ready); CoreMark under Linux costs 0.8% over bare-metal.
- `float`/`double` play no part here (CoreMark's `ee_f32` only reports).

## 22 October 2026, evening: tick x18 and the denser compiler

The whole stack built again by the compiler of `c837089` (LLVM at that commit in a snapshot of its own: lit 13/13, e2e,
exec 56/56, zlib and Lua as the host's, CoreMark 113.0k bundles an iteration, gzip 15.25M, gunzip 1.85M bundles bare);
musl, builtins, busybox, the rootfs and every kernel clean; gvm_gpu's default tick x18; RAM's last 64 bytes unused.
Same card, same lean image and harness as above; the RISC-V side also with its best flags from the same clang
(`build.sh rv` with `RVOPT=`; the four builds side by side in the main checkout's private image `lean_bench2`).

Rounds alternating in one session (lean, gvm, gvm, lean, lean, gvm, lean; CoreMark it/s, gzip and gunzip s):

| round | lean -O2 | lean -O2 inl+unroll | lean -O3 | lean -O3 inl+unroll | gpu_vm |
|---|---|---|---|---|---|
| 1 | 23.91, 6.820, 0.741 | 27.06, 6.680, 0.796 | | | |
| 2 | | | | | 56.93, 3.291, 0.458 |
| 3 | | | | | 56.95, 3.294, 0.457 |
| 4 | (disturbed) | 27.14, 6.657, 0.790 | 24.78, 6.725, 0.751 | 26.94, 6.701, 0.794 | |
| 5 | 23.94, 6.660, 0.742 | 27.00, 6.688, 0.792 | 24.68, 6.636, 0.756 | 26.83, 6.625, 0.796 | |
| 6 | | | | | 56.71, 3.311, 0.459 |
| 7 | 23.68, 6.662, 0.744 | 26.43, 6.712, 0.796 | 24.71, 6.762, 0.756 | 26.84, 6.623, 0.796 | |

(inl+unroll: `-mllvm -inline-threshold=675 -funroll-loops`, gpu_vm's own inlining factor. Round 4's -O2 ran while
the console echoed the rest of the typed script: left out.) Bare-metal CoreMark on gpu_vm 57.66, 57.42, 57.09.

| | lean, as before (-O2) | lean, its best | gpu_vm | gpu_vm / lean best | / lean -O2 |
|---|---|---|---|---|---|
| CoreMark, iterations a second | 23.84 | 26.91 (-O2 inl+unroll) | 56.86 | **2.11** | 2.39 |
| gzip of 256 KB, s | 6.714 | 6.650 (-O3 inl+unroll) | 3.299 | **2.02** | 2.04 |
| gunzip, s | 0.742 | 0.742 (-O2) | 0.458 | **1.62** | 1.62 |

| | RISC-V instructions | lean ns an instr | gpu_vm bundles | ns a bundle | RISC-V instr a bundle | passes ended on a full cache |
|---|---|---|---|---|---|---|
| CoreMark, an iteration | 270.6k (-O2), 243.2k (best) | 155, 153 | 113.3k | 155 | 2.39 (-O2) | 28% (6,842 bundles a pass) |
| gzip | 39.71M (-O2), 40.48M (best) | 169, 164 | 16.00M | 206 | 2.48 | 97% (2,205) |
| gunzip | 4.685M | 158 | 2.18M | 210 | 2.14 | 70% (3,408) |

(RISC-V instructions by `rvc_cpu --count-from` on `lean_bench2`; bundles by `passes.py`.) **CoreMark and gzip now
reach 2x the lean tick, gunzip does not (1.62).** A CoreMark bundle costs what a lean instruction does (155 ns) and
carries 2.4 of them; gzip's and gunzip's bundles cost a third more (206-210 ns): their passes end on a full write cache
(97% and 70%) after 2,200-3,400 bundles against 6,800 on CoreMark. The write cache under scattered stores is the
limit on memory-heavy code now; at CoreMark's cost a bundle gzip would be 2.7x and gunzip 2.2x.

Earlier the same evening, the compiler of `0179186` (124.8k bundles a CoreMark iteration; the same tick): CoreMark
2.01x, gzip 1.76x, gunzip 1.68x (rounds lean, gvm, gvm, lean: 21.58 / 43.49 / 43.54 / 21.62 it/s, gzip 7.701 / 4.320 /
4.353 / 7.595 s, gunzip 0.942 / 0.557 / 0.553 / 0.924 s; RISC-V -O2 only).

### The boot, to the same point on both

gpu_vm's root on the ROM as the lean machine's (`gpu_vm/userland/romroot.sh`: romfs through MTD's phram, no initramfs
to unpack; the same busybox system as the system test). Wall time from the kernel's first line, alternating:

| | to "Run ... init" | to the shell |
|---|---|---|
| lean (its kernel, root romfs on phram) | 1.589, 1.538 s | 1.634, 1.584 s (`/emuinit` starts the login shell; its profile then lays out workers) |
| gpu_vm, root on the ROM | 1.523, 1.596 s | 3.63, 3.78 s to the prompt (busybox init, rcS: mounts, hostname; 1.26 s of the machine's time) |
| gpu_vm, root in the initramfs (`rootfs.sh`'s 10.6 MB kernel) | 3.77 s | 5.51 s |

The kernels' parts are now as long (gpu_vm 0.67 s of its own clock, 9.9M bundles to the bench's init.c with its
kernel, 87% of the passes ended on a full write cache at 2,000 bundles a pass); the lean kernel does more (networking,
the machine's drivers). What follows init is not alike: gpu_vm's init, rcS and the shell are about ten executions of
the 2 MB busybox, each loaded whole into RAM (bFLT); the lean `/emuinit` is one small program.
