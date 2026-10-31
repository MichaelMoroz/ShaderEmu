# Age of Empires, the 1997 trial

The decompiled game (`github.com/dcostap/aoe1-decomp`, pinned at `6d6d8b1`; the plan read `f4fdf3f`) over a Win32 and DirectDraw layer of our own, with the data of the trial (`docs/plans/aoe-plan.md`). It starts on the desktop and plays. It has no sound and uses no worker cores.

**Not ours to pass on.** The code is recovered from Microsoft's binary and the data is under the trial's EULA. The developer decides what may be done with them. Until then the program is not in `ALL` in `linux/nommu/build.sh` or in `linux/prebuilt`, and nothing of the game or the trial is committed: only the port, the patch and the tools. The trial's `Empires.exe` is not read at all. The game's own `verify_cd` is set to 0, because the trial has no CD.

## Making it

    export HOME=~/agents/NAME PYTHONUSERBASE=/home/micha/.local      # a private home: building-the-system
    AOE_OUT=DIR bash linux/nommu/build.sh aoe/build.sh               # fetches the pinned tree, applies aoe.patch
    python3 linux/nommu/flat.py DIR

The data: unpack `MSAoE.exe` (`archive.org/details/aoe-trial-1997`, md5 `dadea8f91c7e2b70ff5c2be7aad6c682`) with 7-Zip, run `linux/aoe/make_data.py` on it (`language.txt`, `aoe-fonts.bin`, and the files copied flat), and put the result in `/usr/share/aoe` of a copy of a flat root. Then `python linux/nommu/image.py NAME ROOT KERNEL`. `build.sh` does not fetch the trial. `aoe-q.elf` is the same game for `qemu-riscv32`, with `AOE_NOWINDOW=1` and `AOE_DATA`.

## What the trial is

- Its `Empires.dat` says `VER 3.4`, and the game's 3.7 reader reads it to the exact byte (758,114). Only the version check changed, so no converter is needed: `dat34.py` copies.
- 12 civilizations and Gaia, and 5 random maps; a type past the last is map 0.
- Full pictures only for 800 x 600, so that is the only mode. On an 800 x 600 desktop the window has no frame or it would not fit, and it covers the bar.
- No `BLKEDGE.DAT` (edges black) and no Roman button (`0xcf0d` is `0xc610`). Other `0xCF..` resources of the game screen are the Rise of Rome cultures' and have not been checked one by one.
- `data2\*.drs` are reported missing at each start. It is harmless.
- `rand()` is MSVC's (`aoe_rand`): the game divides by `0x7FFF`.

## Checks

| Switch | Does |
|---|---|
| `AOE_TICK_MS=33` | a clock that moves by what the game does: a run comes out the same |
| `AOE_SCRIPT="MS click X Y;MS rclick X Y;MS key K;MS char C;MS close;MS quit"` | each step MS after the screen last changed or the step before |
| `AOE_SUM_EVERY=N`, `AOE_SUM_END=N` | `aoesum:` a sum of the clock, the random state, players' attributes, objects' ids, hit points and places |
| `AOE_STATS_MS=N`, `AOE_PLAYERS=1`, `AOE_DEBUG=1` | `aoestat:`, players' resources, panels by time |
| `AOE_DUMP=FILE`, `AOE_DUMP_EVERY=N` | pictures under qemu |
| `AOE_BACK=copy` | the drawing surface apart from the page, as on Windows |

`test_all.py aoe --image build/images/NAME` plays the script below to update 1000. An image with no data prints `AOE-NO-DATA` and passes.

The script starts a Random Map game, sends a villager to a tree and trains a villager:

    3000 click 400 245;3000 click 400 172;3000 click 237 568;2000 click 369 118;1000 rclick 505 170;1000 click 370 215;1000 click 163 508

The sums compare the machine with qemu running the same source. No build of the original Windows game was compared, so they do not say it plays as Microsoft's does.

## What a frame costs

Measured on `rvc_cpu`, updates 50 to 300 of that script. One update is one frame.

| | instructions an update | passes in the span |
|---|---|---|
| first working build | 2.66 million | 93,058 |
| with the changes below | 1.26 million | 50,606 |

The pictures stayed the same bytes under qemu (23 pictures over 200 updates). The `whole area` row was made after that table's second line; it changes no picture either, but its saving was not measured alone.

- **Row index of big pictures.** The minimap redraws, the two 30-pixel buttons over it redraw, and each draws its piece of the 800-pixel background picture from the start of every row: about 1 million instructions a frame. `ASMDraw_Sprite` now keeps where each row stands every 64 columns (`row_index`).
- **The surface is the page.** The game draws on the GPU page and a blit to the screen is only a present, with `seglSettle` at `Lock`, `GetDC` and blits. It was 11% of a frame in copies.
- **A clip rectangle** is made in one go (`AddRect`), and the whole area's 600 lines only if something draws under it (`aoe_clip_whole`): a panel's end asks for them each time.

What is left: `ASMDraw_Sprite` 23%, clip spans 17% (before the deferral), terrain 8%, double-precision calls about 10% (`sincos` in `doTrivialMove`, `boundAngle`). The world's update was 290 thousand an update.

Speed on the calibrated clock (`--pass-model`, not checked on the card), at the start of a game with few units: 4.4 to 5.2 frames a second in steady stretches, and game time at about half of real time (0.48 to 0.52). An earlier build, with no rest when idle, held 6.2 frames and 0.62 further into a game; the two were not compared turn about, so the difference is not explained. A start reached the main menu at 17 s and the game screen at 138 s of the calibrated clock, about 9 s of that the script's waits. Most of it is loading the data.

## A bug to remember

`Visible_Unit_Manager::Update_Unit_Info` took the place of `VUR_Ptrs` in an object as `0x194`, the Windows build's. Here it is `0x190`, so a list that grew left stale pointers, and `RGE_Combat_Object::teleport` wrote tile coordinates through them into freed memory. It showed after about 700 updates as a `TDrawArea::CheckSurface` crash on a surface like `0x707359c0`. Found by a write-protected page under qemu, as qemu's gdb stub in user mode has no watchpoints. `obj + 0x0C` in `taiinfmd.cpp` went the same way. `(char*) this + 0xFF90` (the learn file's name) and `terrain + 8` there remain, unchecked.

## Not done

- Sound and music: `DirectSoundCreate` fails and the game runs silent. `docs/machine/sound.md` is the way.
- Workers: none, so `RVC_RACES` has nothing to say yet. The plan's next gains are the drawing and the start.
- A timing on the card; `linux/nommu/memory.py`.
- `tools/app_fuzz.py`: `start:a:aoe` starts it, a kill leaves no process and the memory comes back, but nxfuzz looks for framed windows and says `no-window`. A close request at the main menu ends the program (exit 0, no process, the GPU released). In a game it brings up the game's own question, which was not tried.
- Whether the game's own cursor shows under real pointer input: it is in pictures from the script and not in the one capture from `nxkey`.
- Saving, multiplayer (DirectPlay stubs) and scenarios were not tried. Computer players gather and build over 5 game minutes; a whole game was not played.
