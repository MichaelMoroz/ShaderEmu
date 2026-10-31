# A NES

An emulator inside the emulated machine: Nofrendo (Matthew Conte's, as Espressif keep it for
the ESP32) as a Nano-X client, with two games anyone may pass on. Held to a NES's 60 frames a
second it has room to spare: unheld, 378% of a NES (calibrated, 31 October 2026, below).

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
  `tools/private_image.py`) where it would go in the shared root. `NES_PROFILE=count` builds the
  program that counts, `nes.profile` is its counts of Thwaite's and Nova's played frames
  (`docs/games/tdawn.md`, "What the compiler is told").
- The build is `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops`, as one program (`-flto`).
  The GPU's library is compiled apart, a section a variable and each on 16 bytes, and the tables
  the list's worker writes (`across`, `down`, `on_line`) are whole 16s: inside the one program the
  library's variables would lie among the emulator's by chance, and two cores must not store to
  the same 16 bytes (`docs/machine/multicore.md`).
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

`game_par.py nes --calibrated` (31 October 2026): Thwaite, the 1,000-frame run of "Testing", the
list by a worker, frames 100 to 1,000, as fast as they go.

| Frames a second | Of a NES | Passes a frame | First core a frame | All cores a frame | The busiest worker |
|---|---|---|---|---|---|
| 226.9 | 378% | 2.76 | 2.7k | 30.3k | 21.9k |

A game is held to 60 frames a second (`frame_paced`; a test with `NES_FRAMES`, or `NES_FAST=1`,
runs as fast as it goes). A game ahead of its time rests (`mcw_rest()`, `docs/machine/multicore.md`,
"A core that rests"), and one held to its time that falls behind is not owed the time back: the
slow frames (a level made ready, new code translated) are late and the quick ones between them
held to a sixtieth. Nofrendo's own drawing on one core (`NES_PPU=here`, a frame in three) is
about a third of a NES. `nesstat:` and `nesgpu:` lines say a frame's instructions.

What the speed rests on:

- **A game waits in a loop**, most of every frame: for its interrupt (`cmp zp; beq`, or a
  `jmp` to itself, which is how Super Mario Bros. waits), or for the PPU's status to say the
  picture has reached sprite 0 (`bit $2002; bmi; bvc`). A taken branch a few bytes back, or a
  `jmp` up to 16 bytes back, has one turn of its loop tried on a copy of the registers
  (`idle_loop`, `idle_step` in `nes6502.c`): when the loop only looks (loads, compares, tests,
  branches) and comes round to the registers and flags it began with, the turns left in the
  slice are counted, not run. The status counts as the same only until the cycle sprite 0 is
  hit (`ppu_status_steady`).
- **A loop that counts** (`dex`, `dey`, `inx` or `iny` as the last before the `bne` that closes
  it, no other branch, nothing else of X or Y, no read of the status) is passed over the same
  way, as many turns as fit the slice (`idle_counting`), and slept in while its turns last: the
  delay a game makes after sprite 0. The count is made again from the register as the 6502 goes
  to sleep (`idle_count_from`).
- **A slice that ends inside such a loop leaves the 6502 asleep** (`nes6502_idle`): the slices
  after it are not entered at all, only counted from the cycles of the loop's instructions,
  until an interrupt, cycles to burn or a change of the status wakes it; the registers are then
  brought to where the count has got to by doing the instructions between to them once more.
- **Quiet lines are one sum** (`ppu_quiet_lines`, `quiet_tables`, `quiet_run` in `nes_ppu.c`).
  Lines with nothing for the PPU to find (sprite 0 not in the line, no frame interrupt due, no
  mapper counting lines) pass as a table looked up: where a slice of the sleeping 6502 ends is
  the first instruction's end at or after the slice's, by where in a turn of the loop that is.
  A line's cycles and what is left of them are 131072ths (a float of their size has no finer),
  so many lines are done at once in whole numbers, which are the floats' own sums, with the walk
  down the name tables worked out and not stepped. Line 240 and the lines of the blanking (242
  to 260) pass so too; the sound's quarters are called as the loop called them, one a line.
