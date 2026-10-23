# gpu_vm as a whole machine: devices, workers, drivers (design, 22 October 2026)

What makes gpu_vm the RISC-V machine's equal for the project's programs: the devices (display, the GPU's command
lists, input, sound, the ROM, the network, the fetch and the kept folder), the worker cores, and the Linux drivers for
them, on the card. The programs and games themselves are ported by the apps agent (`gpu_vm/userland/apps`); the tick
belongs to the ISA agent (`gpu_vm/tick`) and is taken from an option.

## The rule: the device window moved down by 0x86000000

gpu_vm's devices are the RISC-V machine's, at their addresses less 0x86000000 (`GPU_VM.md`, "Memory"): the control
words at 0x01000000 (RISC-V 0x87000000), the GPU's memory from 0x00010000, the mailboxes at 0x00c00000, the
counters at 0x00c10000, the console and the network's row at 0x016b8000, the fetch's answer at 0x016c0000, the sound
memory 0x01b00000-0x01e00000, RAM from 0x02000000 (RISC-V 0x88000000). The ROM stays at 0x40000000. Every layout,
word and protocol of `docs/machine/{display,gpu,input,sound,console,multicore}.md` and `docs/system/{lan,fetch,persist}.md`
holds unchanged at the moved address; the 64 KB below 0x00010000 (RISC-V 0x86000000-0x8600ffff) is gpu_vm's boot area
and is not device memory.

So a gpu_vm address `a` below the ROM is RISC-V address `a + 0x86000000`, everywhere in the window and in RAM alike.
The device shaders keep their RISC-V numbering (`(address - 0x80000000) / 16` as a texel index) and read the gpu_vm
RAM texture through one mapping (below); a C program's addresses change by the one constant.

## The harness: a gpu_vm mode of rvc_harness (the plan's choice, (a))

`bin\rvc_harness.exe --gvm DIR` runs a gpu_vm machine in the RISC-V machine's window, with all of its host: the
display, keyboard and pointer, the sound card through XAudio2, the console and its scripts (`--send`, `--expect`,
`--until`), the fetch, the kept folder, the network, captures. (b), the devices brought into `gvm_gpu`, would have
meant a second window, a second input path and a second sound path; `gvm_gpu` stays the bare runner with `--check`.

- **A backend of its own** (`harness/apps/gvm_backend11.cpp`, D3D11, fxc2 bytecode as Unity would run it): RAM is one
  `R32G32B32A32_UINT` texture 8192 texels wide from address 0 (`gpu_vm/tick/STATE.md`), the ROM a texture of its own,
  the cores' state the tick's eight textures, a row a core, in two sets. A frame is: the passes of the tick (the step
  from `--gvm-tick FILE`, default `gpu_vm/tick/steps/x18.hlsl`; its commit's four draws and the operations' rounds as
  `gvm_gpu` draws them), then the devices' passes.
- **The devices' passes are the RISC-V machine's shaders** (`experiments/rvc_opt/gpu.shader`, `sound.shader`, their
  `src/gpu.h` and `src/sound.h`) compiled with `GVM_RAM`: `ram(index)` reads texel `index - 0x600000` of the gpu_vm
  RAM texture at `(t % 8192, t / 8192)`, `texel_of(address)` adds 0x86000000 first, and the ROM is read from the gpu_vm
  ROM texture (a word of a texel, not one of four byte textures). Nothing else in them changes.
  - GPUDraw: as before, reading the gpu_vm RAM and ROM.
  - GPUControl is drawn as zones (the control words' 256 texels, the console's texel, the network's ring, the fetch's
    rows, the counters' totals, every core's counts), each into a scratch texture reading RAM and then copied into
    RAM: no pass reads the texture it draws. The workers' counts come from the tick's state (CYCLE and OPS of a row).
  - The writeback (a picture copied into RAM, `SUBMIT_WRITEBACK`/`SUBMIT_INTO`) is four draws of the destination's
    rows into RAM, a word of each texel a draw by the write mask, a pixel that has no word of the picture discarded:
    nothing read from RAM. It is made in the frame after the list was drawn, as the RISC-V commit makes it; the
    control pass clears the copy word and counts the copy.
  - SoundMix: as before.
- **What the host reads**: the backend gives `rvc_harness.cpp` the row it reads from the RISC-V machine (core 0's
  state, the control words, the network's row with the console, the field's texels), with core 0's texels made from
  the gpu_vm core's (its pc, its ops as instructions, the passes as commits) and the UART's words saying "none":
  the console is the words in the network's row, which both machines have. The memory view shows the gpu_vm RAM
  texture with the display decoded from it (`harness/core/memview.cpp` with the address offset in its constants).
