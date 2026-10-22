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
  globals addressed from the bundle), adds `-ffunction-sections` (see the bugs), force-includes `nommu_fork.h` as the RISC-V machine does (fork is vfork) and keeps every link's ELF beside the
  bFLT as `PROG.gdb`, which its `nm` reads. C++ is clang++ with libstdc++ 13.3's headers and the RISC-V build's fixes
  of them (`linux/tdawn/cxx`), no exceptions or run-time types, as on RISC-V (a program brings its runtime:
  `linux/tdawn/cxxrt.cpp`).
- **Math** (`gpu_vm/userland/README.md`, "libm"): `-O2` with the sysroot's libm on the card's exact ops (the same
  sums on `gvm_cpu` and the card; `fmad` formed always, sin and cos of one value one `sincosf`). Quake and
  Half-Life link `-lmfloat` (their double sin, pow ... at float precision). `GVM_MATH` adds flags to every
  compilation (default `-ffp-contract=fast -fapprox-func`: the card's special functions, the user's choice). A program is installed with the RISC-V image's stack (`flat_stack.py`).
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

## Status (22 October 2026)

`gvm_cpu`: `test_all.py` on `build/gvm_apps/img` (32 cores, 255 where a case asks), the last full run 102 of 131 cases,
since then the terminal and ports fixes (below). Card: `card.py` (`rvc_harness --gvm`, RTX 5070 Laptop, `--wall-scale 2`).
Flags: every program `-O2`/`-O3` as its RISC-V script says, plus `-ffp-contract=fast -fapprox-func -ffunction-sections`
(`GVM_MATH`, `GVM_CFLAGS`); Quake and Half-Life `-lmfloat`. A float output is compared with gvm_cpu's reference within
the tolerance named (`gvmcmp`, `NEAR` in `test_all.py`); an exact sum where the program's logic must not drift.