- **A background tile's row is two words looked up**, not eight bytes each through the
  palette (`bg_words` in `nes_ppu.c`). A line that starts inside a tile is drawn beside the
  picture and copied in: as it came, such a line wrote up to seven pixels before its start.
- **The GPU draws the picture** (`gd_tables`, `gd_follow`, `gd_draw` in `osd_shaderemu.c`;
  `docs/machine/gpu.md`, "A layer of tiles"), unless `NES_PPU` says workers, inline or here. The
  background is a layer of tiles whose cells are the four name tables two by two (and, under
  them, each pair's rows 30 and 31, the attribute bytes a game shows when it scrolls up past the
  top); the sprites are another, a cell or two a sprite; the pattern tables are kept a byte a
  pixel in GPU memory, in pieces of a page so that a cartridge may turn them; a palette is a
  bank of 16 for each of the NES's eight of four. The record of a frame is runs of lines: a new
  run only where the game wrote a register of the PPU, so a frame is a few quads a run (the
  colour behind, the sprites behind the background, the background, the sprites in front) and a
  game that scrolls every line is 240 short runs. The cells follow what the game writes to the
  name tables (a log of the addresses), the tiles what it writes to pattern RAM. Every frame is
  drawn, but one like the last is not (twice a second all the same). Eight sprites a line are
  kept to, as the PPU does. What differs from nofrendo's own drawing: a sprite in front is not
  hidden by an earlier sprite that is behind the background, and a write to the name tables in
  mid-frame shows in the whole frame.
- **The background's cells are kept by the memory they are of** (`gd_tables`, `gd_follow`): the
  PPU marks the bytes of its own four kilobytes of name tables a game writes, a bit each, and the
  cells are worked out once for that memory; a table of the picture that moves to other memory
  is copied a row at a time. A cartridge that sets its tables at every write to their register
  with nothing changed costs nothing for it. A written byte's cell is made once, a row with more
  than eight whole (`gd_cells_written`); `gd_tile` goes by a table of whole bytes.
- **The picture by the worker cores** is `NES_PPU=workers` (`frame_elsewhere`, `ppu_capture`,
  `ppu_draw`). The PPU keeps a record: its memory as the frame began (the name tables and pattern
  RAM copied only when written since) and each line's registers. The workers draw the lines from
  the record, each a share of them, into a texture not shown, and the window turns to it when
  they are done. Sprite 0's hit stays the emulating core's to find, and a write to the name
  tables in mid-frame shows a frame late. A cartridge whose pattern pages turn as they are drawn
  (MMC2) is drawn by the first core. It asks for twelve workers of size 5 (`NES_WORKERS`), in
  teams, a frame a team (`NES_TEAMS`): a frame given out and heard done costs two passes
  whoever draws, and what bounds the pictures shown is the machine's instructions a second on all
  its cores together, not the number of cores.
- **The 6502 is recompiled on the machine** (`rc6502.h`): the cartridge's code is translated
  to RISC-V instructions when it is first reached, with the 6502's registers and flags in
  registers of the machine, and nofrendo's interpreter runs only what is not translated (`brk`,
  the undocumented, and `jmp ()` through a pointer outside RAM). A translated instruction does
  what the interpreter's macro for it does and takes the same cycles, and after each the cycles
  left are tested, so a slice ends at the instruction the interpreter's would. A read or write of
  a device goes to nofrendo's own handler at the cycle count the interpreter would have. A jump
  is straight to its target's code inside a 4 KB page, or anywhere when the cartridge turns no
  pages; else it looks the target up as the pages are now. New code is fetched after `fence.i`,
  which ends the machine's pass.
- **A `jsr`'s routine is translated when it is first called** (`rc_later` in `rc6502.h`): the
  `jsr` leaves, and is made a jump when its routine is there. `NES_RC=eager` translates everything
  jumps reach at once.
