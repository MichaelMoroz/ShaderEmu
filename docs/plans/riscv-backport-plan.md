# What gpu_vm's week can give the RISC-V machine (28 October 2026)

gpu_vm (this branch's `gpu_vm/` from 21 October, then the GPU-VM repository from 25 October) beats the RISC-V machine
in the harness, but in VR the two are level: a Unity round's work ends when core 0's write cache fills, and gpu_vm's
cheaper instructions do not change how many stores fit in a round. What it found on the way does carry over:

- bugs in the sources both machines build;
- programs and kernel paths that store too much;
- copies that need not go through the write cache;
- a few tick, harness and toolchain facts.

This plan takes those to the RISC-V machine. It has no measuring step: each item is taken for the reason gpu_vm
measured, and checked only for correctness (the program's own sums, `tools/test_all.py`, `--races` for anything workers
touch). Items are in order of what they give a VR round, the ISA fork last.

## 0. This branch's fixes to shared sources, onto `main`

`custom_isa` holds all of `main` and 334 commits more. `main` stops at 21 October (3e89e5f; b935841 on origin), so all
RISC-V work since lives only on `custom_isa`:
- features: lanes in steps (d31b4b7), the HLSL compiler (e4cf58d), Quake III's server (535c58c), Half-Life's drawing
  on a worker (64438ab), OpenTTD's viewports (96b83fc), the LAN as events with `EmuNet.cs` (9e09037), nxedit's Markdown
  (07d38c8), the prebuilt (3ca3e3f), 1d858a2 and 0362c59;
- harness: 9f62350's RISC-V logs, `--unity` (cc31879), every pass drawn (eb13fc3), and `--save-state` (6b644e8, e612c57).

Where this plan says "already on RISC-V", it means `custom_isa`. The rule for `main`: everything of `custom_isa`
outside `gpu_vm/` and `unity/gpuvm/` that is not behind `__gpuvm__` goes over (a merge, not cherry-picks). The local
`main` is behind `origin/main` too.

Among those, fixes to sources the RISC-V machine builds:

| commit | what | why it is RISC-V's too |
|---|---|---|
| bacc205 | Half-Life's variables on 16 bytes each (`linux/halflife/build.sh`); the NES's `frame_paced.due` a volatile struct of its own texel (`osd_shaderemu.c`) | the RISC-V commit writes whole texels too: two cores storing into one texel in a pass lose a word (20-21 texels and 1-3 changes undone in hldemo1 under the race check) |
| 632e982 | Half-Life's server worker's statics in texels apart (`world_ahead`, the loopback's `get` and `send`, `net_drop` one a side) | the same: `svc_bad` and overflows 10 to 30 s into the level on the card |
| 830fbdb | mcwx gives a call to the lowest free worker, not round-robin | busy workers together in the geometry: 128 busy cores scattered cost 0.332 ms a pass against 0.225 in a run (`gpu_vm/tick/WAVES.md`) |
| cf99ef8 | libmedia's `media_lcw` with no end given, and Red Alert's use of it | a signed pointer difference that goes wrong below 2 GB |
| 4e58aa5 | `nxfiles --kinds` printed from a dead stack buffer | plain bug |
| b160c1e | `programs/mc/copyrace.c` | the test of a `read()` buffer summed by waiting workers |
| 5f54d7d | nxbar: a window restored shows as restored | plain bug |
| 1d858a2 | Half-Life: no freeze in hldemo1's first corridor, no death at a level change (`SE_LevelSettle`) | AGENTS.md describes it as there; it says it takes over "main's deeffd3", which no ref here holds |
| 56ff5ab | Red Alert's Soviet score screen read by bytes, and the `ralert-won-soviet` case (state `01e0dd60`) | under `SHADEREMU`, not `__gpuvm__`: harmless here, and the case is new |
| 0362c59 | Quake III's snapshot error prints its numbers | a diagnostic |

The rest of `custom_isa`'s changes to shared files are behind `__gpuvm__` or are gpu_vm's own (word-bound loads read by
bytes in Red Alert, Tiberian Dawn, Doom and the NES; `mcw_rest_on`'s declared workers; the HLSL back end).

`linux/prebuilt` was last made on 24 October (07d38c8, 1d858a2), so the RISC-V image has none of the above even on
this branch: the prebuilt is made again after them (`pwsh tools\make_image.ps1 --save-prebuilt`). Its kernel
(`linux/prebuilt/nommu/Image`) is older still, made at b935841 on 21 October: it lacks 9e09037's network driver polled
from the tick (an idle shell 229 to 157 thousand instructions a second). The rebuild must make `Image` too.

What the merge brings that the commits' subjects do not say (read from their code):
- **07d38c8:** besides the editor's Markdown reader, links followed to headings and other files in the same tab
  with Back; the repository's `docs/` (not `plans/`) at `/usr/share/doc/shaderemu`, with a Docs folder and icon on
  the desktop; and the browser engine's name tables stored in place (`char name[N]` in `css.h`, `web.h`), not as
  pointers: 682 relocations to 233 at every start of a program using the engine.
- **cf99ef8:** Red Alert's movie player drew a skipped frame already freed and being reloaded; a skipped frame now
  waits for its worker's unpacking (`workers_lcw_wait`). A use-after-free race with a worker on any machine.
- **96b83fc:** OpenTTD's station names no longer stay after their windows close, and lines no longer stay behind
  when scrolling.
- **64438ab:** Half-Life's studio hit hulls on every core (the server worker's bullets went through monsters),
  `glPolygonOffset` working (decal flicker 21.5 to 2.7 changed pixels a frame), a seated hand's shots from the hand,
  and `CL_DrawString` no longer dropping characters of a say line.
- Harmless on RISC-V and coming with it: Half-Life's save and restore read their fields through `memcpy` (5b36b37's
  `SR_Read`); mc's `ecall`, `ebreak` and the pause hint through macros that give the same words, with `mcw_lane_stub`
  aligned on 16 (`libmcw.a`'s bytes and so `cc-bytes` change); OpenTTD's explicit `TryGetBaseSetFile` instances
  (`host_sum.sh`'s sums to be checked after).

One hazard: `tools/make_linux_image.py`'s `documentation()` (lines 197-223) opens `gpu_vm/GPU_VM.md` and
`gpu_vm/platform.md` and writes a gpu_vm section into the image's docs index. Without `gpu_vm/` on `main` the image
build fails there: the section goes, or depends on the folder being there. Only that file and `programs/wl` need
`gpu_vm/`.

Two more correctness items, not yet anywhere on RISC-V:

- **`access_ok` checks a user range is in RAM** (25e38c0): nine system calls given a bad pointer said EFAULT, not an
  oops. The RISC-V no-MMU kernel selects `UACCESS_MEMCPY`, whose asm-generic `__access_ok` returns 1: a hook defines it
  for the two RAM ranges. What the RISC-V tick does with a kernel access outside RAM is not checked. The same hook
  gives **`get_user` and `put_user` one access when aligned** (gpu_vm's `arch/gpuvm/include/asm/uaccess.h`):
  asm-generic's path is safe for any alignment, so the RISC-V kernel stores a user `u32` as four `sb` and three `srl`
  (`__se_sys_getresuid` in the image) and loads one in about 7 instructions. Every futex word, ioctl argument and
  signal frame's word goes through it.
- **The lazy list dropped a list** (4edb861): one submitted in a frame where none was drawn was never drawn. The fix in
  `src/gpu.h` is under `GVM_RAM`, so RISC-V's opt-in `RVC_LIST_LAZY` (a4ff4d4) still has it: fix it or take it out.

Check: `test_all.py` and `--races` on `main` as on `custom_isa`.

## 1. Fewer stores in the programs

Each is a change to a source both machines build. GPU-VM's commit applies to ShaderEmu's file as it is: the file before
each commit is ShaderEmu's byte for byte, except where noted.

| program | GPU-VM commit | what | gpu_vm's figure |
|---|---|---|---|
| Red Alert | 6c43f4a (`linux/ralert/vanilla-conquer.patch`) | `CellRedraw` cleared by the rows with a cell flagged (a row of 128 cells is one texel), not all 2 KB each frame the map is drawn | that memset filled core 0's write cache 0.6 times a game frame (128 of RISC-V's 336 texels); full ends 5.12 to 4.76 a frame |
| nano-X | 002bcdf (`linux/nanox/scr_shaderemu.c`) | a filled shape's rows (ellipse, polygon, arc) kept and drawn as one command, a rectangle or a 1-bit mask, not three texels of command a row | Pinball 4.21 to 11.02 frames a second, hitches 0.54 to 0 a second. The `string_words` part is gpu_vm's (its GPU memory at address 0) and harmless here |
| Doom | a06f225 (`doom_gl.c`, `doom_fast.c`) | a level's wall pictures made ahead on the workers, no 64-bit divide in `clip`, texture names' answers kept | core 0's level load 3.68M to 2.86M; the load's freeze 175 to 143 host frames |
| Quake III | 75c98c8, f1e36dd (`programs/mc/handoff.c`, `mc.h`, `client_shaderemu.c`; the latter differs from ShaderEmu's by f1e36dd's own lines) | `mc_publish()`: the compiler stored a count before its entry, and a pass that ended between them gave the worker an old snapshot; the client's worker waits for both texels of its list | `n < cg.latestSnapshotNum` in 293 of 20,000 frames on the card, 0 after. A RISC-V pass can end between two stores too, and GCC may order them the same way |

Two more, not straight ports:

- **Every other "data, then a ready word" hand-off** between cores wants `mc_publish` as Quake III's did. `mcw_post` is
  safe (the call orders the stores); a flag a program keeps for another core itself is not. Not yet audited.
- **nxray's cost floor only for workers with lanes** (GPU-VM `nxray.c`, `least[]`). The floor of a sixth of a pass a
  texel is a lane's rule, but it was applied to every worker. `rvc_opt`, the world's tick, has no lanes, so on it every
  share came out at the floor and even. On gpu_vm the dearest share went to 54-58 thousand instructions and a picture
  from 124 passes to 110. This is the app's own path, not only its test.
- **nxray with one picture out** (c13b2cb, `__gpuvm__` only): 38.4 to 40.6 pictures a second, the worst picture's
  latency 111 to 55 ms. With the floor above, the default on a tick without lanes.
- (`mcw_post` is safe only where `mcw.c` is compiled apart: see section 2b for the NES.)
- **Floats in floats: libm.** RISC-V's musl `sinf`, `cosf`, `expf`, `logf`, `powf` work in doubles, which are library
  calls (`docs/system/cc.md`); Quake III makes its own sines to avoid them. gpu_vm's float-only libm and `libmfloat`
  (62af8c2, c4d9e26; GPU-VM `userland/musl/src/math/gpuvm`, `userland/musl/mfloat`) are 14 to 58 ops a call against
  788 to 1,898 soft-double instructions: `AnglesToAxis` 9,998 instructions, `SinTable` 1.75M, `GammaTable` 0.95M. Its
  C goes into RISC-V musl's `src/math` (with no `fmadd`, or through the general path until section 6), and Quake and
  Half-Life link `-lmfloat`. Checked by the libm table's tolerances and the programs' float sums, which move. The
  doubles' `floor`, `ceil`, `trunc` and `round` done on the bits go with it (`userland/musl/src/math/gpuvm`: 571 to 37
  bundles a call).
- **Quake III's scene job is the frame's chain** (PROFILE_Q3.md sections 3, 6, 7): its 45 thousand instructions fill
  94% of a frame's 5.83 x 8,192, and it ends last in 97% of frames. Posted earlier (it starts 3.3 passes into the
  frame) or split over two workers.
- **Profile-guided builds** (b86e265: Quake III 82.4 to 85.5 fps on gpu_vm). RISC-V has GCC's `-fprofile-use` in
  `linux/ralert` and `linux/tdawn` only; Quake III, Half-Life, Doom and Quake have none.
- **Half-Life's `R_SceneCopyTo` storing only the words that changed** (`gpu_vm/IDEAS.md` row 32, never committed;
  `linux/halflife/gl_kept.c`): +5.6% on gpu_vm, inside its layout noise. To be written again here, with
  `game_par.py halflife` at 0 undone.