| program | builds | gvm_cpu | card | check (tolerance) |
|---|---|---|---|---|
| mctest, lanetest, steptest | yes | PASS (3, 255) | PASS | as RISC-V (no lanes: plain calls) |
| fptest | yes | PASS | PASS | 0 of 15 wrong |
| Nano-X, nxbar, glxgears, demos | yes | nano-X runs (frames counted) | the desktop (platform agent) | (no check of their own) |
| desktop: nxview, nxpaint, nxask, nxmon, nxfiles, nxweb, open, unarc | yes | PASS (nxview, nxpaint, nxask, nxmon, nxfiles x3, nxweb x4, open-kinds, unarc) | nxview, unarc PASS | exact sums (integer decoders) |
| nxfiles --kinds | yes | PASS after the nxfiles fix (static `lower`) | - | as RISC-V |
| libmedia: media_test, nxplay, nxvideo | yes | PASS (media-sound, -mpeg, -avi, nxplay, nxvideo x8) | media-sound, media-avi PASS; nxplay's WAV, FLAC, MOD sums the RISC-V's (MP3 a6c2aba3: floats) | FLAC, MOD, WAV, MJPEG exact; MP3 by SNR >= 30 dB |
| nxray | yes | PASS | PASS | last picture PSNR >= 35 dB (RAY_SAVE), 4 sum lines |
| nxpath (8 scenes) | yes | PASS (all) | nxpath, cornell PASS | pictures PSNR >= 35 dB (NXPATH_SAVE) |
| nxphys | yes | PASS (6 cases) | wall, tower PASS | heights within 2 %, speeds 15 %, depth 10 mm |
| nxpinball | yes | PASS | not run | gpu_vm's own: score 18060 state 51ec1e4b (floats take the ball elsewhere) |
| HLSL: hlsl_bench, hlslc, hlslview | yes (IR run: `hlsl_interp.c`) | PASS | PASS | picture PSNR >= 35 dB (host's sum 3614e8bb on gvm_cpu with exact libm) |
| nxide | yes | js, py, problems, tree, storm PASS; C fails (tcc); hlsl too slow (20 pictures) | - | as RISC-V |
| QuickJS, MicroPython | yes | PASS | qjs PASS | as RISC-V |
| Doom | yes | PASS (doom-1/2/3, told, rest) | PASS (all five) | exact state digests (integer game) |
| NES | yes (interpreter) | PASS (8 cases) | thwaite-120, -lists, nova-120 PASS | exact RAM and picture sums |
| ClassiCube | yes | meshes, meshes-workers, kept, pack PASS | meshes PASS | pack: 30-42 textures (speed decides how many before "steady") |
| Micropolis | yes | PASS | PASS | exact sums |
| Tiberian Dawn | yes | PASS (300, 1300, paths, won) | 300, 1300 PASS | exact state sums |
| Red Alert | yes | every case the RISC-V machine's state sums but ralert-movie (5fed4b4: a mix from the ROM mapped writable, the iconset header read by bytes) | soviet-300 PASS | exact state sums |
| Quake | yes (-lmfloat) | quake-light PASS (QUAKE_STATS_FRAMES); walk, exit, particles fail on gvm_cpu's clock pacing | quake-light PASS | as RISC-V; walk reaches 480 687 24 with commands typed ahead |
| Quake III | yes | workers, inline, bots, bots-serial PASS; server: digest by load time | workers/inline: 237 of 237 lines the same, fewer than 500 (load time) | as RISC-V where QUAKE3_ENTER fixes the entry |
| Half-Life: Uplink | yes (-lmfloat, 1 MB stack) | plays through the fight, HL_ROTATION=check 0 wrong; GVM_RACES 0 or 7 undone (light styles) | not run | HLGL_PICSUM 656aa416 (GPU addresses moved: not RISC-V's) |
| terminal: nano, mg, htop, dialog, sl, file | yes | PASS | not run | as RISC-V (TERM=linux) |
| mc, mcedit | yes | mc's subshell says fork () = -1 | - | - |
| ports: pcre, lockfile, socat, thttpd, curl, dropbear, sftp, at | yes | PASS (dropbear: uname -m gpuvm) | pcre not reached | as RISC-V |
| iproute2 | yes | all but /proc/net/stat/arp_cache (the kernel's) | - | - |

## Bugs found (gpu_vm/userland/apps/bugs)

- `far_call.c`: a call farther than 2 MB inside one section is an assembler error (QuickJS); every function a section.
- `spec_load.c`: the fill across blocks hoisted a guarded load (QuickJS faulted); fixed in 7764c11.
- `dtor_debug.cpp`: -g crashed on a deleting destructor (Vanilla Conquer); fixed in 7764c11.
- `weak_undef.c`: under -fPIE an undefined weak function's address is the load address, not 0 (nano's SIGILL; nano
  links its weak pthread functions in: `fix/terminal.sh`). Open.
- Not the compiler: unaligned `uint16_t` casts (the NES), a variadic call through a thunk taking register arguments
  (Quake III: gpu_vm passes `...` on the stack), a 64 KB stack (Half-Life: `flat_stack.py`), a stack buffer printed
  after its function returned (nxfiles).

## Not ported

- **OpenTTD**: its C++ needs exceptions and the whole of libstdc++ (the RISC-V build makes its own GCC). gpu_vm's
  compiler emits no unwind information and there is no C++ runtime library: it waits for both.
- **TinyCC** (`cc`, `tcc`) makes RISC-V code: it needs a gpu_vm back end (the IDE's C builds and `cc-bytes` with it).
- **The HLSL compiler** runs its IR instead of making code (`programs/hlsl/hlsl_interp.c`, `HLSL_INTERP`, on for
  `__gpuvm__`): each operation as the RISC-V code makes it, a step whole (no lanes). `hlsl.sh` builds libhlsl,
  `hlslc`, `hlslview` and `hlsl_bench`; the fractal's sum is the host's (`3614e8bb`).
- Quake's walk, exit and particles cases and Quake III's server case on gvm_cpu depend on the clock's pace against the
  machine's speed; the card's runs of them are to be made. Red Alert's movie check.

## Checks

`test_all.py` (gvm_cpu) and `card.py` (the card) run `tools/test_all.py`'s cases with gpu_vm's changes (its module text):
skipped are the cases that read the host (`--host`, `--fixed-dt`); `OVERRIDES` keeps gpu_vm's own values where they
differ for a reason that is not a fault; outputs that are floats go to `gvmcmp` against `build/gvm_apps/refs`
(`--save-refs` on gvm_cpu, then `image.sh` puts them in `/usr/share/gvm-refs`).
