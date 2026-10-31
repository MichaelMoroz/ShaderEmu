# Micropolis

`nxmicropolis` is Micropolis, the city simulator of 1989, on the desktop. The simulation is
MicropolisCore's C++ engine, unchanged but for the fixes below. The window around it is new
(`linux/micropolis/nxmicropolis.c`). The map, its sprites and the tools are drawn by the GPU;
the other windows are drawn by Nano-X.

    / # nxmicropolis                                     # a new city
    / # nxmicropolis /usr/share/micropolis/cities/haight.cty
    / # MICROPOLIS_SUM=CITY,TICKS[,SEED] nxmicropolis    # no window: a run's sum (CITY a file, or a scenario 1 to 8)

    wsl -- bash linux/nommu/build.sh micropolis/build.sh    # 6 s; after Nano-X's, Tiberian Dawn's and libmedia's builds

The Start menu has it under Games, the desktop has its icon (the X11 release's map,
`linux/apps/icons/micropolis.png`), and a `.cty` file opens in it and has that icon in Files
(`docs/system/open.md`). The system sheet in the world lists it.

## Where it comes from

`build.sh` fetches [MicropolisCore](https://github.com/SimHacker/MicropolisCore) at
`9d5b400b`, only the engine (`packages/micropolis-engine/src`) and the game's data
(`content/micropolis`: the cities, the texts, the pictures and the sounds). Upstream has
become a web app; the engine is built here without `emscripten.cpp` and `callback.cpp`, and
`linux/micropolis/engine.cpp` is the callback instead: what the engine says (its messages, a
query's answer, a sound, the budget, a scenario won or lost) goes into a queue that the
window reads (`mp.h`). The C++ is compiled as Tiberian Dawn's is, by the `rv32-c++` driver
over musl, with `std::map`'s tree and `std::list`'s links compiled from libstdc++'s
sources (`tree.cc`, `list.cc`).

`micropolis.patch` changes the engine in five places:

- The constructor leaves `callback` unset, so a load that calls back before `setCallback`
  jumps through garbage; it is set to NULL. A stray `printf("init")` is taken out.
- A sprite was `malloc`'d, and it holds a `std::string`: it is made with `new`.
- `loadScenario` built two `std::string`s from NULL.
- `voteProblems` read one past the end of its table (`problem > PROBNUM`, now `>=`). That read
  was why the build computer and the machine gave different sums for one run.
- `Quad` is `int` (32 bits, as on the machine and in 1989), not `long`; and with a test's seed
  `randomlySeedRandom` uses that seed and not the clock.

The engine is compiled with `-fwrapv`: its sums overflow 32 bits in a large city, and without
it the two compilers worked that overflow out differently.

## Is it the same game

The build compiles the engine for the build computer too and runs Haight-Ashbury 1,000 ticks
(`micropolis-sum`). The machine must give the same sums (`tools/test_all.py`, case
`micropolis`):

| Run | Sum |
|---|---|
| `haight.cty,1000` | `909517ac` (February 2002, population 205,940, funds 982,014) |
| `1,500` (Dullsville) | `e14d1a50` |
| `4,800` (Bern) | `15c7151d` |
| `8,1500` (Rio de Janeiro) | `75e86f73` |

A sum is the map's tiles, the funds and the population after the run, each tick followed by a
step of the whole map's animation as the original front end made it. Under AddressSanitizer
and UBSan, Haight-Ashbury for 3,000 ticks and every scenario for 1,500 ran clean.

## The pictures

`pack.py` turns upstream's PNGs into what the GPU reads:

- **The map** is a layer of tiles (`SEGL_TILES`, `docs/machine/gpu.md`): 960 tiles of 16 x 16 in 14
  colours. Its cells are the engine's own tile numbers, kept in a ring of 64 x 64 cells that
  the view scrolls through: a scroll writes only the cells that came into view, and a tick
  writes the cells the view shows. Animated tiles (fire, traffic, the radar) are stepped only
  where the view is: the whole map's animation was a third of a tick.
- **The sprites** (train, helicopter, aeroplane, ship, monster, tornado, explosion, bus; 61
  frames) are a second tile layer, byte 0 a hole, drawn with `seglSprite`.
- **The tools** are the original's 64 x 64 icons halved, over the window's grey.
- **The overview map** colours a tile by the mean of its picture (`overview.bin`).

## The windows

- **The main window**: the menus (City, Speed, Options, Disasters, Windows, Help), the tools
  and the demand for zones on the left, the map, and two status lines (the city, the date,
  the funds, the population, the speed and the tool; the last message). The left button uses
  the tool (roads, rails and wires along a drag), the right button drags the map, and the
  wheel and the arrows scroll it. Space pauses.
- **Map**: the whole city at two pixels a tile, with the view's rectangle. Its layers are the
  original's: zones of each kind, transport, power (powered, unpowered, conductors),
  population, growth, traffic, pollution, crime, land value and the fire and police cover. A
  click or a drag moves the view there. It is drawn again every two seconds.
- **Budget**: the tax rate and the funding of roads, fire and police as sliders, what each
  asks for and what was spent, the cash flow, and "Budget by itself". A change of funding is
  paid (or given back) at once, for the rest of the year. With "Budget by itself" off, the
  year's budget opens this window and stops the city until "Go on".
- **Evaluation**: the mayor's approval, the worst problems, the population and its change, the
  assessed value, the city's class, the game level and the score.
- **Graphs**: residential, commercial and industrial population, cash flow, crime and
  pollution over 10 or 120 years, each line on or off.

Cities are saved in `/persistence/micropolis` (`docs/system/persist.md`). The speed, "Budget by
itself" and disasters are the registry's (`micropolis.speed`, `micropolis.autobudget`,
`micropolis.disasters`).

## Sound

Thirteen of upstream's sounds (the engine's explosions, horns, sirens, monster and "uh-uh",
and the bulldozer and building for the tools) are decoded from MP3 by libmedia's build for
the build computer (`media_test-host`) and kept in `sounds.bin` as unsigned bytes at 22,050
Hz, 330 KB. The sound card plays them where they lie in the ROM (`docs/machine/sound.md`), on four
voices. A city's sound is heard only near the view, from its side; a disaster's far off is
heard quietly. A sound still playing is not started again (a drag of roads is one sound).

## Speed

One core runs the simulation and the window. Haight-Ashbury, about 200,000 people, at the
middle speed on `rvc_cpu`: ten ticks and ten frames a second, 1.7 million instructions a
second of the 2 to 3 the machine has, nearly all of them the simulation's scans (`mapScan`,
the traffic's `tryGo`, power). The scans that write only their own map (pollution, crime,
land value, population density, the fire and police cover) are the candidates for the
worker cores; that is not done.

## Testing it

`MICROPOLIS_REPORT=1` prints where the window is, where the other windows open, and every five
seconds the ticks, frames and sounds of those seconds. With it, a script can click the menus
and windows by `nxkey` at the places printed.

## The licence

The code is GPLv3 with EA's added terms (`/usr/share/micropolis/COPYING`,
`MicropolisGPLLicenseNotice.md`): no "SimCity", no claim of a tie to EA, and a modified
version marked as modified (Help, About says so). "Micropolis" is a trademark of Micropolis
GmbH, used under the Micropolis Public Name License (`MicropolisPublicNameLicense.md`), whose
notice the game shows when it starts.