- **A straight run of translated instructions is there twice** (`rc_run_length`, `rc_leaders`
  in `rc6502.h`): the cycles left tested after every instruction, and a copy with no test in it,
  entered when the cycles left cover the whole run (one test at its top). A run begins wherever
  a jump comes to, so a loop's body is a run from its top, and the branch, `jmp`, `jsr` or `rts`
  that ends it is in the copy and takes the run's cycles with its own. Reads of the cartridge are
  a load through the page table in line, and a write to a register of the PPU, the sound or the
  pads does not leave translated code. `NES_RC=plain` is without the copies and the next step, to
  compare. A read that may be a device's is in no unchecked copy of a run: the slice may be cut
  there, so the cycles left must be tested after it.
- **A slice that ends in translated code goes on into the next line from there** (`rc_line`, and
  `ppu_line_between` in `nes_ppu.c`): what the frame's loop does between two ordinary lines is
  done from inside, and the code never leaves. Not at the line the picture ends, the two the
  blanking begins and ends at, or when an interrupt waits that the 6502 would take. The line that
  needs nothing called (no run of a record to begin, sprite 0 not on it, no cartridge counting
  lines) is done by a function that calls nothing (`ppu_line_plain`), which `-flto` puts into
  `rc_line` whole.
- **C and V in a run's copy** are worked out only where they are read before they are set again
  (`rc_flags`), as N and Z are; `adc` after `clc` and `sbc` after `sec` add no carry; a
  comparison's C is one instruction (the difference against -1, which a register keeps); `inx`,
  `iny`, `dex`, `dey`, `asl a` and `lsr a` several times over are done once.
- **`rts` goes by a table of the 6502's addresses** when the cartridge turns no pages
  (`rc_rt_direct`). An `rts` that keeps where it went last does not pay: it slows a cartridge
  whose returns go to many places.
- **`rti` is translated**, a routine's (`rc_rti`): the interpreter's only when it has the
  registers or the flags it restores let a waiting interrupt in. **Cycles to burn are burnt in
  the slice** (`rc_slice`), the four lines and a half of a copy of the sprites among them, with
  the lines stepped from there.
- **`php`, `plp`, `sei` and `cli` are translated** (`rc_flag_op`): i, d and b are `cpu.p_reg`'s
  while translated code runs, the interpreter's too (`RC_RUN` puts its own there and takes them
  back). A `cli` that lets a waiting interrupt in leaves after itself; one in the interpreter's
  slice leaves before itself, and the interpreter's `cli` takes it. A `plp` that lets one in cuts
  lines ahead at its line, as the interpreter's slice of one line would. An instruction of these
  is never entered from the interpreter (as `rti`).
- **The machine's own registers are called without the search** (`rc_slow_read`, `rc_slow_write`:
  they are the first handlers of Nofrendo's list whatever a cartridge adds). The cartridge's
  memory at 0x6000 is read and written in place by `lda`, `sta` and their kind at an address (and
  indexed within a page), with no routine, when no handler is there. **The PPU's data register and
  the pads have routines of their own** (`rc_vdata`, `rc_pad`); a pad's sources are a list of
  their own (`nesinput.c`).
- **The sound's four quarters** work a channel's voice out only when its period, level or wave is
  another than last time (`as_seen` in `apu_shaderemu.c`).
- **The GPU's list is a worker core's to make** (`gd_draw` as a job, `gd_show`), with two records,
  the worker's and the one the PPU writes. The first core shows the list when it finds it made. A
  frame whose turn comes while the worker is still at the one before is not shown (`nesgpu:`
  counts them). `NES_LIST=inline` is without the worker; a test (`NES_FRAMES`, `NES_FAST`) has it
  only with `NES_LIST=worker`. The sprites are written straight into the list (`seglSpriteSpace`,
  `seglSpritesUsed`).
