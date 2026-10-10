# A NES

An emulator inside the emulated machine: Nofrendo (Matthew Conte's, as Espressif keep it for
the ESP32) as a Nano-X client, with two games anyone may pass on. It is slow, a fifth to a
quarter of a NES's speed, because both layers are interpreters: about 40 of the machine's
instructions go to one of the 6502's, and the machine runs 4M a second.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh     (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/nes/build.sh       (2 s; the first time it also fetches)
    python tools\make_linux_image.py                                  (then a new shell snapshot)

In the guest: `nano-X -p & nes thwaite` or `nes nova` (the Start menu's Games has both), or
`nes FILE.nes` for any other cartridge of a mapper Nofrendo has. Arrows are the pad, X is A,
Z is B, Enter is Start, Tab is Select and Escape ends. There is no sound yet.

The games are Thwaite by Damian Yerrick (mapper 0) and Nova the Squirrel by NovaSquirrel
(mapper 1), both GPL-3; the build fetches their release files and the repository keeps neither.

## What the build is made of (`linux/nes`)

- `build.sh` fetches esp32-nesemu at a fixed commit, applies `nofrendo.patch` and compiles
  Nofrendo with `osd_shaderemu.c` and `programs/linux/gles.c`.
- `osd_shaderemu.c` is the machine's side: the window, the pad and the frame. The NES's
  picture is 256 x 240 bytes of GPU memory, each a place in the display's palette, which holds
  the NES's colours; the GPU stretches it over the window (two times, on an 800 x 600 screen).
  A drawn frame is copied there whole, so the window never shows half of one.
- `nofrendo.patch` is ours (edit a clean clone at the commit `build.sh` names and save
  `git diff`); the build puts the tree back each time. What it changes is below.

## Speed

Instructions of the machine for a frame of the NES (`nesstat:` lines; one frame in three drawn):

| | Thwaite's title | Thwaite, a game | Nova's title |
|---|---|---|---|
| Nofrendo as it came | 704k (9% of a NES) | | |
| the background by table, every turn of a loop run (`NES_PLAIN=1`) | | 894k | 700k |
| with the three changes | 292k (22%) | 380k to 400k (16 to 18%) | 274k to 324k (20 to 24%) |

- **A game waits in a loop**, most of every frame: for its interrupt (`cmp zp; beq`), or for
  the PPU's status to say the picture has reached sprite 0 (`bit $2002; bmi; bvc`). The 6502
  ran every turn, 80 instructions each. Now a taken branch a few bytes back has one turn of
  its loop tried without changing anything (`idle_loop` in `nes6502.c`): when the loop only
  looks (loads, compares, tests, branches) and comes round to the registers and flags it
  began with, the turns left in the slice are counted, not run. The cycle count is the same,
  so the game is too: `NES_PLAIN=1` runs every turn, and the picture's sum must not differ.
  The status counts as the same only until the cycle sprite 0 is hit (`ppu_status_steady`).
- **A background tile's row is two words looked up**, not eight bytes each through the
  palette (`bg_words` in `nes_ppu.c`: by the tile's palette and a nibble of each bit plane,
  made again when the colours change). A drawn frame went from 776k to 470k over an undrawn
  one. A line that starts inside a tile is drawn beside the picture and copied in: as it
  came, such a line wrote up to seven pixels before its start, over the end of the line above.
- **A frame in three is drawn** (`NES_SKIP=N`: one in N + 1). An undrawn frame is 122k, so
  a NES nobody watches would run at half speed, and that is the most this design can reach.

Where the rest goes, in a game of Thwaite: the background 27%, the 6502 25%, sprites 14%,
`idle_loop` itself 10% (it is asked 262 times a frame, once a scanline), the frame's copy 6%,
the kernel 8%.

Not done: the picture on a worker core (`docs/multicore.md`; the frames would then cost the
first core 122k each, drawn or not), sound, a second pad, saved games.

## Testing

- `NES_FRAMES=N` ends after N frames with `nes: done, picture SUM`; `NES_PRESS="150 start
  390 a"` presses buttons at frames, so a run repeats. Sums a change to the emulator must
  leave (one frame in three drawn): `nes thwaite` at 180 `afcb4a98`; with
  `NES_PRESS="150 start 270 start 390 a 450 left 480 a"` at 600 `fe0c8081`; `nes nova` at
  300 `d8a04406`.
- `NES_HOLD=N` stops at frame N with the window up and prints where the picture is in GPU
  memory (bytes past the palette at 0x87000400): in a snapshot, the window's layer must be
  that picture through the palette, each pixel twice, with no pixel different.
- A key let go between two of the emulator's looks at the keyboard (a fifth of a second
  apart) stays down until the next: a tap would otherwise never reach the game.
