# Age of Empires, the 1997 trial

The decompiled game (`github.com/dcostap/aoe1-decomp`, pinned at `6d6d8b1`; the plan read `f4fdf3f`) over a Win32 and DirectDraw layer of our own, with the data of the trial (`docs/plans/aoe-plan.md`). It opens as a window of 640 x 480 on the desktop and plays, with its sound effects and its music on the sound card and the map dragged by the right button ("Windowed", "Sound", "Music", "Controls"). It draws its view on a worker core, and at 800 x 600 played at 11 to 15 frames a second on the card ("Speed in play").

**No game data in the repository.** The data is under the trial's EULA, and the code is recovered from Microsoft's binary. Nothing of the game or the trial is committed: only the port, the patch and the tools. The program is not in `ALL` in `linux/nommu/build.sh` or in `linux/prebuilt`. The trial's `Empires.exe` is not read at all. The game's own `verify_cd` is set to 0, because the trial has no CD.

## Making it

The game goes into an image only when asked:

    pwsh tools\make_image.ps1 --aoe                                  # or AOE_IMAGE=1
    python linux/nommu/image.py NAME ROOT KERNEL --aoe

**Turning this on is accepting the trial's EULA**, the licence that came with Microsoft's 1997 trial of Age of Empires: read it (`trial/eula.txt` in the cache) before you use the game, and do not ask for it if you do not accept it. The source is `archive.org/details/aoe-trial-1997` (`MSAoE.exe`, 24,786,864 bytes). That item states no licence of its own. The EULA allows copies of the whole trial, with the EULA, not for commercial purposes, and it limits reverse engineering and decompiling as far as the law allows: the decompiled code comes from the upstream project and is fetched, not kept here. An image with the game holds parts of the trial, so it is for your own use and is not to be passed on or sold.

`linux/aoe/data.py OVERLAY` then does this:

1. It fetches `MSAoE.exe` into the cache (`AOE_CACHE`, by default `~/.cache/shaderemu/aoe`), and refuses it unless its md5 and sha256 are those in the script.
2. It unpacks it with 7-Zip and makes the data (`make_data.py`: `language.txt`, `aoe-fonts.bin`, and the files copied flat). A mark says which scripts made it, and a cache with the same mark is used as it is, with no network.
3. It builds the program (`program.sh`, into `build/aoe/out`; `AOE_WSL_HOME` is a private home, `AOE_PROGRAM` a folder already made).
4. It fetches the music's instruments into the same cache (`genmidi.op2`, DMXOPL's General MIDI bank, MIT, the commit OpenTTD fetches, against its checksum). If that fails the game goes in with no music.
5. It puts the data, the bank, `aoe.bin`, `aoe` and the Start menu's line into that image's overlay, and never into the root.

If any step fails it says which, the image goes on without the game, and `test_all.py aoe` prints `AOE-NO-DATA` and passes. A plain build runs none of this and is byte for byte as before. `--save-prebuilt` turns the switch off, and what it saves comes from the root, so neither the data nor the game can be saved.

The program alone:

    export HOME=~/agents/NAME PYTHONUSERBASE=/home/micha/.local      # a private home: building-the-system
    AOE_OUT=DIR bash linux/nommu/build.sh aoe/build.sh               # fetches the pinned tree, applies aoe.patch
    python3 linux/nommu/flat.py DIR

`aoe-q.elf` is the same game for `qemu-riscv32`, with `AOE_NOWINDOW=1` and `AOE_DATA`.

## What the trial is

- Its `Empires.dat` says `VER 3.4`, and the game's 3.7 reader reads it to the exact byte (758,114). Only the version check changed, so no converter is needed: `dat34.py` copies.
- 12 civilizations and Gaia, and 5 random maps; a type past the last is map 0.
- Full pictures only for 800 x 600: `0xC62D` to `0xC630`, the game screens for 640 x 480, and the menus' are not in `Interfac.drs` ("Windowed").
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
| `AOE_SIZE=800` | 800 x 600, above the bar: the way before; else the window of 640 x 480 ("Windowed") |
| `AOE_SCROLL=edge` | the map scrolls at the window's edges, and by the game's own right drag with nothing selected: the way before ("Controls") |
| `AOE_SOUND=0`, `AOE_SOUND_LOG=1` | no sound; `aoesnd:` each wave found, played and seen to end (`python tools/aoe_sound_check.py LOG IMAGE`, "Sound") |
| `AOE_MUSIC=0`, `AOE_MUSIC_LOG=1` or `2`, `AOE_MUSIC_CUT=S` | no music (`AOE_SOUND=0` has none either); `aoemus:` each song and what the card's words say, with 2 every note and instrument for `tools/aoe_music_check.py`; each song ends after S s ("Music") |
| `AOE_INPUT_LOG=1` | `aoeinput:` an order, a box and a drag, each with its places; the script's `view` step prints `aoeview:` where the view stands and how many are selected |