- **Booting**: `DIR` has `vmlinux` (an ELF, loaded into RAM), `gvm.dtb` (at 0x1000) and `rootfs.romfs` (the ROM), as
  `gpu_vm/userland/romroot.sh` makes them; core 0 starts at the ELF's entry with r5 = 0x1000. The RAM texture is as
  tall as RAM's end (`--gvm-ram MB`, 128).
- **Safety**: every new or changed shader runs on WARP first (`--warp`), every loop in the new passes has a fixed
  bound, every run goes through `pwsh tools\gpu_queue.ps1` and is under five minutes.

The RISC-V machine is untouched: the new code is a backend that only `--gvm` makes, the shaders' changes are under
`#ifdef GVM_RAM`, and the memory view's offset is 0 for RISC-V.

## Workers: the RISC-V protocol, made by the machine's firmware

The mailboxes, the start words, the jobs, answers and taken words, the fault and call records, the owners, the recall
and offer words are `programs/mc/mc.h`'s at 0x00c00000 (`MC_PAGE_GPU_OFFSET` is the same 0x00c00000 in `/dev/gpu`).
What a worker does is `rvc_cpu`'s (`worker_wake`, `mc_fault`):

| event | the worker |
|---|---|
| its start word becomes `MCSU` with a pc it has not started at (or it was stopped since) | registers 0, pc, r2 = sp, r5 = its number, user mode, interrupts as below |
| its start word is `MCST` | parks at once, wherever it is; a later `MCSU` starts it again |
| user `wait` (`mc_next_pass`) | its pass ends (`GPU_VM.md`: a user's wait is the pass's end) |
| user `sync` (`mc_end_pass`) | its pass ends |
| `break` (`ebreak` of `mcw_loop`) | parks |
| `syscall` | the call record: r12 (the number) and r5-r10 at `MC_CALL_AT(k)`; the fault record `{resume + 1, 8, 0, pc}`; it waits until core 0 changes the resume word's low 24 bits, then r5 = the resume texel's second word and it goes on after the `syscall` |
| any other trap | the fault record `{resume + 1, cause, address, pc}` with RISC-V's cause numbers (illegal 2, break 3, load fault 5, store fault 7, fetch fault 1), and it waits as above (the library ends the program with a line saying where) |

There is no MMU: a worker runs in its program's memory untranslated, as on the no-MMU RISC-V machine (root 0), and a
worker never stops at a page.

**How**: not in the tick. Each worker core is a whole gpu_vm core (a row of the tick) with a supervisor mode, so the
protocol is a small supervisor program, the machine's **worker firmware** (`gpu_vm/platform/worker.S`, assembled with
the gpu_vm clang into `worker_fw.h`), which the host puts into the boot area (read only to the cores) at
`GVM_WORKER_FW` 0x0400, as `gvm_gpu` puts its semihosting stub at 0. A worker core starts there in supervisor mode with
r5 = its number; the firmware polls its start word once a pass (`sync` ends the pass), starts the job in user mode by
`eret`, takes its traps, and keeps its own four words a core at `GVM_WORKER_SAVE` 0x00c0f000 + 16k (a free texel of
the device memory between the mailboxes and the counters). A running worker has the timer set for the next pass
(TCMP = TIME + 1), so its firmware looks at the start word at the start of every pass and parks it on `MCST`, as the
RISC-V tick's worker reads its mailbox as the pass begins. The tick is not changed, and it stays the ISA agent's.

