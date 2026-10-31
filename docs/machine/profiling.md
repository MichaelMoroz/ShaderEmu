# Profiling the programs the machine runs

`tools/prof.py` says where a program's instructions go on `rvc_cpu`, which of its loops and
calls could be a worker's, which known slow forms it has, and which layout of its switches is
fastest. Its data comes from `rvc_cpu` itself (`RVC_PROF`, `harness/apps/rvc_prof.h`), not from
the program: nothing is built again for it, and the kernel is in it too.

    python tools/prof.py run OUT [--exe EXE] -- RVC_CPU_ARGS...       a run with RVC_PROF=OUT
    python tools/prof.py report OUT --elf ~/nm/work/src/doom-build/doom [--top N] [--lines]
    python tools/prof.py auto OUT --elf ... -- RVC_CPU_ARGS...        run, choose, run again, say which are independent
    python tools/prof.py deps OUT --elf ...                           what the second run found
    python tools/prof.py search --pre "nano-X -p & sleep 2" --cmd "PHYS_SCENE=1 PHYS_STEPS=120 nxphys" --param PHYS_WORKERS=31,63,127,191,255 --check "sum [0-9a-f]+"
    python tools/prof.py patterns                                     the library of slow forms

A profile is of what `--count-from` counts: give the command as `echo FL-''GO; CMD; echo LX-''DONE`
with `--count-from FL-GO --until LX-DONE` (the quotes keep the echoed line from matching).
For example, Doom's timedemo:

    python tools/prof.py run C:\...\b_doom -- --image build/images/nommu --quiet --expect "/ # " --send "echo FL-''GO; nano-X -p & sleep 2; doom -1 -timedemo demo1; echo LX-''DONE\n" --count-from FL-GO --until LX-DONE --seconds 120

`RVC_PROF` runs the workers in order on one thread (whatever `--threads` says): Doom's
timedemo takes 19 s of the host's time profiled, the first 150 s of Quake III 97 s. Before
`--count-from`'s line the run is a plain one (since 28 October 2026: the fast handlers, the
workers on their threads), so a window late in a long program costs only its own instructions
profiled: Quake III's 600 frames from its level's first frame take 11 s of the host's time,
OpenTTD's play from `play PL-GO` reaches its game in seconds.

## What `rvc_cpu` writes

`RVC_PROF=DIR` (a Windows path) writes into DIR when the run ends:

- `pcs.txt`: every pc run, core 0 and the workers apart, with its word, how often it ran, the
  loads and stores it made to RAM, its stores that took a texel new to the core's pass with
  bytes that changed (`newtex`: what the write cache keeps), and, under `RVC_WCACHE=1`, its
  stores that found the write cache full and ended the pass (`full`). `prof.py report` gives
  both by function ("texels new to a pass", "where core 0 found the write cache full").
- `edges.txt`: calls, caller to callee, how many and their instructions inclusive. A stack is
  kept for each thread: by the hart, the privilege and `tp` (the C library's thread pointer in
  a program, the task in the kernel), so the programs of a machine without an MMU, which share
  one address space, are not one stack. `RVC_CALLS` keeps one stack and mixes them.
- `mem.bin`: for every page of RAM touched, the loads and stores of each texel by core 0 and by
  the workers; `wcache.txt`: how many distinct texels a core stored a pass.
- `timeline.bin`: a record a pass (core 0's instructions and where it ended the pass, whether it
  waited or rested, how many workers ran and how much); `RVC_PROF_CORES=1` adds every core's
  instructions a pass (`cores.bin`).
- `RVC_PROF_DEPS=f:ADDR,l:LO-HI,...` watches calls of a function or a loop's runs on core 0
  and writes `deps.txt` (below). `RVC_PROF_MEM=0` leaves the memory out.
- `RVC_PROF_PASS=N` starts the profile N passes after `--count-from`'s mark (or the start), so
  a level's load or a demo's first seconds stay out: Doom's timedemo
  from 20,000 passes on had 52,583 pcs against 133,419 from the mark.

A profiled run takes about twice as long as a plain one.

## The report

The program's pcs are physical (the kernel puts a flat program where it has room), so
`report` places each ELF given by its words: anchors are hot pcs whose word is rare in the ELF,
and a base counts when at least 32 and three quarters of the hot pcs it covers have the ELF's
words there. The ELF must be the build the image has (an ELF rebuilt after the image matched
72% and was refused). The kernel (`~/nm/k-rom/vmlinux`) is added by itself; it is not quite
the build that runs (84 to 94% of its words), so its function names near a change are a guess.
`--find NAME` looks for an unstripped NAME under `~/nm/work/src`.

- Where core 0 ran and where the workers ran, by function, with loads and stores an instruction.
- Calls on core 0 and on the workers: inclusive instructions, the heaviest edges.
- Memory: the busiest pages, stores by function, and how often a core stored more distinct
  texels a pass than its write cache keeps (`AGENTS.md`: some 384 for core 0, 192 a worker).
