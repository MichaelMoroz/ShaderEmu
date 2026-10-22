# The machine's programs on gpu_vm

Every user program and game of the RISC-V machine (`linux/*`, `programs/mc`) built for gpu_vm's Linux, from the same
sources and by the same build scripts, and the ROM image and runner to try them on `gvm_cpu`.

| | |
|---|---|
| `build.sh [script ...]` | the RISC-V build scripts (`linux/*/build.sh`, default all of them in order) run from a copy of the repository (`~/gpu_vm/apps-repo`) whose `linux/userland/toolchain.sh` is gpu_vm's; the programs into `build/gvm_apps/root`, logs in `~/gpu_vm/apps-logs` |
| `toolchain.sh` | gpu_vm's stand-in for `linux/userland/toolchain.sh`: `rv32-cc`, `rv32-c++`, `riscv32-linux-{gcc,g++,ar,ranlib,nm,objcopy,strip,ld,size,...}` over `gpu_vm/userland/bin/gpuvm-linux-cc` and LLVM's tools |
| `fix/*.sh` | what a build script must do otherwise for gpu_vm, as a `sed` on the copy (run by `build.sh` before the builds) |
| `mc.sh` | the worker cores' tests (`mctest`, `lanetest`, `steptest`: `linux/nommu/root.sh`'s part for RISC-V) |
| `image.sh` | a romfs ROM of `rootfs.sh`'s tree, `build/gvm_apps/root` and the RISC-V image's data, with a kernel (`romroot.sh`'s, with the machine's drivers) and a device tree: `build/gvm_apps/img` |
| `rcS` | the image's start: rootfs.sh's, a 32 MB `/tmp` and a `/persistence` in memory |
| `run.sh [cpu\|gpu] 'cmd' ...` | types commands into that image on `gvm_cpu` (or the card's `gvm_gpu`) and prints the console |
| `hlsl.sh` | the HLSL compiler (its IR run, `hlsl_interp.c`), `hlslc`, `hlslview`, `hlsl_bench` |
| `romfs_extract.py` | the RISC-V image's data out of its ROM (`~/gpu_vm/riscv-rom-data`, which `image.sh` lays under the rest) |
| `test_all.py` | `tools/test_all.py`'s cases typed into `image.sh`'s machine on `gvm_cpu`, checked by its rules |
| `bugs/` | compiler bugs met on the way, each a small file with how to reproduce it |

In WSL:

    bash gpu_vm/userland/apps/build.sh                         # everything (about 10 minutes)
    bash gpu_vm/userland/apps/build.sh nanox/doom.sh           # one program
    KOUT=~/gpu_vm/linux-build-apps W=build/gvm_apps/kernel bash gpu_vm/userland/romroot.sh   # the kernel, once
    bash gpu_vm/userland/apps/image.sh                         # DATA=0: without the games' data
    bash gpu_vm/userland/apps/run.sh 'NXVIEW_SUM=1 nxview --decode /usr/share/picture-fox.png /tmp/a.ppm'

Builds side by side: `GVM_APPS_REPO`, `GVM_APPS_WORK` and `GVM_APPS_LOGS` name another copy, work folder and logs.

## How a program is made for gpu_vm

- **The same build script.** `build.sh` copies the repository (rsync), writes its `linux/userland/toolchain.sh` as
  one line that sources `gpu_vm/userland/apps/toolchain.sh`, and runs the scripts there. A script's `$WORK` is
  `~/gpu_vm/apps-work`; what the RISC-V builds downloaded once (games' demos, meshes) is taken from their folders.
- **The compiler driver** filters what means nothing to clang or gpu_vm (`-march`, `-mabi`, gcc's `--param` and tree
  passes, `-flto`, `-fno-toplevel-reorder`, a text segment's address, `-fno-pie`: gpu_vm's programs are PIE,
  globals addressed from the bundle), adds `-ffunction-sections` and `-mllvm -gpuvm-spec-window=0` (see the bugs) and keeps every link's ELF beside the
  bFLT as `PROG.gdb`, which its `nm` reads. C++ is clang++ with libstdc++ 13.3's headers and the RISC-V build's fixes
  of them (`linux/tdawn/cxx`), no exceptions or run-time types, as on RISC-V (a program brings its runtime:
  `linux/tdawn/cxxrt.cpp`).
- **Math** (`gpu_vm/userland/README.md`, "libm"): `-O2` with the sysroot's libm on the card's exact ops (the same
  sums on `gvm_cpu` and the card; `fmad` formed always, sin and cos of one value one `sincosf`). Quake and
  Half-Life link `-lmfloat` (their double sin, pow ... at float precision). `GVM_MATH` adds flags to every
  compilation (default `-ffp-contract=fast`).
- **The sources.** What differs on gpu_vm is in the RISC-V sources and patches, behind `__gpuvm__`, so that the
  RISC-V machine compiles what it did. The conventions (`programs/mc/mc.h`):

| RISC-V | gpu_vm |
|---|---|
| the pause hint (`.word 0x0100000f`, `mc_next_pass`) | `wait` (a user's wait ends the pass; a worker sleeps until its job word changes) |
| `fence.i` (`.word 0x0000100f`, `mc_end_pass`) | `sync` |
| `ebreak` (a worker parks) | `break` |
| `ecall` | `syscall` (`MC_ECALL`); the same registers by name (a0-a7) |
| `rdcycle`, `rdinstret` / `rdtime` | `mfsr rd, cycle` (bundles) / `mfsr rd, time_lo` (100 kHz) |
| `amoor.w`, `amoadd.w` | `amoor`, `amoadd` |
| a device address 0x86000000 + x | x (`GPU_PHYS` 0, the control words 0x01000000; `/dev/gpu`'s offsets are the same) |
| a worker's entry: `la gp, __global_pointer$` | none (no global pointer) |
| lanes, the field, steps (CSRs 0x0b8-0x0ba) | none: 0, so `mcw_lanes`, the field and the steps make plain calls, as on `rvc_cpu` |
| `real_vfork` (asm) | musl's `vfork` |
| the NES's 6502 translator (RISC-V code) | not used: the interpreter (as `NES_CPU=interp`) |

- **A half or a word stays in its address's word** on gpu_vm (`isa.h`): a `uint16_t`/`uint32_t` read through a cast of
  an unaligned pointer, which RISC-V allows, gives the bytes past the word as 0 and nothing says so. The compiler
  splits only what it does not know to be aligned, and a cast tells it the opposite. The NES read its 6502 words so
  (`nofrendo.patch`: byte by byte under `__gpuvm__`); look for such casts first when a program's sums are wrong.
- `gpu_vm/userland/flat.ld` names the ends GNU ld names (`etext`, `__DATA_BEGIN__`, `edata`, `__executable_start`).
- Hand-written assembly is rewritten in gpu_vm's (`mcwx_setjmp`: ra, sp, s0-s23; Quake III's and Half-Life's call
  on a stack; Half-Life's renderer thunks).

## Not ported

- **OpenTTD**: its C++ needs exceptions and the whole of libstdc++ (the RISC-V build makes its own GCC). gpu_vm's
  compiler emits no unwind information and there is no C++ runtime library: it waits for both.
- **TinyCC** (`cc`, `tcc`) makes RISC-V code: it needs a gpu_vm back end (the IDE's C builds and `cc-bytes` with it).
- **The HLSL compiler** runs its IR instead of making code (`programs/hlsl/hlsl_interp.c`, `HLSL_INTERP`, on for
  `__gpuvm__`): each operation as the RISC-V code makes it, a step whole (no lanes). `hlsl.sh` builds libhlsl,
  `hlslc`, `hlslview` and `hlsl_bench`; the fractal's sum is the host's (`3614e8bb`).
- The ports of `linux/ports` (dropbear, curl, ...) are not tried yet.

## Checks

See the status table in the agent's report and `HANDOFF.md`; headless on `gvm_cpu` (no display): fptest; nxview's
two sums (`72ae6c0d`, `95b8fd40`); libmedia's sounds (`518c5ac8`, `aa07a2e5`, `c714a845`); MicroPython; unarc
against unzip; open's kinds: all the RISC-V machine's.