- **A list is shown without waiting for it** (`seglSwapEarly`, `docs/machine/gpu.md`; `NES_SWAP=wait`
  waits). What the list reads and is written again: the cells and tiles, which `frame_gpu` settles
  for before it follows the game's writes at a frame's end, and the sets and sprite cells, of which
  there are two pairs, a list made with the pair the list on show was not made with. The workers'
  pictures (`NES_PPU=workers`) are shown the same way: no team is given the picture on show. Only
  nofrendo's own drawing (`NES_PPU=here`) waits, since the next frame is copied into the picture
  the list reads.
- **The pad is asked for when something moved.** The machine's own input words
  (`docs/machine/input.md`: the pointer, the buttons, the key events so far) say when there can be
  any: the window system is asked when they have moved, eight times after that, and twice a second
  (`input_due`).
- **Start**: Nofrendo's palette is a table for the hue and tint it starts with; the cartridge's
  file is mapped; the translator's tables are mapped, not cleared.

Cartridges a visitor brings differ a lot, and none but the two games is in the image. What is slow
in them:

- Donkey Kong, Ice Climber and their kind wait by turning their random numbers, a `jsr` and eight
  `ror` a turn: work all frame and a line's step at every line. Nothing sleeps. Duck Hunt's main
  loop does the same.
- Castlevania reads the PPU's status in a loop that is not one that only looks, and a cartridge's
  menu may pass time in `lda $2002; inx; bne`: each read is the device's handler from translated
  code.
- Battletoads writes pattern RAM and the PPU's registers all frame.
- Punch-Out!! (MMC2) turns pattern pages as they are drawn, so the first core draws it.

Not done: a read of the PPU's status from translated code without the handler, when it changes
nothing; a second pad, saved games; translation on a worker (two cores would write one table and
one block of code).

### Lines ahead

**A slice of the 6502 is many lines** (`lines_ahead`, `ppu_lines_settle` in `nes_ppu.c`; `rc_settle`
in `rc6502.h`). When a line begins from inside translated code and the lines after it have nothing
in them (no record's run to begin, sprite 0 not before them, no cartridge counting, the frame
interrupt far off, the picture's end or the blanking's not reached), the slice is given the cycles
of them all. A line's cycles and what is left of them are 131072ths, so where each of those lines
would have ended is known without the 6502 stopping there: the first instruction's end at or after
the line's cycles, whatever the lines before came to. They are counted when something looks: a read
or write of a register that is not a pad's or a sound channel's, a loop tried for idleness,
translated code left, or the slice's end. The lines that have passed are then done at once (the walk
down the name tables worked out) and the slice is cut to the line it is in, so what looks finds the
line and the cycles it would have. `NES_LINES=each` is a line a slice.

A game that looks at the PPU in every line gains nothing by it: a slice cut in its first line gives
the next four lines one by one (`ahead_rest`), which limits what it loses. A write's cut of a slice
does not do that.

**A cartridge of our own** (`linux/nes/exact_rom.py OUT.nes [0 to 4]`, to be put in an image as
`/usr/share/nes-own0.nes` and run as `nes own0`): its program never waits long, reads the PPU's
status through a pointer and across a page at every cycle of a line, pushes its flags every turn,
and folds what it reads into RAM, so a cycle or a line out of place is another RAM sum. 600 frames
of each of its five forms must give one sum in every mode of "Testing". It does not read the data
register: Nofrendo reads memory it does not have when the address there is what a picture left.

### A cartridge that counts lines

MMC3 (`ppu_lines.hblank`): its count is made from inside, and when it asks for an interrupt the
translated code is left with the next line begun and the interpreter takes it first (`rc_leave`). An
interrupt asked for while translated code has the registers always waits so (`rc_running` in
`nes6502_irq`).

Its lines come ahead too (`hblank_quiet`, `hblank_skip` in the mapper's interface; `map4_quiet` and
`map4_skip` for MMC3). Its count is known: the lines before the one it asks for an interrupt in only
count. So they are given at once as other cartridges' are, asleep (`ppu_quiet_lines`,
`ppu_quiet_counted`) or awake (`lines_ahead`), and counted when they are settled; the line it asks in
goes the slow way.