- Passes: what ended them early (a wait in the kernel, `workers_client` being core 0 waiting
  for its workers, the idle loop), and over twenty stretches of the run how busy core 0 was,
  how many workers ran and how full their passes were.
- Loops of core 0, found by their branches back: trips, instructions a trip, loads, stores,
  calls. `--lines` adds the hottest source lines (`addr2line` in WSL; a program built without
  `-g` gives `?`).
- The known slow forms found, and the loops and calls worth asking about (`watch.txt`).

## Independent work

`auto` takes up to ten loops and calls that are at least 1% of core 0 (loops branched back to
at least eight times, the widest of those with one head; calls of 2,000 instructions or more a
call, made at least 16 times, not the kernel's) and runs the program again with
`RVC_PROF_DEPS` on them. A run of a loop is from its head to when the pc has been outside it
for 256 instructions (a compiler lays parts of a body past its branch back); an instance is
one trip. A run of calls is the calls made from one caller's frame. Every store and load of
core 0 inside an instance is kept by texel (up to 4 million), and an access to bytes another
instance of the same run wrote or read is counted by kind and by the two pcs. The program's
own stack below where the run began is left out (each call's locals are its own).

| verdict | |
|---|---|
| INDEPENDENT | no instance reads or writes what another one did: `mcw_for` over them |
| INDEPENDENT but neighbours write one 16 bytes | only stores beside each other in one texel: give a worker whole texels (`mcw_for_opts.out_bytes`) |
| A SUM carried in memory | the same place read and written again by each instance: a share each, joined at the end |
| DEPENDENT | with the pairs of pcs, most first |
| about one instance a run | nothing to share out at once, however independent |

Each verdict comes with what 8 and 64 workers would save of core 0 at most (Amdahl, less a
pass or two a run). A run that is short (a few thousand instructions) is not worth a pass
whatever it says.

## Patterns

`tools/prof_patterns.ini` is the library of slow forms with their better ones, a section each,
matched against the loops (by a regular expression over the body's mnemonics and an
expression over its features: `n`, `loads`, `stores`, `calls`, `divs`, `floats`, `branches`,
`lbu`, `sb`, `lw`, `sw`, `step1`, `andi255`, `system`, `atomics`, `adds`, `per`, `trips`) and the
functions (by name: `calls`, `per`, `share`, `inclusive`). There are byte copies and fills, a
byte-wise CRC, a wait in place, a division in a loop, soft floats, large clears and copies,
many small library calls, stdio a character at a time, allocation in a loop, `printf` in a hot
path, busybox's and zlib's inflate, and loops that store every trip. Add one whenever a
profile shows a slow thing that has a known better form.

## Soft doubles

`python tools/test_all.py --doubles` runs `game_par.py`'s workloads profiled, one at a time on one thread, and each
game over its limit (`DOUBLES` in `test_all.py`, each a little over what it measured) is a FAIL with its callers. It
stops before a minute: run it again to go on (`doubles.json` in its log folder). On 31 October 2026: Half-Life's fight
5.4% of core 0 (`CL_ComputePlayerOrigin`, `CL_RunUsercmd`), ClassiCube 1.9%, the rest 0.0%. Not in it: OpenTTD
and the two Command & Conquer games, whose unstripped programs and workloads it does not name yet.

The machine has float instructions and no double ones, so a double is a call: some 80 to 100 instructions a sum
or a product, against one for a float. `python tools/prof.py doubles OUT --find NAME [--limit PERCENT]` says what
share of each side's instructions went to those routines (`SOFT_DOUBLE` in `prof.py`) and who called them, and
exits 1 over the limit (5% by default), and 2 when no ELF was placed, since nothing could be counted then. On 31
October 2026: QuickJS's loop of doubles 39.3% (its numbers are doubles), Half-Life's `hldemo1` load and first
seconds 4.6% (`Mod_PointVecsProduct`, `atof`), OpenTTD's play with its frame rate window open 1.2%, Quake's
timedemo and Doom's 0%. The frame rate window was the slow form it is there to find: every 100 ms 512 sums of
doubles for each element, a million instructions, which held the game's tick (`GetAverageDurationMilliseconds`,
now summed in whole numbers).

## Searching a layout

`search` runs a command at every combination of its switches (`NAME=a,b,c` or `NAME=lo..hi:step`),
several at once (half the host's processors), and ranks them by the machine's seconds, the
passes, core 0's instructions, the busiest worker's or all of them. `--check REGEX` must match
what the console said after the command, or the layout is wrong whatever its time. It says
what the search will take first and refuses more than five minutes without `--yes`. On
nxphys's first scene (16 s): `PHYS_WORKERS` 191 2.925 s of the machine's time, 255 the same,
127 1,147 passes against 1,130, 63 1,294, 31 1,526; every sum right.

## What could be parallel: the main programs (18 October 2026)

Profiles on `rvc_cpu` of the shared image's programs of that morning, the commands of
`tools/test_all.py` (without the test sums, which were 66% of nxview's first core and 85% of
nxvideo's). Ranked by what is to be had.

1. **Archives** (done: `unarc`, `docs/system/media.md`, "One deflate stream on many cores").
   busybox's `tar xzf` of the busybox source, 2.1 MB: 831.7 million instructions on core 0
   and 252 s of the machine's time (`fill_bitbuffer` 17%, `inflate_codes` 13%, CRC 12%, a group
   lookup a file 17%); `unarc` 31 s, the zip 207 s to 20 s.
2. **A program's text read from address 0** (done: every one reads from `_start` now). A flat
   program's `__executable_start` is 0 at run time (a GOT word of 0, which the loader leaves
   alone) while `etext` is relocated, so `mcw_touch_code(__executable_start, etext -
   __executable_start)` reads every page from physical 0 up to the program: 529 thousand
   loads, 1.6 million instructions, half a second of the machine's time, at the first worker
   start of every program. `nxview --decode` of Earthrise.jpg went from 1.75 s to 1.23 s with
   libmedia's (`m_core.c`, from `_start`'s page) and 0 page stops of workers as before. The same
   line was in `nxweb.c`, `nxphys.c`, `nxray.c`, Quake III's `tr_shaderemu.c` and Half-Life's
   `gl_shaderemu.c`, which read from `_start` too since 18 October 2026; programs that link
   libmedia must be linked again to have its.