Costs: a parked worker runs three bundles a pass; a running one a trap, five bundles and an `eret` a pass (of 8,192).
A sleeping worker (`mcw_loop`'s `wait`) runs its loop's few bundles a pass where the RISC-V machine skips it: the
difference is measured in the first runs, and if it matters the tick may learn to skip a core whose start and job words
are as it last saw them (an ISA change, the ISA agent's).

**Which cores exist**: the host's. The machine says how many it could have at control word 0x3cc (`WORKERS_MOST_NOW`,
256 in the harness), the kernel lays them out as on RISC-V (`workers_layout`: the shape word's count at 0x3d4), and
the host then starts cores 1 to count at the firmware (and draws that many rows: none while there are none, which is
the RISC-V harness's `workersNow`) and says how many there are at 0x3c0. The counts each core ran (`MC_STATS`,
`MC_STATS_ALL`) are its ops; "running" is a started core, "asleep" one in the firmware's park loop.

**Stores**: a gpu_vm core's stores reach the others at the commit, and the commit writes words, not texels: two cores
may store to the two halves of one texel in a pass and both stay (on RISC-V one undoes the other). A race is a word
two cores store in one pass (or bytes of one word: a core's write cache holds the whole word as it saw it).
`gvm_cpu`'s race check (`GVM_RACES=FILE`) says where, as `RVC_RACES` does on `rvc_cpu` ("undone" and "as it was").

**No lanes**: the lanes' special registers read 0 on gpu_vm, and `mcw_lanes`, the field and the steps take their plain
paths (`mcw.c`), as on `rvc_cpu`.

## gvm_cpu

The same firmware at the same place, the same start rule, up to 256 cores, the cores run one after another in a pass
(deterministic), the commit as the card's. The control words a host gives each pass (`rvc_cpu`'s): the clock, the
submit word taken back and the frames counted (no picture), the copies counted, the host flags, the cores' counts.
`GVM_RACES=FILE`.

## The Linux drivers (`gpu_vm/linux/drivers.py`)

The RISC-V kernel's drivers (`linux/kernel/shaderemu_{gpu,fb,input,sound,net}.c`) are the one source: `drivers.py`
copies them into the gpuvm tree with their physical addresses less 0x86000000 and the few RISC-V lines in them made
gpu_vm's (the clock's read, the satp root as 0, `wfi` as `wait`, the pause hint as `sync`, no zero pool), adds their
Kconfig and Makefile entries (`install_drivers.py`'s) and the no-MMU mmap (`linux/nommu/patch_drivers.py`'s), and runs
`linux/kernel/tick_input_hook.py` (input and the network polled from the tick). `gvm.dts` keeps the device window out
of RAM; `gpuvm_defconfig` turns the drivers on.

### Resting (gpu_vm only, 23 October 2026)

A program's first core that waits for its workers rests (`mcw_rest`, the `REST` ioctl). On gpu_vm two words of the
mailboxes' kernel page change what a rest costs and what the workers get:

- **0x00c01130: { "REST" 0x54534552, crowded }**, the kernel's (`drivers.py`'s `rest()`, `shaderemu_gpu_crowd`): crowded is
  `nr_running() > 1`, written from the timer's tick and after each `REST` ioctl's yield, a store only when it changes.
  With the mark there and crowded 0, `mcw_rest` ends the pass by itself (`sync` in user mode): no system call, so none
  of the trap's frame and the scheduler's stores before the pass ends. Crowded 1 is the ioctl as before (the kernel
  yields to the others). A task woken meanwhile runs at the next tick at the latest (or at once, by the wake's
  preemption). `echo 0 > /sys/module/shaderemu_gpu/parameters/rest_alone` takes the mark away: every rest a call.
