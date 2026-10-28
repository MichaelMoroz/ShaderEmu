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

`custom_isa` holds all of `main` and 334 commits more. Most are gpu_vm's own, but some fix sources the RISC-V machine
builds, and `main` has none of them:

| commit | what | why it is RISC-V's too |
|---|---|---|
| bacc205 | Half-Life's variables on 16 bytes each (`linux/halflife/build.sh`); the NES's `frame_paced.due` a volatile struct of its own texel (`osd_shaderemu.c`) | the RISC-V commit writes whole texels too: two cores storing into one texel in a pass lose a word (20-21 texels and 1-3 changes undone in hldemo1 under the race check) |
| 632e982 | Half-Life's server worker's statics in texels apart (`world_ahead`, the loopback's `get` and `send`, `net_drop` one a side) | the same: `svc_bad` and overflows 10 to 30 s into the level on the card |
| 830fbdb | mcwx gives a call to the lowest free worker, not round-robin | busy workers together in the geometry: 128 busy cores scattered cost 0.332 ms a pass against 0.225 in a run (`gpu_vm/tick/WAVES.md`) |
| cf99ef8 | libmedia's `media_lcw` with no end given, and Red Alert's use of it | a signed pointer difference that goes wrong below 2 GB |
| 4e58aa5 | `nxfiles --kinds` printed from a dead stack buffer | plain bug |
| b160c1e | `programs/mc/copyrace.c` | the test of a `read()` buffer summed by waiting workers |
| 5f54d7d | nxbar: a window restored shows as restored | plain bug |

The rest of `custom_isa`'s changes to shared files are behind `__gpuvm__` or are gpu_vm's own (word-bound loads read by
bytes in Red Alert, Tiberian Dawn, Doom and the NES; `mcw_rest_on`'s declared workers; the HLSL back end).

`linux/prebuilt` was last made on 24 October (07d38c8, 1d858a2), so the RISC-V image has none of the above even on
this branch: the prebuilt is made again after them (`pwsh tools\make_image.ps1 --save-prebuilt`).

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
   go together.
3. **Later, the copy without the pass's end**, as gpu_vm's `GVM_UOP_ASYNC`: only if the tick can stop a core's own
   access to a range still in flight without a test in the fast loop. gpu_vm's test in the fast loop (`oprange`) cost
   Quake 3.7%, so it is not there; its general step makes those accesses.
4. **The kernel's small copies.** RISC-V's `arch/riscv/lib/memcpy.S` (5.17) copies a byte at a time when the two
   pointers' offsets in a word differ. gpu_vm's `linux/arch/gpuvm/lib/copy.c` copies 64 bytes a round, and words built
   by shifts when the offsets differ: Doom's core 0 2.6% fewer instructions, its card time 5.6% less. A `memcpy_hook.py`
   step for the misaligned path does the same here.

Check: `programs\copytest` (`sum 0e7f76c3`) on `rvc_cpu` and the shader machine, `test_all.py`, Quake's and Quake
III's sums.

## 3. The kernel: starts and the boot

Smaller for a frame, but each is a stall a player sees (a program's first window, a level, the boot).

| item | GPU-VM source | what | gpu_vm's figure | RISC-V |
|---|---|---|---|---|
| workers laid out at boot | c519897 | `workers_layout`'s three `msleep(60)` moved from the first `WORKERS` ioctl to delayed work at the driver's init | nxweb's page 0.44 s to 0.24 | the same sleeps in `linux/kernel/shaderemu_gpu.c`, paid by the first program with workers and by the window server at the pointer's first move |
| the struct pages copied | `linux/patches/0003-gpuvm-memmap-copied.patch` | made once and copied by doubling memcpy (the commit's copy), nothing stored when freed | boot to the shell 6,448 to 3,642 passes with 0004 and fastrun | upstream code: the RISC-V boot stores the same 2 MB |
| no BLAKE2s self-test | `0004-gpuvm-no-blake2s-selftest.patch` | | part of the above | the same |
| a program's memory in blocks | `linux/fastrun.py` | from the free lists and given back in blocks | part of the above | to read: what of it the RISC-V kernel lacks |
| `/dev/zero` a megabyte a step | `linux/drivers/zeroread_hook.py` | `read_zero` cleared a page a step, each a fill that ended the pass | 4 MB 0.0256 to 0.0016 s | `read_zero` is 5.17's, a page a step through `clear_user` |
| relocations a word at a time | `linux/drivers/flatreloc_hook.py` | `load_flat_file`'s loop by words, one bounds check, `calc_reloc` once | 37 to 13 bundles a relocation | RISC-V's programs carry few: Half-Life 48,015, OpenTTD 28,694, the rest under 5,400. Last |

Also small: **a rest with no system call when the program is alone** (82cda8e's first half: the kernel writes whether
another task could run, and `mcw_rest` ends the pass in user mode). RISC-V's `REST` ioctl is 322 instructions a call
and does the zero pool's idle work, which would have to go elsewhere. Low value.

Check: `zerotest` and `tailtest` PASS, `mctest`, the boot to the desktop, `test_all.py`.

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
- **A run captured and replayed** (add364c, a338987: every frame's host values recorded and checked row by row), and
  **the world started from a snapshot** (27d22c5: `--save-world` and the editor's import). Both large; they let a
  world check fit in a minute.

## 5. Unity, only if the main thread is the limit

The RISC-V world runs fewer rounds a frame than gpu_vm's, so its main thread's cost a round matters less. Not the tick
as one blit. If a world's main thread is the limit, gpu_vm's two largest:

- the commit as blits (`GvmCommitBlit`: Doom's main thread 3.4 ms a frame less, Half-Life's world 6 ms);
- the GPU's lists as blits (`GvmGpuDrawBlit`: a camera's render a frame).

Both draw patches at tessellation factor 8 and amplify in the geometry shader.

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
| a bigger write cache | tried on RISC-V too | did not help |
| the tick as one blit | main thread 0.33 to 0.19 ms a pass | few rounds a frame on RISC-V |
| 8 x 8 cores in one row, one triangle | +16-21% over gpu_vm's c8 | c8 launched twice its pixels; RISC-V's padded blocks were measured with nxray to share no warp ("Eight texels a pixel") |
| zeroed static arrays (`#pragma static_noinit`) | DXBC 190 KB to 173 KB | RISC-V's write cache is a local array already (`L1_LOCAL` under fxc2) |
| a hit with no texel fetch, a stored-texel bitmap, probing past two buckets | -5 to -15% | lost on gpu_vm |
| silent stores, evicting at a full cache, overflow entries | passes longer, every bundle dearer | lost on gpu_vm |
| the lazy GPU list | gpu_vm's default | RISC-V's Quake III slower with it (`gpu_vm/PROFILE_Q3.md`) |
| a worker's budget cut to core 0's (`wad`) | +4-51% on gpu_vm | RISC-V's workers run their whole budget in 85-99% of passes |
| RAM in strips, texel pairs, lanes warming the cache | code past 16 KB faster | games 1-6% slower |