Check: the programs' own sums (`DOOM_SUM` at tic 260 `bfc19200`, Pinball's state at frame 600, Red Alert's state sum),
their `test_all` cases and `--races`.

## 2. Copies that do not go through the write cache

A RISC-V program's `memcpy` is musl's own: a 64 KB copy is 4,096 texels of stores, 12 passes ended full. Only the
kernel asks for the commit's copy (`memcpy_hook.py`: 1 KB or more, on a texel, in the linear map); a program can borrow
workers (`ui_bulk.h`) but cannot ask for the copy itself.

gpu_vm's musl `memcpy` asks for the commit's copy from 2 KB up, so the texels never enter the cache. Quake's copies of
2 to 64 KB, nearly all into GPU memory, made 40 of `memcpy`'s 57 thousand stored texels in its timedemo. On gpu_vm:
Quake 1.022x, Quake III 1.055x with a sync (GPU-VM RESULTS.md, 25 October); without a sync while core 0's copies of the
pass stay under 192 KB, Quake 1.010x and Quake III 1.020-1.067x more.

For RISC-V:

1. **A copy a program can ask for.** The RISC-V standard keeps CSRs 0x800-0x8FF for custom user-mode registers: the
   tick takes the commit's copy (source, destination, length, go) at four of them in user mode, as it takes 0x0b0-0x0b3
   from the kernel, with the same alignment rule. No new instruction, and nothing for the compiler.