`test_all.py aoe --image build/images/NAME` plays the script below to update 1000 (at 800 x 600), `aoe-640` the same at the window's size, and `aoe-drag` the controls. `aoe-music` is `aoe-640` with the card's clock moving (`RVC_SOUND=1`), which those have not, and songs cut to 4 s. An image with no data prints `AOE-NO-DATA` and passes.

The script starts a Random Map game, sends a villager to a tree and trains a villager. At 640 x 480 the menus' places are times 0.8, the view's 80 and 60 less and the bottom panel's 120 up (`AOE_PLAY_640` in `tools/game_par.py`), and the sums are the same: `5032c153` at update 1000.

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

## Speed

The aim is 15 frames a second, and it is not reached. All of the game is on the first core. Measured on 31 October and 1 November 2026 with `tools/game_par.py` (`aoe`, `aoe-late`, `aoe-scroll`, on an image made with `--aoe`), one run a scene. Game time is the game's milliseconds over real ones.

| Scene | Clock | Frames a second | Game time | Core 0 a frame |
|---|---|---|---|---|
| `aoe`, updates 50 to 300, before the changes below | calibrated | 4.7 | 0.47 | 990k |
| `aoe`, after | calibrated | 6.0 | 0.60 | 782k |
| `aoe`, after | the card | 8.3 | 0.80 | 94.7 passes, 784 passes a second |
| `aoe-scroll`, before | calibrated | not comparable: it scrolled into black | | |
| `aoe-scroll`, after | calibrated | 5.6 | 0.52 | 890k |
| `aoe-late`, updates 4000 to 4300, after | calibrated | 8.0 | 0.80 | 579k |

The card ran 38% more frames than the calibrated clock in the one scene run on both, so that clock is not close for this game; the cause was not looked for. `aoe-late` is a game left to the computer players, and how many units it had was not counted, so it may not be the heavy game it is meant to be. It costs less than the early scene, which says it should be read with care.

At 784 passes a second, 15 frames a second is 52 passes a frame, about 350k instructions of core 0. Where the early scene's 782k go (`tools/prof.py`, before the clip and top bar changes, when it was 820k):

| Part | Instructions a frame |
|---|---|
| The world's update | 223k (Gaia's animals 99k, players 115k) |
| Walking the visible tiles and their objects | about 180k (the loop itself 103k, objects 55k, `get_tile_mask_num` 21k) |
| Drawing the view from its lists | about 95k |
| Panels: copies (`Blt`), the message panel, the minimap, text | the rest, about 280k |

What paid, with the same sums and the same 23 pictures under qemu:

- Under a button, the bottom and top pictures are copied from what was kept at the last whole drawing (`TRIBE_Screen_Game::draw`), not drawn through their rows: most of the 150k a frame it cost.
- A sine and cosine of an angle seen already are kept (`doTrivialMove`): an angle is one of an object's facets, and a pair was 4,200 instructions.
- `SetClipRect` with the rectangle the list already holds does nothing.

What did not pay: `-O3 -fsched-pressure -funroll-loops` (`AOE_OPT`) gave 0.5% fewer instructions for 38% more code, so it is not the default.

The way on, not tried, and these are estimates from the table. Drawing the view from its lists needs no game state (`view_function_terrain` from "Draw_Level_Head" on), so a worker could do it while the first core goes on, for about 95k. It wants its own copy of `aoe_asm.cpp`'s statics and its own copy of the `TDrawArea` for lines, and the cursor and panels over the view must wait for it. Copying the panels less is worth a few percent each. That alone leaves about 590k, or roughly 10 frames a second on the card. 15 also needs the tile walk cheaper or shared out, or the world on a worker. The world allocates and the panels do too, and `malloc` is one side at a time (`docs/machine/multicore.md`), so the world on a worker while panels draw wants an allocator of its own.

A start, on the card, reached the game screen at about 250 s from the command, where the calibrated clock said 138 s. It was not worked on.

