- The project's name is ShaderEmu (31 October 2026): no other name for it, and none of its old ones, in anything in this repository: code, docs, skills, lock names, a tool's text, a default path. A script finds the tree from where it lies, not from a folder of one computer.
- The window manager and the bar know programs (`docs/system/nanox.md`, "The window manager and the bar"; 31 October 2026): a new
  key or state is told over `GR_WMC_*`/`GR_WM_TELL` (no new request, no change to the client's library), what changes the window tree
  raises `gd_scene_gen`, and `python tools/test_all.py wm` must pass, with a look at `tools/wm_shots.py`'s pictures: the cases say places
  and states, not what is drawn. A program of which one window is enough calls `ui_single_instance()` (`ui_single.h`).
- Tick simplification (2 November 2026, `docs/plans/tick-simplification-plan.md` has the numbers and what was not done). The tick needs fxc2 67 or later, the submodule
  `tools/fxc2` (`git submodule update --init`; a new compiler is a commit moving it), and Unity's editor needs its `bin/unity` too: `c` and `ot` are statics under `#pragma static_noinit`,
  which an older DLL ignores (448 zeroing instructions a pass); `xr` stays zeroed (without it 0.6 to 0.8% slower). The geometry is one row of cores
  (`docs/machine/multicore.md`, "The geometry"): no division by a variable in it, a worker has 2^5 at most and its texels are in tiles (rows: 50% slower), and one triangle draws the tick, `frag` discarding what no core runs: a warp is then 4 x 8 pixels of one core. The timer comes from the host: no doubles in the tick. The ROM and RAM's start image are one RGBAFloat texture each, from their first row.
- The rules of the work (23 October 2026). We own the whole stack, from the games' and programs' sources down to the
  tick's DXBC instructions: the games and userland, the toolchain, the tick and the other shaders, and
  fxc2 (the submodule `tools/fxc2`), so a cost is fixed at whichever layer it is cheapest.
  - Any sacrifice that makes userland programs faster is worth it.
  - The shaders can be rewritten completely if needed.
  - Everything must work under VRChat's constraints (no UAVs in the fragment shader, no texture arrays, D3D11 DXBC).
  - fxc2 can be changed freely for the tick, as long as it still works on all its uses (the harness, Unity, the
    ShaderEmu and Poiyomi shaders its README measures).
  - Optimizations are weighed together, not one at a time: one that loses alone may win with another (a layout and the
    compiler's address, a cache and the ISA's stores), and one that wins alone may not add up with the rest.
- One core a wave (25 October 2026): cores whose lanes share a wave run two programs, and the wave pays for both paths.
  On NVIDIA a warp is a 4 x 8 pixel tile under one big triangle, so blocks of whole tiles (4 x 8, 4 x 16, 8 x 8) drawn
  by one triangle never mix; a primitive edge inside the cores' area does. Padding lanes are kept out of the geometry,
  not discarded.
- The no-MMU build goes by nothing of the build folder's name (31 October 2026; `-ffile-prefix-map` in `setup.sh`'s drivers:
  `cc-bytes` must not move with a private `HOME`). `linux/nommu/kernel.sh` puts this repository's drivers and hooks into its
  copy of the tree and stops at a failed make: before, the old `Image` was copied on. A flat program's relocations and the
  words they name are in the machine's order (flag `0x40000000`, `elf2bflt.py`, `elf2flt.c`); a ROM directory of 32 entries
  or more has a table of its names (`slim.py`, `patch_romfs.py`): what writes the ROM after `slim.py` keeps it.
- Linux with the MMU is no longer supported or developed for (21 October 2026): build, test, measure and document the
  no-MMU machine only (`linux/nommu`, `build/images/nommu`). Do not fix, rebuild, test or keep in step anything that only
  the MMU image uses, and do not add MMU paths to new work.
- `tools\make_image.ps1` passes its arguments on to `make_linux_image.py` (at first it did not: `$args` inside a `Measure-Command` block is the block's own): `pwsh tools\make_image.ps1 --save-prebuilt` saves the prebuilt folder under the lock. It makes the
  no-MMU image after the shared one (`linux/nommu/image.py`, from `build/nommu` or
  `linux/prebuilt/nommu`), and with `--save-prebuilt` keeps that build too: Linux without the MMU
  is what the harness, `rvc_cpu` and `tools/test_all.py` run by default; with the MMU is legacy.
- Tiberian Dawn's scan bits come from a table (`docs/games/tdawn.md`, "Fewer instructions"): a new write of `IsLocked`, `IsInLimbo`, `IsDiscoveredByPlayer`, `IsHuman` or a techno's `House` raises
  `ShaderEmu_Scan_Gen` (`TDAWN_CHECK_SCANS=1` must say 0 wrong), a new `CellRedraw` setter sets `ShaderEmu_Cell_Flagged`, and what writes cells outside `DisplayClass::Read_INI` and a load unsets `ShaderEmu_Cells_Clean`.
  After a change to a function many inline, make the counts again (`experiments/rv_fewer/counts.ps1` of commit b6b5f2b9, in git only since `experiments` went), or its callers run without them. h2h's 27.4 s to 21.95 s (1.248x): a load is 1.59x, play 1.15x; say which.
  Played counts hold only where a non-public function's path is the same (GCC's sum has it): Tiberian Dawn compiles with relative paths (`tdawn-relative`); a game with whole paths loses its counts silently in another folder (113.6 M against 108.8), so before claiming a build's speed, build the commit in a second home and compare the bytes (`experiments/rv_fewer/home2.sh` of commit b6b5f2b9).
- Tiberian Dawn makes a unit's second path search on a worker core while the first core makes
  the first, and does not run a search that must come out as one already made (`docs/games/tdawn.md`,
  "A unit's path searches"). Code a search reaches (`Can_Enter_Cell` of units and infantry,
  `Find_Path` and what it calls) must store nothing outside `ShaderEmu_Core[who]`, and a new
  use of the threshold other than `Passable_Cell`'s comparison breaks the reuse:
  `TDAWN_CHECK_PATHS=1` must say `0 of them wrong`. A counting build runs with
  `TDAWN_SCENE=inline`, which has no workers.
- A worker stops at every page of the program's own code that no core has run yet, and every
  worker that comes to it stops too (cause 12): nxphys had 921 such stops in its first substep
  with 255 workers, and a third stack page each. A program that starts many workers on code
  the first core has not run reads its text first (`take_workers` in `nxphys.c`), and
  `STACK_TOUCHED` in `mcw.c` is three pages. `PHYS_COUNT=1` prints how many pages were brought
  for workers: it must say 0.
- nxphys's shares are its grid's cells by remainder: fewer shares than workers with the same
  fullest share were tried (the fewest with the least full one) and were slower, 9.1 ms a
  substep against 6.1; how full a share is does not say how long its substep is.
- The two Command & Conquer games decode a mission's shape files ahead on a worker in the
  first few hundred frames: a frame summed over all cores is measured from frame 301 on.
