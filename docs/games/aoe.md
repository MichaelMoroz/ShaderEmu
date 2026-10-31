# Age of Empires, the 1997 trial

The decompiled game (`github.com/dcostap/aoe1-decomp`, pinned at `6d6d8b1`; the plan read `f4fdf3f`) over a Win32 and DirectDraw layer of our own, with the data of the trial (`docs/plans/aoe-plan.md`). It starts on the desktop and plays. It has no sound and uses no worker cores.

**No game data in the repository.** The data is under the trial's EULA, and the code is recovered from Microsoft's binary. Nothing of the game or the trial is committed: only the port, the patch and the tools. The program is not in `ALL` in `linux/nommu/build.sh` or in `linux/prebuilt`. The trial's `Empires.exe` is not read at all. The game's own `verify_cd` is set to 0, because the trial has no CD.

## Making it

The game goes into an image only when asked:

    pwsh tools\make_image.ps1 --aoe                                  # or AOE_IMAGE=1
    python linux/nommu/image.py NAME ROOT KERNEL --aoe

**Turning this on is accepting the trial's EULA**, the licence that came with Microsoft's 1997 trial of Age of Empires: read it (`trial/eula.txt` in the cache) before you use the game, and do not ask for it if you do not accept it. The source is `archive.org/details/aoe-trial-1997` (`MSAoE.exe`, 24,786,864 bytes). That item states no licence of its own. The EULA allows copies of the whole trial, with the EULA, not for commercial purposes, and it limits reverse engineering and decompiling as far as the law allows: the decompiled code comes from the upstream project and is fetched, not kept here. An image with the game holds parts of the trial, so it is for your own use and is not to be passed on or sold.

`linux/aoe/data.py OVERLAY` then does this:

1. It fetches `MSAoE.exe` into the cache (`AOE_CACHE`, by default `~/.cache/shaderx86/aoe`), and refuses it unless its md5 and sha256 are those in the script.
2. It unpacks it with 7-Zip and makes the data (`make_data.py`: `language.txt`, `aoe-fonts.bin`, and the files copied flat). A mark says which scripts made it, and a cache with the same mark is used as it is, with no network.
3. It builds the program (`program.sh`, into `build/aoe/out`; `AOE_WSL_HOME` is a private home, `AOE_PROGRAM` a folder already made).
4. It puts the data, `aoe.bin`, `aoe` and the Start menu's line into that image's overlay, and never into the root.

If any step fails it says which, the image goes on without the game, and `test_all.py aoe` prints `AOE-NO-DATA` and passes. A plain build runs none of this and is byte for byte as before. `--save-prebuilt` turns the switch off, and what it saves comes from the root, so neither the data nor the game can be saved.

The program alone:

    export HOME=~/agents/NAME PYTHONUSERBASE=/home/micha/.local      # a private home: building-the-system
    AOE_OUT=DIR bash linux/nommu/build.sh aoe/build.sh               # fetches the pinned tree, applies aoe.patch
    python3 linux/nommu/flat.py DIR

`aoe-q.elf` is the same game for `qemu-riscv32`, with `AOE_NOWINDOW=1` and `AOE_DATA`.

## What the trial is

- Its `Empires.dat` says `VER 3.4`, and the game's 3.7 reader reads it to the exact byte (758,114). Only the version check changed, so no converter is needed: `dat34.py` copies.
- 12 civilizations and Gaia, and 5 random maps; a type past the last is map 0.
- Full pictures only for 800 x 600, so that is the only mode. On an 800 x 600 desktop the window has a frame like any program's, with its sides off the screen (`docs/system/nanox.md`), and it is kept above the bar, so the game's picture is whole.
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

`Visible_Unit_Manager::Update_Unit_Info` took the place of `VUR_Ptrs` in an object as `0x194`, the Windows build's. Here it is `0x190`, so a list that grew left stale pointers, and `RGE_Combat_Object::teleport` wrote tile coordinates through them into freed memory. It showed after about 700 updates as a `TDrawArea::CheckSurface` crash on a surface like `0x707359c0`. Found by a write-protected page under qemu, as qemu's gdb stub in user mode has no watchpoints. `obj + 0x0C` in `taiinfmd.cpp` went the same way. `(char*) this + 0xFF90` and the map's sizes there are by name now, and the sums did not move: they had been right.

## Closing

`python tools/app_fuzz.py plan "start:a:aoe wait:a:120 close:a:x end" --image build/images/NAME` starts it and closes it by its frame. While it loads the close takes time to be taken (25 s in that plan). In a game a close brings up the game's own question: Yes ends the program (exit 0), and No goes back to the game and a later close asks again (`AOE_SCRIPT` with `close`, under qemu and on the machine). `aoe` is not in `app_fuzz.py`'s list because a plain image does not have it.

The cursor is the game's own, drawn into its picture, and it is in captures from the script and from `nxkey`. Whether a person sees a second one, the window system's, over it was not looked at on a live screen.

## Not done

- Sound and music: `DirectSoundCreate` fails and the game runs silent. `docs/machine/sound.md` is the way.
- Workers: none, so `RVC_RACES` has nothing to say yet. The plan's next gains are the drawing and the start.
- A timing on the card; `linux/nommu/memory.py`.
- Saving, multiplayer (DirectPlay stubs) and scenarios were not tried. Computer players gather and build over 5 game minutes; a whole game was not played.