## Playing

The scenes above are a scripted, mostly idle game, and do not show what a person feels. `python tools/game_par.py
aoe-play` (and `aoe-play-moving`, the pointer going round all the time; `--card` or `--calibrated`) plays on the desktop by
`nxkey`: select, order, select a building, train, a box dragged, order many, the build menu, scroll. `AOE_LAT=1` keeps what
became of each input and every picture in memory and writes them at the end, and `python tools/aoe_play.py LOG` gives per
act the milliseconds to the event's coming to the game, its being taken, the next pictures, and the panel's reply on the
screen, with the frames a second until the next act. The time starts at the shell's `cat /proc/uptime` before `nxkey`, so
it holds `nxkey`'s start and its resolution is 10 ms. The fps of `aoe-play` comes from those lines: the log's writing is
minutes of console after the acts. `rdinstret` reads 0 on the card, so instructions a frame are from `rvc_cpu` only.

On the card, lean tick, 1 November 2026, the pointer moving:

| | Before | After |
|---|---|---|
| Acts whose click reached the game within its 5 s | 2 of 12 | 12 of 12 |
| Input to the game having it | 3.4 and 3.9 s | 44 to 298 ms |
| To the first picture after it | 3.5 and 4.1 s | 91 to 427 ms |
| To the panel's reply on the screen | 3.6 and 4.4 s | 246 to 857 ms |
| Frames a second | 6.0 to 10.6 | 7.6 to 10.0 |

The "before" ran with `AOE_MOVES=all` and already had the other fixes below. What was wrong and what is done:

- **Moves.** Every `WM_MOUSEMOVE` is a call of the game's idle work, a whole frame, and the port kept every move: with the
  pointer moving the queue stood full at 255, and the window system's events were read only when it was empty. As Windows
  does, moves not yet taken are now one move (`AOE_MOVES=all` is the way before).
- **Zone maps.** The first unit to ask for a table's zones flooded the map, a block from `calloc` a tile and double
  comparisons a neighbour, 11 million instructions: 2 s with the game standing, several times in a game's first minutes.
  It floods through an array, and all tables are made at the first update. Which tiles a zone gets does not depend on the order.
- **The screen's picture.** A redraw of the whole screen went through the pictures' rows, 6 million instructions, 1.1 s.
  It is copied from what was kept: the rows between the two pieces are the view's, which draws them whole.
- **A selection** waited for the object panel's half-second look. Another object selected passes that gate.

- **A stop of 1.3 s** was not the first order's, though it fell there: a computer player's change of alliances
  (`update_mutual_allies`) asked for the whole view and map again and wrote 900 lines of log, 7 million instructions in all.
  The log is gone, and the redraw comes only if what the player sees changes (`AOE_ALLIES_REDRAW=1` is the way before). State
  sums do not move. Pictures do, by what the redraw used to repair: at update 500 of the script one stale 4 x 4 speck at about
  (388, 362), which a whole redraw clears and which looks like the terrain's own specks. Scrolling over it clears it too only
  where the tile is drawn again; how many gather in a long game was not measured.

Still open: one calibrated run had a stop of 2.1 s (11.6M instructions in a world update), which later runs did not have; it
was not named.

## Speed in play

The aim is 15 frames a second in play, 52 passes a frame at the card's 860 to 900 passes a second, and it is not reached in
every run. `aoe-play` on the card, the build before and the build after in turn (1 November 2026, lean tick):

| | Before | After |
|---|---|---|
| `aoe-play`, frames a second | 9.3, 8.7 | 13.6, 11.3, and 9.9 on the merged build |
| Passes a frame | 98, 104 | 66, 80, 85 |
| `aoe-play-moving` | 11.5 (78 passes) | 14.6 (62 passes), 12.6 (65 passes) on the merged build |

The runs on one side spread as much as that because other sessions' runs held the machine between them. On the calibrated clock:
`aoe-play` 14.7, `aoe-play-moving` 13.8. Standing acts reach 13 to 21 on that clock, scrolling 5 to 15.

- **Soft doubles** were 21.5% of the first core and are 4.1%: a moving object's angle in floats (`aoe_atan2f`, a double's `atan2`
  was 9%), the double's only near where the decision changes; and in the mover, sprites and placement a float against a double's
  constant is compared with the float that says the same of every float. `aoe_soft.c` has the conversions and comparisons
  in code of its own.
