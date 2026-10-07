# Tiberian Dawn

Command & Conquer (1995) runs in the Linux image: [Vanilla Conquer](https://github.com/TheAssemblyArmada/Vanilla-Conquer),
the portable version of the game's released source, as a Nano-X client. The game draws its
own 320x200 screen in software and the GPU shows it; nothing of the game is drawn by the GPU yet.
The window is the largest whole multiple of that the display has room for: 960x600 on the
desktop's 1280x720, which costs the game nothing (the GPU does the stretching).

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh     (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/tdawn/build.sh     (9 s; the first time it also fetches)
    python tools\make_linux_image.py                                  (then a new shell snapshot)

In the guest: `nano-X -p & tdawn` at the console (it is also listed for the desktop's Start
menu, which was not tried). The data is the 1995 demo's (GDI missions 1, 3, 5, 6 and 10; the
menu starts the first), which the build fetches and the repository does not keep. There is no
sound.

## What the build is made of (`linux/tdawn`)

- `build.sh` fetches Vanilla Conquer at a fixed commit, applies `vanilla-conquer.patch`, adds
  the files below and builds with CMake and the image's toolchain.
- **No C++ library.** The toolchain's libstdc++ is for glibc and an FPU. The game uses neither
  exceptions nor run-time type information, and little of the library: g++ is given
  libstdc++'s headers only, over musl, and `cxxrt.cpp` is the rest (allocation, the string
  class's code, what a failure calls). `cxx/bits/c++locale.h` stands in for the one header
  that needs glibc's types.
- `host.c` is the machine: a Nano-X window, two 8-bit pages and a pointer picture in GPU
  memory, the palette, events. C, because the Nano-X and OpenGL headers are.
- `shaderemu.cpp` is the game's side of it: what `video_sdl2.cpp` and `wwkeyboard_sdl2.cpp`
  are for SDL.
- `-fno-lifetime-dse`: the game's `operator new` marks an object active before its constructor
  runs. A compiler may drop that store; a native `-O3` build without the flag loads a mission
  with no walls and no tiberium.

## What was changed for this machine

- **The clock.** The game asks the time constantly, and each answer was a system call and a
  64-bit division (a library call of some 400 instructions): 45k instructions a game frame.
  It reads the machine's clock word and divides in 32 bits.
- **Showing a frame.** The game finishes a frame by copying its hidden page to the visible
  one, 64,000 bytes. Both pages are GPU memory: the copy is put off, the hidden page is
  shown, and the copy is made only if something reads the visible page or draws on it
  (`Catch_Up` in `shaderemu.cpp`; the figures say how often).
- **Finding files.** Every file was looked for in any spelling, a folder at a time from the
  root down, and reading the root folder reads the ROM: 6,282 folder listings before the first
  mission, 212M instructions. Only the file's own name is matched now, and the starter runs
  the program from a folder of its own (the game lists its program's folder too): 70M.
- **Waiting** ends the machine's frame with `pause`; the screen is shown at most 60 times a
  second unless the game asks.

## Speed

The game wants 15 frames a second at its default speed, and a frame of the game is one step
of its logic and one drawing of what changed. With no delay between frames, 300 frames of
each mission, nobody playing:

| Mission | Objects | Logic | Drawing | A frame | Frames/s, D3D11 + FXC (3.0M/s) | D3D12 + DXC (3.8M/s) |
|---|---|---|---|---|---|---|
| 1 | 22 to 36 | 76k to 128k | 141k to 386k | 239k to 540k | 5.8 to 12.2 | 8.6 to 15.6 |
| 3 | 82 | 177k to 207k (466k as it starts) | 67k to 162k | 280k to 567k | 5.4 to 10.3 | 7.8 to 13.1 |
| 10 | 75 | 133k to 213k (356k at times) | 115k to 468k | 281k to 701k | 4.4 to 10.3 | 6.4 to 12.5 |

So it plays, at a half to two thirds of its speed where it matters (FXC is what VRChat runs).
Starting takes about 25 seconds to the first mission's first frame (DXC). Where a frame goes
in mission 10:

- 21% in one sprite routine (`BF_Ghost_Fading_Trans`: shadows and the shroud, two table
  lookups a pixel), 10% in `memcpy` (the terrain's 24-pixel rows, a call a row), 2% drawing
  text. Drawing is software throughout, and this is the part the GPU can take.
- 7% looking for targets (`Evaluate_Cell`, `Greatest_Threat`): logic, as with Doom's sight checks.
- Mission 10 takes about 40 seconds to start, 15 more than mission 1: 11% of that run's
  samples are musl's `qsort`, called from the scenario file's index (`IndexClass::Is_Present`).

Each run prints these figures (`tdstat:` lines, every five seconds):

    TDAWN_STATS_MS=N    how often (0: never)
    TDAWN_FRAMES=N      N game frames with no delay between them, then leave
    TDAWN_STOP_FRAME=N  hold the picture of game frame N; TDAWN_NO_DELAY=1 gets there with no delay
    TDAWN_SCEN=N        start at mission N
    TDAWN_SEED=N        the same game every run
    TDAWN_AUTO=1        Return is pressed through the menus (with FROMINSTALL: straight to the mission)
    TDAWN_INPUT_LOG=1   print the keys and buttons that arrive, and where in the window
    TDAWN_RESIZE=WxH    give the window that size after its 60th frame, as a drag would

## Checking it

The game is integer-only and, with a seed, plays the same on any CPU. Both ends print a sum
of the game's state (every unit's, soldier's and building's place and strength, and the map's
overlays): build the patched tree natively with a `host.c` that only writes the shown page
to a file, hold both at one frame, and compare the sums and the pictures.

    TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=10 TDAWN_STOP_FRAME=300 tdawn FROMINSTALL
    tdawn: stopped at frame 300: SCG10EA, 15 units, 23 infantry, 38 buildings, 552 overlays, state f7650705

The sums of missions 1, 3 and 10 agree with an x86-64 build at frame 300, on both backends.
The pictures (`--gpu-capture`: the game's picture is the top left of the GPU's target) differ
only where real time shows: the pointer, a tooltip under it, the water's colour cycling and
the credits counter counting.

## Not done

- Drawing on the GPU: sprites and terrain as commands instead of pixels. House colours and the
  cloak effect need fragment modes the GPU does not have.
- Sound. The full game's data (the demo's is 6 MB; the CDs' movies and music are not wanted).
- Red Alert is the same tree (`BUILD_VANILLARA`) and is not built.
- With `NANOX_SIZE=640x480` the window has no frame (640x400 leaves no room for one) and is
  left through the game's own menu (Escape).