- **0x00c01100: eight words, a bit a core**, the workers the first core waits for (`mcw_rest_for(workers, n)`,
  `mcw_rest_on(handles, n)` in `mcwx.h`, or the `REST_MASK` ioctl, `_IOW('G', 12, u32[8])`): set before the rest ends
  the pass and 0 again after. The tick (the builder's `wad=5`) reads them when core 0's last pass ended on sync: a
  worker whose bit is set has its whole budget, the others are held to core 0's bundles of that pass and a quarter.
  All zero is the tick's rule without them. **0x00c01120** is the pid of the program that passes them (set at its first
  `mcw_rest_for`, 0 again when it closes `/dev/gpu`, `drivers.py`'s release): the tick's `wad=6` caps the others on
  full ends too only while it is set, so a later program that says nothing is not held. Quake III passes its client game's job and helper, the server's job and
  its helpers, the scene's job; Half-Life its server's worker and helpers, the drawing's worker and the ring and
  studio workers it polls; each where it waits for them.

## Tests

- `gvm_cpu`: a bare-metal worker test (`gpu_vm/cpu/tests`), mctest under Linux, `GVM_RACES` 0 undone.
- The card: `gvmdev` (`gpu_vm/userland/test/gvmdev.c`): draws through `/dev/fb0` and through the GPU, reads a key and
  the pointer, plays a tone; mctest with its worker counts; nxray, nxpath and nxphys's own checks.
- The RISC-V machine as before: `tools/test_all.py`'s cases, a Doom run.

## State (22 October 2026, RTX 5070 Laptop)

How to run it:

    bash gpu_vm/platform/build.sh                          (WSL: the firmware into worker_fw.h, after a change to worker.S)
    bash gpu_vm/platform/test/run.sh                       (WSL: the bare workers' test on gvm_cpu, 3 and 15, GVM_RACES)
    OUT=~/gpu_vm/linux-build-romroot-machine bash gpu_vm/linux/build.sh sync config image   (the kernel with the drivers)
    pwsh tools\gpu_queue.ps1 bin\rvc_harness.exe --gvm build\gvm_desktop --viz --sound      (then `nx &` for the desktop)
    pwsh tools\gpu_queue.ps1 bin\gvm_gpu.exe --check --cores 16 build\gvm_platform\workers.elf 15

An image folder for `--gvm` is what `gpu_vm/userland/apps/image.sh` makes (`K=` a kernel folder with this kernel:
`romroot.sh` with `W=build/gvm_machine KOUT=~/gpu_vm/linux-build-romroot-machine`); `build/gvm_desktop` is the apps'
root, mctest and gvmdev, and `/root/Desktop` made by `tools/make_linux_image.py`'s `desktop_shortcuts`.

What works:

- **The harness** (`rvc_harness --gvm`): gpu_vm Linux boots on the card to its prompt in 3.6-4.3 s (the ROM 255 MB, RAM
  192 MB), the console both ways and its scripts, the memory view with the display, the fetch and the kept folder's
  words (the host answers `host:name` and `persist:load`). The desktop starts (`nx`): Nano-X with the GPU's lists, its
  picture (a JPEG) and icons, the bar, the cursor. On WARP it boots too (110 s to init).
- **The devices, `gvmdev all`** on the card: `/dev/fb0` drawn, read back and shown (mode 1, 640 x 480); a GPU list (a
  clear and a rectangle) drawn and its picture copied into RAM, 3,072 of 3,072 pixels as drawn; a 440 Hz tone written to
  `/dev/sound`, captured by `--sound-capture` at its pitch and amplitude; the pointer's 174 moves and its button through
  evdev (`--pointer-sweep`, `--pointer-button`). The keyboard is the window's (not scripted). The network's driver and
  zones are there, untried.
- **Workers**: the bare test passes on gvm_cpu (3, 15; GVM_RACES: the 40 halves of one word undone, nothing else) and on
  the card under `gvm_gpu --check` (3 and 15 workers, every core the reference's, RAM too). Under Linux, mctest 3, 15 and
  255 pass on gvm_cpu (`mctest 15` and `mctest 3 0 race` under GVM_RACES: 0 undone; the race test keeps both halves,
  `49 0 49 0`, since the commit writes words) and on the card in the harness (counters merged by the machine). nxray's
  four sums with 255 workers are the RISC-V machine's on gvm_cpu (53ea472d 226b5e92 37366b32 3ffdbe63); nxphys's wall
  b28b77b4 with 255 workers and with none, 0 pages brought for workers; nxpath d63313e9 with 255 workers and with none
  (the RISC-V machine's is 02f213e9: floats). On the card nxray gives 6e3bc0ef de66b7e3 ... with 255 workers and with
  none alike, and nxphys 44d0d906: the card's float special functions (as `--check` knows them), not the workers.
- **Idle workers**: parked or sleeping workers are frozen by the harness (not drawn); a shell loop is 7.1 s after
  mctest 255 against 7.4 before any worker (20.3 s when the parked rows were drawn).
- **The RISC-V machine** in this harness: mctest 3 and 255 pass, Doom's timedemo state digest is 0ab4df93.

Not done, or for others:

- The apps' image (`apps/image.sh`) has no `/root/Desktop`: the desktop's `nxfiles --desktop` leaves at once without it.
- A thawed worker starts a few frames late (the readback is behind); a program posting many short jobs to workers that
  were frozen pays that once per wake. Workers in use are never frozen (24 readbacks of quiet first).
- A pass a frame in the harness (0.33 ms a frame), and passes of a shell loop end early (about 1,400 ops of 8,192):
  `--tick-rounds N` runs more a frame; why the passes end early is the tick's and the kernel's to look at.
- A `write()` of a half-second tone to `/dev/sound` took about 7 s in the harness (the tone plays at its end, right):
  the stream's pace against the frames, to be compared with the RISC-V machine's.
- LAN between two harnesses, the kept folder's save, Unity: untried.