A mapper's write goes on in translated code when it turned no page of the program and asked nothing
of the slice (`nes6502_turns` counts the times pages are set to others; `rc_mapper_write` from `sta
$8000` and up, the handler kept between writes, `rc_mapper`). `mmc_bankrom` sets its pages one by one
(`nes6502_setpage`).

### The emulation on a worker core

The first core does not emulate. A worker runs the frames, another makes the GPU's lists from them,
and the first core shows the lists, reads the window's events for the pad and helps the two on at a
page or a system call (`run_elsewhere`, `emulate`, `lists_job`, `lists_step` in `osd_shaderemu.c`).
`NES_EMU=here` is the emulation on the first core; `NES_EMU=lists-here` makes the lists there, the
fewest instructions in all.

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
  the list its copy last went into is drawn: `nesgpu` counts the others as not shown.
  `NES_CELLS_CHECK=1` works every copy out whole as it is given out and must say `0 of N frames
  differ`.
- **Its lines are `mcw_print()`'s**, written when the call ends, or at once for `nesmark` and
  each `nesstat`, by a system call the first core makes (`mcw_print_now()`). The clock in `nesstat`
  is the machine's word, not a system call.
- **The pad**: the first core counts each key's presses and keeps those down; the emulating
  worker presses them every fourth frame, as `osd_getinput` did. A test's presses (`NES_PRESS`)
  are the worker's, at the same frames.
- **Pages**: the first core reads the cartridge and writes the translator's tables and the
  first 256 KB of its code before the worker starts (`nes6502_recompile_touch` in `rc6502.h`). The
  translator's list of routines to come back to is in its tables, not `realloc`ed (no `malloc` on
  two cores).
- **The sound's recordings** go to `mcw_call()` too (`apu_shaderemu.c`); on the emulating
  worker a recording is decoded there and then.
- **Without a sound card the voices are not worked out** (`show` in `apu_shaderemu.c`): the
  channels are followed all the same.
- **A line with more than eight sprites is found by counts** of the sprites that begin and end on
  each line, and only such a line is walked in their order (`gd_draw`).

What is left of the first core's work is its looking on: a first core with nothing to do still costs
the rest (`mcw_rest()`) and the kernel's own every pass. It can neither sleep longer nor end its pass
without the kernel.

### A NES's speed in the world

