# A NES

An emulator inside the emulated machine: Nofrendo (Matthew Conte's, as Espressif keep it for
the ESP32) as a Nano-X client, with two games anyone may pass on. On the four-core machine it
runs at a NES's speed or near it; on one core at a third.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh     (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/nes/build.sh       (2 s; the first time it also fetches)
    python tools\make_linux_image.py                                  (then a new shell snapshot)

In the guest: `nano-X -p & nes thwaite` or `nes nova` (the Start menu's Games has both), or
`nes FILE.nes` for any other cartridge of a mapper Nofrendo has. Arrows are the pad, X is A,
Z is B, Enter is Start, Tab is Select and Escape ends. There is no sound yet.

The games are Thwaite by Damian Yerrick (mapper 0) and Nova the Squirrel by NovaSquirrel
(mapper 1), both GPL-3; the build fetches their release files.

## What the build is made of (`linux/nes`)

- `build.sh` fetches esp32-nesemu at a fixed commit, applies `nofrendo.patch` and compiles
  Nofrendo with the files below, `programs/linux/gles.c` and `programs/mc/mcw.c`.
- `osd_shaderemu.c` is the machine's side: the window, the pad, the frame and its pace. The
  NES's picture is 256 x 240 bytes of GPU memory, each a place in the display's palette, which
  holds the NES's colours; the GPU stretches it over the window.
- `rc6502.h` is the 6502 recompiled (below); nofrendo's `nes6502.c` includes it.
- `nofrendo.patch` is ours (edit a clean clone at the commit `build.sh` names and save
  `git diff`); the build puts the tree back each time.

## Speed

Instructions of the first core for a frame of the NES (`nesstat:` lines), and the speed that
is of a NES's 60 frames a second at about 4M instructions a second:

| | Thwaite's title | Thwaite, a game | Nova's title |
|---|---|---|---|
| Nofrendo as it came, one core, a frame in three drawn | 704k (9%) | | |
| idle loops counted, the background by table | 292k (22%) | 380k (17%) | 274k (24%) |
| the picture by three worker cores | 162k (39%) | 185k to 215k (26 to 31%) | 153k (42%) |
| the 6502 recompiled, asleep in its idle loops | 49k (116%) | 70k (80%) | 31k (172%) |
| the GPU draws: every frame that differs is shown | 51k (134%) | 87k (80%) | 34k (212%) |

With the limiter on, the two titles hold 60 frames in 1,000 ms and a Thwaite game 60 in
1,035 to 1,070 ms with 30 of them drawn. The GPU needs no worker cores, so this is what
one core does too. Nofrendo's own drawing on one core (`NES_PPU=here`, a frame in three)
is 205k to 215k, a third of a NES. A game is held to 60 frames a second (`frame_paced`; a test with
`NES_FRAMES`, or `NES_FAST=1`, runs as fast as it goes).

What each step is:

- **A game waits in a loop**, most of every frame: for its interrupt (`cmp zp; beq`), or for
  the PPU's status to say the picture has reached sprite 0 (`bit $2002; bmi; bvc`). A taken
  branch a few bytes back has one turn of its loop tried on a copy of the registers
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
- **The picture is drawn by the worker cores** (`docs/multicore.md`; `frame_elsewhere` in
  `osd_shaderemu.c`, `ppu_capture` and `ppu_draw` in `nes_ppu.c`). The first core draws
  nothing: it emulates every frame and, for the frames the workers are free for, the PPU
  keeps a record: its memory as the frame began (the name tables and pattern RAM copied only
  when written since) and each line's registers. The workers draw the lines from the
  record, each a share of them, into a texture not shown, and the window turns to it when
  they are done. Sprite 0's hit stays the first core's to find (as nofrendo does for a frame
  it skips). What is lost: a write to the name tables in mid-frame shows a frame late.
  A cartridge whose pattern pages turn as they are drawn (MMC2) is drawn by the first core.
- **As many workers as fit, in teams.** The NES asks the machine for twelve workers of size 5
  (`mcw_shape`, `docs/multicore.md`: 3 KB of new stores a pass, which holds the eight or so
  lines one draws in a pass; `NES_SHAPE`, `NES_WORKERS` for others), and takes the workers
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
- **The GPU draws the picture** (`gd_...` in `osd_shaderemu.c`; `docs/gpu.md`, "A layer of
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
  and one in two to four when the game is behind its time. Eight sprites a line are kept to,
  as the PPU does. What differs from nofrendo's own drawing: a sprite in front is not hidden
  by an earlier sprite that is behind the background (8 pixels of a Thwaite game frame),
  and a write to the name tables in mid-frame shows in the whole frame.
  A Thwaite game frame is 30 quads and 14 thousand instructions (`nesgpu:` lines say).
- **The 6502 is recompiled on the machine** (`rc6502.h`): the cartridge's code is translated
  to RISC-V instructions when it is first reached (everything jumps reach from there:
  Thwaite's 3,950 instructions in one go, 140 KB of code), with the 6502's registers and
  flags in registers of the machine, and nofrendo's interpreter runs only what is not
  translated (the flags i and d, `php`, `plp`, `brk`, `rti`, `jmp ()`, the undocumented).
  A translated instruction does what the interpreter's macro for it does and takes the same
  cycles, and after each the cycles left are tested, so a slice ends at the instruction the
  interpreter's would. A read or write of a device goes to nofrendo's own handler at the
  cycle count the interpreter would have; a write that is not RAM leaves translated code
  after its instruction (it may have turned pages or ended the slice). A jump is straight
  to its target's code inside a 4 KB page, or anywhere when the cartridge turns no pages;
  else it looks the target up as the pages are now. New code is fetched after `fence.i`,
  which ends the machine's pass.

Where the first core's 70k of a Thwaite game frame go (measured at 85k, before the last cuts): the quiet lines 17%, the interpreter
for what is not translated and the way in and out of translated code 15%, the lines' records
9%, translated code 10%, the kernel 13%.

Not done: sound, a second pad, saved games; the first core's own drawing (one core) from
the record, which would let it skip more frames.

## Testing

- `NES_FRAMES=N` ends after N frames with `nes: done, picture SUM`; `NES_PRESS="20 start
  80 a"` presses buttons at frames, so a run repeats; `NES_SUM=1` adds a sum of the 6502's
  RAM after every frame, which is the one to trust: the last picture was the same through a
  mistake that the RAM's sum showed (a loop taken for idle at sight: a branch can be come
  to from elsewhere than the instruction before it).
- The sums a change to the emulator must leave, with `NES_SUM=1 NES_PPU=inline`:
  `NES_FRAMES=120 nes thwaite` RAM `31a18295`, picture `afcb4a98`; with `NES_FRAMES=180
  NES_PRESS="20 start 50 start 80 a 100 left 110 a"` RAM `2f58587d`, picture `39c90e07`;
  `NES_FRAMES=120 nes nova` RAM `3a2e5c47`, picture `d8a04406`.
- Each of these must give the same: `NES_CPU=interp` (no recompiler), `NES_PLAIN=1` (every
  turn of an idle loop run), `NES_PLAIN=2` (loops passed over, nothing asleep), `NES_PLAIN=3`
  (asleep, no quiet lines), `NES_PPU=workers`, `NES_PPU=inline`. `NES_PPU=here` is nofrendo's own drawing, a frame in three: its
  frames are counted otherwise, so it has sums of its own (`b6f2e9ee`, `451f1f1d`,
  `70810528`), the same with each switch.
- All of that is instructions, so `rvc_cpu` does it (`docs/cpu-harness.md`), workers and
  all: the three runs in two seconds, with the same instructions a frame as `rvc_harness`.
  Speed in real seconds and pictures are `rvc_harness`'s.
- The GPU's drawing is checked at a held frame, where the first core draws the same record
  too: the window against that picture (`NES_HOLD`, below: no pixel different in the two
  titles and in Nova scrolled; the 8 above in a Thwaite game), and the GPU's target against
  its software model (`--gpu-capture`, `tools\gpu_reference.py SNAP BMP --size 512x480`:
  every pixel away from a triangle's edge the same).
- `NES_HOLD=N` stops at frame N with the window up and prints where the picture is in GPU
  memory (bytes past the palette at 0x87000400): in a snapshot from `rvc_harness`,
  the window's layer must be that picture through the palette, each pixel twice, and the
  workers' picture the same as `NES_PPU=inline`'s, with no pixel different.
- `mctest 3 120000` must print `PASS` before and after `nes` in one boot.
- A key let go between two of the emulator's looks at the keyboard (every fourth frame)
  stays down until the next: a tap would otherwise never reach the game.
