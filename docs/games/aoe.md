# Age of Empires, the 1997 trial

The decompiled game (`github.com/dcostap/aoe1-decomp`, pinned at `6d6d8b1`; the plan read `f4fdf3f`) over a Win32 and DirectDraw layer of our own, with the data of the trial (`docs/plans/aoe-plan.md`). It opens as a window of 640 x 480 on the desktop and plays, with its sound effects and its music on the sound card and the map dragged by the right button ("Windowed", "Sound", "Music", "Controls"). It draws its view on a worker core, and at 800 x 600 played at 11 to 15 frames a second on the card ("Speed in play").

**No game data in the repository.** The data is under the trial's EULA, and the code is recovered from Microsoft's binary. Nothing of the game or the trial is committed: only the port, the patch and the tools. The program is not in `ALL` in `linux/nommu/build.sh` or in `linux/prebuilt`. The trial's `Empires.exe` is not read at all. The game's own `verify_cd` is set to 0, because the trial has no CD.

## Making it

The game goes into an image only when asked:

    pwsh tools\make_image.ps1 --aoe                                  # or AOE_IMAGE=1
    python linux/nommu/image.py NAME ROOT KERNEL --aoe

The licence is the trial's EULA, the one that came with Microsoft's 1997 trial of Age of Empires. The source is `archive.org/details/aoe-trial-1997` (`MSAoE.exe`, 24,786,864 bytes). That item states no licence of its own. The EULA allows copies of the whole trial, with the EULA, not for commercial purposes, and it limits reverse engineering and decompiling as far as the law allows: the decompiled code comes from the upstream project and is fetched, not kept here. So since 2 November 2026 an image holds **the installer itself, byte for byte**, and nothing taken out of it: the machine shows the EULA read out of it, and once it is accepted there unpacks the trial into memory ("The trial in the machine", `docs/system/install.md`).

`linux/aoe/data.py OVERLAY` then does this:

1. It fetches `MSAoE.exe` into the cache (`AOE_CACHE`, by default `~/.cache/shaderemu/aoe`), and refuses it unless its md5 and sha256 are those in the script.
2. It builds the program (`program.sh`, into `build/aoe/out`; `AOE_WSL_HOME` is a private home, `AOE_PROGRAM` a folder already made): `aoe.bin`, the starter `aoe` and `aoeprep`.
3. It fetches the music's instruments into the same cache (`genmidi.op2`, DMXOPL's General MIDI bank, MIT, the commit OpenTTD fetches, against its checksum). If that fails the game goes in with no music.
4. It puts the installer as it is (`/usr/share/aoe/MSAoE.exe`), the bank, the programs and the Start menu's line into that image's overlay, and never into the root.

If any step fails it says which, the image goes on without the game, and `test_all.py aoe` prints `AOE-NO-DATA` and passes. A plain build runs none of this and is byte for byte as before. `--save-prebuilt` turns the switch off, and what it saves comes from the root, so neither the installer nor the game can be saved. Nothing of the trial is unpacked or converted on the build computer any more (`make_data.py` and `dat34.py` are gone).

## The trial in the machine

The starter runs `cabinst` (`docs/system/install.md`): the EULA in a window, Accept only once it has been scrolled to its end, the registry's `aoe.eula` keeping an acceptance, then 105 of the cabinet's 129 files (34,085,752 bytes: the DRS files, the AI and personality files, maps, campaigns, songs, waves, `Empires.dat`, `language.dll`, the six faces) unpacked on the workers into `/tmp/aoe`, every session. The game reads the trial's own files from there (`AOE_DATA=/tmp/aoe`), with two made by `aoeprep` (`linux/aoe/prep.c`) the first time:

- `language.txt`, from `language.dll`'s string table: the same bytes `make_data.py` made (2,276 strings).
- `aoe-fonts.bin`, the six faces at 8 to 36 pixels, on the workers, by `linux/aoe/ttf.h`: the outlines' coverage without hinting, where `make_data.py` had FreeType's hinting through Pillow. Line heights and ascents are the same in all 174 fonts, 97.3% of the cells' widths and 97.1% of the pixels of cells of one width; side by side the text reads the same. FreeType is not in the machine, and a font library fetched and run was not wanted. So the menus' pictures are not the bytes they were; the state sums do not move.
- `Empires.dat` is read as the trial has it (`dat34.py` copied it). The DRS files are the trial's own, whose waves lie mostly on odd addresses: the sound port copies such a wave once, at its load, to memory of its own ("Sound"). `genmidi.op2` stays in the ROM, linked into `/tmp/aoe`.

On the card (2 November 2026), `aoe --unpack` from the command to its end: 16.6 s; the EULA read out 0.87 s, the files 6.25 s, `aoeprep` 0.26 s and 4.30 s. Memory on `rvc_cpu`, the `aoe` case's script at 800 x 600, 60 s into the game: 194,700 KB free with the trial in `/tmp` (36.8 MB of it there), where the data in the ROM left 230,900 KB; 245,944 KB at the shell. `test_all.py aoe-install` starts it on the desktop: Accept refused, then scrolled, accepted, the bar to the last file, `aoeprep`. The other `aoe` cases unpack first with the EULA answered on the terminal (`echo yes | aoe --unpack`) and play as before: the same sums.

The program alone:

    export HOME=~/agents/NAME PYTHONUSERBASE=/home/micha/.local      # a private home: building-the-system
    AOE_OUT=DIR bash linux/nommu/build.sh aoe/build.sh               # fetches the pinned tree, applies aoe.patch
    python3 linux/nommu/flat.py DIR

`aoe-q.elf` is the same game for `qemu-riscv32`, with `AOE_NOWINDOW=1` and `AOE_DATA`.

## What the trial is

- Its `Empires.dat` says `VER 3.4`, and the game's 3.7 reader reads it to the exact byte (758,114). Only the version check changed, so no converter is needed: the game reads the trial's file.
- 12 civilizations and Gaia, and 5 random maps; a type past the last is map 0.
- Full pictures only for 800 x 600: `0xC62D` to `0xC630`, the game screens for 640 x 480, and the menus' are not in `Interfac.drs` ("Windowed").
- No `BLKEDGE.DAT` (edges black) and no Roman button (`0xcf0d` is `0xc610`). Other `0xCF..` resources of the game screen are the Rise of Rome cultures' and have not been checked one by one.
- `data2\*.drs` are reported missing at each start. It is harmless.
- `rand()` is MSVC's (`aoe_rand`): the game divides by `0x7FFF`.

## Checks

| Switch | Does |
|---|---|
| `AOE_TICK_MS=33` | a clock that moves by what the game does: a run comes out the same |
| `AOE_SCRIPT="MS click X Y;MS rclick X Y;MS key K;MS char C;MS close;MS quit"` | each step MS after the screen last changed or the step before; `move X Y` keeps a held button, `rsweep X Y` presses the right button and drags 200 pixels to the left and back ten times, 20 a 100 ms (`game_par.py aoe-drag-scroll`), `ready` writes `/tmp/aoe-ready` and then `/tmp/aoe-size` |
| `AOE_SUM_EVERY=N`, `AOE_SUM_END=N` | `aoesum:` a sum of the clock, the random state, players' attributes, objects' ids, hit points and places |
| `AOE_STATS_MS=N`, `AOE_PLAYERS=1`, `AOE_DEBUG=1` | `aoestat:`, players' resources, panels by time |
| `AOE_DUMP=FILE`, `AOE_DUMP_EVERY=N` | pictures under qemu |
| `AOE_BACK=copy` | the drawing surface apart from the page, as on Windows |
| `AOE_SIZE=800` | 800 x 600, above the bar: the way before; else the window of 640 x 480 ("Windowed") |
| `AOE_SCROLL=edge` | the map scrolls at the window's edges, and by the game's own right drag with nothing selected: the way before ("Controls") |
| `AOE_SOUND=0`, `AOE_SOUND_LOG=1` | no sound; `aoesnd:` each wave found, played and seen to end (`python tools/aoe_sound_check.py LOG IMAGE`, "Sound") |
| `AOE_MUSIC=0`, `AOE_MUSIC_LOG=1` or `2`, `AOE_MUSIC_CUT=S` | no music (`AOE_SOUND=0` has none either); `aoemus:` each song and what the card's words say, with 2 every note and instrument for `tools/aoe_music_check.py`; each song ends after S s ("Music") |
| `AOE_SEED=N` | every seeding of the game's random numbers takes N, not the clock: the same map and start in a run on the real clock (`game_par.py`'s window scenes) |
| `AOE_SCROLL_COPY=words`, `AOE_BLT=bytes` | a scroll moves the view by words on 8 pixels across, and a keyed copy goes a byte at a time: the ways before ("Speed in play") |
| `AOE_MENU_PICTURE=middle` | a menu's picture for 800 x 600 cut to its middle in the window, the way before ("Windowed") |
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