The emulation on a worker makes the first core idle, not the game faster: the frames are as many
instructions on the worker as on the first core (less the kernel's), and a worker runs no more of
them a second. What the world shows is the emulating core's instructions a frame against the
instructions a core runs a second. Measure so under `rvc_cpu`: every pass a round of 8,192 at 3 MIPS a
core, whatever it ran (`--ticks 8192 --ips 1e12 --pass-ms 2.7307`, `docs/machine/cpu-harness.md`),
`NES_LIST=worker NES_FRAMES=N` as fast as it goes, and `nesstat`'s percentage, which is frames over the
machine's clock, over the frames of play.

The first core's readout: `speed_caption` in `osd_shaderemu.c`, from the frames the emulating core
says it has made (`emu_said.frames`) and the machine's clock; its variables are 16 bytes of their own.

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
  few stores to the voices' words.
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

`tools/test_all.py`'s `nes-*` cases hold the sums of this section, with the lists on the first core
and on a worker; `nes-cells` and `nes-kill` the checks named below.

- `NES_FRAMES=N` ends after N frames with `nes: done, picture SUM`; `NES_PRESS="20 start
  80 a"` presses buttons at frames, so a run repeats (`+right` holds a button down from its frame and
  `-right` lets it go); `NES_SUM=1` adds a sum of the 6502's RAM after every frame, which is the one
  to trust: the last picture can stay the same through a mistake that the RAM's sum shows.
- The sums a change to the emulator must leave, with `NES_SUM=1 NES_PPU=inline`:
  `NES_FRAMES=120 nes thwaite` RAM `31a18295`, picture `afcb4a98`; with `NES_FRAMES=180
  NES_PRESS="20 start 50 start 80 a 100 left 110 a"` RAM `2f58587d`, picture `39c90e07`;
  `NES_FRAMES=120 nes nova` RAM `3a2e5c47`, picture `d8a04406`.
- Short runs are not enough by themselves. The 1,000-frame runs: `NES_FRAMES=1000 NES_PRESS="20 start
  50 start 80 a 100 left 110 a 200 a 210 +right 300 a 400 a 420 +up 500 a 800 a 900 -right 1000 a
  1100 +left 1300 a" nes thwaite`, RAM `bf00e7da`; Nova with `NES_PRESS="60 start 120 start 180 start
  240 start 300 a 330 +right 600 a 700 a 800 a 900 +a 1100 -a 1200 a 1400 +left"`, RAM `fca83c4e`.
  What the GPU is given can be compared under `rvc_cpu` without a picture: at a hold, sum the list
  (address and count at 0x81000300), both sets (0x81700000 to 0x817C0000), texture memory to its top,
  and the palette.
- `NES_MARK=N` prints `nesmark` at frame N: `rvc_cpu --pc-log FILE --pc-from nesmark --ticks 256
  --pass-ms 0.0078` then samples the first core from there (which emulates only with
  `NES_EMU=here`), for `tools\pc_profile.py` with `nes.nm` (the pass's milliseconds go down with its
  instructions, or the kernel's timer is a quarter of the profile).
- Each of these must give the same: `NES_RC=plain`, `NES_RC=eager`, `NES_LINES=each` (a slice of the
  6502 is one line), `NES_LIST=worker`, `NES_CPU=interp` (no recompiler), `NES_PLAIN=1` (every turn of
  an idle loop run), `NES_PLAIN=2` (loops passed over, nothing asleep), `NES_PLAIN=3` (asleep, no
  quiet lines), `NES_PPU=workers`, `NES_PPU=inline`, `NES_EMU=here` (the emulation on the first core),
  `NES_EMU=lists-here` (the lists on the first core); `NES_CELLS_CHECK=1` must say `0 of N frames
  differ`. `NES_PPU=here` is nofrendo's own drawing, a frame in three: its frames are counted
  otherwise, so its RAM sums are its own (`b6f2e9ee` for the first run above), the same with each
  switch.
- All of that is instructions, so `rvc_cpu` does it (`docs/machine/cpu-harness.md`), workers and
  all. Speed in real seconds and pictures are `rvc_harness`'s; a game's speed without the card is
  `game_par.py nes --calibrated`.
- `RVC_RACES` over both 1,000-frame runs: 0 undone (`--races`).
- The GPU's drawing is checked at a held frame, where the first core draws the same record too: the
  window against that picture (`NES_HOLD`, below), and the GPU's target against its software model
  (`--gpu-capture`, `tools\gpu_reference.py SNAP BMP --size 512x480`: every pixel away from a
  triangle's edge the same). A sprite in front that nofrendo hides (above) differs.
- `NES_SWAP=wait` waits for every list. `NES_HOLD_LATE=1` with `NES_HOLD=N` leaves frame N on the
  window while frame N + 1 is emulated and its writes followed, and stops then: with `--gpu-capture`,
  once with `NES_SWAP=wait` and once without, the two pictures must be the same. A plain hold stops
  before the next frame writes anything, and cannot show a store made too early.
- `NES_HOLD=N` stops at frame N with the window up and prints where the picture is in GPU memory
  (bytes past the palette at 0x81000400): in a snapshot from `rvc_harness`, the window's layer must
  be that picture through the palette, each pixel twice, and the workers' picture the same as
  `NES_PPU=inline`'s, with no pixel different.
- The cartridge of our own (`exact_rom.py`, "Lines ahead" above) must give one sum for each of its
  five forms in every mode.
- `mctest 3 120000` must print `PASS` before and after `nes` in one boot, and after one
  killed while its worker made lists (`nes nova & sleep 8; killall -9 nes.bin`).
- A key let go between two of the emulator's looks at the keyboard (every fourth frame) stays down
  until the next: a tap would otherwise never reach the game.