- **Clips**: the spans of the last four rectangles are kept (`aoe_clip_lists`). **A building's place**: the influence map is
  looked at before `check_placement`; neither changes anything, so the same places come out.

What bounds it: a frame is the walk on its worker, about 30 passes with the first core waiting (the first core has next to nothing
to do beside it: panels are redrawn only when they change), then the world update beside the raster, about 35 passes.
The world update cannot start before the walk ends, since the walk reads the objects. Next would be the walk on more workers, or
its terrain part after the world starts: the latter stores to tiles the world's stores share texels with, so tile state the view
keeps must first move apart.

Tried and not taken: `-O3 -fsched-pressure -funroll-loops` was no faster in play (10.9 against 11.6 on the calibrated clock).
LTO does not link: the decompiled source defines a class differently in different files (`taiuaimd.h`). PGO was not tried.
A branchless `get_tile_mask_num` was 64 instructions a call against 59.

## The view on a worker

`RGE_View::draw`, the walk and the raster, is one job on a worker (`port/aoe_view.cpp`). The first core draws the other
panels beside the walk (both only read the game), waits for "walked" to make the pick, and makes the next world update
beside the raster: the world runs while a frame is drawn. A frame's end joins the job, draws what was deferred (a message
over the view), the cursor, and shows the page. `AOE_VIEW=inline` is the same order on the first core, `AOE_VIEW=whole`
the way before; under `AOE_NOWINDOW` there are no workers.

What the walk sets around a shape and a panel's drawing must not see is one for each core: `fog_next_shape` and
`SDI_Capture_Info` (`aoe_cores.h`). They were shared at first, and the panels' shapes went into the walk's list: a town
centre with no picture, found only by comparing pictures. The sprites' state is one for each core too (`aoe_asm.cpp`),
the view has its own copy of the page's draw area and span list, and what grows in a job comes from a piece of its own
(`aoe_any_calloc`), never the heap. A view's clock looks do not move `AOE_TICK_MS`'s clock, which moved the state sums of
script runs on purpose: update 500 `64d33a6f`, 1000 `5032c153`, inline, and under qemu when it was last compared.

With the view on a worker a script run's sums are not one value: the same program gave 1000 `5032c153` and, with
`RVC_RACES` or `AOE_PICSUM`, `138ef893`. The first core's clock looks are not the same in number from run to run, so a script's
later steps come at other clock values (a step at 14521 against 14488) and a click at another update. That is what was seen; no
other cause was ruled out. So `test_all.py aoe` runs `AOE_VIEW=inline`, and sums compare only there. Its check also used to
pass on any run, as the console echoes the `AOE-NO-DATA` of the typed command: it is anchored now.

`AOE_PICSUM=N` prints sums of a picture's three bands, `AOE_PICDUMP=F` a grid of block hashes of frame F, and
`AOE_PICROWS=1` the bottom left's pixels. Worker against inline at updates to 500: top and view bands equal; the bottom
band differs in 4 of 21 pictures, when the panel's half-second look falls on another frame (a percent trained shown a look
earlier), which was read off the pictures once and not checked for each. Comparing a worker's pictures needs care: which
frame a band shows moves with timing, and differs between images of one program. `RVC_RACES` with `RVC_RACES_NEAR=4`: 0
undone in the game's script run; 3 near places not named. On the desktop one undone was seen, at 0x81000040: the window system's
pointer worker (`wm_job`, `scr_shaderemu.c`) and its first core (`cursor_put`) on the cursor's registers, not the game's;
`wm_job` writes again what differs, so it heals.

## Windowed

The game opens as an ordinary window of 640 x 480, the size its code calls its least, in the frame and with the bar's button of any program; the frame is 646 x 509 on the 800 x 600 desktop. It is `GR_WM_PROPS_ONTOP` only on a screen with no room for the frame and the bar (under 538 rows), one copy runs (`ui_single_instance`: a second start brings the first forward), and an unmapped window draws no list. `AOE_SIZE=800` is the way before, on top of the bar.

The game's code has a layout for 640 x 480 (`TRIBE_Screen_Game::handle_size`), but the trial has no pictures for it. So:

- The game screen's two bands are cut from the 800 x 600 picture (`aoe_picture_cut`): the top bar without its columns 376 to 535, the bottom panel without 406 to 565, which the layout's places fit to the pixel. What is drawn is kept as before.
- A menu's background is the middle of the picture for 800 x 600 (`background_pos` 1, in `TEasy_Panel::setup`): the title's top is cut off.
- At that size the buttons are five columns, with "more" and "cancel" over the map's corners (`button_other_pic`), as the code lays them out.
- The victory and "history" screens ask for `bgA640` (`scr_end.cpp`, `scr_trb.cpp`): they were not looked at, and are likely to have no picture.

The smaller view is faster. `game_par.py aoe-play` and `aoe-play-800` calibrated, on a model fitted to an older tick so only the comparison counts, six timings each, alternating, frames a second over the acts: 12.3 at 800 x 600 before this work, 12.1 at 800 x 600 now, 15.5 in the window (14.7 to 17.0). The acts are not quite the same: the window's two scroll acts are drags, the 800's rest at the edges.

## Controls

The rules are the porting skill's ("Controls for VR"). What a hand has is a beam that drives the pointer: trigger the left button, grip the right one, and no edge to rest on.

- **No scrolling at the edges** (`info.auto_scroll` 0): a pointer resting on the window's edge moves nothing.
- **The right button dragged moves the map**, a pixel for a pixel (scroll action 10, `RGE_View::handle_scroll_view`; a tile is 64 x 32): the map goes with the hand, from where it stood at the press. The right button, because the left drag is the game's box, because the grip is a hand's natural hold, and because Tiberian Dawn and Red Alert already do the same. It works because the game gives its right-click order at the release: `TPanel` holds a press back for 10 pixels, past them `mouse_action` is 3 and the release orders nothing. So a press that stays within 10 pixels is an order (the steady pointer holds a press for 7), the selection stays through a drag, and a building being placed stays. The game's own right drag worked only with nothing selected, and moved the cursor back with `SetCursorPos`, which a beam does not follow.
- **While a button is held** the pointer and the release come from the machine's words (`held_by_words` in `host.cpp`), which go on past the window's sides, where a beam stays on the display's edge with the button down (`EmuPointer`, `onEdge`). Only for a press those words have shown: `nxkey`'s presses are not in them.
- The minimap's click and the keyboard's arrows move the view as ever.
- `AOE_SCROLL=edge` is the way before.

`test_all.py aoe-drag` holds this: a right click is an order, a drag by -200,-100 moves the view 0, +6.25 tiles and gives no order, the unit stays selected for the next one, a left drag is a box, and the pointer at the window's right edge for 9 s moves nothing. `tools/aoe_play.py`'s `scroll` acts are that drag by `nxkey rdrag:`. `aoe_play.py` prints the view's place for each drag. A pointer's move came to the game in about 80 ms on those acts, as on the others.

Not tried in a headset, nor at all in Unity: the beam, the grip and the steady pointer are read from `EmuPointer.cs` only. Dragged in the machine's words by `rvc_cpu --pointer-button`: `--pointer-circle 620,250,120 --pointer-button 2,77,78.5` pressed at 457,195 of the window and logged a drag of 239,3, which ends past the window's right side, and the view moved. A press logs the buttons word (`aoeinput: press`), which said 0 in these runs, and the case does not separate the words' path from Nano-X's own hold on the pointer. The game's start is about 40 s of the guest's in, and a right button held during the menus stopped the script's clicks from starting a game..

## Sound