2. **musl's `memcpy`** asks for it from 2 KB up, when both ends are on a texel and in RAM (GPU memory is RAM), and the
   pass ends there as the kernel's copy does. A program on an older tick traps on the CSR, so the image and the tick
   go together. Its statics, if any, in a texel of their own (49c688e). Its aligned path in rounds of 64 bytes as
   gpu_vm's (5ce8b3f: 1 KB 396 to 286 bundles), a small saving.
3. **Later, the copy without the pass's end**, as gpu_vm's `GVM_UOP_ASYNC`: only if the tick can stop a core's own
   access to a range still in flight without a test in the fast loop. gpu_vm's test in the fast loop (`oprange`) cost
   Quake 3.7%, so it is not there; its general step makes those accesses. Other cores and the GPU read what a copy
   writes too: the kernel drains a task's copies before it returns to user mode (5b36b37, `TIF_GVM_DRAIN`: without it
   hldemo1's models were damaged, 8 of 480 slices read early).
4. **The kernel's small copies.** RISC-V's `arch/riscv/lib/memcpy.S` (5.17) copies a byte at a time when the two
   pointers' offsets in a word differ. gpu_vm's `linux/arch/gpuvm/lib/copy.c` copies 64 bytes a round, and words built
   by shifts when the offsets differ: Doom's core 0 2.6% fewer instructions, its card time 5.6% less. A `memcpy_hook.py`
   step for the misaligned path does the same here.

Check: `programs\copytest` (`sum 0e7f76c3`) on `rvc_cpu` and the shader machine, `test_all.py`, Quake's and Quake
III's sums.

## 2b. The compiler's settings

Read from gpu_vm's compiler code and wrappers (`llvm/Target/GPUVM/*.cpp`'s options, `userland/bin/gpuvm-linux-cc`,
`userland/apps/toolchain.sh`) against what every RISC-V build passes.

- **Every RISC-V program is PIE, whatever its build says.** `rv32-cc` and `rv32-c++` drop `-fno-pie`, `-fno-PIE`,
  `-no-pie`, `-fno-pic` and `-fno-PIC` and always compile `-fPIE` (`linux/nommu/setup.sh:57`, 65-66), so the
  `-fno-pie` of Quake, Quake III, Half-Life, Doom, Tiberian Dawn and the rest does nothing. Compiled with the
  toolchain's GCC (28 October 2026): under `-fPIE` a global declared `extern` is reached through the GOT
  (`la a5,ext_var; lw a0,0(a5)`), a load more at every use; with `#pragma GCC visibility push(hidden)` before it, it is
  `lw a0,ext_var`, PC-relative with no load. A global defined in the same file is PC-relative either way.
  `-fvisibility=hidden` does not cover `extern` declarations; the pragma does. gpu_vm's like of this (`-gpuvm-fnrel`)
  cut globals reached through a load, 5.3% of Quake III's worker bundles. So the wrapper force-includes a header with
  the pragma, for every program; the count of GOT entries before and after says how much. Not checked: whether the
  linker relaxes GOT loads already.
