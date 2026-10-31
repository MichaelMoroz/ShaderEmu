# The machine on the processor

`bin\rvc_cpu.exe` (`harness/apps/rvc_cpu.cpp`) is the machine as an ordinary interpreter. A
full pass of core 0's on the card is about 1.5 ms, about five million instructions a second
(`tools/card_model.json`, calibrated, 31 October 2026); what a program costs on it is a number of
instructions, and that number does not depend on what runs them. So work that is about instructions
(a game's logic, a frame's cost, a state sum, a test that a program still runs) does not need
the graphics card. On 31 October 2026, `RVC_THREADS=8`, by `rvc_cpu`'s last line: `factor 999999999989`
and `seq 1 40000 | gzip` from power-on, one core, 202 million instructions a second; Doom's timedemo, 342
million instructions on core 0 and its workers, 2.1 s; Red Alert's check, from the shell prompt through
the window system's start, the opening movie on worker cores and 300 frames of a mission, 345 million
instructions, 1.1 s.

The worker cores run on host threads (`--threads N`, or `RVC_THREADS`; the default is half the
computer's threads, at most 8), after core 0's part of each pass and only when the last pass's
workers ran 16,384 instructions or more. `--threads 1` runs them one after another: the same
instructions on every core and the same console from run to run. With threads, a program whose
workers wait on each other's words inside a pass counts other numbers of instructions from
run to run (Quake's and nxphys's statistics, Quake III's passes); its sums do not change.
`RVC_PROF` runs the workers in order on one thread; under `RVC_RACES` each worker keeps the texels
it changes apart and they run on the threads too, with the reports of the cores in order.

`RVC_BESIDE=1` runs core 0 beside the workers, as the card does, in a pass where any worker runs:
core 0 is one more job on the threads. Every core keeps the texels it changes in a log of its own
(RVC_RACES' logs) and reads the rest of RAM as the pass began, the mailboxes and counters too. The
logs go to RAM when the pass ends, lowest core first, so the highest core's 16 bytes win. Core 0's
fills and copies from the ROM are made after the logs, and its `fence.i` ends its pass, as every
core's does on the card: code it has just written (a signal's return on the stack) is fetched from
RAM, so it can run only in the next pass. Without it, core 0 runs first and stores
into RAM, and a worker sees in the same pass what core 0 stored and the job words it posted, which
the card shows only in the next pass. At the end it says how many texels workers stored in the
pass that core 0 parked them in: such a store lands in memory the program may no longer have.
A pass where no worker runs is core 0's alone, as without it.

    bin\rvc_cpu.exe --quiet --uart-log logs\uart_x.log --expect "/ # " ^
        --send "nano-X -p & sleep 2; TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7 RALERT_FRAMES=300 ralert; echo LX-''DONE\n" ^
        --until "LX-DONE"

It boots the image in `build\images\nommu` from power-on every time (`docs/system/nommu.md`;
`--image DIR` boots another), takes the harness's `--expect`, `--send`, `--until`,
`--uart-log`, `--ticks`, `--pointer-sweep`, `--desktop` and `--tabs`, and ends with
what each core ran, the firmware calls and the traps by cause. `--seconds` is the guest's
time, `--wall` this computer's, `--instructions` core 0's. `--help` lists the rest: the host's
words (`--host`, `docs/world/holodeck.md`), the network (`--net`, `docs/system/lan.md`), `--native`
(`experiments/isa_proto/ISA.md`). `\n`, `\r` and `\t` are `--send`'s only escapes here; the
harness's has `\xNN` as well. Start it from PowerShell or cmd: Git Bash turns `"/ # "` into a path.

`--pc-log FILE` writes the first core's pc at the end of every pass and the instructions the
pass ran, as the harness's does, for `tools\pc_profile.py`; `--pc-from TEXT` begins when the
console has said TEXT. For samples fine enough, `--ticks 256 --pass-ms 0.0078`: the pass's
milliseconds must go down with its instructions, or the guest's clock runs four times as
fast for each instruction and the kernel's timer is a quarter of the profile.

Other logs, each a measure that leaves the run as it was:

- `RVC_PASSLOG=FILE`: from `--count-from`'s mark, a line a pass, `pass end ran pc ra ms`:
  how core 0's pass ended (0 its budget, 1 it ended the pass itself, 2 it waited, 4 the write cache was
  full, 5 a copy), its instructions, pc, `ra` and the guest's milliseconds (a jump is a sleep).
  `RVC_PASSLOG_STACK=N` adds, after an end of 1 or 4, the first N words above `sp`
  that could be return addresses: who ended the pass, not only the leaf. Doom's timedemo on 28 October 2026: 16,331
  passes to their budget, 8,082 ended, 710 waits, 829 full.
- `RVC_WC_LOG`'s records (with `RVC_WCACHE=1`) end with the address of the store that found the cache
  full, 0 for a pass that did not end so: Doom's 8,351 full records all had one (28 October 2026).
- `RVC_WC_BITS=N` (4 to 8, with `RVC_WCACHE=1`): core 0's tables of 2^N buckets, not the tick's 6
  (`L1_TABLE_BITS`). `python tools/wcsize.py model` runs Doom's timedemo at 5 to 8, `card` on the card with
  the tick built at `L1_TABLE_BITS=N` under a world's frame. On 28 October 2026:

  | bits | texels (core 0) | core 0's passes ended full | workers' passes ended full | passes | realtics | fps (guest) |
  |---|---|---|---|---|---|---|
  | 5 | 168 | 21,128 | 8,102 | 34,961 | 1,739 | 101.2 |
  | 6 (the tick's) | 336 | 861 | 7,522 | 29,970 | 1,664 | 105.7 |
  | 7 | 672 | 106 | 7,531 | 29,748 | 1,664 | 105.7 |
  | 8 | 1,344 | 10 | 7,545 | 29,691 | 1,663 | 105.8 |

  On the processor's clock a full end at 6 costs the demo nothing: the pass's budget is not what limits it.
  On the card under a VR frame (`--unity 11.1,7.1,1.0`, some 2.2 rounds a frame), the same: 40.1 fps (guest) at 6,
  40.0 at 7 (3.27 and 3.23 card ms a frame).
- `RVC_DEVICE_EVERY=N`: the devices' pass takes the GPU's list back every Nth pass
  only, as a world does once a frame; every pass hides a game's waits for its list (Doom's timedemo 1,664
  realtics, 1,812 at 4, on 28 October 2026).
- `RVC_CON_TRACE=1`: each delivery of console input to the guest and each take, with
  the pass and the guest's seconds (`[con] delivered`, `[con] taken`).

`--count-from TEXT` prints every core's instructions from when the console said TEXT to the
end (`COUNT from ...`), and `--until-times N` stops at the Nth `--until` after it:
`tools\game_par.py` measures the games' frames with them (`docs/machine/multicore.md`, "Every core's
share of a game's frame").

## With the window: `rvc_harness --cpu`

The same interpreter inside the harness, for using the machine by hand at that speed:

    bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_opt --image linux-net --cpu --desktop --viz

Everything the harness has is there: its window with the display and the memory view, the
keys and the pointer, the console, `--expect`, `--send`, `--until`, `--save-state`,
`--gpu-capture`. The instructions run on the processor; the display, the GPU device, the
input and the control words stay the shader's passes, which is where they are cheap:

- The processor runs passes until the devices have something to do (a list submitted, a
  picture to copy back) or a sixtieth of a second is over.
- What it wrote of RAM since the last time goes into the state texture, in 4 MB bands (72; the
  shader's mask has 32 bits, bit b standing for bands b, b + 32 and b + 64, `ram_bands` in
  `src/types.h`, and the GPU's bands are drawn every commit, `_CommitAlways`): the
  GPU's memory and the control words every time they changed, Linux's own RAM four times a
  second (the memory view shows it, and a GPU draw may read a texture there: Half-Life's pool), or every
  frame with `--cpu-fresh`. A GPU list that reads the program's own memory a frame after it wrote it (commands
  or records there, as OpenTTD's: `docs/games/openttd.md`, "The picture on the GPU") draws what was there up to
  a quarter of a second before without it, and on the card never does. The commit pass is told those bands (state
  texel (41, 0), where the tick pass says which it wrote) and carries them into the buffer it
  draws, as it does the tick's.
- The frame is then the harness's own without the tick: the commit (which copies a drawn
  picture back into RAM), the GPU's draw, the control pass.
- The control words come back with the frame's row, as they always did, and are the
  processor's again; and when the commit copied a picture into a window's buffer, those rows
  of the texture are read back into the processor's RAM.

The guest's clock is this computer's, so a game runs at its own speed with a processor far
faster than the shader machine: `--cpu-ips 3000000` holds it to the world's speed, to see
what a thing feels like there. `python tools\snapshot_screen.py SNAPSHOT OUT.png`
shows the screen a `--save-state` snapshot holds. `--screen-log FILE` writes the display as it
is shown (its layers, the cursor over them) every frame: 16 bytes (width, height, frame, the
guest's milliseconds), then red, green, blue a pixel; `--screen-from TEXT` not before the
console has shown TEXT, `--screen-every MS` one not sooner than that after the last,
`--screen-frames N` stops after N. `tools/share_motion.py` plays such a recording
(`docs/world/share.md`). It reads the layers back from the state texture, where the GPU draws them,
and can miss a window a snapshot of the same run shows drawn: a capture of what programs look like
is taken with `--save-state` at the run's end and `snapshot_screen.py`, never from the log.

That snapshot is one the shader machine resumes from: `--save-state` writes
the processor's RAM and every core's registers, CSRs, timer and worker word where
`experiments/rvc_opt` keeps them, with empty write caches (`cpuSnapshotCores`), and the guest's
clock goes on from the processor's. So a moment reached here in seconds is measured on the card from the same state:

    bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_opt --define NO_PAGING --image linux-net --cpu --no-desktop
        --input "nano-X -p & sleep 10; quake +timedemo demo1\n" --until "You got the nails" --save-state Q.snap
    bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_opt --define NO_PAGING --image linux-net --no-desktop --load-state Q.snap

The interpreter knows only the geometry of workers all alike, which geotune then leaves; the
snapshot lays them out as geotune does on the card (16 big, the rest small in blocks of 2, or as
`--workers` says), or nxray draws 12.9 lists a second where it draws 19.2. A program's state that
goes by the guest's clock is not the card's at the same line (Quake III's frame 200 is 16 s in
here), and a part measured just after `nano-X` starts is not settled: give it 10 s. Checked on the
card against states reached there (card, 26 October 2026): Quake's timedemo 66.3 against 67.2 fps, nxray with its scene
held still 19.4 against 19.2. It goes one way only: under `--cpu`,
`--load-state` still restores only the texture and the processor boots afresh.

`rvc_harness --compute fast` is the same with the processor a compute shader on the card in
place of this interpreter (`experiments/rv32_min/README.md`, "The whole machine"): core 0 and 255
workers, a thread group each, RAM in buffers of their own (core 0's and the workers', merged
after every pass) kept in step with the state texture there.

Not in this mode: D3D12. Sound has not been tried in it.
The shader's own cores are not used (the cores are the interpreter's), so nothing here says
anything about the shader machine's passes.

## One card, many processors

Every harness takes one lock as it starts, after its options: any number of CPU harnesses run
together (`bin\rvc_cpu.exe`, `rvc_harness --cpu` with or without `--warp`, although without
`--warp` it makes a device on the card for its display), or one GPU harness alone (`rvc_harness`
and `rvc_harness_dxc` without `--cpu`, the bare double-click one too). A harness that would
break that does nothing, prints one line naming who holds the lock (`[lock] rvc_harness refused
(GPU mode): held by CPU harness rvc_cpu pid 32856`) and exits with 75. It is a `LockFileEx` on
`%LOCALAPPDATA%\ShaderX86\harness.lock`, shared or exclusive, and each holder keeps
`holders\PID.txt` open beside it: both go when the process ends, killed or crashed, so no lock
is ever stale. It is for every checkout on the computer. It does not wait: `tools\gpu_queue.ps1`
still gives the card's runs their turns one at a time, and says `REFUSED` under a run the lock
turned away. `RVC_LOCK_HOLD=S` takes the lock in the mode the options say, holds it S seconds
and exits, touching nothing else; a test of the rule in a lock of its own sets `LOCALAPPDATA`
to a folder of its own.

## What it is

- **The processor**: RV32IMA with the float instructions (the host's own floats, which are
  IEEE single precision too; `fptest` passes), supervisor and user mode, no page tables under
  the no-MMU kernel. No machine mode: as in the shader machine with `SBI_HLE`,
  every trap is the supervisor's and its `ecall` is answered in place (timer, console, the
  base extension). `RVC_ZB=1` and `RVC_ISA=zb,zbs,zicond,zfinx` add instructions, to try them.
- **Memory**: 288 MB from `0x80000000` (the GPU's memory and the control words to `0x82000000`,
  which are RAM too, then Linux's to `0x92000000`), the ROM at `0x40000000`, the parallel copy and fill (CSRs `0x0b0` to `0x0b3`, whose
  source may be the ROM), the clock chip and a UART that prints; the console ring of the
  kernel (`docs/machine/console.md`, "Words in memory") is read in the control pass.
  A copy from RAM to RAM is made after the cores' pass, 256 KB a pass for one core a pass, as
  the shader machine makes it with RAM in one texture (`docs/machine/gpu.md`): its core runs
  nothing until it is made (`RVC_COPY_STATS=1`: how many, and how many passes cores waited).
- **A pass** is `--ticks` instructions (8,192), or fewer when the core waits (`wfi`, `pause`) or
  asks for a copy, as on the shader machine, and the host's part of the machine runs between
  passes: the clock word, the host flags, the pointer, what each core ran, and the GPU's
  submit word.
- **The worker cores** (`docs/machine/multicore.md`) follow the same mailbox: a start texel, sleeping
  on the job word, stopping at a fault or a system call until core 0 answers. They run after core 0
  inside a pass and see each other's stores at once; on the shader machine a store is seen a
  pass later (`RVC_BESIDE`, `RVC_RACES`). A program that is right there is right here, not the
  other way round: `mctest` and the games' own checks belong to the card too (`tools/test_card.py`).
- **The guest's clock** is counted, not measured: a pass takes `--pass-ms` (0.25) and its
  instructions `1 / --ips` seconds each (3,700,000); an idle machine goes straight to its timer's
  next deadline. A game's instructions a frame are exact; its frames a second under this clock are
  not the card's. `--pass-model A,B,C,D` with the numbers of `tools/card_model.json` gives a pass the
  card's time: Doom's timedemo is 115.5 frames a second on the plain clock and 146.1 on that one
  (calibrated, 31 October 2026, with `RVC_SOUND`, `RVC_WCACHE`, `RVC_ADFAULT`;
  the `calibrated-measurements` skill says how far to trust it).

## What it is not

- **`rvc_cpu.exe` has no GPU, no display, no sound card.** A submitted list is taken as drawn
  (the submit word cleared, the counters counted), and a picture a program asked to have
  copied back into its window is not there. Those devices are cheap on the graphics card and
  stay there: `rvc_harness --cpu` (above) runs them as the shader's passes. Without a sound
  card `/dev/sound` does not open and a game's own sound code never runs: `RVC_SOUND=1` moves
  the card's clock and voices as its control pass does (nothing is mixed), so a game counts
  what it counts on the card.
  `tools\game_par.py` sets it, with `RVC_WCACHE`, `RVC_ADFAULT` and `RVC_RACES`
  (`docs/machine/multicore.md`, "A game's speed"; `--plain` drops all four, `RVC_ADFAULT=0` that one).
- No keys from the host (`nxkey` in the guest makes them). The pointer goes round (`--pointer-sweep N`, from pass N; `--pointer-circle
  X,Y,R` for a circle of its own on the display) and its buttons word is set for a while
  (`--pointer-button B,S,T`: B from second S to T of the guest's). No pages from the host
  (`docs/system/fetch.md`) but the kept folder's (`--persist FILE`, `docs/system/persist.md`).
- As it stands, not a model of the shader machine's limits: no write cache that fills, no one-writer-per-16
  -bytes rule for workers. It answers "how many instructions", not "does it run there";
  `RVC_RACES=FILE` gives the cores memory as the card has it, and says where that mattered
  (`docs/machine/multicore.md`, "Races"); `RVC_CALLS=FILE` what core 0's calls cost ("Finding the
  work"). `RVC_WCACHE=1` gives every core the shader machine's write cache (three tables of
  buckets of three texels, 6 bits for core 0 and the geometry's for a worker,
  `experiments/rvc_opt/src/mem.h`): a store that changes a texel no bucket has room for ends that
  core's pass before it, and the end says how many passes ended so. `RVC_ADFAULT=1` makes a
  clear accessed or written bit in a page table entry fault, as a walk on the shader machine does
  (`src/mmu.h`); under the no-MMU kernel there is no walk and it changes nothing. Every run
  ends with a `[cpu] stops:` line: the workers' faults, those on such a bit, and core 0's on
  one, from the mark too; `RVC_ADFAULT=2` prints each (`[adfault]`: core, cause, address, pc).
- `--save-ram FILE` writes the 288 MB of RAM as the run ends: a picture in GPU memory at a
  hold can be read from it (the NES's at `0x81000400` plus what `nes: holding` prints).
- `--save FILE` writes the whole machine as the run ends (unless at `--until`): RAM, every core and worker, the
  devices' counts and the console's and the script's place, 315 MB; `--load FILE` goes on from one with the same
  image, cores and arguments. `--max-passes N` and `--save-after S` (seconds of this computer's) stop the run there,
  and with `--save` it exits 5, to be gone on with. A run stopped and gone on with ends
  at the same pass with every core's instructions and the console the same bytes as one run. Not kept:
  `RVC_RACES`', `RVC_PROF`'s and `RVC_CALLS`' tables (each call reports its own part), the write caches' lines (empty at
  a pass's start), the network. `tools/test_all.py --sections` runs every case in calls of `--section-secs` (40 s)
  through it: a case not yet ended prints `part`, and the next call of the same line goes on with it.
- A world's rounds in time: `--ticks 8192 --ips 1e12 --pass-ms 2.7307` makes every pass a
  round of 8,192 instructions at 3 MIPS a core, whatever it ran, so a program's own clock (the
  NES's `nesstat` percentages) counts as on cores of that speed (`docs/games/nes.md`).

## Checking it

`fptest` in the guest (every float instruction against the runtime's integer routines), the
games' state sums (`docs/games/ralert.md`, `docs/games/tdawn.md`), and `tools/test_card.py` below, which
holds the same cases to the card.

`RVC_FAST_CHECK=1` checks the fast handlers against the general step: every core's pass is run
both ways from the same state (RAM read-only meanwhile, a page copied at its first store, so the
first run is undone), and the core, its worker's state, what it said and RAM must be the same to
the byte; the first pass that differs stops the run (exit 4) with what differs and where. On one
thread, about four times slower; with `RVC_WCACHE` too, not with `RVC_RACES`, `RVC_PROF`, `RVC_CALLS`.

## The test set

`python tools/test_all.py` runs every program's own checks on the image under `rvc_cpu`, half
the computer's threads at once: the machine (`mctest`, `fptest`, `lazy`, `cc`'s bytes,
libmedia in the image), the games' state and picture sums (Doom, Quake, Quake III, ClassiCube,
Tiberian Dawn, Red Alert, the NES, OpenTTD, nxphys), nxray and nxpath, libmedia and the players, and the
desktop's programs (Paint's undo, the viewer's sums, Files' box, a question, the browser's
counts, the monitor, the launch note). One PASS or FAIL line a case with why and its seconds,
a summary, and exit 1 on any failure; the consoles are in `build/test_all`. On 31 October 2026 there
are 152 cases by default (`--list`), about two minutes, and 161 with `--long`. The longest cases start
first, by the seconds each took last time (`build/test_all/times.json`), and each case's `rvc_cpu` gets
the threads left over (`RVC_THREADS`); `--cpu FILE` runs another build of `rvc_cpu`.

    python tools/test_all.py                 # the default set
    python tools/test_all.py games nes-cells # tags (machine, games, doom, quake, ..., workers, media, programs, desktop) or names
    python tools/test_all.py --long          # and the long cases: no workers, zerotest and tailtest, three bots
    python tools/test_all.py --races         # RVC_RACES on every case: 0 changes undone (RVC_RACES_NEAR=4 said)
    python tools/test_all.py --sections      # each case in calls of 40 s (--section-secs): run it again till none is left
    python tools/test_all.py --max-wall 55 --wall-scale 2   # a case's wall at most 55 s, every wall doubled
    python tools/test_all.py --card-model    # RVC_SOUND, RVC_WCACHE, RVC_ADFAULT as game_par.py has them
    python tools/test_all.py --image build/images/NAME --list

A case's expected values are in the script, each with the doc it comes from; where no doc gives
a value (Doom's, Quake's and Quake III's server sums, Quake's particles, ClassiCube's
meshes and pack, `cc`'s outputs) a case keeps the md5 of every such line. A change that
moves one on purpose sets the new value with the reason. Captures are not in it.

`python tools/test_card.py CASE_OR_TAG ...` runs the same cases on the card: `bin/rvc_harness.exe --d3d11`
with `experiments/rvc_opt`, one at a time through `tools\gpu_queue.ps1`, each console checked by test_all's
rules. The card boots in some 50 s, so a case resumes a snapshot of the shell made once on the processor
(`build/test_card/shell.snap`; `--snapshot` makes it again) and its wall is at most 55 s (`--max-wall`). A case
is skipped for another image, for host words that change as it runs, for `rvc_cpu`'s own devices (its `env`),
or when it is longer than a run may be. The card's own values are in its `CARD` table, each the same in two
runs. On the card on 28 October 2026 the `machine` tag with `doom-3`, `nxpath-balls` and `nxpaint` passed 10 of 10 in
some 5 minutes (`mctest-255` and `cc-bytes` over 55 s; `hlsl` sums `a4b289dd` and `nxpath-balls` `5cd7620a`
on the card, the tick's floats). `--parts` runs a case in runs of `--part-secs` (45 s), the machine saved at
the end of each (`--save-state`) and resumed by the next, until its `--until` or its wall: the long cases
(`mctest-255`, `cc-bytes`) then run too.

The card runs a processor folder only when its `warp_ok` holds the folder's stamp, its sources hashed
(`rvc_harness --stamp`; the devices' folder too when another); otherwise
`rvc_harness` says so and exits 3. `--warp` runs the cases on WARP, D3D11's software device, and with
`--mark-ok` writes the stamp when every case passed: `python tools/test_card.py mctest-3 --warp --parts
--max-wall 240 --mark-ok`. WARP is some 300 thousand instructions a second: `mctest 3` took 313 s in parts on
28 October 2026 (`steptest` did not end in 240). The stamp is of the sources only: a define (`--define`,
`--lazy`) is the same tick checked. `--cpu` and `--compute` are not the card's tick and need none.