A sound is a voice pointed at its wave where it lies in the ROM (`port/sound.cpp`, the game's `DirectSound` buffers; `TDigital::load` in `Sounddrv.cpp` asks `aoe_sound_load` and copies nothing). 141 waves of `Sounds.drs`, 14 of `Interfac.drs` and five files of their own; 16 voices, the one begun longest ago given up; the game's pan and volume by its numbers, and the options' slider where Windows had a mixer. No memory: a wave's first bytes are read once, and `Play` is a few stores.

A card's 16-bit samples must lie on an even address and most of the trial's waves did not, so `make_data.py` writes the two DRS files with every item on an even offset (`write_drs_even`), padding only. An image made from an older cache says so. A plain image is not touched by any of this.

`AOE_SOUND_LOG=1` says each wave played and when its voice was seen to end, and `python tools/aoe_sound_check.py LOG IMAGE` reads each address from the image's ROM as the card will and finds that the samples are a wave of the trial, whole: on `aoe-play`, 8 plays of 6 waves, 0 not of the trial, 0 of silence, 8 seen to end and none in under half their length, each after the act that asks for one. What was heard was not checked: the check goes by the addresses, and no mix was captured. Over the same six timings the window gave 15.5 frames a second with sound and 16.1 with `AOE_SOUND=0`, inside the spread between timings (14.3 to 17.9 silent).

## Music

The game's own `TMusic_System` (`music.cpp`, not changed) plays it: what plays when, what comes next and how loud are all the game's. It asks Windows for a CD, which fails, then for the MCI sequencer and the synthesizer's mixer line, and `port/aoe_music.cpp` is those two on the card's FM voices. `info.use_music` is 1 again. So: `open.mid` at the first menu, once; in a game `music1.mid` to `music9.mid` in turn and round again, the next at each song's end by the game's `MM_MCINOTIFY`; `won.mid` or `lost.mid` at the end; the options' slider is the mixer's level, and its default of half (registry `Music Volume`, -5000) is the game's. A slider at the bottom turns music off as the game does. Windows' notice of a stopped song comes too, at the next look, since the game counts them.

The notes are made as OpenTTD's are: `linux/userland/shaderemu_fm.h` has the bank's instruments and the channel messages as notes given to voices, and OpenTTD's driver now goes through it too (`OPENTTD_MUSIC_SUM=all`: the 32 songs of OpenMSX give the same sums and notes as before). What is the port's own is 60 lines of Standard MIDI File reader, since OpenTTD's `MidiFile` is its own C++. So as in OpenTTD: program, volume, expression, pan and all-notes-off are followed, and the pitch wheel, the sustain pedal and drums outside the bank's 35 to 81 are not. `python tools/aoe_music_check.py --files IMAGE` says how many each song has. `Lost.mid` has 1,302 wheel messages and `Music1.mid` 1,554 (and 316 drums outside the bank, the most), so those will sound the least like the original.

**Voices.** The effects keep their 16 and music has the other 16 (`VOICES` in `aoe_music.cpp`), since 32 is all the card has. OpenTTD has 20, because its mixer takes 8. The songs want up to 19 to 32 at once, counting a two-voice instrument as two, so a note is cut when all 16 sound (the oldest goes): 7 to 179 notes of a song, at most about 3% (`Music1.mid` 122 of 3,766, `Music6.mid` 179 of 6,138). More voices for music would mean fewer than the effects' 16.

**A song on a worker.** The first core reads the file into sound memory and posts the job to a second worker (`mcw_open(2)`, the view has the first), which makes the notes and writes the tracks; the voices start when the first core next looks. All the job writes lies in sound memory or its own `SongJob`, 1.4 MB for the largest song the format can have here, and none of it in the program's data. With `AOE_VIEW=inline` or no workers the first core makes it. Nothing here moves the game: a song's end is a sent message with `AOE_TICK_MS`'s clock held (`aoe_clock_still`), not one of the queue, as a message taken is a look with the idle work not done. The state sums do not move: `aoe-music`, nine songs a round cut short at 4 s, says `5032c153` at update 1000.

**What it costs** (`rvc_cpu`, 1 November 2026): on the first core at most 289 instructions a look on average, about one look a frame, against about 340 thousand a frame, and that with the log's own prints in it; 25 to 42 thousand a song to read and post. On a worker 1.7 to 5.6 million a song. `game_par.py aoe --calibrated`, with `AOE_MUSIC=0` and without: core 0 342.0k and 342.9k a frame, 11.6 and 11.1 frames a second, all cores 586k and 616k. But two runs of that scene differed by 44k in core 0 before salts were tried, and salts did not move it, so that says little about 0.9k. The card's mix of 16 more FM voices was not measured: it is not in any `rvc_cpu` number, and it is the card's sound pass, not the first core.

**Checks.** `AOE_MUSIC_LOG=2 AOE_SOUND_LOG=1` on `rvc_cpu` with `RVC_SOUND=1`, then `python tools/aoe_music_check.py LOG IMAGE`, reads each MIDI file with a reader of its own and works out each note from the file and the bank: start, instrument, loudness, pitch against 440 Hz's scale, and length. Against the words the machine wrote for the card, on `open.mid`, `music1.mid` and `music2.mid` in play: all notes found, none other, pitch within 0.000%, every note held as the file says or cut (as many as the driver counted), all 350 instruments as the bank says, 16 voices kind 5 on voices 16 to 31 with the effects on 0 to 15, the card's position moving with its clock to the song's end, and the next song after it. Effects played beside the music in those runs. `RVC_RACES` with `RVC_RACES_NEAR=4`, 35 songs on the worker with `AOE_MUSIC_CUT=2`: 0 undone, and the near places are core 0 and core 1's, not the song's worker.

`--wav FILE` writes what those words sound like: `tools/sound_reference.py`'s FM, in the card's integers, with the effects' samples from the ROM at the clock values in the log (not interpolated). It is that model's mix and not a capture of the shader, since `rvc_cpu` mixes nothing and a capture wants the card. A song at the default level peaks at 5,442 of 32,767 alone, and with the effects of those runs at 22,119, none clipped. **Nobody has listened to it yet**: whether the instruments sound right is not checked by any of this, and it has not run on the card or in a world. The options' slider was not moved in a run, and `won.mid` and `lost.mid` were not reached by play: the first level the mixer gets is the only one exercised, and those two songs' notes were only read from the files.

A MIDI file is game data: it comes from the trial's cache, with the rest (`KEEP` in `make_data.py`), and goes only into an image made with `--aoe`.

## What ends a round

A world runs a fixed number of rounds a Unity frame, so a pass that ends early loses the rest of its instructions there; in the harness it costs 0.2 ms, and the card's figures do not show it. `RVC_PASSLOG` on `rvc_cpu` says who ended each pass, and `reached` in `EmuMachine` is the share a round ran in the world.

`TSpan_List_Manager::ResetAll` cleared five arrays of 600 lines, 2,400 bytes each. A `memset` from 2 KB up is a parallel fill, which ends the pass, so a clip of a few lines could end five. It now clears only the lines written since the last reset (`aoe_dirty_lo`, `aoe_dirty_hi`: the regions set since then, and every line after a scroll or a merge), in pieces under 2 KB. `TRIGONOMETRY` makes only the two tables anything reads. On the calibrated clock, from power-on through the script above to update 300:

| | before | after |
|---|---|---|
| instructions lost to passes ended by those `memset`s | 66 million | about 1 million |
| share of the pass run, the last quarter of the run | 84.3% | 99.1% |
| the main menu | 17.0 s | 8.8 s |
| the game screen | 116 s | 107 s |

The sums and the menu's picture stayed the same. A rest when nothing was drawn (`aoe_events`) is a program at its menu with nothing to do: `reached` of about 0.12 and stall 9 there is idle, not stuck. The largest cost left in the start is drawing: each panel's 800-pixel picture is painted twice on the way to the menu, by `set_focus` and by `UpdateWindow`.

## A window made again

The window manager gives a new window its frame after the program has mapped it, and the window then has other memory (800 x 600 at 20, 20 became 806 x 629 at -3, -26). A game at its menu draws nothing more, so the window stayed black in the world with the program resting. A capture taken before the manager has done this shows the menu, so it says nothing. The port now shows its page again on any update of the window but an unmap, and on an exposure: the page is still the picture.

## A bug to remember

`Visible_Unit_Manager::Update_Unit_Info` took the place of `VUR_Ptrs` in an object as `0x194`, the Windows build's. Here it is `0x190`, so a list that grew left stale pointers, and `RGE_Combat_Object::teleport` wrote tile coordinates through them into freed memory. It showed after about 700 updates as a `TDrawArea::CheckSurface` crash on a surface like `0x707359c0`. Found by a write-protected page under qemu, as qemu's gdb stub in user mode has no watchpoints. `obj + 0x0C` in `taiinfmd.cpp` went the same way. `(char*) this + 0xFF90` and the map's sizes there are by name now, and the sums did not move: they had been right.

## Closing

`python tools/app_fuzz.py plan "start:a:aoe wait:a:120 close:a:x end" --image build/images/NAME` starts it and closes it by its frame. While it loads the close takes time to be taken (25 s in that plan). In a game a close brings up the game's own question: Yes ends the program (exit 0), and No goes back to the game and a later close asks again (`AOE_SCRIPT` with `close`, under qemu and on the machine). `aoe` is not in `app_fuzz.py`'s list because a plain image does not have it.

The cursor is the game's own, drawn into its picture, and it is in captures from the script and from `nxkey`. Whether a person sees a second one, the window system's, over it was not looked at on a live screen.

## Not done

- The pitch wheel and the sustain pedal in the music ("Music").
- `linux/nommu/memory.py`.
- Saving, multiplayer (DirectPlay stubs) and scenarios were not tried. Computer players gather and build over 5 game minutes; a whole game was not played.