- **Inlining.** gpu_vm inlines 3 times LLVM's threshold (`GPUVMTargetTransformInfo.cpp:30`; Quake 8): fewer calls are
  fewer prologue saves, which are stores (5.2% of Quake III's workers' cost, 7.3% of Half-Life's, 14.7% of Half-Life's
  core 0; IDEAS 28). Quake's tuned set: -2.37% bundles, +1.4% fps in the Unity measure, code +30%. GCC:
  `--param max-inline-insns-auto=60 --param max-inline-insns-single=400 --param inline-unit-growth=80` (as imgui and
  pathtrace already have; Tiberian Dawn and Red Alert go further), on the compile and the link line (the params act in
  LTO's link). Quake, Quake III, Half-Life, Doom and the NES are on GCC's defaults.
- **Half-Life is still `-O2 -fno-tree-vectorize`.** The others' measured recipe is `-O3 -fno-tree-vectorize
  -fsched-pressure -funroll-loops` (Quake -6% instructions a frame, Doom -3% a tic and -11% a frame, the NES -5 to -7%).
  Checked by `se_sum_exact`'s sums and `game_par.py halflife`.
- **The NES may post a job before its data.** It compiles `mcw.c` and `mcwx.c` inside its `-flto -flto-partition=one`
  link (`nes/build.sh:38,48`): once `mcw_post` is inlined, GCC may move the program's stores past the volatile `seq`
  store, which C allows. That is Quake III's `mc_publish` bug. `mcw_post` gets a compiler barrier before `seq`
  (`asm volatile("" ::: "memory")`, no instructions), which makes the section 1 note true for every build.
- **Not to take:** `-msave-restore` (a call and a return a frame for the same stores), `-fallow-store-data-races`
  (stores into texels workers write), gpu_vm's bundle-filling passes, its approximate special functions.
- **To check once:** `readelf -S` that the `ALIGN16` sections are on 16 (GNU objcopy takes the patterns; not seen on a
  built ELF).

## 3. The kernel: starts and the boot

Smaller for a frame, but each is a stall a player sees (a program's first window, a level, the boot).

| item | GPU-VM source | what | gpu_vm's figure | RISC-V |
|---|---|---|---|---|
| workers laid out at boot | c519897 | `workers_layout`'s three `msleep(60)` moved from the first `WORKERS` ioctl to delayed work at the driver's init | nxweb's page 0.44 s to 0.24 | the same sleeps in `linux/kernel/shaderemu_gpu.c`, paid by the first program with workers and by the window server at the pointer's first move |
| the struct pages copied | `linux/patches/0003-gpuvm-memmap-copied.patch` | made once and copied by doubling memcpy (the commit's copy), nothing stored when freed | boot to the shell 6,448 to 3,642 passes with 0004 and fastrun | RISC-V's `bootpages_hook.py` already takes away each page's second store: 0003 goes on top of it |
| no sysfs for the boot's slab caches and built-in parameters; the boot loader's seed trusted | d7996f4 (`drivers.py`: `mm/slub.c`, `kernel/params.c`, `drivers/of/fdt.c`) | the sysfs folders were a third of the kernel's start | SLUB, no IPv6 and one ramdisk are in RISC-V's config already; the sysfs skips and `RANDOM_TRUST_BOOTLOADER` are not |
| no fault-in of a write's buffer | a004391's second half | `fault_in_readable` reads a byte a page, for nothing with no MMU | the same code |
| no BLAKE2s self-test | `0004-gpuvm-no-blake2s-selftest.patch` | | part of the above | the same |
| a program's memory in blocks | `linux/fastrun.py` | from the free lists and given back in blocks | `alloc_pages_run` walks the zone from its first page, about 35 thousand bundles a call and two calls a start; a program's exit freed page by page, a quarter of a boot's bundles | RISC-V lacks both halves: its `alloc_pages_run` (`linux/nommu/patch_drivers.py`) is the same code. fastrun's is behind `CONFIG_GPUVM`: the guard goes |
| the kernel's config trimmed | `linux/machine.config`, `gpuvm_defconfig` | `BASE_SMALL=1` (smaller hash tables), no `VM_EVENT_COUNTERS` (a store at every page allocation, free and fault); also no `KALLSYMS`, `AIO`, `IO_URING`, `COREDUMP`, a 16 KB log | not measured | RISC-V's `config.extra` keeps them; the two that store at run time are worth taking, then the boot's passes looked at |
| `/dev/zero` a megabyte a step | `linux/drivers/zeroread_hook.py` | `read_zero` cleared a page a step, each a fill that ended the pass | 4 MB 0.0256 to 0.0016 s | `read_zero` is 5.17's, a page a step through `clear_user` |
| relocations a word at a time | `linux/drivers/flatreloc_hook.py` | `load_flat_file`'s loop by words, one bounds check, `calc_reloc` once | 37 to 13 bundles a relocation | RISC-V's programs carry few: Half-Life 48,015, OpenTTD 28,694, the rest under 5,400. Last |

Also small: **a rest with no system call when the program is alone** (82cda8e's first half: the kernel writes whether
another task could run, and `mcw_rest` ends the pass in user mode). RISC-V's `REST` ioctl is 322 instructions a call
and does the zero pool's idle work, which would have to go elsewhere. Low value.

Check: `zerotest` and `tailtest` PASS, `mctest`, the boot to the desktop, `test_all.py`.

## 3b. RAM in one piece: the devices below it

gpu_vm keeps every device of the RISC-V machine, layouts and protocols word for word, at its address less 0x86000000
(GPU-VM `docs/platform.md`, "The device window"): the GPU's memory from 0x00010000, the mailboxes at 0x00c00000, the
control words at 0x01000000, sound memory to 0x01e00000, and RAM one range from 0x02000000. The RISC-V machine's RAM is
two ranges, 0x80400000-0x86000000 and 0x88000000-0x92000000, with the device window between (`docs/system/nommu.md`,
"Memory").

Not a speed-up in itself, but one range of RAM means:
- one memory node for the kernel, so no allocation is limited by the split;
- no band mask spread over two pieces (`ram_bands`);
- no rule that a GPU copy into RAM stays below 0x87e00000.

For RISC-V the window moves below RAM in the same way: the device tree, the kernel's drivers' addresses
(`linux/kernel/shaderemu_*.c`), `programs/mc/mc.h`'s `MC_PAGE_GPU_OFFSET`, the device shaders (`src/gpu.h`,
`sound.h`), the harness's uploads and readbacks, Unity's copies. Every program built against the addresses is rebuilt:
it goes with an image of its own, and a snapshot from before does not load.

## 3c. The devices' textures and draws

- **The ROM as one `uint4` texture.** RISC-V keeps the ROM in four RGBA8 float textures (`_Data_MTD_R/G/B/A`: `src/mem.h:264`,
  `types.h:1542`, `pend_texel` in `mem.h:102`, `gpu.h:132`, `GpuVolumePass.cginc`): a word is a 4-way switch and a
  float-to-byte unpack, a whole texel four fetches (ROM copies, Quake III's draws from the ROM, sound's samples). gpu_vm's
  `_GvmRom` is one integer texture, unpacked once at power-on by a blit (`GvmInitRom`, `EmuMachine.ApplyImage`); its
  device tree is in RAM at 0x1000, so `_Data_DTB_*`'s four textures go too. Not measured on RISC-V.

  Better than gpu_vm's blit: the ROM shipped as one 16-byte texture, with no unpack and no second copy. Tried in the
  world's editor (Unity 2022.3.22f1, 28 October 2026):
  - `R32G32B32A32_UInt` cannot be a `Texture2D` there: Unity says the format cannot be sampled and will not make one.
  - `R32G32B32A32_SFloat` (`RGBAFloat`), saved and loaded again, kept every byte, NaNs of every kind, denormals, -0
    and infinities included. The card reads such a texture's bits exactly: `Load` or `SampleLevel` at a point, then
    `asuint` (`gpu_vm/IDEAS.md` row 30).
  - The project saves assets as text (`ForceText`), and a texture's pixels go into it as hex: 4,096 bytes made a
    9,281-byte file. The ROM is 282 MB (`build/images/nommu/rootfs.bin`), so it does not go in as a saved asset but
    through a `ScriptedImporter` that reads the image file and makes the `RGBAFloat` texture in Unity's own binary cache.
    That replaces `ShaderEmuImages.WriteLanes`' four PNGs and the importer settings that keep them byte for byte.

  At 2,048 texels wide the ROM is 8,600 rows, under the 16,384 limit: one texture of the same size in video memory as
  the four RGBA8 ones together. In the shaders, `_Data_MTD_R/G/B/A` become one `Texture2D<float4>` read by `asuint`,
  one fetch a texel. The harness uploads the same texture.
- **The GPU's picture copied into RAM by a draw of its own.** Every pixel of RISC-V's commit reads `GPU_COPY` and
  branches (`gpu_writeback()`, `main.shader:763`); gpu_vm draws the copy apart (`devices.shader`'s GPUWriteback, the
  destination's rows only). Minor.

## 4. Tools

- **The race check with the card's pass ends.** gpu_vm's `GVM_RACES_NEAR=N` reports a texel another core stored other
  words of within N passes, and `--card-budget` gives a worker the budget the card would. Half-Life's three shared
  texels (632e982) showed only under those, never in `gvm_cpu`'s own schedule. `rvc_cpu`'s `RVC_RACES` has the same
  blind spot: `RVC_RACES_NEAR` in `rvc_cpu`, and `test_all.py --races` runs with it.
- **Long cases in sections.** `gvm_cpu --save/--load` and `test_all.py --sections` keep every case under the minute.
  `rvc_cpu` has `--save-ram` but no load, and `tools/test_all.py` no `--sections`: both to add.
- **The control zones in one draw** (feaddd5): the control words and the cores' stats drawn together, the stats' loads
  unrolled. On gpu_vm 22 to 6.7 us a pass. `rvc_backend11.cpp` draws them apart, and `mc_stat_bits` in `src/gpu.h`
  loads serially in a loop with `continue`. Unity has the same zones.
- **nxray's test sum on the workers** (`shares_sum`): under `RAY_FRAMES` core 0 summed the picture, 94% of its bundles;
  nxrayseg 11,640 to 3,513 card ms on gpu_vm. The benchmark only, so a RISC-V nxray figure is the app's again.

### Capture and the harness's window

| item | GPU-VM source | what | RISC-V today | effort |
|---|---|---|---|---|
| every run captured | add364c (`harness/vm/vm.cpp`) | always on into `build/captures/live-PID`, kept as `last.gvmcap`: each frame's host values, answers, packets and switches, each readback hashed with the console; Ctrl+F5 saves a copy; `--replay-capture` checks every row, with overrides such as `--warp` (a determinism check and the card against WARP). Quake 45 s, 50,030 rows, the same twice | missing: rvc_harness's `--replay` runs ticks in turn from a snapshot, another thing | L |
| input recorded and replayed | a338987 | `--record-input`, `--replay-input`: keys, pointer and typing with their wall seconds | missing | S |
| the window on GLFW | a338987 | the pointer read every frame (Win32 sent moves once a key repeat: a drag with an arrow held reached Quake 20 times a second), flip-model swap at the screen's rate | `harness/core/memview.cpp` is `WM_MOUSEMOVE` and `Present(0,0)` | M |
| ticks alternated in one run | 5fa6f70 (`GVM_TICK_ALT`, `_SHIFT`, `GVM_TICK_PAIRED`) | each frame's tick picked by a hash, the card times kept apart; paired runs every tick each pass and checks they agree | partial: `--replay DIR;DIR` from a snapshot | M |
| segment timing | `GVM_MARK`, `GVM_STATS_EVERY` (`vm.cpp`) | the card's tick time a console mark: two programs in one run timed apart | missing | S |
| `--unity`'s logs | be87943, a2ea5a3 | `--frame-log`'s `.rounds` and `.timed`; `GVM_UNITY_PACE=world` as `EmuMachine.SteadyRounds` | partial: the model, not these | S |
| the WARP stamp | `tick.hlsl.warp_ok`, `gvm.exe --warp --mark-ok`, `gvm12 --check` (RAM's FNV after 20 passes) | the card refuses a tick WARP has not checked against the reference | missing for `experiments/rvc_opt` (`--warp` only) | M |

A likely bug, not verified: under `--unity` or `--tick-rounds N`, rvc_harness moves the clock every round but reads the
console's ring once a frame (`rvc_backend11.cpp:1320`), so a guest may write 255 bytes a round and lose what is past
256 a frame: gpu_vm's c3713e9 (Quake's statistics lines cut). The world reads it every round. gpu_vm's fix is the
guest's (6eac8ac): the console puts at most 255 bytes while both the clock and a word the control pass counts up once
a host frame (`GPU_CLOCK` word 0) are unchanged. On RISC-V `se_con_put` (`linux/nommu/patch_console.py`) counts by
`CSR_TIME` alone: it compares the frame word too, and `gpu_control` in `src/gpu.h` and Unity's control pass count it.

### Measuring and comparing

| item | source | what | RISC-V today | effort |
|---|---|---|---|---|
| the comparison | GPU-VM `compare/` (`cmp.py`, `parts.py`, `short.py`, `report.py`, `next.py`) | 52 parts under `--unity` in sections under a minute; hitches (gaps over twice the median) a second, p95, p99 | it drives RISC-V already, with this worktree's paths; no RISC-V-only run and no hitch figure in ShaderEmu: a `tools/` copy with machine `rv` | M |
| fps at budget, A/B of images | `ab.py`, `grid.py`, `many.py`, `ends_report.py` | fps scaled by the card ms a frame took; two images or ticks in turn | only on branch `wip/isa-pairs-2026-10-25` (2c69e39) | S |
| wl's kernels | `programs/wl` (fdf3370), GPU-VM `apps/wl` `run.py`, `plot.py` | 24 kernels of real code in modes c0, w, p, against `baseline/riscv.json` | builds for RISC-V; not in the image or the tick's A/B beside the shell loop and Doom | S |
| the cache's size against a VR round | `compare/wcsize.py` | the model and the card at several sizes | `RVC_WCACHE` has one design | M |

### Profiles and logs

| item | source | what | RISC-V today | effort |
|---|---|---|---|---|
| a log a pass | 108b3e7, befdf40 (`GVM_PASSLOG`) | each pass's end, bundles, pc and TIME (a jump is a sleep), cores the kernel recalled | partial: `--pc-log`, pc and count | S |
| who ended the pass | 5ea4dd6 (`GVM_SYNCLOG_STACK`) | the return addresses above sp at a sync or full end | partial: `--ra-log`, `pc_callers.py`, the leaf's only | S |
| a full end's store | 108b3e7 | its address | missing in `RVC_WC_LOG` | S |
| the list taken back every Nth pass | `GVM_DEVICE_EVERY` | as a world takes it, once a frame | missing: `rvc_cpu`'s `control_pass` every pass hides a game's waits | S |
| a profile from N passes on | `GVM_PROF_PASS` | | missing in `RVC_PROF` | S |
| the console's input traced | `GVM_CON_TRACE` (08b9de2) | each delivery and take | missing | S |
| Nsight line by line, SASS, registers | cc6ae9e, 8153807, 9280d3b (`nsight_lines.py`, `nsight_sass.py`, `nsight_table.py`, `-Dxil` signing) | DXIL lines' samples, the card's code, registers and occupancy | partial: `gpu_trace.ps1`, `nsight_summary.py` give counters | M |

### Tests

| item | source | what | RISC-V today | effort |
|---|---|---|---|---|
| the race check in sections | `GVM_RACES_AT`, `GVM_RACES_UNTIL` | AGENTS.md asks for them on long programs | missing in `rvc_cpu` | S |
| a call's wall limits | aeae927, d5c0d8d (`--max-wall`, `--wall-scale`) | every call under a minute | missing | S |
| the fast step against the general | `GVM_FAST_CHECK=1` | each pass both ways, stopped at the first byte that differs | partial: `RVC_NATIVE_CHECK` covers `--native` only | S |
| stop at N passes and save | aeae927 (`--max-passes N` with `--save`) | | missing | S |
| `--races` on every case | GPU-VM `userland/apps/test_all.py` | nxpaint, nxweb and the IDE take workers though their cases do not say so | `tools/test_all.py:541` checks only cases marked `workers`: 56 of 155 never checked (`nxpaint`, every `nxweb*` and `nxide-*`, `openttd`, `micropolis`, `nxpinball`, `nxfiles`). The condition goes | S |
| the test set on the card | `userland/apps/card.py` (vm.exe through `gpu_queue.ps1`, test_all's checks, the card's own expected values) | every case on the card, not only the CPU model | `--card-model` on `rvc_cpu` only: a runner on `rvc_harness` | S-M |
| the word below a program's data | GPU-VM `userland/flat.ld`, `elf2flt` | binfmt_flat writes the data's start into the word below it even with `NO_DATA_START_OFFSET` (`fs/binfmt_flat.c:1001` in RISC-V's tree too): on gpu_vm it overwrote musl's `decfloat` and broke `strtod` | `linux/nommu/elf2bflt.py` and `elf2flt.c` round the data's start down to 32 bytes and never look at what lies there; the word is 0 in all 98 of today's programs, which a live 0 would also be. Both refuse a section there | S |

### `rvc_cpu`'s speed

`gvm_cpu`'s speed-up is one commit, aeae927 (27 October): Red Alert's second mission from 24.5 to 126 million bundles a
second on one thread, 156 on four; 146 cases in 7 calls under a minute. It did four things:
- a bundle decoded once into a cache of 32K, valid by its 4 KB page's generation, with threaded computed-goto handlers;
- every core of a pass on threads, core 0 too, each storing into a write log of its own until the commit;
- the race table cleared by the slots it used (it cleared 4 MB every pass);
- `test_all` gives each case the host's threads over its jobs.

`rvc_cpu` already dispatches threaded handlers that tail-call the next (`[[msvc::musttail]]`, 100-200 M instructions a
second since 18 October), decoding at each fetch through an 8K table (`step_key`). Its workers run on threads, but core 0
runs alone first, because its stores go straight to RAM. What costs it:
- `RVC_RACES` and `RVC_PROF` force one thread (`workers_pass`);
- `RVC_RACES` takes loads and stores off the fast path (`plain_loads`) and keeps a `std::unordered_map` a texel,
  cleared every pass;
- `RVC_WCACHE` takes every store off it (`plain_stores`);
- `RVC_CALLS`, `RVC_PROF` and `RVC_CPU_LAST` send every instruction to `general_step`.

What to take, first to last:
1. the race table as flat open-addressed arrays cleared by their used slots, on threads (M);
2. the write cache's model in `h_store`'s fast path (S-M);
3. core 0 beside the workers, with a write log a core and the commit at the pass's end, which models the card more
   exactly too (L);
4. a fast-handler cross-check as `GVM_FAST_CHECK` (S).

Predecoding would gain little: RISC-V's decode is cheap already.

### Unity's tools

| item | source | what | RISC-V today | effort |
|---|---|---|---|---|
| the world from a snapshot | 27d22c5, with 11abdcb/8e16071 (the power-on applies the image again while core 0 reads pc 0: Unity uploads images in the background) and 4b6f185/f849920 (the snapshot's textures declared as properties) | a world check under a minute (how gpu_vm checks Half-Life in the world) | missing in `unity/ShaderEmu`; 6b644e8's snapshot is its input | M |
| the editor scripted and the card timed | `GvmProbe.cs`, `GvmAuto.cs` (`rvscene`, `play rv`), `GpuTimer.dll`, `unity/measure.py` | play mode by a script (wait, type, sample, shot), the card's time an event | partial: drives RISC-V's `RvMachine.unity` in the machine-alone project only (`unity/gpuvm/minimal`); the world is read by hand | M |

## 5. Unity, only if the main thread is the limit

The RISC-V world runs fewer rounds a frame than gpu_vm's, so its main thread's cost a round matters less. Not the tick
as one blit. If a world's main thread is the limit, gpu_vm's two largest:

- the commit as blits (`GvmCommitBlit`: Doom's main thread 3.4 ms a frame less, Half-Life's world 6 ms);
- the GPU's lists as blits (`GvmGpuDrawBlit`: a camera's render a frame).

Both draw patches at tessellation factor 8 and amplify in the geometry shader. They need fxc2 65 or later: the hull
shader's relative index before fxc2 patch 64 reset the card (6d6588d), and `fxc2.cginc` refuses an older fxc2 (f96b392).

## The tick's DXBC: gpu_vm's lessons, to look for in `rvc_opt`

Not audited on RISC-V's DXBC yet:
- fxc2 does not fold `(j * 8 + k) & 7` to `k`: an 8-way switch of loads, 325 instructions (f0dfc60). `rvc_opt` indexes
  its eight targets the same way.
- the exit's outputs in a loop and an empty entry's data not read: DXBC 16% smaller, integer loops 1% faster (fa57740);
- a local pad array holds the card's local-memory layout: without it the fast loop was 10% slower (880f7d1). Any change
  to `L1_LOCAL`'s arrays is measured on the fast loop, however cold the code it touches.

From the two ticks read side by side (`GPU-VM/tick/tick.hlsl` against `experiments/rvc_opt`; nothing compiled or
timed). Most of gpu_vm's savings are in the RISC-V tick already or lost there before (`docs/machine/tick-costs.md`
369-526: both next texels fetched every step -3.2%, the cache as words -3%). RISC-V's skip of stores that change
nothing (`mem.h:446`) stays: gpu_vm dropped silent stores for their cost a bundle, but in VR a round ends when the
cache fills. What is left:

| area | RISC-V today | what to do | how sure | effort |
|---|---|---|---|---|
| a round's draws besides the tick | a 2 MB copy of the state rows, the unpack, a gather through a geometry shader (`ram.h:273`, 24 vertices), a quad over 2048 x 64 state pixels (`ram.h:384-420`), the RAM commit through a geometry shader (`ram.h:430`); order at `rvc_backend11.cpp:887-905`. gpu_vm keeps the state in the targets (`state_k`) and commits by one point draw with no geometry shader, a vertex an entry (`tick.hlsl:759-768`) | time each first (`RVC_PASS_PROF` labels them; tick-costs.md's 0.134 ms a pass predates `RAM_INPLACE`), then: read the targets directly, the state's commit folded into the next `decode`, the RAM commit as vertex-shader points. Fewer or cheaper draws a round count most where rounds are few | a guess: no one timed a geometry shader's commit against a vertex shader's | M-L, Unity too |
| D3D11's speed levels | about 40 shapes rejected at -0.4 to -8% in tick-costs.md, each judged before the levels were known | after the pad sweep (0-20 words), the near misses of that list timed again on the shell loop, Doom and Quake | the levels measured on gpu_vm (up to 20% between them; a pad of 9 took t4 from 18.6 to 20.6), on RISC-V a guess | M |
| cold copies of the store path | `MC_FAULT` (`mc.h:102-132`, expanded at `emu.h:1560`) inlines up to 11 copies of `mem_set_ram`'s three-bucket search, four more in the fast and general paths | the fault's record through a `[loop]` over a table of 10 words with one `mem_set_ram`, with the pad sweep (gpu_vm's one store site: 5.4 KB less, level, nxray +1%) | an analogy | S |
| work on every access that does nothing | every fast load compares `ram_l1_last_addr` (`mem.h:200`; the stores' copy `mem.h:441`), which is NONE until a store stalls, and then the run stops anyway; every changed store sets `mem_dirty` (`mem.h:449`), read only by the commit that is not in place (`main.shader:577`) | the load test only where `slow` is set; `mem_dirty` gone under `RAM_INPLACE` | about 0.55 ns each of a 215-450 ns access (tick-costs.md:170): under 1%, a guess | S |
| a store to a new texel waits for its fetch | making an entry fetches the texel and waits on the chain, for the silent compare and the merge (`mem.h:419-446`); gpu_vm's whole-word store makes the entry with no fetch and fills the missing words at the exit (`tick.hlsl:243, 635-644`, measured free) | for whole-word misses only, which then lose the silent test: weighed against it first | up to 336 new entries a pass at 86 ns a cold texel, about 30 us a pass; an analogy | M |
| `l1_occ[4]` read by a dynamic index on every probe (`types.h:627`) | arrays of 4 words or more are in local memory (gpu_vm, 1-3 are registers) | a `uint4` picked by `idx_uint4`, timed with the pad | a guess | S |
| `xr[65]` zeroed on entry (`types.h:692`), though `xreg_load` and `fp_state_load` write all of it | | `#pragma static_noinit` before it (fxc2 patch 62). `l1_has` and `l1_occ` must stay zeroed | gpu_vm's was mixed (before `wd` level, before `r` floats -2.3%) | S |
| loads from the ROM go the general way (`emu.h:1143`, `mem.h:249-277`, about 2,700 ns) | | the ROM in the fast load after 3c, only if a profile shows ROM loads (no-MMU copies a mapped file's pages into RAM) | low | S |

The rule stands: every core in the one tick draw, one compile, no worker drawn or built apart from core 0. Whatever
the tick's arrays cost a lane is paid by every core alike.

Lessons that hold for the RISC-V tick: two pixel shaders of different local sizes never overlap on the card (e6e8121);
starting the tick's pixels is the cost, and discarding them does not help (759f259); the texture cache is cold at every
draw (98f2222, f2e848a); under the Unity measure RISC-V is slower at 16,384 a pass than at 8,192 (Quake III 50.6
against 65: 62fc7fc); Quake III's `com_maxfps` 85 capped both machines before 24 October (fef1431); Half-Life's fps moves
5-9% with where its data lies (03e2fc0, IDEAS 31).

## 5b. What gpu_vm did without

- **One tick in the tree.** vm.exe has no legacy modes; RISC-V carries `rvc_lean`, `rvc_compute` (each with a `gpu.h` of
  its own), `rvc_backend12`, `rvc_compute_harness` and options such as `--compute`, `--dxc`, `--ping-pong`,
  `--tick-split`, `--zap`, `--isa`, `--rvc`. gpu_vm's rule: an experiment's result is a row of RESULTS.md and its code
  goes. Clean-up, no speed.
- **The overlay only where programs write** (gpu_vm's `rcS`: the ROM's root, overlays on `/root`, `/var`, `/etc`,
  `/home`). RISC-V lays one over the whole root, so every lookup searches two layers, and any file of the image can be
  written or renamed (`nxfiles-rename`). A trade, not measured: only with a reason.

## 6. The ISA fork, last

- **Zfinx: one register set.** Floats in the integer registers (`-march=rv32ima_zfinx`, GCC 12 and later). The tick
  already keeps the float registers in `xr[]` (`XR_F`), so what goes is:
  - the eight float state texels a core loads and writes every pass (`fp_state_load`);
  - under `-mabi=ilp32`, an `fmv` on each side of every call that passes or returns a float.

  Float-heavy code then has 32 registers for everything. gpu_vm's count says that is enough: 64 to 32 registers added
  0.5% bundles, 1.8% loads and no stores (GPU-VM RESULTS.md, "the register count"). Every program, musl and the
  kernel's float code are rebuilt.
- **`fmadd` on the fast path.** It reads three registers and takes the general path today, so programs are built with
  `-ffp-contract=off`. With it fast, the flag goes, except where a port needs exact sums (Half-Life's `se_sum_exact` and
  `Mod_PointVecsProduct` keep their attribute).

## Not for RISC-V, and why

| idea | gpu_vm's result | why not here |
|---|---|---|
| fused instructions, bundles, a new encoding | cheaper instructions | a pass that ends on its write cache never sees them; VR showed it |
| more registers | 64 to 32: +0.5% bundles, +1.8% loads, -0.5% stores | the encoding's 5-bit fields; no gain at 32 |
| a bigger write cache | tried on RISC-V too | did not help flat out. Under a VR frame (`--unity 11.1,7.1,1.0`, about 3 rounds a frame) gpu_vm's 170 to 250 entries gave 1.7-2.1x (Doom 24.8 to 43.8, Quake 16.8 to 33.6 frames a second; GPU-VM 261a597, cbd5eb7): if RISC-V's attempt was judged flat out, that model is the one to judge it by |
| the tick as one blit | main thread 0.33 to 0.19 ms a pass | few rounds a frame on RISC-V |
| 8 x 8 cores in one row, one triangle | +16-21% over gpu_vm's c8 | c8 launched twice its pixels; RISC-V's padded blocks were measured with nxray to share no warp ("Eight texels a pixel") |
| zeroed static arrays (`#pragma static_noinit`) for the write cache | DXBC 190 KB to 173 KB | RISC-V's write cache is a local array already (`L1_LOCAL` under fxc2); `xr` is the one array left (the tick's table above) |
| a hit with no texel fetch, a stored-texel bitmap, probing past two buckets | -5 to -15% | lost on gpu_vm |
| silent stores, evicting at a full cache, overflow entries | passes longer, every bundle dearer | lost on gpu_vm |
| the lazy GPU list | gpu_vm's default | RISC-V's Quake III slower with it (`gpu_vm/PROFILE_Q3.md`); weak evidence: those runs were under the 85 fps cap, on an `RVC_LIST_LAZY` with the dropped-list bug (section 0) |
| a worker's budget cut to core 0's (`wad`) | +4-51% on gpu_vm | RISC-V's workers run their whole budget in 85-99% of passes |
| RAM in strips, texel pairs, lanes warming the cache | code past 16 KB faster | games 1-6% slower |