- An idle machine ran the whole tick path about 48 times a second only to poll its input:
  the idle loop re-armed the driver's timer at the next tick. It now does so for a second
  after any key, button or pointer move, and every fourth tick on a machine nobody has
  touched (`IDLE_POLL`, `touched_at` in `shaderemu_input.c`): 209 thousand instructions a
  second idle became 125, an idle desktop 222 to 144, and a desktop under a moving pointer is
  as it was (577). Polling every other tick always was a third fewer samples of a moving
  pointer: not taken.
- nxphys asked for 207 workers (`WORKERS_ASKED`, 255 now: see the later note on its shares with no
  bodies) when a substep was as long as with 255 in its three scenes on `rvc_cpu` and on the
  shader machine (120 substeps of the wall 1,439 ms against 1,442), and 48 cores were not
  turning in place. After a change to how bodies are shared
  out, measure 191 to 255 again (`PHYS_WORKERS`).

- Tick utilization is instructions run over busy passes times 8,192 (`pwsh tools\tick_util.ps1
  -Name N -Cmd "..." [-From TEXT]`, `docs/machine/boot.md` "Passes that end early"): it prints what
  ended passes early by reason and place, the kernel's idle loop apart, and a WORKERS line.
  In the harness an early end costs 0.2 ms, not the rest of the pass (a pass is 0.2 ms + 0.24
  us an instruction); in the world's fixed mode it costs the instructions.
- The write cache is three tables (64 + 32 + 16 buckets of three: core 0 is 500 texels, 8 rows,
  as a worker of 2^6; the third at 32 ended 0.02% fewer of Doom's passes). Its stalls were collisions
  at 50 to 80% full: another hash, or one that changes with a seed, did next to nothing in
  simulation, another bucket to try did (Doom 1,840 full-cache passes to 93). Four tables
  measured 0.6% slower.
- A port ends its frame with `seglSwapEarly()` and calls `seglSettle()` before it next writes,
  through a pointer, memory the list in flight reads (`docs/machine/gpu.md`); each keeps a switch for
  the old wait (`QUAKE_SWAP`, `NES_SWAP`, `CLASSICUBE_SWAP`, `IMDEMO_SWAP`, `GL_SWAP` `=wait`),
  which gives before and after from one binary. A plain hold cannot show a store made too
  early: hold late (`QUAKE_HOLD_LATE=1` and the like) and compare the two pictures.
- One core keeps about 6 KB of distinct 16-byte texels a pass. A structure stored to every
  frame is laid out in texels by what is written together (Quake's `particle_t`); a table is
  not cleared whole at every page or frame (`css_reset` was 168 KB a page); a decoded piece is
  made where it will lie, not copied there (`piece_place` in `nxplay.c`); megabytes go to
  borrowed workers and are not waited for until the memory is next used (`ui_bulk.h`).
- The kernel's part (`linux/kernel/*_hook.py`, in `build.sh`'s order): cleared memory comes
  from a pool the idle loop fills (`zeropool_hook.py`; `zerotest` and `tailtest` must say
  PASS), the ROM is read ahead a window a copy and a whole ROM page needs no `fence.i`
  (`stall_hook.py`), romfs looks names up where they lie (`romfs_hook.py`), the console's
  driver stops at 62 characters a pass (`console_hook.py`), input is polled from the tick
  (`tick_input_hook.py`). A hook that replaces another hook's text must keep that hook's
  mark: run `build.sh` twice after adding one. A hook skips a file that has its marker: after
  editing a hook, `git checkout` the files it patches in the kernel tree first.
- `GrGetWindowInfo` does not wait for the GPU any more. A program that writes its window's
  buffer reads a pixel first (`GrReadArea`), as `nxmon` and `nxpaint` do.
- `programs\lazy` tests fills and copies in every order on `rvc_cpu`: 20 lines ok, sum `1a4a1226`.
- A change to the tick is measured whole, alternating with the tree, on the shell loop AND on
  Doom's timedemo: they disagree (two SBI calls in the general path: level, and 1.9% under).
  Nearly every change to the tick's function, however far from the fast loop, has cost 1 to
  3% of every instruction; the tree sits at a level most changes leave.
- The kernel tick at 20 Hz with high-resolution timers is worth 5.5% of a game's frame at 2
  MIPS and was not installed: input is polled from the tick, so a key or the pointer would
  wait up to 50 ms; with the input poll kept on a timer of its own about 1% is left.
- The compiler's libraries are laid out for the ROM by `linux/tcc/pack.py` (`docs/system/cc.md`):
  what the examples take comes first in each archive and `mapped.c` asks for it in windows.
  After a change to an example, `gles.c`, `mcw.c`, Nano-X's library or musl run
  `linux/tcc/build.sh`. Reorder an archive's members, never its index, or every linked
  program's bytes change; `cc`'s outputs must stay the same bytes.
- `rvc_cpu` flushes a worker's TLB when it wakes for a new job: before, a program that freed
  memory its workers wrote gave wrong sums there and right ones on the shader machine.
- Every `rvc_harness` run goes through `tools\gpu_queue.ps1` (one at a time for every session),
  `--cpu --warp` captures too, which beside other runs gave garbage; the shared image is made through
  `tools\make_image.ps1` (one writer at a time).
- Whether the window system lets other programs run is measured by another program's frames:
  glxgears on the desktop under `rvc_cpu`, its 5 s lines while a load runs (menus in Files by
  `nxkey`, letters into the editor, nxbench, programs starting, `--pointer-sweep 0`), as a
  share of its lines with nothing else. On 12 October 2026: 52 to 95% before, 81 to 97% after
  (menus 52 to 89, three programs starting 56 to 86).

- A program's first core with nothing to do until the next pass calls `mcw_rest()`
  (`docs/machine/multicore.md`, "A core that rests"), not the pause hint: it is the `REST` ioctl of
  `/dev/gpu` (`_IO('G', 11)`, `shaderemu_gpu_rest`): the kernel yields, or does the idle
  loop's work and ends the pass. 322 instructions a call. Never from a job on a worker; with
  no workers open or on an older kernel it is the pause hint. `tools\tick_util.py` counts a
  pass that ended there apart (the `REST` line); the function must stay noinline with the
  pause inside it. `shaderemu_gpu.c` calls `zeroed_idle()`: it needs `zeropool_hook.py`.
- A page wanted of a mapped or executed ROM file brings the aligned 32 pages round it, a file
  of up to 32 pages comes whole, a file's last partial page rides in the same copy, and a lone
  piece under a page is copied by the processor (`linux/kernel/romwindow_hook.py`): the
  editor's first start went from 13 parallel copies to 3. `tailtest`'s sum covers
  `/usr/share/rom-dates`, which every image build writes anew: compare two kernels on one
  `rootfs.bin`.
- A breakpoint poked into another program sets a flag, and the kernel makes one `fence.i`
  when that program next runs (`linux/kernel/poke_hook.py`); a program poking itself still
  flushes at once.
- The shell vforks `command &` too, when every word of the line is plain text
  (`linux/userland/ash_vfork_hook.py`): a `$VAR`, a pipe or a redirection in the line is a
  fork again, with its copied pages.
- Before any timing on the card look for an `rvc_harness.exe` with no arguments (the
  developer's own): it holds the card at 100% outside the queue. A kernel build on all cores
  beside a run spoils a timing too.
- In WSL `$NAME` is the computer's name: a build script must not take an image's name from a
  variable called `NAME`.
- The display stream between players is judged while the picture changes, not by bytes or by
  the time to a settled picture (`docs/world/share.md`, "Judged while it moves"): record 300 frames
  with `rvc_harness --cpu --screen-log`, play them with `python tools/share_motion.py`, and
  hold two real clients' logs together with `python tools/share_clients.py` (`netTest`)
  (the tiles of before against the units, a packet every 0.15 s and every second), and a change to
  `ShareUnits.shader` must still give `tools/share_units.py`'s bytes (`bin\share_test.exe`,
  then `--against`), under fxc2 and under Windows' FXC: a `continue` in a loop, or a vector's
  component written by index in one, compiled under one and not the other.
- The pointer has a worker core of the window system's (`docs/system/nanox.md`, "The pointer on a core of its
  own"): the cursor and a dragged window follow it every pass, and the kernel reports no move
  inside the server's quiet rectangle. The core is lent (job, answer and "taken" words kept
  equal: the kernel takes it at once for a program that keeps workers). The test is a drag
  with the first core busy: where the layer is against the pointer, pass by pass (9 moves a
  second and 19 pixels behind before, every pixel and 1 behind now), and nxray must still say
  255 workers. A run's instructions shift half a percent with its timing: compare means of
  several timings (`sleep 0.3; ...`), never one run. What changes the window tree must raise
  `gd_scene_gen`, or the layer table and that rectangle are stale.
