# A NES

An emulator inside the emulated machine: Nofrendo (Matthew Conte's, as Espressif keep it for
the ESP32) as a Nano-X client, with two games anyone may pass on. Held to a NES's 60 frames a second it
has room to spare: unheld, 373% of a NES on an RTX 5070 laptop (19 October 2026, `docs/machine/speed.md`), 378%
calibrated (31 October 2026, `game_par.py nes --calibrated`, 226.8 frames a second); on one core at a third.

    wsl -- bash linux/nommu/build.sh nanox/build.sh     (once)
    wsl -- bash linux/nommu/build.sh nes/build.sh       (2 s; the first time it also fetches)
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1                                  (then a new shell snapshot)

In the guest: `nano-X -p & nes thwaite` or `nes nova` (the Start menu's Games has both), or
`nes FILE.nes` for any other cartridge of a mapper Nofrendo has. Arrows are the pad, X is A,
Z is B, Enter is Start, Tab is Select and Escape ends. Its sound is the sound card's (below).
The window's caption is the emulation's speed, a NES's 60 frames a second as 100% (`NES  97%`),
made again about once a second by the machine's clock ("A NES's speed in the world", below).

The games are Thwaite by Damian Yerrick (mapper 0) and Nova the Squirrel by NovaSquirrel
(mapper 1), both GPL-3; the build fetches their release files.

## What the build is made of (`linux/nes`)

- `build.sh` fetches esp32-nesemu at a fixed commit, applies `nofrendo.patch` and compiles
  Nofrendo with the files below, `programs/linux/gles.c` and `programs/mc/mcw.c` and
  `mcwx.c`. `NES_OUT=DIR` puts the program in DIR (a private image's overlay,
  `tools/private_image.py`) where it would go in the shared root.
- `osd_shaderemu.c` is the machine's side: the window, the pad, the frame and its pace. The
  NES's picture is 256 x 240 bytes of GPU memory, each a place in the display's palette, which
  holds the NES's colours; the GPU stretches it over the window.
- `rc6502.h` is the 6502 recompiled (below); nofrendo's `nes6502.c` includes it.
- `nofrendo.patch` is ours (edit a clean clone at the commit `build.sh` names and save
  `git diff`); the build puts the tree back each time.
- `apu_shaderemu.c` is the sound (below).
- `exact_rom.py` makes a cartridge of our own that a change to the 6502 is checked with
  ("Lines ahead", below).

## Speed

The tables here follow the work as it went; where it stands is the head of this page.

Instructions of the first core for a frame of the NES (`nesstat:` lines), and the speed that
is of a NES's 60 frames a second at about 4M instructions a second:

| | Thwaite's title | Thwaite, a game | Nova's title |
|---|---|---|---|
| Nofrendo as it came, one core, a frame in three drawn | 704k (9%) | | |
| idle loops counted, the background by table | 292k (22%) | 380k (17%) | 274k (24%) |
| the picture by three worker cores | 162k (39%) | 185k to 215k (26 to 31%) | 153k (42%) |
| the 6502 recompiled, asleep in its idle loops | 49k (116%) | 70k (80%) | 31k (172%) |
| the GPU draws: every frame that differs is shown | 51k (134%) | 87k (80%) | 34k (212%) |
| straight runs unchecked, lines crossed inside translated code, the lists a worker's | 47k (142%) | 63k (106%) | 37k (180%) |
| the line's step with no registers saved, the build optimised as one program | 46k (145%) | 59k (113%) | 36k (185%) |

With the limiter on, the two titles hold 60 frames in 1,000 ms and a Thwaite game 60 in
1,035 to 1,070 ms with 30 of them drawn. The GPU needs no worker cores, so this is what
one core does too. Nofrendo's own drawing on one core (`NES_PPU=here`, a frame in three)
is 205k to 215k, a third of a NES. A game is held to 60 frames a second (`frame_paced`; a test with
`NES_FRAMES`, or `NES_FAST=1`, runs as fast as it goes).

What each step is:

- **A game waits in a loop**, most of every frame: for its interrupt (`cmp zp; beq`, or a
  `jmp` to itself, which is how Super Mario Bros. waits), or for
  the PPU's status to say the picture has reached sprite 0 (`bit $2002; bmi; bvc`). A taken
  branch a few bytes back, or a `jmp` up to 16 bytes back, has one turn of its loop tried on a copy of the registers
  (`idle_loop`, `idle_step` in `nes6502.c`): when the loop only looks (loads, compares,
  tests, branches) and comes round to the registers and flags it began with, the turns left
  in the slice are counted, not run. The status counts as the same only until the cycle
  sprite 0 is hit (`ppu_status_steady`).
- **A slice that ends inside such a loop leaves the 6502 asleep** (`nes6502_idle`): the
  slices after it are not entered at all, only counted from the cycles of the loop's
  instructions, until an interrupt, cycles to burn or a change of the status wakes it; the
  registers are then brought to where the count has got to by doing the instructions
  between to them once more. Scanlines that pass so, with nothing for the PPU to find
  (sprite 0 not in the line, no frame interrupt due, no mapper counting lines), are one loop
  (`ppu_quiet_lines`): about 60 instructions a line where a line was 700.
- **A background tile's row is two words looked up**, not eight bytes each through the
  palette (`bg_words` in `nes_ppu.c`). A line that starts inside a tile is drawn beside the
  picture and copied in: as it came, such a line wrote up to seven pixels before its start.
- **The picture is drawn by the worker cores** (`docs/machine/multicore.md`; `frame_elsewhere` in
  `osd_shaderemu.c`, `ppu_capture` and `ppu_draw` in `nes_ppu.c`). The first core draws
  nothing: it emulates every frame and, for the frames the workers are free for, the PPU
  keeps a record: its memory as the frame began (the name tables and pattern RAM copied only
  when written since) and each line's registers. The workers draw the lines from the
  record, each a share of them, into a texture not shown, and the window turns to it when
  they are done. Sprite 0's hit stays the first core's to find (as nofrendo does for a frame
  it skips). What is lost: a write to the name tables in mid-frame shows a frame late.
  A cartridge whose pattern pages turn as they are drawn (MMC2) is drawn by the first core.
- **As many workers as fit, in teams.** The NES asks the machine for twelve workers of size 5
  (`docs/machine/multicore.md`: they are the machine's, 1.5 KB of new stores a pass, some six lines
  of the picture; `NES_WORKERS` for another number), and takes the workers
  there are where it is refused (three on a machine of four cores). A frame given out and
  heard done costs two passes whoever draws, so one frame at a time is at most a picture
  every five passes: the workers are in teams, a frame a team, as many teams as the game's
  pace allows (`frame_paced`: one more while it is ahead of its sixtieth of a second, one
  fewer when it falls behind; `NES_TEAMS` in a test). It has to be so because busy workers
  make every pass of the machine longer: of 60 frames of Thwaite's title, twelve workers as
  one team showed 22 with the game at 99% of its speed, three teams 43 at 57% and six 55 at
  44% (the graphics card 39% busy with other work meanwhile). The pictures shown a second
  stay near 22 to 25 however they are shared out: what bounds them is the machine's
  instructions a second on all its cores together, not the number of cores.
- **The GPU draws the picture** (`gd_...` in `osd_shaderemu.c`; `docs/machine/gpu.md`, "A layer of
  tiles"), unless `NES_PPU` says workers, inline or here. The background is a layer of
  tiles whose cells are the four name tables two by two (and, under them, each pair's rows
  30 and 31, the attribute bytes a game shows when it scrolls up past the top); the sprites
  are another, a cell or two a sprite; the pattern tables are kept a byte a pixel in GPU
  memory, in pieces of a page so that a cartridge may turn them; a palette is a bank of 16
  for each of the NES's eight of four. The record of a frame is runs of lines: a new run
  only where the game wrote a register of the PPU, so a frame is a few quads a run (the
  colour behind, the sprites behind the background, the background, the sprites in front)
  and a game that scrolls every line is 240 short runs. The cells follow what the game
  writes to the name tables (a log of the addresses), the tiles what it writes to pattern
  RAM. Every frame is drawn, but one like the last is not (twice a second all the same),
  and one in two to four when the game is behind its time (with the list this core's: below).
  Eight sprites a line are kept to,
  as the PPU does. What differs from nofrendo's own drawing: a sprite in front is not hidden
  by an earlier sprite that is behind the background (8 pixels of a Thwaite game frame),
  and a write to the name tables in mid-frame shows in the whole frame.
  A Thwaite game frame is 30 quads and 14 thousand instructions (`nesgpu:` lines say).
- **The 6502 is recompiled on the machine** (`rc6502.h`): the cartridge's code is translated
  to RISC-V instructions when it is first reached (everything jumps reach from there:
  Thwaite's 3,950 instructions in one go, 140 KB of code), with the 6502's registers and
  flags in registers of the machine, and nofrendo's interpreter runs only what is not
  translated (the flags i and d, `php`, `plp`, `brk`, `rti`, the undocumented, and `jmp ()`
  through a pointer outside RAM; `rti`, `php`, `plp`, `sei` and `cli` are translated since:
  "Lines ahead" and "A NES's speed in the world" below).
  A translated instruction does what the interpreter's macro for it does and takes the same
  cycles, and after each the cycles left are tested, so a slice ends at the instruction the
  interpreter's would. A read or write of a device goes to nofrendo's own handler at the
  cycle count the interpreter would have; a write that is not RAM leaves translated code
  after its instruction (it may have turned pages or ended the slice). A jump is straight
  to its target's code inside a 4 KB page, or anywhere when the cartridge turns no pages;
  else it looks the target up as the pages are now. New code is fetched after `fence.i`,
  which ends the machine's pass.

- **A straight run of translated instructions is there twice** (`rc_run_length`, `rc_leaders`
  in `rc6502.h`): as it was, the cycles left tested after every instruction, and a copy with
  no test in it, entered when the cycles left cover the whole run (one test at its top). A
  run begins wherever a jump comes to, so a loop's body is a run from its top, and the
  branch, `jmp`, `jsr` or `rts` that ends it is in the copy and takes the run's cycles with
  its own. Reads of the cartridge are a load through the page table in line, and a write to
  a register of the PPU, the sound or the pads no longer leaves translated code.
  `NES_RC=plain` is without the copies and the next step, to compare.
- **A slice that ends in translated code goes on into the next line from there**
  (`rc_line`, and `ppu_line_between` in `nes_ppu.c`): what the frame's loop does between two
  ordinary lines (the PPU's end of one line and start of the next, and the
  cycles' book-keeping) is done from inside, and the code never leaves. Not at the line the
  picture ends, the two the blanking begins and ends at, when an interrupt waits that the
  6502 would take, or for a cartridge that counts lines. An awake line was 350 instructions
  round its slice and is about 110.
- **The GPU's list is a worker core's to make** (`gd_draw` as a job, `gd_show`): turning a
  frame's record into quads was 13 thousand instructions of a frame that is shown, on the
  core that has none to spare. The first core now only follows the game's writes and swaps
  (and since then it does not emulate either: "The emulation on a worker core" below):
  it gives the record to one worker (two records, the worker's and the one the PPU writes),
  goes on with the next frame, and shows the list when it finds it made, which it asks in
  the middle of the next frame and at its end. A frame whose turn comes while the worker
  is still at the one before is not shown (`nesgpu:` counts them: one or two at a level's
  start), and none is left out because the game is late, which was for when showing a
  frame cost this core 13 thousand. The picture is a frame later than it was.
  `NES_LIST=inline` is without the worker; a test (`NES_FRAMES`, `NES_FAST`) has it only
  with `NES_LIST=worker`. The sprites are written straight into the list
  (`seglSpriteSpace`, `seglSpritesUsed`), 50 instructions a sprite where the library's call
  for each was 250, and the eight-a-line count is made only when more than eight sprites
  begin within a sprite's height.
- **A list is shown without waiting for it** (`seglSwapEarly`, `docs/machine/gpu.md`). The swap
  waited, with `pause`, until the GPU had drawn the list, and the rest of that pass of the
  machine was not run (`docs/machine/boot.md`, "Passes that end early"): 6% of a game's passes'
  instructions with a frame drawn each sixtieth. The emulation of the next frame runs
  meanwhile now. What the list reads and is written again: the cells and tiles, which
  `frame_gpu` settles for before it follows the game's writes at a frame's end (the wait is
  long over then), and the sets and sprite cells, of which there are two pairs, a list made
  with the pair the list on show was not made with. So a list from the worker is shown and
  the next given out at once. The workers' pictures (`NES_PPU=workers`) are shown the same
  way: no team is given the picture on show. Only nofrendo's own drawing (`NES_PPU=here`)
  waits, since the next frame is copied into the picture the list reads.

  | a game of Thwaite, 1,500 frames as fast as they go (`tools\tick_util.ps1`) | utilization, waiting (`NES_SWAP=wait`) | not waiting |
  |---|---|---|
  | the list by the first core | 94.4% (the wait 5.5%) | 99.8% |
  | the list by a worker (`NES_LIST=worker`) | 93.5% (the wait 6.2%) | 99.6% |

  A game held to its sixty frames a second ends most of its passes in `frame_paced`, with
  nothing to run: that is its pace, not a loss.

- **The line's step saves no registers** (`ppu_line_plain` in `nes_ppu.c`, called from
  `rc_line`): the line that needs nothing called (no run of a record to begin, sprite 0 not
  on it, no cartridge counting lines) is done by a function that calls nothing, and the
  build is `-flto`, which puts it into `rc_line` whole. About 80 instructions a line where
  the three functions were 150; any other line goes to `ppu_line_between` as before.
- **A cartridge that counts lines goes the same way** (MMC3: `ppu_lines.hblank`): its count is
  made from inside, and when it asks for an interrupt the translated code is left with the
  next line begun and the interpreter takes it first (`rc_leave`). An interrupt asked for
  while translated code has the registers always waits so (`rc_running` in
  `nes6502_irq`), which it did not before. Mega Man 3: 213k to 138k a frame.
- **The background's cells are kept by the memory they are of** (`gd_tables`, `gd_follow`):
  the PPU marks the bytes of its own four kilobytes of name tables a game writes, a bit
  each, and the cells are worked out once for that memory; a table of the picture that
  moves to other memory is copied. Before, every cell of all four tables was worked out
  again whenever the cartridge set its tables, which some do at every write to their
  register with nothing changed: half the frame of Battletoads, Metroid, R.C. Pro-Am and
  Marble Madness (520k, 447k, 391k and 361k a frame; now 211k, 149k, 105k and 76k).

Super Mario Bros. (mapper 0, a visitor's own cartridge: not in the image), in play, with all
of that: 113k to 57k instructions a frame on the first core while the picture stands and
63k to 72k while it scrolls, and with the limiter on 60 of 60 frames shown a second where
26 to 46 were, at 98 to 103% of a NES's speed standing and 92 to 103% scrolling.

Thirty more cartridges were run for this (a visitor's own again: none is in the image).
All give the interpreter's RAM sums. A frame, in thousands of instructions, with the GPU
drawing (`NES_FAST`, the pad's Start pressed four times and Right held), against 65 for
a NES's speed: Balloon Fight 38, Zelda 60, Mega Man 2 65, Tetris 70, Bubble Bobble 73,
Ninja Gaiden 76, Marble Madness 76, Dr. Mario 87, Gradius 89, DuckTales 98, Pac-Man 102,
Double Dragon 104, Excitebike 105, R.C. Pro-Am 105, Ice Climber 114, Mega Man 122, Donkey
Kong 131, Contra 134, Mega Man 3 138, Metroid 149, Kirby's Adventure 180, Castlevania
188, Battletoads 211, Punch-Out!! 325. What the slow ones have:

- Donkey Kong, Ice Climber and their kind wait by turning their random numbers, a `jsr`
  and eight `ror` a turn: work all frame, 1.4 instructions a cycle of the 6502, and a
  line's step at every line. Nothing sleeps.
- Castlevania reads the PPU's status in a loop that is not one that only looks: a fifth of
  its frame is `rc_rt_read`, `rc_slow_read`, `mem_readbyte` and `ppu_read`.
- Battletoads writes pattern RAM and the PPU's registers all frame (`ppu_write` and its way
  there 15%, `gd_tile` 2%).
- Punch-Out!! (MMC2) turns pattern pages as they are drawn, so the first core draws it.
- An `rts` that kept where it went last was tried (a key and an address at the
  instruction, a turn counted when pages are set): 4% off Donkey Kong, and 5% on to a
  cartridge whose returns go to many places. Not kept.

Where the first core's 70k of a Thwaite game frame go (measured at 85k, before the last cuts): the quiet lines 17%, the interpreter
for what is not translated and the way in and out of translated code 15%, the lines' records
9%, translated code 10%, the kernel 13%.

Another cartridge, measured (Super Mario Bros. + Duck Hunt, mapper 66; not in the image):
its three programs wait in three ways, and one of them was not known.

| | a frame, before | now |
|---|---|---|
| Super Mario Bros., its title | 175k (31%) | 98k (56%) |
| the cartridge's menu | 192k (28%) | 192k (28%) |

- Super Mario Bros. waits in `jmp` to itself: no branch, so no loop was looked for and every
  line of every frame ran it. That is the row above. What is left of its frame is the game
  (it is long in its interrupt) and what a line costs.
- The menu passes time in `lda $2002; inx; bne` four times 256 to a frame: X and Y change, so
  it is not a loop that only looks, and each read of the status is the device's handler
  from translated code (a profile of it is a third `ppu_read`, `mem_readbyte`,
  `rc_slow_read` and `rc_rt_read`).
- Duck Hunt's main loop turns its random numbers (`jsr` to a shift of four bytes of RAM)
  until the interrupt sets a flag: work whose every turn shows in memory.
- `jmp ()` through a pointer in RAM is translated now (the game's table jumps are all that):
  it took 2% off the times translated code is entered and nothing off the frame.

Not done: a read of the PPU's status from translated code without the handler, when it
changes nothing; a second pad, saved games; the first core's own drawing (one core) from
the record, which would let it skip more frames.

### A quarter to two fifths fewer again (12 October 2026)

Thousands of instructions a frame under `rvc_cpu`, the build before against this one: Thwaite's
title 39 to 26, Thwaite in a game 64-48 to 48-33, Nova's menus 81-54 to 58-41 and Nova in
play 31 to 18; with the list by a worker the same shares. From the command to the first
`nesstat:` line: Thwaite 7.8 million to 4.3, Nova 9.9 to 6.0. `NES_PPU=here` (Nofrendo's own
drawing) is as it was.

- **A line asleep is a table looked up** (`quiet_tables`, `quiet_run` in `nes_ppu.c`): where
  a slice of the sleeping 6502 ends is the first instruction's end at or after the slice's,
  by where in a turn of the loop that is. A line's cycles and what is left of them are
  131072ths (a float of their size has no finer), so many lines are done at once in whole
  numbers, which are the floats' own sums, with the walk down the name tables worked out
  and not stepped: 12 instructions a line where it was 65. Line 240 and the lines of the
  blanking (242 to 260) pass so too; the sound's quarters are called as the loop called
  them, one a line.
- **A loop that counts** (`dex`, `dey`, `inx` or `iny` as the last before the `bne` that
  closes it, no other branch, nothing else of X or Y, no read of the status) is passed over
  like one that only looks, as many turns as fit the slice (`idle_counting`), and slept in
  while its turns last: the delay a game makes after sprite 0. A trial turn costs about 450
  instructions; Thwaite's nine trials a frame are 11% of its frame, the next thing to take.
- **A `jsr`'s routine is translated when it is first called** (`rc_later` in `rc6502.h`):
  the `jsr` leaves, and is made a jump when its routine is there. Thwaite translated 3,951
  instructions before its first frame and now 101; 1,651 after 600 frames. `NES_RC=eager`
  is as it was (and `NES_RC` means plain only when it says `plain`).
- **The machine's own registers are called without the search** (`rc_slow_read`,
  `rc_slow_write`: they are the first handlers of Nofrendo's list whatever a cartridge
  adds), and a cartridge's memory at $6000 is read and written in place when no handler is
  there, the write no longer leaving translated code.
- **The cells follow by rows** (`gd_cells_written`): a written byte's cell once, a row with
  more than eight whole, a table that moves copied a row at a time; `gd_tile` by a table of
  whole bytes; the ninth sprite looked for only in crowded bands of eight lines. Nova's
  busy frames were 8 to 12 thousand instructions to follow its writes and are 3.
- **Start**: Nofrendo's palette (64 colours through `sin` and `cos` in doubles, 300 thousand
  instructions) is a table for the hue and tint it starts with; the cartridge's file is
  mapped; the translator's tables are mapped, not cleared.
- The build is `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops`: 5% fewer
  instructions a frame than `-O2` (`-O3` alone 2%, `-O2 -funroll-loops` 3%).

Not done: `ppu_write` itself (60 instructions a `$2007` write, 11% of Nova's busy frames),
what an instruction costs to translate (210), `rc_rt_dispatch` at every `rts` of a cartridge
that turns pages (3.4% of a Nova frame).

Checking these: the sums the doc lists hold in every mode, `NES_RC=eager` among them, and
they are not enough by themselves: a quiet run that began at line 240 and went on through
241 passed all of them and parted from the build before at frame 612 of `NES_FRAMES=1000
NES_PRESS="20 start 50 start 80 a 100 left 110 a 200 a 210 +right 300 a 400 a 420 +up 500 a
800 a 900 -right 1000 a 1100 +left 1300 a" nes thwaite` (RAM `bf00e7da`); Nova with
`NES_PRESS="60 start 120 start 180 start 240 start 300 a 330 +right 600 a 700 a 800 a 900 +a
1100 -a 1200 a 1400 +left"`, 1,000 frames: RAM `fca83c4e`. What the GPU is given can be
compared under `rvc_cpu` without a picture: at a hold, sum the list (address and count at
0x81000300), both sets (0x81700000 to 0x817C0000), texture memory to its top, and the palette.

### The window system asked less (13 October 2026)

Thousands of instructions a frame under `rvc_cpu`, the list by a worker, the two runs of
1,000 frames above: Thwaite 39.2 to 37.1, Nova 38.7 to 36.6. The RAM sums are as they were
(`bf00e7da`, `fca83c4e`), and every sum of "Testing" in every mode it names. On the shader
machine at 4 ms a pass (2M instructions a second) the same runs, as fast as they go: Thwaite
49.0 to 51.5 frames a second, Nova 48.7 to 51.4, with those sums; and a Thwaite game held to
its time there, eleven counts of 60 frames: 51.0 to 53.8 a second.

- **The pad is asked for when something moved.** Every fourth frame the window system was
  asked for events: two system calls and two task switches, some 8 thousand instructions a
  time, nearly always for nothing. The machine's own input words (`docs/machine/input.md`: the
  pointer, the buttons, the key events so far) say when there can be any: it is asked when
  they have moved, eight times after that, and twice a second (`input_due`).
- **A game ahead of its time rests** (`mcw_rest()` in `frame_paced`, `docs/machine/multicore.md`,
  "A core that rests") where it paused: the kernel has the core meanwhile.

Looked at and not done then (done since: "The emulation on a worker core" below): the
emulation itself on a worker core, with the first core only showing lists and asking for the
pad. A worker has no kernel in its way (the tick and the
rest are 7% of the first core's frame now, 12% before the above), so a frame would be near
34 thousand of the worker's instructions. Against it: a job and its answer are a pass each
way, a worker keeps 1.5 KB of new stores a pass where the first core keeps 6, and every
variable of Nofrendo would need 16 bytes of its own (the build is one program with common
variables). A frame's instructions were not halved: what is left is spread thin (translated
code 29%, the lines' steps 20%, the ways in and out of translated code 12%).

A game held to its time that falls behind is not owed the time back (`frame_paced`): Nova's
menus at 3M instructions a second show 60 frames in 1,650 ms where the same frames as fast
as they go take 1,090, the slow frames (a level made ready, new code translated) late and
the quick ones between them held to a sixtieth.

### Lines ahead: a third fewer (13 October 2026, later)

Thousands of instructions a frame on the first core under `rvc_cpu`, the two runs of 1,000
frames above, the build before against this one:

| | all 1,000, the list a worker's | the game itself | the list this core's |
|---|---|---|---|
| Thwaite | 36.9 to 25.5 | 35 to 40, now 23 to 24 | 43.1 to 32.3 |
| Nova | 36.5 to 28.6 | 14 to 19, now 10 to 15 (a level made ready 154 to 128) | 38.5 to 30.7 |

On the shader machine, the same runs as fast as they go, frames a second, with the RAM
sums as they were (`bf00e7da`, `fca83c4e`):

| | Thwaite's 960 counted frames | Thwaite in its game | Nova's 960 |
|---|---|---|---|
| 4 ms a pass (2M instructions a second) | 47.5 to 66.4 | 52.4 to 82.5 | 47.4 to 59.3 |
| 5.46 ms a pass (1.5M) | 34.1 to 47.5 | 37.4 to 59.2 | 33.9 to 42.6 |
| as the card goes (RTX 5090, in turn twice) | 121.9 to 165.8 | 137 to 216 | |

`NES_FAST=1 NES_FRAMES=1200 nes thwaite` on 19 October 2026 on an RTX 5070 laptop (`docs/machine/speed.md`),
the last 60 frames: 373% of a NES's speed on the shader machine, 332% on `rvc_lean`, 653% on the
compute machine, 5,263% on the CPU harness.

Held to its time, 60 frames of a Thwaite game took 1,028 to 1,108 ms at 2M and take 1,000,
59 or 60 of them shown, the first core resting in 36% of its passes; at 1.5M they took 1,524
to 2,124 and take 999 to 1,076. So it holds a NES's speed at 2M with a third to spare and is
at its edge at 1.5M. Of the passes that have work, 99.7% of the instructions are run, as before.

- **A slice of the 6502 is many lines** (`lines_ahead`, `ppu_lines_settle` in `nes_ppu.c`;
  `rc_settle` in `rc6502.h`). When a line begins from inside translated code and the lines
  after it have nothing in them (no record's run to begin, sprite 0 not before them, no
  cartridge counting, the frame interrupt far off, the picture's end or the blanking's not
  reached), the slice is given the cycles of them all. A line's cycles and what is left of
  them are 131072ths, so where each of those lines would have ended is known without the
  6502 stopping there: the first instruction's end at or after the line's cycles, whatever
  the lines before came to. They are counted when something looks: a read or write of a
  register that is not a pad's or a sound channel's, a loop tried for idleness, translated
  code left, or the slice's end. The lines that have passed are then done at once (the walk
  down the name tables worked out) and the slice is cut to the line it is in, so what looks
  finds the line and the cycles it would have. Awake lines were 16% of a Thwaite frame and
  are 4%; `NES_LINES=each` is a line a slice, as it was (32.6 thousand a frame against 25.5).
- **A read that may be a device's is in no unchecked copy of a run**: the slice may be cut
  there, so the cycles left must be tested after it. It cost 0.3% of a frame.
- **A game that looks at the PPU in every line gains nothing by it and loses 3%**: a slice
  cut in its first line gives the next four lines one by one (`ahead_rest`), which is a
  third of what it lost. The cartridge of our own (below), which reads the PPU three times
  a turn of its loop: 163.8 thousand instructions a frame before, 170.9 without the rest,
  168.4 with it.
- **Quiet lines are one sum** (`quiet_run`): where the last of them ends says where they all
  do, for the same reason, so the loop over the lines is gone (5% of a Thwaite frame).
- **C and V in a run's copy** are worked out only where they are read before they are set
  again (`rc_flags`), as N and Z were; `adc` after `clc` and `sbc` after `sec` add no carry;
  a comparison's C is one instruction (the difference against -1, which a register keeps);
  `inx`, `iny`, `dex`, `dey`, `asl a` and `lsr a` several times over are done once. The code
  of a cartridge is 7% smaller.
- **`rts` goes by a table of the 6502's addresses** when the cartridge turns no pages
  (`rc_rt_direct`): two loads where the search by page was eight.
- **`rti` is translated**, a routine's (`rc_rti`): the interpreter's only when it has the
  registers or the flags it restores let a waiting interrupt in. **Cycles to burn are burnt
  in the slice** (`rc_slice`), the four lines and a half of a copy of the sprites among
  them, with the lines stepped from there. So the line the interrupt comes in no longer
  goes through the interpreter.
- **The PPU's data register and the pads have routines of their own** (`rc_vdata`,
  `rc_pad`): a write to `$2007` was near 115 instructions and is 70, a pad's bit near 100
  and is 57; a pad's sources are a list of their own (`nesinput.c`). Nova's level made
  ready is a tenth shorter by the first.
- **The sound's four quarters** work a channel's voice out only when its period, level or
  wave is another than last time (`as_seen` in `apu_shaderemu.c`).

- **The GPU's library is built apart, a section a variable and each on 16 bytes**
  (`build.sh`, as `linux/tdawn/build.sh` has it), and the tables the list's worker writes
  (`across`, `down`, `on_line`) are whole 16s. Since the build was made one program, the
  library's variables lay among the emulator's by chance of the build, and two cores must
  not store to the same 16 bytes (`docs/machine/multicore.md`): in this round's builds the PPU's
  `line_news`, which the first core stores to at every write of a register, shared 16 bytes
  with the library's `matrices_dirty`, which the worker stores to once a list, and `on_line`
  with `gd_spent`; the build before had `gd_out` and `drawn` with the library's `bound`. One
  run of nine as fast as the card goes showed no frame after the first second, the worker
  never done: its cause was not found, and it was not seen in eight runs after this. The
  list this core's (`NES_LIST=inline`) is 2% more instructions for it; a worker's is none.

Where a Thwaite game frame's 23 to 24 thousand go now: translated code 42%, the kernel 7%
(its tick: nothing a program can do), the frame's loop and quiet lines 6%, slices begun and
ended 5%, the rest in pieces of 1 to 3%.

Not done, and why: the emulation on a worker core (done since, below);
translation on a worker (a level made ready is a fifth translation, 300 instructions for
one of the 6502's: two cores would write one table and one block of code); `php`, `plp`,
`sei` and `cli` as routines like `rti`'s (neither game uses them in a frame; done since,
"A NES's speed in the world"); the kernel's
tick at a lower rate (`AGENTS.md`).

Checking it: every sum of "Testing" in every mode, `NES_LINES=each` among them; and since
two games are few, each with three more scripts of presses over 2,000 frames, the build
before against this one, this one with `NES_LINES=each`, `NES_RC=plain` and
`NES_CPU=interp`: thirty runs, each game's five agreeing in RAM and picture. Held frames
of the GPU's target (`--gpu-capture`, frame 600 of the Thwaite run, 450 and 700 of Nova's)
are the build before's, no pixel different.

**A cartridge of our own** (`linux/nes/exact_rom.py OUT.nes [0 to 4]`, to be put in an
image as `/usr/share/nes-own0.nes` and run as `nes own0`): its program never waits long,
reads the PPU's status through a pointer and across a page at every cycle of a line,
pushes its flags (the interpreter's instruction) every turn, and folds what it reads into
RAM, so a cycle or a line out of place is another RAM sum. 600 frames of each of its five
forms give one sum in nine modes (`NES_CPU=interp` and `NES_PLAIN=1` among them): `596309bb`,
`e0f45e7f`, `6bc942f0`, `61402db1`, `c0df8670`. It does not read the data register: Nofrendo
reads memory it does not have when the address there is what a picture left.

It found a mistake that was there before, in the interpreter as in translated code: **a
loop that counts was slept in for a turn too many** when the slice it was found in had
exactly one turn left after the turns passed over. That turn is run, and the count made
before it stood, so the sleep took the loop's last turn for one whose branch is taken: a
cycle more (forms 1 and 4 gave `033d4345` and `550cefc2` asleep, and the sums above with
`NES_PLAIN=1` or `2`). The count is made again from the register as the 6502 goes to
sleep (`idle_count_from` in `nes6502.c`). Neither game's sums moved.

### The emulation on a worker core (14 October 2026)

The first core no longer emulates. A worker runs the frames, another makes the GPU's lists
from them, and the first core shows the lists, reads the window's events for the pad and helps
the two on at a page or a system call (`run_elsewhere`, `emulate`, `lists_job`, `lists_step`
in `osd_shaderemu.c`). `python tools\game_par.py nes` (Thwaite, the 1,000-frame run below, the
list by a worker, frames 100 to 1,000; the mean of three timings under `rvc_cpu`), and Nova's
1,000-frame run measured the same way:

| | first core a frame | all cores | all over the first | lists shown of 900 | guest seconds |
|---|---|---|---|---|---|
| Thwaite, before | 24.6k | 31.0k | 1.26 | 792 | 6.74 |
| Thwaite, now | 3.3k | 30.9k | 9.33 | 798 | 5.94 |
| Thwaite, `NES_EMU=lists-here` | 9.4k | 30.6k | 3.27 | 798 | 5.95 |
| Thwaite, `NES_EMU=here` (as before) | 23.8k | 30.1k | 1.26 | 792 | 6.45 |
| Nova, before | 25.8k | 27.9k | 1.08 | | |
| Nova, now | 3.7k | 29.3k | 7.96 | | |
| Nova, `NES_EMU=lists-here` | 5.5k | 29.0k | 5.22 | | |

The game is 12% faster as fast as it goes (the emulating worker has no kernel in its way:
21.3k instructions a frame where the first core spent 24.6k), and held to its time Thwaite
keeps 60 frames a second with 60 shown. All cores together run as many instructions as before
for Thwaite and 5% more for Nova, whose frames are short: a first core with nothing to do still
costs about 1,100 instructions a pass (the rest, 322, and the kernel's own), 3.0k of its 3.3k a
frame, where the emulation's kernel was 1.85k. Sleeping in the kernel instead of resting was
tried: 830 a pass, and a list shown in four. The lists' worker waited in 2,675 passes of the
run, some 40 instructions each (2% of its 6.3k a frame); the emulating worker waits only for
the last frame's list.

- **The frames are one long call** (`mcw_try_call(emulate)`, `programs/mc/mcwx.h`): nofrendo's
  loop of frames is the job (`nes_run_elsewhere` in `nes.c`). It ends at the test's last frame
  or hold by a jump out (the first core then writes its lines and ends or holds), and at the
  end of a frame when the kernel wants its core back (`mcw_recalled`): the frames go on in
  another call, or on the first core as without workers.
- **What each side says is 16 bytes of its own**: the frame given out, its record and copy
  (`emu_said`); the list made (`lists_said`); the frame whose list is drawn, the keys
  (`first_said`). Everything else a side writes is its own too: the emulating worker all of
  nofrendo, the lists' worker `gd` and the GPU's library, the first core the window system and
  the swap (which takes the GPU's lock, also the window system's: never from a worker).
- **The cells and pattern RAM's patterns are three copies** (`COPIES`, `gd_tables`): a list is
  made with one while the GPU draws another and the emulation makes a third up. Each copy keeps
  the bytes and tiles written since it was last made up (a word of bits a row of 32, and a word
  a row of those words: `ppu_written_rows` in `nes_ppu.c`), and a frame is given out only when
  the list its copy last went into is drawn: `nesgpu` counts the others as not shown. With two
  copies 711 of 900 were shown. `NES_CELLS_CHECK=1` works every copy out whole as it is given
  out and must say `0 of N frames differ`.
- **Its lines are `mcw_print()`'s**, written when the call ends, or at once for `nesmark` and
  each `nesstat`, by a system call the first core makes (`mcw_print_now()`, added to
  `mcwx.h`). Written in one go, the 2 KB log stopped the emulating worker for 450 ms while the
  console took it. The clock in `nesstat` is the machine's word, not a system call.
- **The pad**: the first core counts each key's presses and keeps those down; the emulating
  worker presses them every fourth frame, as `osd_getinput` did. A test's presses (`NES_PRESS`)
  are the worker's, at the same frames, so every sum is as it was.
- **Pages**: the first core reads the cartridge and writes the translator's tables and the
  first 256 KB of its code before the worker starts (`nes6502_recompile_touch` in
  `rc6502.h`): 134 pages were brought for it in the run, now about 50. The translator's list of
  routines to come back to is in its tables, not `realloc`ed (no `malloc` on two cores).
- **The sound's recordings** go to `mcw_call()` too (`apu_shaderemu.c`); on the emulating
  worker a recording is decoded there and then.
- **Without a sound card the voices are not worked out** (`show` in `apu_shaderemu.c`): the
  channels are followed as before, and what a voice would be told is left; 0.7k a frame.
- **A line with more than eight sprites is found by counts** of the sprites that begin and end
  on each line, and only such a line is walked in their order (`gd_draw`): it was a byte
  counted for each of a sprite's 8 or 16 lines, behind a clear of 256. The same lines are lost
  (200,000 random sets of sprites on the host, 118,403 with such a line: none different).
- Smaller: the sprites' cells and quads are made with the counts and tables in registers (the
  list 0.6k fewer), the test's presses read once each (`sscanf` every fourth frame was 0.4k a
  frame), and the last picture's sum four bytes a step (the same sum).

`NES_EMU=here` is the old way, `NES_EMU=lists-here` makes the lists on the first core (the
fewest instructions in all, a third of the first core's). Checked: every sum of "Testing" in
all the modes it names, `NES_EMU=here` and `lists-here` among them; the cartridge of our own's
five forms; `RVC_RACES` over both 1,000-frame runs, 0 undone (the reads as they were are the
words above); `mctest 3 120000` before and after a `nes nova` killed with `-9` and a held one;
Thwaite held to its time; `NES_HOLD` and `NES_HOLD_LATE`; held frames on `rvc_harness --cpu
--warp` (Thwaite 260, Nova 450) against the build before: no pixel of the NES's picture
different (the caption's frames a second and the bar's figures are). Not checked: the shader
machine and the world (no runs on the card), a recall (the kernel never asked for these cores with 256 of
them), sound on a card. What would take the instructions down from here: the first core's
1,100 a pass of looking on (it can neither sleep longer nor end its pass without the kernel),
and the list's 6.3k a frame, of which the sprites' cells and quads are some 3k.

### A NES's speed in the world (15 October 2026)

The emulation on a worker made the first core idle, not the game faster: the frames are as
many instructions on the worker as they were on the first core (less the kernel's), and a
worker runs no more of them a second. What the world shows is the emulating core's
instructions a frame against the instructions a core runs a second. Measured so under
`rvc_cpu`: every pass a round of 8,192 at 3 MIPS a core, whatever it ran (`--ticks 8192 --ips
1e12 --pass-ms 2.7307`, `docs/machine/cpu-harness.md`), `NES_LIST=worker NES_FRAMES=N` as fast as it
goes, and `nesstat`'s percentage, which is frames over the machine's clock, over the frames of
play (a visitor's own cartridges, in a private image; none is in the image):

| | thousand instructions a frame, the emulating core, before | now | % of a NES's speed, before | now | now, `NES_EMU=here` |
|---|---|---|---|---|---|
| Super Mario Bros., 1-1 (frames 240 to 1,200) | 39.3 | 37.1 | 125 | 131 | 122 |
| Super Mario Bros. 2 (USA), 1-1 (720 to 1,500) | 105.8 | 45.6 | 47 | 107 | 99 |
| Super Mario USA (Japan), the same | 105.7 | 45.5 | 47 | 107 | 99 |
| Super Mario Bros. 3, 1-1 (1,080 to 2,000) | 113.3 | 54.7 | 44 | 90 | 85 |
| Thwaite, a game (120 to 1,000) | 22.8 | 20.1 | 213 | 237 | 212 |
| Nova (300 to 1,000) | 22.8 | 20.7 | 210 | 227 | 215 |

"Before" is the build of 12 October (`NES_EMU` came after it). With the shader machine's write
cache modelled (`RVC_WCACHE=1`) every figure is the same: the emulating worker's passes are not
cut by its stores. The two MMC3 cartridges were at half a NES; what took them there:

- **A cartridge that counts lines has lines ahead too** (`hblank_quiet`, `hblank_skip` in the
  mapper's interface; `map4_quiet` and `map4_skip` for MMC3). Its count is known: the lines
  before the one it asks for an interrupt in only count. So they are given at once as other
  cartridges' are, asleep (`ppu_quiet_lines`, `ppu_quiet_counted`) or awake (`lines_ahead`), and
  counted when they are settled; the line it asks in goes the old way. Before, every line of
  an MMC3 cartridge was a slice of its own.
- **A mapper's write goes on in translated code** when it turned no page of the program and
  asked nothing of the slice (`nes6502_turns` counts the times pages are set to others;
  `rc_mapper_write` from `sta $8000` and up, the handler kept between writes, `rc_mapper`).
  It left translated code at every write, some fifty a frame of SMB3. `mmc_bankrom` sets its
  pages one by one (`nes6502_setpage`) where it copied the 6502's whole context twice.
- **`php`, `plp`, `sei` and `cli` are translated** (`rc_flag_op`): i, d and b are `cpu.p_reg`'s
  while translated code runs, the interpreter's too (`RC_RUN` puts its own there and takes
  them back). A `cli` that lets a waiting interrupt in leaves after itself; one in the
  interpreter's slice leaves before itself, and the interpreter's `cli` takes it. A `plp` that
  lets one in cuts lines ahead at its line, as the interpreter's slice of one line would: a
  cartridge of our own (form 2, `plp` of data) found that (the interrupt was taken lines late).
  An instruction of these is never entered from the interpreter (as `rti`).
- Smaller: the cartridge's memory at 0x6000 read and written in place by `lda`, `sta` and their
  kind at an address (and indexed within a page), with no routine; a write's cut of a slice no
  longer makes the next four lines one by one (that is for a game that reads the PPU every
  line); `memcmp` is not used where a frame goes (the library's is a byte at a time).

The first core's readout: `speed_caption` in `osd_shaderemu.c`, from the frames the emulating
core says it has made (`emu_said.frames`) and the machine's clock; its variables are 16 bytes
of their own (with them among the emulating worker's, `RVC_RACES` found 8 stores undone).

Checked: every RAM and picture sum of the six cartridges and the five forms of our own, in
`NES_LIST=worker`, `NES_CPU=interp`, `NES_EMU=here`, `NES_LINES=each`, `NES_RC=plain`,
`NES_RC=eager`, `NES_PLAIN=1`, 2 and 3: one sum each, and the build before's; the sums of
"Testing" below; `NES_CELLS_CHECK=1` 0 frames different in Thwaite, Nova and SMB3; `RVC_RACES`
0 undone in SMB, SMB2, SMB3, Thwaite and Nova. Pictures at holds (`--save-ram`) of the four
Mario cartridges in play look right, the status bar of SMB3 split at its line. SMB2 (USA) 1-1
is black behind: the game writes `$0F` to `$3F00` there, the same in every mode and in the build
before. Not done: anything on the card or in the world; SMB3 is short of a NES at 3 MIPS a core
(its frame: translated code a fifth, writes to devices a fifth, lines a tenth, and new code
being translated near a tenth over its first 2,000 frames).

## Sound

No sample is computed. The NES's five channels are five voices of the sound card
(`docs/machine/sound.md`), each playing a wave that lies in sound memory from the start, and
`linux/nes/apu_shaderemu.c` only tells the card how fast and how loud: the card reads a
voice's step and volume all the time. Nofrendo's own synthesis (`apu_process`) is never
called; its register handlers call `se_apu_write` first and are otherwise as they were, so
what a game reads back at `$4015` is what it read before.

| Channel | The voice plays | Pitch | Loudness |
|---|---|---|---|
| two pulses | a turn of 256 samples, one for each of the four duties | step from the 11-bit period; the sweep moves it | the envelope or the fixed level, 0 to 15 |
| triangle | its 32 steps | the period; a step of 0 when the length or linear counter stops it, so it stands where it was, as the NES's does | fixed |
| noise | the shift register's 32,767 bits, or the 93 of its short mode | one of 16 rates | envelope or fixed level |
| DMC | a recording decoded to bytes, in one of six slots | one of 16 rates | fixed |

- **The frame counter** is four calls a frame, at lines 65, 130, 196 and 261 of the picture
  (`se_apu_quarter`; `nes.c` compares the line with one number): envelopes and the
  triangle's linear counter at each, length counters and sweeps at every other. Each is a
  few stores to the voices' words. A game's frame did not move: 60 thousand instructions.
- **Workers.** With the channels on the card there is nothing to give a worker every frame.
  Two things are work, and both are a worker's when the program has one (`mcw_call()`, on a
  worker with nothing to do): the long noise table at the start, and a DMC recording the
  first time a game plays it (up to 32,648 samples from one bit each). The first core copies
  the recording's bytes out of the cartridge, gives out the call, and starts the voice at the
  quarter of a frame after it is made; the same recording again is found in its slot (by
  address, length, start level and first bytes). While the emulation is a worker's, a
  recording is decoded on that worker there and then.
- **A game that runs slower than a NES plays its tune slower**, with no gaps: nothing is
  queued that could run dry.
- Loudness is the NES's own mix in its linear form (a pulse 0.00752 a level, the triangle
  0.00851, noise 0.00494, DMC 0.00335), 1.2 of it to the card's full scale.
- Without a sound card (`rvc_cpu`, `NES_SOUND=0`) the channels are followed all the same.

Not done: writes to `$4011` as a way to play samples (the level is only where the next
recording starts), the DMC's interrupt, the frame counter's interrupt and its five-step
timing (five steps are run at the four-step pace), and the exact `$4015` (it is Nofrendo's:
a channel counts as sounding from its first note on).

To check it:

- The sums below hold with it, on a machine with a card and without.
- `NES_APU_PRINT=1` prints every frame's channels (`nesapu: FRAME p1 PERIOD LEVEL DUTY p2 ...
  tri PERIOD RUNS noise RATE LEVEL SHORT dmc PLAYING`): under `rvc_cpu`, a Thwaite game
  (`NES_PRESS="20 start 50 start 80 a 100 left 110 a"`) has pulse period 253 (440 Hz).
- The card itself, as any program's sound: `--fixed-dt 0.004 --sound-capture F.wav
  --save-state F.snap` with `nes thwaite & sleep 14`, then `tools\sound_reference.py F.wav
  F.snap --rom build\images\nommu\rootfs.bin` must say MATCH.
- `NES_APU_TEST=1 nes thwaite` on the shader machine plays a recording made of the
  cartridge's first bytes (neither game of the image has one) and must print `the same as
  this core's` for what the worker decoded; `mctest 255 20000` passes beside it.

## Testing

- `NES_FRAMES=N` ends after N frames with `nes: done, picture SUM`; `NES_PRESS="20 start
  80 a"` presses buttons at frames, so a run repeats; `NES_SUM=1` adds a sum of the 6502's
  RAM after every frame, which is the one to trust: the last picture was the same through a
  mistake that the RAM's sum showed (a loop taken for idle at sight: a branch can be come
  to from elsewhere than the instruction before it).
- The sums a change to the emulator must leave, with `NES_SUM=1 NES_PPU=inline`:
  `NES_FRAMES=120 nes thwaite` RAM `31a18295`, picture `afcb4a98`; with `NES_FRAMES=180
  NES_PRESS="20 start 50 start 80 a 100 left 110 a"` RAM `2f58587d`, picture `39c90e07`;
  `NES_FRAMES=120 nes nova` RAM `3a2e5c47`, picture `d8a04406`. A cartridge that turns pages
  and waits in `jmp`, if one is at hand (Super Mario Bros. + Duck Hunt, `NES_FRAMES=600
  NES_PRESS="60 start 200 start 400 right"`): RAM `3b4dae3d`, picture `8f460adf`.
  Super Mario Bros. alone (mapper 0), `NES_FRAMES=600 NES_PRESS="60 start 200 right 260 a
  300 right 400 a"`: RAM `b8a863d7`, picture `208ec03b`. In `NES_PRESS`, `+right` holds a
  button down from its frame and `-right` lets it go.
- `NES_MARK=N` prints `nesmark` at frame N: `rvc_cpu --pc-log FILE --pc-from nesmark --ticks 256
  --pass-ms 0.0078` then samples the first core from there (which emulates only with
  `NES_EMU=here`: "The emulation on a worker core"), for `tools\pc_profile.py` with
  `nes.nm` (a profile of a cartridge in seconds; the pass's milliseconds go down with its
  instructions, or the kernel's timer is a quarter of the profile).
- Each of these must give the same: `NES_RC=plain`, `NES_LINES=each` (a slice of the 6502 is one line), `NES_LIST=worker`, `NES_CPU=interp` (no recompiler), `NES_PLAIN=1` (every
  turn of an idle loop run), `NES_PLAIN=2` (loops passed over, nothing asleep), `NES_PLAIN=3`
  (asleep, no quiet lines), `NES_PPU=workers`, `NES_PPU=inline`, `NES_EMU=here` (the
  emulation on the first core), `NES_EMU=lists-here` (the lists on the first core);
  `NES_CELLS_CHECK=1` must say `0 of N frames differ`. `NES_PPU=here` is nofrendo's own drawing, a frame in three: its
  frames are counted otherwise, so it has sums of its own (`b6f2e9ee`, `451f1f1d`,
  `70810528`), the same with each switch.
- All of that is instructions, so `rvc_cpu` does it (`docs/machine/cpu-harness.md`), workers and
  all: the three runs in two seconds, with the same instructions a frame as `rvc_harness`.
  Speed in real seconds and pictures are `rvc_harness`'s.
- The GPU's drawing is checked at a held frame, where the first core draws the same record
  too: the window against that picture (`NES_HOLD`, below: no pixel different in the two
  titles and in Nova scrolled; the 8 above in a Thwaite game), and the GPU's target against
  its software model (`--gpu-capture`, `tools\gpu_reference.py SNAP BMP --size 512x480`:
  every pixel away from a triangle's edge the same).
- `NES_SWAP=wait` waits for every list, as it was. `NES_HOLD_LATE=1` with `NES_HOLD=N` leaves
  frame N on the window while frame N + 1 is emulated and its writes followed, and stops
  then: with `--gpu-capture`, once with `NES_SWAP=wait` and once without, the two pictures
  must be the same (frame 260 of a Thwaite game: no pixel of 512 x 480 different). A plain
  hold stops before the next frame writes anything, and cannot show a store made too early.
- `NES_HOLD=N` stops at frame N with the window up and prints where the picture is in GPU
  memory (bytes past the palette at 0x81000400): in a snapshot from `rvc_harness`,
  the window's layer must be that picture through the palette, each pixel twice, and the
  workers' picture the same as `NES_PPU=inline`'s, with no pixel different.
- The cartridge of our own (`exact_rom.py`, "Lines ahead" above) must give its five sums in
  every mode.
- `mctest 3 120000` must print `PASS` before and after `nes` in one boot, and after one
  killed while its worker made lists (`nes nova & sleep 8; killall -9 nes.bin`).
- A key let go between two of the emulator's looks at the keyboard (every fourth frame)
  stays down until the next: a tap would otherwise never reach the game.