3. **The kernel's copy a byte at a time.** When source and destination lie differently in
   their words, the kernel's `memcpy` copies byte by byte (5 instructions a byte, at 0x806b25bc):
   10% of Quake's core 0 while e1m1 loads and its first 150 frames (1.9 million bytes, 9.7
   million instructions), 5% of Red Alert's start (11.2 million). Words shifted together would
   be some 1.5 instructions a byte. Red Alert's `MapClass::Read_Binary` makes 32,768 reads of
   a cell each through it, and `OverlayClass::Read_INI` 16,384.
4. **Quake's level load.** `AllocBlock` (light maps' places) is 8.6 million of e1m1's start (9%,
   about 3 s of the machine's time): a search for the lowest row over every column for every
   surface, carrying its best in memory (`prof.py deps`: a sum), 1,660 instructions a call, too
   short for workers; a skyline of each sheet's heights would make it a few hundred. Then
   `BuildSurfaceDisplayList` 4.5 million, `CRC_ProcessBlock` 3.9 million (a byte at a time),
   `GL_MakeAliasModelDisplayLists` 2.6 million, all on core 0, and `Mod_FloodFillSkin` 8.8 on
   the workers already.
5. **Red Alert's start and its movies.** On core 0: `VQA_DrawFrame_Buffer` 16.4 million (7%),
   the frames copied to the screen while the workers decode them (`frames_job` 64 and
   `m_vq_rows` 60 million there); `Build_Fading_Table` 5.6 million (each entry of a table its
   own: a `mcw_for`); `PKey::Decrypt` 5.5; `INIClass::Load` 5.2.
6. **Quake III is waiting for its workers.** Core 0 ended 28,303 of 58,379 passes waiting in
   `workers_client`; the busiest worker ran 354 million against core 0's 320 (the level's
   pictures decoded, `idct`, `mcu_pixels` and `restart_strips` 211 million together, the server's
   `MSG_WriteDeltaEntity` 47, `entity_draw` 85). More cores on the pictures and fewer jobs on one
   core are what is left there. On core 0: `CL_ParsePacketEntities` 34 million (11%), and soft
   doubles (`__adddf3`, `__muldf3` 13 million) under the bots' script parser (`PC_ReadToken`).
7. **Doom's timedemo**: core 0 154.5 million, `P_CheckPosition` 19.5 and `P_RunThinkers` 16.3
   (the game's own, one thinker after another, each moving what others read); the view on at
   most 5 workers as designed (`docs/games/doom.md`). Little left.
8. **The compiler** (`cc example-cube.c`): 25.8 million, one core, 7.8 s; `strcmp` 2.2 million
   (104 thousand calls, the archive's symbols looked up one by one in `tcc_load_alacarte`):
   a hash of them would take most of that.
9. **nxvideo, nxplay, nxview, nxweb, nxfiles**: the work is on the workers already. nxvideo's
   core 0 waits for its 64 workers in 2,446 of 3,661 passes while it plays in time; nxplay's
   core 0 is 5.5 million, a third of it polling its workers; the desktop programs' starts are
   the idle loop waiting for their `sleep`, and the text read of point 2.