- Quake III Arena (`docs/games/quake3.md`) draws from the ROM: its light maps and models' poses are
  read by the GPU where `quake3-se.dat` lies (its textures are decoded from that file into GPU
  memory, "Pictures decoded when a level loads"), so that file must be a file of the ROM
  and the GPU shader must have the draws of 13 October 2026 (`0x800` lit, `0x10000` moved,
  `0x20000` facing). Its server's frame is on a worker core: anything of the engine that the
  server and the client both write needs one for each core (`SE_OnWorker()`), and an
  allocation the server makes goes to its own zone (`server.h`). After a change to what the
  game asks for, run it with `QUAKE3_WANTED=1` on the whole demo pak and write `wanted.txt` anew.
  Wherever the first core waits for a job, it polls that job's helpers too (`helpers_poll`,
  `SE_ClientPoll`): a helper stopped at a page otherwise stops the game. The level's items are
  made on a helper of the client's game (`SE_SideFits` in `cg_ents.c`): what it makes there
  touches its own entity only, and `QUAKE3_SUM=1` with `QUAKE3_SIDE=inline` must say the same.
- The world's speed is measured in the editor with the Game view in front (play mode started by a
  script leaves the Scene view there and the room is drawn small), by `totalInstructions` over
  several seconds, a change and the tree in turn (`docs/machine/multicore.md`, "In Unity"). A frame of 8
  rounds is the card's: the room is 3% of it and a round's other draws 5%, the ticks the rest. A guest's wait for
  something the host gives once a Unity frame shows as rounds of 52 instructions to the frame's end.
- The browser's engine (`linux/apps/css.h`, `web.h`, `web_layout.h`; `docs/apps/web.md`) has no part of
  the window system in it and is tried on the host first: `tools/web_test.c` lays a page out
  with the image's own fonts, counts texts past the edge and over each other (both must be 0
  but for a listing), and `nxweb` with `NXWEB_DUMP=1` must print the same counts in the
  machine. Real pages for it come from `python linux/apps/web/preload.py --pages --cache DIR`
  into a private image's overlay. The fonts are `python tools\make_fonts.py`, before the image
  is made (`docs/system/fonts.md`); without them, or under a window system from before them, `nxweb`
  says so and draws in the built-in font. A capture that is not a timing needs no card:
  `rvc_harness --cpu --warp`, still through `tools\gpu_queue.ps1`.

- The fast step tells an instruction's kind by a tree of tests and a load has one way out
  (`shader_vm/tick.hlsl`; `docs/machine/tick-costs.md`, the
  sections of 13 October 2026). The text's form is the gain: the same tests as bools computed
  first were 4% slower, and operations counted do not say which way a shape goes. After any
  change to the fast loop's control flow run the three loads in turn from one snapshot (a
  shell loop, Doom's timedemo, Quake e1m1), and bring the function to Unity's copy verbatim.
  The fast loop's arithmetic reads no funct7 but bit 30 (and 1 for M), and a branch's funct3 2
  and 3 are beq and bne (`tick-costs.md`, "tests the fast loop did not need": the test was 2% of
  the shell loop): an encoding of an extension we do not have runs as the base instruction it
  resembles, in rvc_cpu too. Zba/Zbb on the tick need a funct7 test of their own. A Quake gain
  under 1.5% is noise until a second 50 s run of two ticks says so.
  The third round (`tick-costs.md`, "checks given up, small shapes, and FAST") gave up more such checks,
  each in rvc_cpu too: a plain fence is no instruction in the fast loop; loads test no funct3 (lwu is lw)
  and no 0, stores no `f3 < 3`; an atomic's funct5 under 16 none of ours is amoswap and lr's rs2 is not
  looked at; jalr's funct3 is not looked at; fadd, fsub and fmul keep the card's NaN (`7fffffff` on NVIDIA,
  `card_nan` in rvc_cpu): a program must not test a NaN's bits; `op_m` relies on D3D's division by zero
  (divu all ones) and INT_MIN / -1. A guest that wants such an encoding trapped, or RISC-V's NaN from
  them, changes the tick first.
- `FAST` (`--define FAST` on the harness, `rvc_cpu --fast` or `RVC_TICK_FAST=1`; off by default, never
  the default): a load or store in RAM not on its size takes its word's bytes, fdiv is the card's `a / b`,
  an sc always succeeds, an opcode of no group is no instruction. 0.2 to 0.8% of a tick, and it breaks Red
  Alert's and the NES's state, the IDE's breakpoints (the word 0) and fptest's fdiv (`tick-costs.md`,
  "FAST"). A change to a FAST block changes rvc_cpu's `tick_fast` paths with it.
- The tick's write cache (`tick-costs.md`, "Fourth round"): a fast-loop store calls `ram_store_nc` with no test
  of `ok` and a mask of 0 when it may not, so a mask of 0 must store nothing there; a texel enters the cache only
  where `ram_store_nc` makes a new entry, which sets its 16 KB region's bit in `regs`, and a load searches only
  where that bit is set: a new way into the cache sets it too, or a load misses its own store. sstatus is a fast-loop
  CSR while `mip & mie` is 0: a new way for an interrupt to become pending within a pass makes it the general step's.
