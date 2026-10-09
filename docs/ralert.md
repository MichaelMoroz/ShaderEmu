# Red Alert

Command & Conquer: Red Alert (1996) runs in the Linux image, from the same source tree as
Tiberian Dawn ([Vanilla Conquer](https://github.com/TheAssemblyArmada/Vanilla-Conquer)) and
on the same layer under it (`docs/tdawn.md`, `linux/tdawn`): a Nano-X window, the map drawn by
the GPU, the sound card. Its screen is 640 x 400, shown one to one.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/tdawn/build.sh     (first: it makes the C++ driver)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/ralert/build.sh    (20 s; the first time it also fetches)
    python tools\make_linux_image.py                                  (then a new shell snapshot)

In the guest: `nano-X -p & ralert` at the console, or Red Alert in the Start menu's Games (not
tried from there). The data is the 1996 demo's: one Soviet and one Allied mission and
skirmish games, 24 MB, which the build fetches and the repository does not keep. Effects,
speech and the demo's two tunes play on the sound card.

## What the build is made of (`linux/ralert`)

- `build.sh` puts the tree at Tiberian Dawn's commit, applies Tiberian Dawn's patch (the
  shared files) and then `vanilla-conquer.patch` (Red Alert's own files, and two shared
  ones), copies `linux/tdawn`'s `host.c`, `shaderemu.cpp`, `soundio_shaderemu.cpp` and
  `cxxrt.cpp` in, and builds `vanillara` with `-DSHADEREMU_RA`.
- `gl.cpp` is Tiberian Dawn's map renderer with Red Alert's names. What differs: a cell is
  "mapped" once any of it shows and "visible" when all of it does (the other way round there);
  a map may be 128 x 128 cells; a shadow has as many levels of darkness as its pixels ask for.
- The sound pack (`ralert-sound.pak`, 12.8 MB, 294 sounds) is made by Tiberian Dawn's tool.
  The sounds are in MIX files inside the demo's two, behind an encrypted index: the build
  compiles the source's own `vanillamix` for the host and unpacks them with it.

## What was changed for this machine

Everything under "What was changed" in `docs/tdawn.md` that is in the shared files applies.
In Red Alert's own:

- **No hash of the data.** A MIX file is hashed (SHA-1) as it is read into memory, to
  compare with the sum it carries: 24 MB, some 500M instructions before the title.
- **Fading tables** (the palette's nearest colour to each faded colour, 65,000 distances a
  table, forty tables before the first mission) are made by a search that looks only at
  colours whose green is near enough to matter. The same tables.
- **The vortex's tables**, sixteen more for each theatre, are made when a vortex first
  appears. (The GPU's scene does not draw the vortex at all yet.)
- **Music that is not there.** A mission names its tune; the demo has two (Hell March and
  Crush). A tune that did not start was asked for again every frame, and each asking lists
  the folder, a million instructions: now another tune is picked, and after four failures
  none.
- **The radar's cover plate**, 160 x 141 pixels and three buttons, was drawn again whenever a
  unit moved (the unit asks for its dot on a radar that is not there): 160k a frame.
- **An object that moved does not change the cells it left and came to**, and a unit that
  looks again at a cell it has seen does not either. The game asks all of those cells to be
  drawn again, a hundred a frame; on the GPU only the scene is made again, and the renderer
  keeps a mark on every cell that has nothing of its own to draw.
- **The first run** goes past the menu straight into the Soviet mission, as after an
  install: the starter writes the setting that says the game has been run.
- The logic that repeats itself is Tiberian Dawn's, with the state sums unchanged: threat of
  a unit that steps, houses found from a table, the layer's sort, and the computer's count of
  units no team has (a unit is asked whether it could join only when one is wanted).

## Speed

On D3D11 with fxc2 (3.5M to 4.1M instructions a second), no delay between frames, nobody
playing, in thousands of instructions a game frame:

| | Objects | Logic | Drawing | A frame | Game frames/s |
|---|---|---|---|---|---|
| Soviet mission, the game's own drawing | 64 | 235 to 300 | 390 to 520 | 660 to 810 | 5 to 6 |
| Soviet mission | 64 | 132 to 175 | 69 to 93 | 222 to 288 | 12 to 18 |
| Allied mission | 92 to 98 | 159 to 173 | 91 to 134 | 289 to 336 | 12 to 14 |

The map's scene is 66k to 106k of the drawing: the view is 20 x 16 cells, four times Tiberian
Dawn's. From the command to the menu is about a minute (190M instructions with the desktop's
own start), and as long again to a mission's first frame.

## Checking it

    TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7 RALERT_FRAMES=300 ralert      (wait for "ralert: done")

- `TDAWN_AUTO=1` presses Return through the menus; `RALERT_SIDE=allies` or `soviet` answers
  the question of sides; `RALERT_SEED`, `RALERT_FRAMES`, `RALERT_STOP_FRAME` (with
  `RALERT_NO_DELAY=1`), `RALERT_STATS_MS`, `RALERT_RENDER=soft` and `RALERT_REFRESH` are
  Tiberian Dawn's switches. `rastat:` lines say where a frame went.
- A change to the game must leave its `state` sum the same: `01e0dd60` for the Soviet mission
  (SCU01EA) and `ea72a5d1` for the Allied one (SCG02EA) at frame 300 with seed 7.
- The picture against the game's own drawing (`RALERT_RENDER=soft`, both held at one frame):
  5% of pixels differ, by more than 16 levels 0.1% to 0.9%. Those are shadows: a true blend
  where the game's table picks the palette's nearest dark colour, which on snow is 27 levels off.
- Sound: `--fixed-dt 0.004 --sound-capture` and `tools\sound_reference.py ... --rom`; a mission's
  first 300 frames, 83 seconds of sound, are the model's in every sample. `TDAWN_SOUND_LOG=1` says what
  the game asked for.
- Palette cycling goes by the clock (Tiberian Dawn's goes by frames in a run with no delay):
  a few dozen pixels of lamps differ between two runs of one build.

## Not done

- Playing it by hand: the pointer and keys are Tiberian Dawn's code, untried here.
- Skirmish, the Allied mission beyond 300 frames, saving and loading.
- The vortex in the GPU's scene; ground units sorted among walls as the game sorts them
  (the scene draws overlays first, then every object).
- Scroll steps sized by time, and frames left undrawn when late (`docs/tdawn.md` has both).
- Movies. The start: a minute to the menu.