### Scrolling and units in the window (3 November 2026)

The world's complaint was "only when nothing happens": scrolling and units acting felt slow. Scenes of `game_par.py`, all in the
640 x 480 window, the map's seed fixed (`AOE_SEED=1`: before it, two runs of one scene were two maps, 5.8 against 9.7 frames a second),
updates 200 to 350: `aoe-still-640`, `aoe-drag-scroll` (the script's `rsweep`: the right button held and the map dragged 20 pixels every
100 ms, 200 to the left and back, all the span), `aoe-late-640` (updates 3000 to 3200, the computer players' towns and units). Each has a
`-before` twin that runs the same program the way before (`AOE_SCROLL_COPY=words AOE_BLT=bytes`). Calibrated clock, one run each:

| Scene | Before: frames a second, passes a frame | After |
|---|---|---|
| `aoe-drag-scroll` | 11.6, 59.0 (core 0 264k, the view's worker 327k a frame) | 12.0, 56.9 (253k, 322k) |
| `aoe-still-640` | 14.1, 47.7 | 14.1, 47.7 |
| `aoe-late-640` | 17.3, 39.1 | 15.2, 44.3 (a played game on the real clock: its frames are not the same frames) |

On the card (`game_par.py --card`, the same image, one run each, lean tick, 3 November 2026): `aoe-drag-scroll` 7.9 to 10.0 frames a
second (88.9 to 57.9 passes a frame), `aoe-still-640` 12.7 and 12.9 (45.2 and 44.6). The card gains more on the drag than the calibrated
clock says: the model has no cost for the many short passes a copy by words left. Start Game with the settings shown (Large, Large Islands,
4 players) to the game's screen: 93 s on the card, 88 s calibrated; it was never stuck (the clicks that missed were, "Windowed").

- **A scroll by the machine's copy.** The view's worker moved the view's 200 KB by words at every scrolled frame (`OverlayMemCopy`:
  `memmove` was 20% of the worker, 106 thousand instructions a frame). The rows are one block in memory, and the machine's copy moves
  it in a pass or two from the worker; the scroll goes on 16 pixels across, not 8, so that the block is whole texels. `memmove` is gone
  from the worker's profile, the picture of a drag was looked at (whole, no seams), `aoe-drag`'s view moved as before, and `RVC_RACES`
  with `RVC_RACES_NEAR=4` on the drag: 0 undone, 5 near places, none in this code (`memcpy` of `aoe_asm_for_worker`,
  `view_function_terrain`, `ASMSet_Xlate_Table`, `SetClipRect`, as before).
- **A keyed copy four pixels a test.** A message over the view is copied with its key every frame (`TMessagePanel::draw`, 58 thousand
  instructions a frame when one shows); a word that is all key is passed over.

What bounds it is not the drawing: the frame is the walk on the worker with the first core waiting, then the world's update (160 thousand
instructions a frame, its objects, sprites and computer players) beside the raster, and the first core's own part is about 250 to 300
thousand a frame either way. Five times faster, the aim asked for, is about 10 passes a frame: the world's update alone is 20. It needs
the update and the walk to overlap (a frame behind, from copies of what the walk reads, as Half-Life and Doom do) or the update on
several cores, neither done here. Measured and left: `mcw_on_worker` is 6% of the worker (a call at every sprite: `asm_state`, the fog
word), `AddSpan` 7.6%, `view_function_terrain` 18% (it walks every visible tile each frame), and the first core's soft doubles 4%.