- The tick's modes (`tick-costs.md`, "Fifth round"): a frozen pass of addi (`RVC_FREEZE=1 --bench`, the word one
  instruction every turn) is about 1.01 ms on the tree; a `mov` moved (`pre_w = w` from before the fast loop's break)
  or an fxc2 switch can put it at 1.18 and every load at +6%. Run that pass (and lw's, sw's) on a tick change before
  believing its whole runs. `fetch`'s read ahead (`fw1`) stays at the fetch block's own level: under a nested branch it
  is waited for there (+6 to +13%). fmv.w.x and fmv.x.w have the float block's own test before `fp_light`.
- A round in Unity costs what a pass costs in the harness (the same fxc2 bytecode); what is
  left between them is 1.4 ms of every frame that is no round's (the room's cameras 0.65).
- Doom's view is made by up to four worker cores from what the first core tells it between
  tics (`docs/games/doom.md`, "The view on worker cores"). A new place the game writes a thing's
  state, place, height or speed needs `doom_thing_changed`, or a sleeping thing never wakes:
  `DOOM_VIEW=told DOOM_VIEW_CHECK=1` must say 0 not as told, and the three demos' state sums
  must hold with and without `DOOM_PORT=rest`. Compare held frames by `DOOM_SUM`'s order-free
  sum: the ordered one depends on where the heap lies.
- A program whose worker calls `gles.c` compiles the library apart, a variable a section on 16
  bytes (`doom.sh`, `linux/tdawn/build.sh`, `linux/nes/build.sh`): inside a whole-program LTO
  link the library's worker-written statics lie among the program's by chance (the NES had
  `line_news` in one texel with `matrices_dirty`).
