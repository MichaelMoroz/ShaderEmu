- The rules of the work (23 October 2026). We own the whole stack, from the games' and programs' sources down to the
  tick's DXBC instructions: the games and userland, the LLVM backend, gpu_vm's ISA, the tick and the other shaders, and
  fxc2 (`C:\Users\micha\Documents\FXC2`, `tools/fxc2`), so a cost is fixed at whichever layer it is cheapest.
  - Any sacrifice that makes userland programs faster is worth it.
  - The shaders can be rewritten completely if needed.
  - Everything must work under VRChat's constraints (no UAVs in the fragment shader, no texture arrays, D3D11 DXBC).
  - gpu_vm's ISA can be changed freely, and so can the LLVM backend.
  - fxc2 can be changed freely for the tick, as long as it still works on all its uses (the harness, Unity, the
    ShaderEmu and Poiyomi shaders its README measures).
  - Optimizations are weighed together, not one at a time: one that loses alone may win with another (a layout and the
    compiler's address, a cache and the ISA's stores), and one that wins alone may not add up with the rest.
- One core a wave (25 October 2026, `gpu_vm/tick/WAVES.md`): every wave of the tick's draw holds the lanes of one core
  only. Cores that share a wave run two programs, and the wave pays for both paths. A tick is valid only with a geometry
  (block, draw, harness, Unity) measured to keep this on the card. Padding lanes are kept out of the geometry, not
  discarded. Measured with `bin\waves.exe`: on NVIDIA a warp is a 4 x 8 pixel tile under one big triangle, so blocks of
  whole tiles (4 x 8, 4 x 16, 8 x 8) drawn by one triangle never mix; a primitive edge inside the cores' area does.
  The new tick's cores are 8 x 8 blocks in one row (2,048 x 8 for 256), one triangle. Its commit is one draw of whole
  texels: two cores must not store to one texel in a pass. `python3 gpu_vm/userland/apps/test_all.py --races` must say 0
  undone and 0 texels for every case (140 on 25 October 2026; `gpu_vm/tick/CACHE.md`). A long program is checked by
  sections under 5 minutes each: full speed to `GVM_RACES_AT`'s console text, the check to `GVM_RACES_UNTIL`'s.
- gpu_vm's fastest machine and how to measure it (its tick, the image and checkpoints for each game, the commands and
  the numbers): `gpu_vm/bench_games/README.md`, "The fastest machine today". run.py's defaults (img-cur, gvm-cur) are
  the image to use; the harness's own default tick (x18) is not the tick: give `--gvm-tick` (v0009).
- Linux with the MMU is no longer supported or developed for (21 October 2026): build, test, measure and document the
  no-MMU machine only (`linux/nommu`, `build/images/nommu`). Do not fix, rebuild, test or keep in step anything that only
  the MMU image uses, and do not add MMU paths to new work.
- `tools\make_image.ps1` passes its arguments on to `make_linux_image.py` (at first it did not: `$args` inside a `Measure-Command` block is the block's own): `pwsh tools\make_image.ps1 --save-prebuilt` saves the prebuilt folder under the lock. It makes the
  no-MMU image after the shared one (`linux/nommu/image.py`, from `build/nommu` or
  `linux/prebuilt/nommu`), and with `--save-prebuilt` keeps that build too: Linux without the MMU
  is what the harness, `rvc_cpu` and `tools/test_all.py` run by default; with the MMU is legacy.
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
- `rvc_harness --lazy` (`LAZY_MEMOP`): a fill or a copy, from the ROM too, is left to the
  commit and the pass goes on; its destination's pages are in no TLB until then. Off by
  default: it costs 1% of a shell loop and 3% of Doom though the fast loop is instruction for
  instruction the same (`tools\cso_loops.py`; `experiments/rvc_opt/README.md`, "The same loop, compiled slower"). With
  it undefined the tick is the same blob. Its test: `programs\lazy`, 20 lines ok, sum
  `1a4a1226`, on `rvc_cpu` and on the shader machine off and on.
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
  (`fast_exec_l1` in `emu.h`, `mem_get_cached_or_tex_l1` in `mem.h`; `docs/machine/tick-costs.md`, the
  sections of 13 October 2026). The text's form is the gain: the same tests as bools computed
  first were 4% slower, and operations counted do not say which way a shape goes. After any
  change to the fast loop's control flow run the three loads in turn from one snapshot (a
  shell loop, Doom's timedemo, Quake e1m1), and bring the function to Unity's copy verbatim.
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
  since 13 October 2026 (`docs/apps/raytrace.md`): its sum is `02f213e9` (160 x 120, 2 samples, with
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
  (28 October 2026; `docs/system/nommu.md`, "Memory"): the layout is `experiments/rvc_opt/src/layout.h` for the tick and
  `harness/apps/rvc_layout.h` for the host. The state texture is 2048 x 9280 (`STATE_TEX_HEIGHT`, `kStateHeight`, Unity's
  StateA). The commit's band mask stays 32 bits, bit b for bands b, b + 32 and b + 64 (`ram_bands`); the control words'
  bands (4 and 5) are drawn apart (`_CommitAlways`, `CONTROL_BANDS`). A GPU copy into RAM goes only below 0x81e00000. A
  snapshot of the layout before (devices at 0x86000000) is refused: make it again.
- RAM is one texture written in place (`src/ram.h`, `docs/machine/gpu.md`, "RAM in one texture"; Unity has StateA alone):
  no commit pass reads what it draws, what it needs of RAM is gathered into a stage first, and a copy from RAM to RAM
  is 256 KB a pass, in `rvc_cpu` too (`copy_pass`). `programs\copytest` must say `sum 0e7f76c3` on both; `--ping-pong`
  is the two textures of before (and `--lazy`, D3D12).
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
  tick mesh's 17th triangle and `_FieldPoints`, wired but not yet run in the world). geotune's small workers (2^3) have 6 lanes
  each: lanes gain from large caches, the field from neither.
- Work longer than a pass goes on the lanes in steps (`docs/machine/lanes.md`, "Steps"; `mcw_field_steps`,
  `mcw_steps_left` is CSR `0x0ba`): a step keeps what it needs in its output, no registers are saved. A
  step stops only where what the pass has left covers the longest stretch to its next stop: run the
  program's self-test on a worker of `rvc_cpu` (`mcw_steps_probe`; `NXPATH_PROBE=3000` on every scene
  of `nxpath`), which must say 0 over a pass and 0 paths not as traced whole. `steptest` must PASS. A
  change there must leave the plain path's instructions as they were (`NXPATH_FIELD=0` on `rvc_cpu`):
  the lane's copy of a search is its own branch (`limited`), so the workers pay nothing for it.
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
  one link's layout (27 October 2026). gpu_vm's check: `gvm_cpu --card-budget` with `GVM_RACES` and `GVM_RACES_NEAR=4`
  (GPU-VM `docs/REGRESSIONS.md`). A change to the game is also walked: `HL_SCRIPT` with `+forward`, turns and shots for
  1,200 s of the guest's on the desktop with `RVC_RACES` (standing in the fight never played a sequence group).
  A level that goes settles the server, the drawing and every job first (`SE_LevelSettle`), and off core 0 a group
  is loaded when its pointer is set (`Mod_CacheCheck` walked pools core 0 moves): a change to either is run through
  the elevator and `changelevel`s, with the default and `HL_RENDER=core` (docs/games/halflife.md, "A level that goes").