Tried and not taken: `-O3 -fsched-pressure -funroll-loops` was no faster in play (10.9 against 11.6 on the calibrated clock).
LTO does not link: the decompiled source defines a class differently in different files (`taiuaimd.h`). PGO was not tried.
A branchless `get_tile_mask_num` was 64 instructions a call against 59.

### The world in slices (3 November 2026)

The aim set after the last round was five times the speed in scrolling and in play. Where a frame went, by the machine's time on the
first core (`aoephase:` lines at a mark's end, `aoe_phase` in `port/aoe_view.cpp`), calibrated, updates 200 to 350:

| | Wait for the walk | World update | Panels | A frame |
|---|---|---|---|---|
| `aoe-drag-scroll` | 37.5 ms (45%) | 26.6 ms (32%) | 10.9 ms | 82.7 ms |
| `aoe-still-640` | 14.5 ms (21%) | 26.0 ms (38%) | 12.3 ms | 67.0 ms |

Every picture waited for one whole update, and the walk could not start before the update ended, since it reads the objects. So:

- **The world's update in slices** (`port/aoe_world.cpp`). The update runs on a stack of its own on the first core and gives way between
  two objects' updates (`RGE_Object_List::update`, `aoe_world_may_give_way`) once its slice is spent; a picture is drawn of the world as
  the slice left it, and the next slice goes on where it stopped. The slice keeps an update to 70 ms of the real clock (`AOE_UPDATE_MS`): the
  world's share of the time is the last update's work over 70 ms, and the slice is the time spent outside it since the last slice in that
  ratio, at least 4 ms (`AOE_SLICE_MS`); an update whose work is over 90% of that is whole. So the game's time keeps up with the real
  clock as before (its step is capped at 100 ms an update), and the pictures get the rest.
  While it is out nothing writes the world: the walk and the panels only read it, a move only moves the view or the pointer, and any
  other input (a button, a key, a close) runs the update to its end first (`aoe_world_finish` in `DispatchMessage`). The world's clock
  stands still while it is out (`aoe_world_clock`), so a computer player's budget of milliseconds is its own work's. A picture between
  two slices shows some objects one update further than others, a few pixels at most; nothing is drawn half moved, since an object's
  update is never cut. `AOE_WORLD=whole` is an update between two pictures, the way before; so are `AOE_TICK_MS`, `AOE_VIEW=inline`
  and `AOE_NOWINDOW` runs, which is how the state sums are made (`test_all.py aoe`: `5032c153` at update 1000 as before).
- **Paths without doubles** (`port/aoe_path.c`). With a dozen villagers ordered, two thirds of the first core was `findTilePath` and its
  soft doubles: `(int) (sqrt((double) d2) * 16.0)` at every step of its search. It is `floor(sqrt(256 d2))`, the same number for every
  whole `d2` (16 times a correctly rounded root is exact, and a root that is not whole is farther from a whole number than any rounding),
  so a table of 131,073 entries up to the largest map's diagonal, made at the first path. `AOE_PATH_CHECK=1` makes both at every call:
  7,380 of 7,380 alike in the busy scene. The terrain's passability is compared with `0.0f`, which `g_path_zero_terrain_value` (0.0) is,
  in place of a float made a double. `AOE_PATH=doubles` is the way before.
- `mcw_on_worker`, a call at every sprite on the view's worker (6%), is a test of the stack pointer against the job's stack
  (`aoe_view_core`, `aoe_cores.h`).

A scene of busy units: `game_par.py aoe-busy-640` types the game's cheats into its chat (food, wood, quick building), builds three houses,
trains nine villagers, boxes the twelve and orders them to and fro across the view every 6 s, and measures 150 updates from the first
order (the script's `chat`, `box` and `mark` steps). Typing into the chat needed a fix: keys went to the game's hotkeys while the chat's
line had the focus, since the game subclasses its edit controls (`DispatchMessage` in `port/win32.cpp`). There is no fight in it:
no enemy is within a script's reach in the first minutes; `aoe-late-640`, the computer players' towns, is the scene with their armies.
The `-whole` twins of the scenes are `AOE_WORLD=whole`; `aoe-busy-640-before` is `AOE_WORLD=whole AOE_PATH=doubles`.

Calibrated (`--calibrated`, updates 200 to 350; busy 150 from the first order; late 3000 to 3200), the image before this work
(the last commit's program) against this one, run side by side; game time is the game's milliseconds over the real ones. Two runs of
one scene differ by up to 10% (the drag after: 16.7 and 18.1; the still: 20.4 and 24.7), so read the first digit:

| Scene | Before: pictures a second, passes a picture, game time | After | Pictures |
|---|---|---|---|
| `aoe-drag-scroll` | 11.2, 60.7, 0.86 | 16.7, 40.8, 0.86 | 1.5x |
| `aoe-still-640` | 14.2, 47.2, 0.97 | 24.7, 27.4, 0.99 | 1.7x |
| `aoe-busy-640` (before: `-before`, the same program the way back) | 5.8, 116.0, 0.55 | 15.9, 42.5, 0.93 | 2.7x |
| `aoe-late-640` | 17.0, 39.7, 0.98 | 23.8, 28.4, 0.94 | 1.4x |

In the busy scene the world's update went from 603 thousand instructions to 173 thousand (the path finder), and its game time from
0.55 of the real clock to 0.93: units walk at the game's speed again. In the others the update costs as before and gets the same share
of the clock (an update about every 70 to 85 ms), and the pictures between are new.

Not run on the card: at the owner's word this round is measured on `rvc_cpu` with the card's model only (`game_par.py --calibrated`).
The last round's card runs gained more than the calibrated clock on the drag (the model has no cost for many short passes).

`RVC_RACES` with `RVC_RACES_NEAR=4` on the drag and the busy scene: 0 undone; near places are core 0's panels and copies (`Blt`,
`memcpy` of `aoe_view_post`) beside the view's job (`view_function_terrain`, `ASMDraw_Sprite`) two to four passes apart, as before.
Pictures of the busy scene with the world in slices were looked at (the script's `shot` step, below): the villagers whole and where
they walk, no seams.

What bounds it now, and why five times was not reached:

- **The walk.** A picture's walk of the visible tiles is 90 to 150 thousand instructions on one worker, and the first core can only wait
  for it: the walk reads the objects, so the world cannot run beside it. That is 15 to 26 ms of a picture. The way on is the view in parts
  over several workers (OpenTTD's `ViewportFlush`): by bands of rows, each worker drawing the terrain and capturing the objects of its band
  with clip masks and object lists of its own, merged by line for the raster. The tile state the walk keeps (`last_drawn_as`,
  `last_drawn_shape`, `draw_attribute`) lies in the tiles beside the world's own and would move to a table of its own, written after the
  join. Not done.
- **The world.** An update is 145 to 165 thousand instructions (computer players' placement 82 thousand a call, Gaia's animals 85
  thousand, the path finder's A* about 1,000 instructions a node expanded); keeping it at 70 ms takes 38% of the first core. Running
  it beside the walk needs the walk to work from copies of what it reads (sprites' frames and the objects on the visible tiles), and
  the draw clears an animated sprite's `frame_changed`, which the game's logic reads: a frame behind would move the state sums.
- **The panels**: 36 thousand a picture (a training button's border sprite 50 thousand when it shows progress, the minimap 76 thousand
  every third picture).

A picture for checking: the script's `shot` step keeps the page and its palette in memory under a mark, and `rvc_cpu --save-ram FILE`
at the end lets a host read them out (marked `AOESHOT!`, then the number, width and height, 1,024 bytes of palette and the pixels).

### Scrolls checked (4 November 2026)

A report from the world after the round above: scrolling "completely broken", the view drawn shifted and torn (black where terrain
should be, its edges stair steps), and the caption's 22 frames a second where the picture changed far less often.

- **The scroll's machine copy is the first core's.** The view's job, on a worker, asked for the machine's copy of its rows itself
  (`OverlayMemCopy`, since the round before). CSR 0x8b0 reads 1 on the first core and 0 on a worker (docs/machine/gpu.md, "A program's
  copy"): a program asks only where it reads 1. On `rvc_cpu` and under `rvc_harness --cpu` a worker's copy is made all the same, so no
  run here showed it; a copy not made leaves the old rows where they were under the strips drawn for the new place, which is the
  picture reported. Now the job hands the copy to the first core (`aoe_view_first_copy`): it writes the request into a texel of its own and
  ends its passes until the first core, waiting for the walk or at the join, has made it. The first core reads 0x8b0 at the first view;
  where it reads 0 the scroll goes by words. On the calibrated clock the drag is as fast as before (15.8 pictures a second, 16.7 before).
- **The caption counts pictures.** With the world in slices a present can show nothing new (a slice with no view drawn). Those are
  not frames for the caption now (`seglNotAFrame`), while the view draws others; a menu's presents still count.
- **The check.** The script's `check` step finishes the world's update, draws a picture as the view makes it (its scrolls, its parts
  drawn again) and then one with every tile drawn again, from the same game, and compares the view's rows: `aoecheck: N pixels differ`,
  and both pictures as shots when they do. `test_all.py aoe-scroll-check` drags the map 20 s by the right button with the GPU's lists
  taken back every sixth pass (`RVC_DEVICE_EVERY=6`, as in a world) and the world in slices: 7 checks, 0 pixels, and the copy by the first
  core. `AOE_SCROLL_COPY=words AOE_WORLD=whole` gives the same 0. `RVC_RACES` with `RVC_RACES_NEAR=4`: 0 undone.

What only the world can confirm: that the copy a worker asked for was the one not made there (no harness here makes a worker's copy
otherwise), and the picture while dragging with a hand. The drag by the machine's pointer words (`--pointer-button`) logged no press in
two runs under `nano-X -p` and was not followed further.

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
- A menu with no picture of its own for 640 x 480 has the one for 800 x 600 brought down whole (3 November 2026; before, the middle of it
  was shown, `background_pos` 1, and the title's top and the award's side were cut off). `port/aoe_slp.h` reads the shape's first frame
  into pixels, makes each pixel of the smaller picture the average of the area it covers (800 to 640 is two source pixels each way,
  weights 4 and 1, 3 and 2, ...) in the menu's palette, and maps it back by the nearest of the palette's colours to its 15 bits; the
  result is a shape of copy and skip commands, which the game draws as it drew the other. `aoeprep` makes them on the workers once the
  trial is unpacked (`aoe-menus.bin`: the information files of `Interfac.drs` that have no picture for 640 and one of 800 x 600, each in
  its palette), and `TEasy_Panel::setup` takes the menu's from there (`aoe_picture_fit`, `port/aoe_scale.cpp`): 47 thousand instructions.
  One not there is made by the game itself: 72 million instructions on the first core when it averaged every pixel through a lazily
  filled table, 17 s at the world's 4 MIPS, which is why aoeprep makes them. In the trial only the main menu is such a menu (picture
  50103, palette 50501; aoeprep 2.6 s of `rvc_cpu`'s plain clock on 173 workers). `AOE_MENU_PICTURE=middle` is the way before.
  Looked at in captures under `rvc_harness --cpu --warp`: the main menu (whole, buttons where they were), the single player menu, the
  game's settings, the scenario builder's menu and the multiplayer connection, which have pictures for 640 of their own. Help shows an
  empty message box: Windows' help is not there.
- At that size the buttons are five columns, with "more" and "cancel" over the map's corners (`button_other_pic`), as the code lays them out.
- The victory and "history" screens ask for `bgA640` (`scr_end.cpp`, `scr_trb.cpp`): nothing in the game makes them (no `TribeEndScreen` or
  `TribeSelectTribeScreen` is constructed), so they cannot be reached.

The window can be resized and maximized, and the picture is stretched over it. Every pointer place the game gets goes through that
stretch (`aoe_win_map` in `port/window.c`, as `seglPicturePoint` does but not held to the picture): Nano-X's button and move events, and
the machine's pointer words read while a button is held (`held_by_words`), which go on past the window's sides. Before 3 November 2026
they did not, so in a rescaled window (in the world) a click landed elsewhere and the menus seemed stuck. `test_all.py aoe-resize`
maximizes the window (794 x 545) and clicks Single Player where it is drawn, then tiles it to the right half (394 x 545) and clicks Random
Map: both menus come. The script's `ready` makes the game write `/tmp/aoe-size` (`X Y W H`) at each change of the window's size after it.

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

A sound is a voice pointed at its wave where it lies, in the trial's files unpacked into memory (`/tmp/aoe`; the card reads RAM by its address) or in the ROM (`port/sound.cpp`, the game's `DirectSound` buffers; `TDigital::load` in `Sounddrv.cpp` asks `aoe_sound_load` and copies nothing). 141 waves of `Sounds.drs`, 14 of `Interfac.drs` and five files of their own; 16 voices, the one begun longest ago given up; the game's pan and volume by its numbers, and the options' slider where Windows had a mixer. No memory: a wave's first bytes are read once, and `Play` is a few stores.

A card's 16-bit samples must lie on an even address and most of the trial's waves do not. `make_data.py` used to write the DRS files anew with padding; the trial's own files are read now, so a wave on an odd address is copied once, at its load, to memory of its own, and a resource the game reads from a file it has not mapped is read into memory the same way. On `aoe-640`'s script with `RVC_SOUND=1` (2 November 2026): 8 waves loaded, all in memory, 5 of them copied (265,514 bytes), 64 played and 64 seen to end, and the same sums. `tools/aoe_sound_check.py` reads the addresses from the image's ROM, so it says nothing of waves in memory.

`AOE_SOUND_LOG=1` says each wave played and when its voice was seen to end, and `python tools/aoe_sound_check.py LOG IMAGE` reads each address from the image's ROM as the card will and finds that the samples are a wave of the trial, whole: on `aoe-play`, 8 plays of 6 waves, 0 not of the trial, 0 of silence, 8 seen to end and none in under half their length, each after the act that asks for one. What was heard was not checked: the check goes by the addresses, and no mix was captured. Over the same six timings the window gave 15.5 frames a second with sound and 16.1 with `AOE_SOUND=0`, inside the spread between timings (14.3 to 17.9 silent).

## Music

The game's own `TMusic_System` (`music.cpp`, not changed) plays it: what plays when, what comes next and how loud are all the game's. It asks Windows for a CD, which fails, then for the MCI sequencer and the synthesizer's mixer line, and `port/aoe_music.cpp` is those two on the card's FM voices. `info.use_music` is 1 again. So: `open.mid` at the first menu, once; in a game `music1.mid` to `music9.mid` in turn and round again, the next at each song's end by the game's `MM_MCINOTIFY`; `won.mid` or `lost.mid` at the end; the options' slider is the mixer's level, and its default of half (registry `Music Volume`, -5000) is the game's. A slider at the bottom turns music off as the game does. Windows' notice of a stopped song comes too, at the next look, since the game counts them.

The notes are made as OpenTTD's are: `linux/userland/shaderemu_fm.h` has the bank's instruments and the channel messages as notes given to voices, and OpenTTD's driver now goes through it too (`OPENTTD_MUSIC_SUM=all`: the 32 songs of OpenMSX give the same sums and notes as before). What is the port's own is 60 lines of Standard MIDI File reader, since OpenTTD's `MidiFile` is its own C++. So as in OpenTTD: program, volume, expression, pan and all-notes-off are followed, and the pitch wheel, the sustain pedal and drums outside the bank's 35 to 81 are not. `python tools/aoe_music_check.py --files IMAGE` says how many each song has. `Lost.mid` has 1,302 wheel messages and `Music1.mid` 1,554 (and 316 drums outside the bank, the most), so those will sound the least like the original.

**Voices.** The effects keep their 16 and music has the other 16 (`VOICES` in `aoe_music.cpp`), since 32 is all the card has. OpenTTD has 20, because its mixer takes 8. The songs want up to 19 to 32 at once, counting a two-voice instrument as two, so a note is cut when all 16 sound (the oldest goes): 7 to 179 notes of a song, at most about 3% (`Music1.mid` 122 of 3,766, `Music6.mid` 179 of 6,138). More voices for music would mean fewer than the effects' 16.

**A song on a worker.** The first core reads the file into sound memory and posts the job to a second worker (`mcw_open(2)`, the view has the first), which makes the notes and writes the tracks; the voices start when the first core next looks. All the job writes lies in sound memory or its own `SongJob`, 1.4 MB for the largest song the format can have here, and none of it in the program's data. With `AOE_VIEW=inline` or no workers the first core makes it. Nothing here moves the game: a song's end is a sent message with `AOE_TICK_MS`'s clock held (`aoe_clock_still`), not one of the queue, as a message taken is a look with the idle work not done. The state sums do not move: `aoe-music`, nine songs a round cut short at 4 s, says `5032c153` at update 1000.

**What it costs** (`rvc_cpu`, 1 November 2026): on the first core at most 289 instructions a look on average, about one look a frame, against about 340 thousand a frame, and that with the log's own prints in it; 25 to 42 thousand a song to read and post. On a worker 1.7 to 5.6 million a song. `game_par.py aoe --calibrated`, with `AOE_MUSIC=0` and without: core 0 342.0k and 342.9k a frame, 11.6 and 11.1 frames a second, all cores 586k and 616k. But two runs of that scene differed by 44k in core 0 before salts were tried, and salts did not move it, so that says little about 0.9k. The card's mix of 16 more FM voices was not measured: it is not in any `rvc_cpu` number, and it is the card's sound pass, not the first core.

**Checks.** `AOE_MUSIC_LOG=2 AOE_SOUND_LOG=1` on `rvc_cpu` with `RVC_SOUND=1`, then `python tools/aoe_music_check.py LOG IMAGE`, reads each MIDI file with a reader of its own and works out each note from the file and the bank: start, instrument, loudness, pitch against 440 Hz's scale, and length. Against the words the machine wrote for the card, on `open.mid`, `music1.mid` and `music2.mid` in play: all notes found, none other, pitch within 0.000%, every note held as the file says or cut (as many as the driver counted), all 350 instruments as the bank says, 16 voices kind 5 on voices 16 to 31 with the effects on 0 to 15, the card's position moving with its clock to the song's end, and the next song after it. Effects played beside the music in those runs. `RVC_RACES` with `RVC_RACES_NEAR=4`, 35 songs on the worker with `AOE_MUSIC_CUT=2`: 0 undone, and the near places are core 0 and core 1's, not the song's worker.

`--wav FILE` writes what those words sound like: `tools/sound_reference.py`'s FM, in the card's integers, with the effects' samples from the ROM at the clock values in the log (not interpolated). It is that model's mix and not a capture of the shader, since `rvc_cpu` mixes nothing and a capture wants the card. A song at the default level peaks at 5,442 of 32,767 alone, and with the effects of those runs at 22,119, none clipped. **Nobody has listened to it yet**: whether the instruments sound right is not checked by any of this, and it has not run on the card or in a world. The options' slider was not moved in a run, and `won.mid` and `lost.mid` were not reached by play: the first level the mixer gets is the only one exercised, and those two songs' notes were only read from the files.

A MIDI file is game data: it is unpacked from the trial's installer in the machine with the rest (`*.mid` of the starter's list), and the installer goes only into an image made with `--aoe`.

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