- Memory a worker will store to wants a store by the first core first, GPU memory and the
  mailbox pages too, and the code a worker starts in a read (`mcw_open` stores to the
  mailboxes; nxpath reads `mcw_entry`'s page): `PHYS_COUNT=1` and mctest's
  "pages brought for their start" must say 0. A job that waits ends its pass with
  `mc_end_pass()`, never with the pause hint, which is a worker's sleep.
- A program that uses workers is run once with `RVC_RACES=FILE` on `rvc_cpu`: 0 undone, and
  its own checks as without it, or it is wrong on the card now and then (`docs/machine/multicore.md`,
  "Races"; reads "as it was" are a core waiting on another's word, and are no fault).
  Its loops, calls, prints and counters go through `mcwx.h` ("The second layer"), not code of
  its own; `RVC_CALLS=FILE` and `tools/mc_candidates.py` say what of core 0's is worth a job.
- Events are pushed to a program (`GrNumPushEvents`) and a window's, GC's and font's ids are
  the program's own (requests 127 to 129; `docs/system/nanox.md`). A program with its own `select()`
  on the socket still gets one event a `GrServiceSelect`. A program's start is measured as the
  first window of a session and as a later one: the first pays for the icon list and the font.
- nxphys asks for 255 workers and gives the grid and the pictures to shares with no bodies
  (`PHYS_SERVICE=0` is the old sharing; this replaces the 207 above). nxpath is a mesh viewer
  since 13 October 2026 (`docs/apps/raytrace.md`): its sum is `d63313e9` (since the float libm of 28 October 2026; 160 x 120, 2 samples, with
  workers and with `NXPATH_CORES=0`), and `NXPATH_CHECK=500` must say 0.
- `tools/private_image.py` reads the shared root each time: a "before" image made again after
  the root was rebuilt is no longer before. `linux/prebuilt/root` of the last commit is.
- A `Con_Printf` while a Quake level loads draws a frame: prints added there move the numbers
  of held frames.
- `/persistence` is the one folder a host keeps between sessions, and only when asked
  (`docs/system/persist.md`): `nxpersist`, the fetch's `persist:` addresses, `--persist FILE` on
  `rvc_cpu` and `rvc_harness`. The `nx` script runs `nxpersist load` before the desktop: a
  desktop's start has that program's start in it. A program that should keep a file writes it
  under that folder (its starter's `cd`, `HOME` or a link), and 96,000 packed bytes are all.
- A setting a program wants kept is a key of the registry all programs share (`registry.h`,
  `nxreg`; `/persistence/registry`, `docs/system/persist.md`), not a file of its own: `program.name`,
  set at once, read at start. A test that must have the defaults does not read it (`RAY_FRAMES`).
- A holodeck's sitter on a desktop plays by the host's words, not by the window's keys
  (`docs/world/holodeck.md`, "Played from the seat"; `EmuHolodeck.ByWords`): W, A, S, D and the arrows
  are the left stick, Ctrl or the left button the right trigger, the right button the left one, Space the jump bit, E the right grip (Half-Life's use). A game
  played from the seat reads the stick and the two triggers while seated (`seated` 1 and 2), and
  its number is in `ByWords`; `rvc_cpu --host 00000112,0,0,0,0,00007f00,0,0` with its `_SEAT`
  switch must walk it. ClassiCube started in the background there wants `</dev/null`.
- The harnesses lock each other out (`harness/apps/run_lock.cpp`; `docs/machine/cpu-harness.md`, "One card, many
  processors"): CPU ones (`rvc_cpu`, `rvc_harness --cpu`) together, a GPU one alone; a refused one prints
  `[lock] ... refused` and exits 75 having done nothing. A GPU timing waits until no CPU run is left.
- `game_par.py`'s card model has `RVC_ADFAULT=1`: the card's walk faults on a clear accessed or
  written bit, and a worker stops there until core 0 serves it. `/dev/gpu` maps its pages with
  both set (`shaderemu_gpu_mmap`): its `[cpu] stops:` line must say 0 on a written bit.
- The browser draws a page in strips of 64 rows, each a GPU list of its own made by lent
  workers (`strip_job` in `nxweb.c`, `docs/apps/web.md`, "Strips"), and styles subtrees on them
  (`web_style_range`). `strip_job` draws what `draw_item` draws: a change to one is a change to
  both, and `NXWEB_CHECK=1` in the harness with `--cpu --warp` must say 0 pixels and 0 elements
  differ. The first core submits the lists; a worker never takes the GPU's lock word.
- Every program decodes and encodes pictures, sound and films with libmedia (`linux/libmedia`,
  `docs/system/media.md`), on the worker cores. A change to it keeps `python linux/libmedia/test/check_pictures.py
  MEDIA_TEST --wsl` at 0 bad and the sums of `docs/system/media.md` (sound, films) and of `nxplay` and `nxvideo`.
  The image's pictures are JPEG in restart intervals or PNG in segments (`png_segments.py`), which every
  core decodes by itself: a tool that writes one asks for that (`restart_marker_blocks`), not a PPM.
- `python tools/app_fuzz.py` opens, uses and closes the desktop's programs in plans that replay (`docs/system/fuzz.md`), and its oracles
  are what is left behind: a process, a window, the GPU's drawing. On this machine a `fork` is a vfork: a child that does not exec calls `_exit`, never `exit`,
  or it runs the parent's atexit functions on the parent's memory (OpenTTD's links), and a program a program starts should close what it was handed.
- After a change, `python tools/test_all.py` (`docs/machine/cpu-harness.md`, "The test set"; two minutes): every
  case must pass, and `--races` too for a change to work on the workers. A value moved on purpose is set anew there.
- The overlay over the ROM could not rename a file or folder of the image: its upper file system (memory on the
  no-MMU kernel) had no rename that leaves a whiteout, so `rename()` and `mv` said EINVAL, and a folder of the
  image could not be deleted (EIO). `linux/nommu/patch_ramfs.py` fixes it (18 October 2026; `docs/system/nommu.md`)
  in a kernel built since; on an older one a file is still copied and unlinked (`move_path` in `nxfiles.c`).
- Where a program's instructions go, which of its loops and calls could be a worker's and which slow forms it has:
  `python tools/prof.py report|auto` on an `RVC_PROF` run (`docs/machine/profiling.md`), with the ELF the image
  was built from; profile without test sums (`NXVIDEO_SUM`, `NXVIEW_SUM` were most of core 0). A flat program's
  `__executable_start` is 0 at run time: `mcw_touch_code(__executable_start, ...)` reads every page from address
  0 (1.6 million instructions); start from `_start`'s page (`m_workers_begin` in `m_core.c`).
- unarc (`linux/apps/unarc.c`) unpacks zip, tar, `.tar.gz` and `.gz` on the workers through libmedia's
  `media_inflate_stream`, and `open --unpack` runs it first; it exits 3 (busybox then) for what it cannot do,
  a lack of memory in one piece too. `tools/test_all.py unarc` packs a folder with busybox and unpacks it both ways.
- The devices are 0x80000000 to 0x82000000 and RAM is one range, 0x82000000 to 0x92000000, the kernel at its start
  (28 October 2026; `docs/system/nommu.md`, "Memory"): the layout is `shader_vm/src/layout.h` for the tick and
  `harness/apps/rvc_layout.h` for the host. The state texture is 2048 x 9280 (`STATE_TEX_HEIGHT`, `kStateHeight`, Unity's
  StateA). The commit's band mask stays 32 bits, bit b for bands b, b + 32 and b + 64 (`ram_bands`); the control words'
  bands (4 and 5) are drawn apart (`_CommitAlways`, `CONTROL_BANDS`). A GPU copy into RAM goes only below 0x81e00000. A
  snapshot of the layout before (devices at 0x86000000) is refused: make it again.
- RAM is one texture written in place (`src/ram.h`, `docs/machine/gpu.md`, "RAM in one texture"; Unity has StateA alone):
  no commit pass reads what it draws, what it needs of RAM is gathered into a stage first, and a copy from RAM to RAM
  is 256 KB a pass, in `rvc_cpu` too (`copy_pass`). `programs\copytest` must say `sum 0e7f76c3` on both.
- OpenTTD (`docs/games/openttd.md`) is compiled by its own GCC with all of libstdc++ (`linux/openttd/toolchain.sh`), runs on
  the first core but for its viewports (`ViewportFlush` in its `viewport.cpp`: the parts of a picture cut in pieces, each
  found, sorted and drawn by a core into a buffer of its own), and its patch keeps the game state the build computer's: after
  a change to `openttd.patch`, `tools/test_all.py openttd` must give `linux/openttd/host_sum.sh`'s sums. A start is measured
  with `-v null:ticks=1 -g` under `tools/prof.py` (350 million instructions on 20 October 2026); the title screen's game is
  `make_title.sh`'s. Play is measured by `OPENTTD_PLAY=2 openttd -g /usr/share/openttd/busy.sav` on `rvc_cpu` at 2 MIPS,
  the second round (757.8 s upstream's drawing, 368.9 on 31 workers, 21 October 2026). What a worker draws may store only to
  its core's state (`VpCore()`; a vehicle's colour map is made by the first core first) or it is wrong on the card:
  `RVC_RACES` 0 undone, and `test_all.py openttd-scroll` and `openttd-scroll-serial` (`OPENTTD_CHECK=1`: every picture
  against one drawn whole) must say 0 pixels after every change to the drawing: the sprites are sorted in one order
  whatever the part ("One order"), and the machine's copy moves only whole words (texels of the destination, "Scrolls").
- The world's LAN carries a machine's packets as network events of a player object of their own
  (`EmuNet.Packets`, `docs/system/lan.md`), not in `EmuShare` with the display: a ping's round trip
  between two clients 300 ms to 230. It is measured by the hub's `lanTest` with `EmuGate.testOpen`
  (a test client never leaves the corridor) and two Build & Test clients; `[Lan] times` lines
  say how long batches were on the way. The guest's driver polls from the kernel's tick
  (`tick_input_hook.py`), not a timer of its own.
- Lanes (`docs/machine/lanes.md`, lean tick only): a worker's pixels each run `fn(index, ctx)`
  (`mcw_lanes`), and a field of 3,968 lanes of no core's takes a job from a program's first core and
  does not wait for it (`mcw_field_begin`/`_ready`; `mcw_shade` in `mcwx.h` for a picture). An index
  stores only to its stack and its own texels of the output, and ends within one pass (8,192
  instructions; a texel of four pixels is an index): one that does not is skipped and made by the
  core that asked, or by its workers (`mcw_field_apart`, as `nxpath` does: 2% of its paths). A group
  leader's texel says which of its lanes skipped any; the first core must not scan every lane (290 ms
  a job). A static the tick sets before a pass's loop and reads after it is carried through every
  core's loop: one such (`lane_skip`) cost Doom 0.7% with no lane running; keep such a value in a word
  carried already (bit 30 of `lane_j`) or read it again after the loop. `rvc_cpu` has no lanes and gives the same output by plain calls: `lanetest` checks both,
  `RAY_FIELD=0`/`RAY_LANES=0` are `nxray`'s ways back. In the tick HLSL evaluates both sides of `||`
  and `&&`: a call with side effects behind one ran for every pixel (every finished lane took itself
  for stuck). The field needs the host's primitive and points (`BackendOptions::laneField`; in Unity the
  tick mesh's 17th triangle and `_FieldPoints`, wired but not yet run in the world). Lanes gain from large caches, the field from neither.
- Work longer than a pass goes on the lanes in steps (`docs/machine/lanes.md`, "Steps"; `mcw_field_steps`,
  `mcw_steps_left` is CSR `0x0ba`): a step keeps what it needs in its output, no registers are saved. A
  step stops only where what the pass has left covers the longest stretch to its next stop: run the
  program's self-test on a worker of `rvc_cpu` (`mcw_steps_probe`; `NXPATH_PROBE=3000` on every scene
  of `nxpath`), which must say 0 over a pass and 0 paths not as traced whole. `steptest` must PASS. A
  change there must leave the plain path's instructions as they were (`NXPATH_FIELD=0` on `rvc_cpu`):
  the lane's copy of a search is its own branch (`limited`), so the workers pay nothing for it.
- The browser opens `http` of the machine's own network over its own sockets (`linux/apps/web_net.h`, `docs/apps/web.md`,
  "Addresses the machine reaches itself"), never waiting in a call: the rest goes to the host. A change there runs
  `tools/web_net_test.c` on the host and `tools/test_all.py web`, and a capture of a form (`NXWEB_CHECK` sees no strip with a field).
- Half-Life's state made per core (`x_of[3]` behind a macro, `SE_Who()`) must be made on every core that
  uses it, not once at start on core 0: the studio hit hulls were core 0's only, and every bullet of the
  server's worker went through monsters (`docs/games/halflife.md`). Decals are pulled toward the eye for
  `glPolygonOffset` (`gl_shaderemu.c`); a seated hand's shots leave the hand (`hlsdk.patch`, `impact_index` 1).
- Half-Life draws its 3D on a worker a frame behind by default (`HL_RENDER=core` is core 0's drawing;
  `docs/games/halflife.md`, "The drawing a frame behind"): what the drawing's worker writes is its own
  (`SE_RenderWho()`), and a renderer function core 0 calls has a kind in `ref_kind` (`rnd_shaderemu.c`):
  a new one settles unless listed; the 2D core 0 records is replayed by the frame's own drawing after its
  3D (`HL_2D=core`: by core 0 at the swap). Measure with `python tools/game_par.py halflife`: 0 undone, both ways.
- Quake III's server skips what is as it was (`docs/games/quake3.md`, "The server's frame told and linked
  cheaper"): a snapshot entity equal to the old frame's (`se_snap`), a link with the box of the last
  (`se_boxed`: clear it if anything else changes an entity's leaves), a view's look at an entity
  whose leaves are as they were (`se_seen`, `se_gen`: anything else that changes an entity's areas or
  clusters raises `se_gen`), the bots' library told an unchanged state (`SE_BotLibTell`). Three bots'
  server sums are compared with `QUAKE3_ENTER=1000`: otherwise they go by the client's load time.
- HLSL shaders are compiled in the machine at run time (`programs/hlsl`, `hlsl.h`, `docs/system/hlsl.md`) to
  steps on the lanes; `linux/tcc/examples/fractal.c` is its example. After a change to the compiler run
  `programs/hlsl/test/hlsl_test.c` on the host (PASS, steps under random budgets the same as whole calls), and
  `test_all.py hlsl`: `rvc_cpu`'s picture must be the host's to the byte (sum `3614e8bb`). Each loop that is not
  unrolled stops by its own measured longest step; on the card `hlsl_bench` must say 0 steps too long.
  The IDE checks and builds a `.hlsl` with `hlslc` and runs it in `hlslview` (the example): `test_all.py nxide-hlsl`.
- Half-Life's server and drawing workers run on stacks of their own (`se_server_stack`, `se_render_stack`), where
  `mcw_on_worker()` is false: code they reach must not load or inflate a file or start workers itself. A packed file
  they open is opened by core 0 (`FS_OpenInflated` through `SE_ServerOnCore0`), `SE_MediaWorkers` is 0 off core 0
  and while the rings hold the workers, and a model's sequence groups load with it (`SE_StudioGroupsLoad`): one first
  played on the drawing's worker froze hldemo1's first corridor (`docs/games/halflife.md`). A static both may write
  is indexed by `SE_Who()`, and what a worker writes lies in texels apart from what another core writes (`SE_APART`):
  `world_ahead`, the loopback's `get` and `send` and `net_drop` shared texels and broke the card's connect only in
  one link's layout (27 October 2026): `RVC_RACES` with `RVC_RACES_NEAR=4` on `rvc_cpu` finds such texels
  (docs/machine/multicore.md, "Passes apart"). A change to the game is also walked: `HL_SCRIPT` with `+forward`, turns and shots for
  1,200 s of the guest's on the desktop with `RVC_RACES` (standing in the fight never played a sequence group).
  A level that goes settles the server, the drawing and every job first (`SE_LevelSettle`), and off core 0 a group
  is loaded when its pointer is set (`Mod_CacheCheck` walked pools core 0 moves): a change to either is run through
  the elevator and `changelevel`s, with the default and `HL_RENDER=core` (docs/games/halflife.md, "A level that goes").
- Half-Life's client keeps its predicted states by the command and its drawing has two sets of entity copies
  (`docs/games/halflife.md`, "A move predicted once", "What core 0 did between the drawing's end and the next give"):
  a change to the prediction or to what `SE_AsPredicted` compares runs `HL_PREDICT=check` (0 over 0.1), and a cache in
  the drawing goes by the live entity (`R_SceneLive`), not a copy's address.
- OpenTTD's game tick runs on a worker while the first core draws (`docs/games/openttd.md`, "The tick on a worker";
  `OPENTTD_TICK=slices|whole` on the first core, "The tick in slices"): it yields at `TickYieldPoint()`, and what the
  drawing does while it is held must change no game state; a new path from the GUI into the game (a command, a save)
  calls `TickFinish()` first. A change the tick makes to what a vehicle looks like goes through `UpdateViewport` or
  `MarkAllViewportsDirty` (its drawn copy is made from that); what reads or writes the screen calls `VpFlushFinish()`
  first, as a picture's last flush is still being drawn. A per-core array a worker writes is 16 bytes a core
  (`WaterRegionCheck`: 12-byte vectors were undone under `RVC_RACES`). `OPENTTD_SUM_AT=36` on the scroll must give
  `d4e04e63`/`26aae0d5` on the worker, whole and sliced; `OPENTTD_CHECK=1` is 0 pixels paused (a running game can
  differ by "One order"). A scroll is measured with `OPENTTD_PLAY_PART=scroll|follow` and the card's model
  (`RVC_ADFAULT`, `RVC_WCACHE`), pictures and game ticks a second both.
- A window in OpenTTD costs the game its tick, which waits while the first core paints (`docs/games/openttd.md`, "Windows"): a viewport marks
  `AddDirtyView` (only windows with a viewport are drawn there, the viewport's widget alone), and what the tick says of a window's widgets
  waits in `AddDirtyLater` for `OPENTTD_WINDOW_MS`; what a click or a key marks, or a window that comes, goes or changes, is at once. Measure
  with `OPENTTD_PLAY_PART=window`, by the share of the time the tick is held, and `test_all.py openttd-windows` must say 0 pixels.
- OpenTTD draws a picture every 33 ms and a picture with only views to draw does not hold the tick (`docs/games/openttd.md`, "Pictures
  the tick does not wait for"; `_se_light`, `OPENTTD_LIGHT=0` and `OPENTTD_FRAME_MS=66` are the way before): what such a picture reaches
  reads a vehicle's drawn copy and changes no game state, and what it owes (stale texts, invalidations, new vehicles' sprites) is done by
  `SeHeldWork` at the next hold. Ships' paths are made during the tick by two workers of their own (`AheadLoop`, `OPENTTD_SHIP_LOOPS=0`
  is without): the map is not still under them, so all they read is compared when the path is taken, and `OPENTTD_SHIP_AHEAD=check` must say 0 differ.
- OpenTTD's lists do not wait for the GPU, which in a world takes one a Unity frame (`docs/games/openttd.md`, "The card's lists"): records and
  chunks live two frames, a list that finds the GPU busy waits on a queue. What a list reads and is changed later settles first (`SeGpuSettle`); run
  `test_all.py openttd-scroll` with `RVC_DEVICE_EVERY=6` too, since `rvc_cpu` otherwise takes a list back at once: calibrated runs do not show this.
- A vehicle in OpenTTD must always have a drawn copy: light pictures and a following view read it (`Vehicle::drawn`; a new one in a depot had none, and its window showed the map's corner; `test_all.py openttd-depot`). A sync runs with the tick
  (`docs/games/openttd.md`, "A sync with the tick running"): copies are made at the tick's end and listed by the tick; what changes a vehicle apart from `UpdateViewport` on the tick's worker sets `tw_made[...].told` and `VehicleChanged`, and `OPENTTD_MADE_CHECK=1` must not rise after the first hold. A check run (`OPENTTD_CHECK`) makes copies by another path: `test_all.py openttd-depot` is what follows a moving vehicle in a run without it.
- OpenTTD makes copies only of vehicles near a view (`docs/games/openttd.md`, "Round four"): a vehicle's `drawn` may be anywhere while `veh_far`, and what reads it for a place outside the views goes by `x_pos`. `OPENTTD_MADE_CHECK=1` must say 0 off; `tools/openttd_title.py` (title and the drive with lists drawn) must PASS: `test_all` draws no lists.
- OpenTTD saves uncompressed by default (`docs/games/openttd.md`, "Saves are not compressed"): `nxpersist` packs what is kept; a format that cannot start falls back to `none`. `test_all.py openttd-save` must say as saved.
- OpenTTD is compiled whole with `openttd.profile.gz` (`docs/games/openttd.md`, "The compiler"): its LTO link keeps
  `-fdata-sections -fno-common` and the 16-byte assembler, or statics a worker writes share texels (`RVC_RACES` undone), and
  after a large change to the source `linux/openttd/counts.py` makes the counts anew. `build.sh` puts the tree back from
  `openttd.patch` on every build: write the tree's diff to the patch first.
- Age of Empires (`linux/aoe`, `docs/games/aoe.md`) is decompiled code with the trial's data, and no game data is ever committed: it stays out of `ALL` in `linux/nommu/build.sh` and out of `linux/prebuilt`, and goes into every image (4 November 2026: no game is optional; `AOE_IMAGE=0` only for a private test image), `image.py` fetching the trial once by `linux/aoe/data.py` against its checksum into a cache outside the tree and building the program (`program.sh`); one it cannot have stops the image. The programs' build (`linux/nommu/build.sh`) downloads nothing. The image holds the trial decoded (4 November 2026, the developer's decision: `data.py` decodes `MSAoE.exe` by 7-Zip into `/usr/share/aoe/installed`, the installer itself no longer in the image): the starter's `cabinst --installed` shows the EULA, Accept only once it is scrolled to its end, and links the files into `/tmp/aoe` at every start, the folder deleted when the game ends (`docs/system/install.md`); the EULA is never skipped; `aoeprep` makes `language.txt` and the faces there, and a wave on an odd address is copied at its load. `test_all.py aoe-install` holds the first start. Upstream's code holds offsets of the Windows build's objects (`0x194` in `vis_unit.cpp` broke memory after 700 updates): take a member's place from the compiler. `test_all.py aoe --image` an image made so must say its sum, and `aoe-640` and `aoe-drag` hold its window and its controls (the right button dragged moves the map, `AOE_SCROLL=edge` and `AOE_SIZE=800` the ways before); a change that draws differently keeps the pictures' bytes under qemu (`AOE_DUMP`), but for the main menu in the window, whose picture for 800 x 600 is brought down whole since 3 November 2026 (`aoeprep`'s `aoe-menus.bin`, `port/aoe_slp.h`; `AOE_MENU_PICTURE=middle` the way before). Every pointer place the game gets goes through the window's stretch (`aoe_win_map`), events and the pointer words alike: `test_all.py aoe-resize`. A change to `cab.h` or libmedia's `media_mszip` keeps `cab_test-host`'s files 7-Zip's (every file of both installers) and the pak's CRC-32 `239abc19`. Its music is the game's own `TMusic_System` over MCI and a mixer of ours (`port/aoe_music.cpp`, "Music"): `aoe-music` must say the same sum with songs ending, what tells the game of a song must not move `AOE_TICK_MS`'s clock, and `linux/userland/shaderemu_fm.h` is OpenTTD's too (`OPENTTD_MUSIC_SUM=all` as before). The world's update runs in slices between pictures on the first core (`port/aoe_world.cpp`, docs/games/aoe.md "The world in slices"): it gives way only between two objects' updates (`aoe_world_may_give_way` in `RGE_Object_List::update`), while it is out nothing may write the world (the view's walk and the panels read it; an input that is not a move calls `aoe_world_finish` first, in `DispatchMessage`), and its clock stands still out; a new place that gives way or a new path from the UI into the game keeps that. `AOE_WORLD=whole`, `AOE_TICK_MS` and `AOE_VIEW=inline` run it whole, as the sums are made. The path finder's distances are a table (`port/aoe_path.c`): `AOE_PATH_CHECK=1` must say 0 not as the doubles. `game_par.py aoe-busy-640` (a dozen villagers ordered at once) and the `-whole`/`-before` twins measure it; `tools/aoe_shots.py` reads the script's `shot` pictures out of `rvc_cpu --save-ram`. Only the first core asks for the machine's copy (CSR 0x8b0 reads 0 on a worker): the view's scroll copy is handed to it (`aoe_view_first_copy`), and `test_all.py aoe-scroll-check` (a drag under `RVC_DEVICE_EVERY=6`, every picture against one drawn whole) must say 0 pixels.
- Jedi Outcast's demo (`linux/jk2`, `docs/games/jk2.md`) is drawn by rd-vanilla over Half-Life's GL shim (`gl_jk2.c`, the renderer's `glX` renamed `jk2_glX`) and stays out of `ALL` and linux/prebuilt: it goes into every image (4 November 2026; `JK2_IMAGE=0` only for a private test image), which holds the demo's pak and licence decoded from LucasArts' installer `jk2demo.exe` (`data.py`; 4 November 2026, the developer's decision: the installer itself before), the starter's `cabinst --installed` shows the licence and links the pak into `/tmp/jk2` after it is accepted, at every start, and deletes the links when the game ends (`docs/system/install.md`, `test_all.py jk2-install`), and nothing made from it is kept (the weather cache file is not written). After a change to the game, `test_all.py jk2 jk2-sums --image` on such an image must give `host_sum.sh`'s and the build computer's sums (runs with no window and no sound card, or `JK2_SOUND=mix`: the card's pace moves them), and a change to `CM_PointContentsGrid` or the weather's cache must keep `JK2_WEATHER_CHECK=1` at 0 words wrong.
  Its music is the card's (`docs/games/jk2.md`, "Music on the card"): the engine's background tracks give `SE_MusicGive` what they gave the mixer and libmedia decodes each track on the workers into a stream voice
  (`JK2_MUSIC=engine` the way before; with no card the engine mixes it, so the sums do not see it); a change to the engine's music keeps `JK2_MUSIC_LOG=1`'s streams starting and ending where the engine meant them, and `RVC_RACES` 0 undone.
  The GPU's passes are not OpenGL's order (`gl_jk2.c`, `docs/games/jk2.md`, "What the menu was missing"): 2D before a frame's first 3D draw is depth-tested at the far plane, 2D after it has no depth, and `GL_ONE, GL_ONE` has alpha 1;
  a change to the shim's drawing is captured (`rvc_harness --cpu --warp`): the main menu must show its turning logo, title and saber, and the level at frame 900 must be the same pixels. Speed is measured by `game_par.py jk2-menu`, `jk2-walk`, `jk2-march` (`--calibrated`).
  In play rd-vanilla's back end runs on a worker a frame behind (`docs/games/jk2.md`, "The drawing on a worker"; `JK2_RENDER=core` the way before): what the back end reads that core 0 changes meanwhile is copied at the give (Ghoul2's bones, `SE_RSBones`) or kept per side (`R_RotateForEntity`'s scratch),
  a GL call of core 0's settles the job first, and the back end must not malloc, print or ask the clock on the worker; Ghoul2 surfaces are skinned by the GPU's vertex stage from the model where it lies (`SE_GhoulOnGPU`, `jk2glSkinned`, gpu.h's `VERTEX_BONES`; `docs/games/jk2.md`, "Ghoul2 on the GPU"; `JK2_SKIN=helpers` the helpers of before), whose operations are `RB_SurfaceGhoul`'s and `RB_CalcDiffuseColor`'s in their order: a change to either, or to what `SE_GhoulOnGPU` takes, changes `gpu_bones_vertex` with it and keeps `linux/jk2/skintest.c`'s `SKINTEST=cpu` and `SKINTEST=gpu` captures under `--cpu --warp` the same pixels; and the mix is painted on a worker during the server's frame, settled at `CL_Frame`'s start (`JK2_MIX=core`). A change there keeps `RVC_RACES` with `RVC_RACES_NEAR=4` at 0 undone on the walk and the march, and a capture drawn on the worker the same pixels as `JK2_RENDER=core` a frame earlier.
  A frame's list has 512 KB of vertices (`SET_SIZE`) and what does not fit is refused, the HUD first: large draws are drawn from words where they lie (`gpuDrawAt`: the world's kept faces, `SE_KeptDraw`, and the memo), and memory a list reads is let go after `jk2glSettle`. A batch may hold kept faces and no indices (`SE_TESS_PENDING`, not `tess.numIndexes`). The renderer's counters are each one side's (`se_pc` the front end's, `backEnd.pc` cleared by the back end), and the weather's random numbers with a window are its own (`SE_WxFlrand`): the game's are in the sums. A change to the drawing is captured with `JK2_HOLD=F` (`rvc_cpu --save-ram` at "jk2: holding", `tools/gpu_reference.py`) against its ways back (`JK2_KEPT=0`, `JK2_WEATHER=drawing`, `JK2_WEATHER_WORDS=drawing`, `JK2_MARKS=chop`, `JK2_PLAYERS_WAVE=all`, `JK2_SKY=clip`, `JK2_MSG=bits`, `JK2_KEPT_BMODELS=0`, `JK2_WEATHER_DRAW=begin`, `JK2_SKY_DRAW=begin`, `JK2_FRONT=core`): the same pixels but the HUD's two dials, which differ between any two runs (`docs/games/jk2.md`, "The drawing's worker lighter").
- Chess (`nxchess`, `docs/games/chess.md`): a searcher stores only to its core, its table, its ring and its status; keepers alone store to the shared
  table, each to its shard; the first core stores only `ch_stop` and `ch_deepest`. So a result on workers has no sum: the check is `nxchess solve`, and
  `NXCHESS_WORKERS=0 nxchess search 6` must be `chess_host 6`'s. It asks for 63 workers because 255 has more nodes and less depth on the card: measure depth in fixed time, not nodes.
- The GPU has a VRAM (`docs/machine/gpu.md`, "VRAM" and "Draw IDs"): Doom, Quake (its light maps too), Quake III, Half-Life,
  Jedi Outcast and ClassiCube draw their textures from it (`DOOM_VRAM`, `QUAKE_VRAM`, `QUAKE_VRAM_LIGHT`, `QUAKE3_VRAM`,
  `HLGL_VRAM`, `CLASSICUBE_VRAM` `=0` the ways before). A texture there is a copy made when its pixels are made: a new way
  of writing a texture's pixels uploads it again (`seglTextureChanged`, `seglVramUpload`), or the card draws the old
  picture, and `rvc_cpu`, which draws no list, never shows it: capture under `rvc_harness --cpu` with the switch at 1 and
  0. `gles.c`'s slots and uploads are the swapping core's alone: a frame drawn on a worker takes them where no frame is
  out or on the drawing core. A sum of a list counts a slot as what it was uploaded from (`gpuListSum`). The vertices find
  their commands in the draw-ID pass: a change to which vertices a command claims or to its pass changes
  `gpu_draw_ids_fragment` with `gpu_command_search`, and `--define GPU_DRAW_IDS_CHECK` must leave no magenta.
  Each vertex is made once a frame by GPUVertices (`gpu_front_fragment`, "The vertex front end"): a change to how a vertex is
  made is a change there, and `--define GPU_FRONT_CHECK` must leave no magenta. A copy (command 7, `glCopyTexSubImage2D`,
  framebuffer objects; "Copies") is made after the whole list: a draw after it in its list is a mistake `SEGL_DEBUG=1`
  reports. GPUDraw reads the VRAM alone ("Streams"): what a draw would read from RAM is copied into the band by the device
  (GPUStreamSums, GPUStream), and the fragment's RAM paths are compiled out (`GPU_VRAM_ONLY`). A new kind of what a draw
  reads, or a new command that reads a texture, is an item of `gpu_stream_item`, a conversion of the stream pass's and a read
  of the fragment's, and `RVC_GPU_READS=1` must still say `vertex no, fragment no`; compare a capture with `--define
  GPU_NO_STREAMS`.