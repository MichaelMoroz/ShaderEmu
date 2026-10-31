# Plan: Age of Empires, the 1997 trial

Where it got to, 31 October 2026: `docs/games/aoe.md`. A game starts and plays on the machine, on private images, without sound and on the first core alone; where that page and this plan differ the page is right.

Written 17 October 2026. Nothing was built then. It comes from reading the trees below at the
commits given, from unpacking the trial's installer, and from reading `linux/tdawn`,
`docs/games/tdawn.md`, `docs/machine/sound.md` and `docs/system/fonts.md`. What the reading did not settle is under
"To find out first".

| | upstream | commit | licence |
|---|---|---|---|
| The game | `github.com/dcostap/aoe1-decomp` | `f4fdf3f` (13 Oct 2026) | CC0, over code recovered from Microsoft's binary (below) |
| The trial | `archive.org/details/aoe-trial-1997`, `MSAoE.exe` | md5 `dadea8f91c7e2b70ff5c2be7aad6c682` | the trial's EULA (below) |

## Which code

There is no released source. Three projects were read:

- **aoe1-decomp** is a decompilation, function by function, of the Rise of Rome beta of 17 July
  1998 (`empiresx.exe`): 5,013 of 5,426 functions byte for byte the original's, the rest
  checked for the same logic, and replays of recorded games play the same. It is the whole
  game: the AI, scenarios, campaigns, the scenario editor. It is also still the Win32 program,
  built by Visual C++ 5.0 and MASM.
- **FolkertVanVerseveld/aoe** (`3234eac`, Apache 2.0) is a rewrite on SDL2, OpenGL and ImGui.
  Its README: "only very basic gameplay and only multiplayer mode", and a computer player
  "only gathers resources and does nothing until attacked". Not the game yet.
- **openage** says "gameplay is basically non-functional", knows no AoE1 trial, and needs
  Python, Qt6 and OpenGL 3.3.

The plan is the decompilation's.

## The licences

- **The trial.** Its EULA lets anyone copy and give away "a true and complete copy" for no
  charge, with the EULA, but says its "component parts may not be separated", and forbids
  reverse engineering. So the build fetches the installer, as `linux/tdawn/build.sh` fetches
  C&C's demo. Whether the files taken out of it may go into `linux/prebuilt`, the shared
  image or the world is the developer's call; until then an image with them is a private one.
- **The code.** CC0 is the decompiler's choice; the code is Microsoft's game recovered from its
  binary. Also the developer's call, before step 1.

## The trial's data

The installer is a 7-Zip archive, 24.8 MB, 47 MB unpacked:

- `Graphics.drs` 12.6 MB (622 SLP), `Interfac.drs` 14.4 MB (SLP, shapes, WAVs, palettes),
  `Sounds.drs` 4.4 MB (141 WAVs), `Terrain.drs`, `Border.drs`. Mapped from the ROM, as other
  games' data is, they cost no RAM.
- `Empires.dat`, raw deflate, 142 KB to 758 KB, header **`VER 3.4`**.
- `Multip_1.scn` (scenario version 1.10), two campaigns (`Reigno_1.cpn`, `Armies_1.cpn`).
- The AI's `.ai`, `.per`, `.rps`, `.cty` files; `language.dll` (the strings); 12 MIDI files;
  TrueType fonts (Copperplate, Arial, Comic Sans).

**The decompilation will not read this dat.** `RGE_Game_World::init` (`src/world.cpp:649`)
refuses anything but `VER 3.7`, which is Rise of Rome's. The scenario reader takes versions
1.01 to 1.11, so the trial's scenario should load.

## What has to change

### The assembly, rewritten in C

Three files, 13,839 lines:

- `src/Asm_draw.asm`, 12,461 lines, is the sprite and span drawing: nine entry points
  (`ASMDraw_Sprite`, `ASMDraw_HLine`, `ASMSet_Shadowing(Ex)`, `ASMSet_Surface_Info`,
  `ASMSet/Get_Color_Xform`, `ASMSet/Get_Xlate_Table`), and the rest unrolled pieces for each
  span length and alignment. In C it is one loop per kind of span, some hundreds of lines.
  Called from `sprite.cpp`, `Shape.cpp`, `drawarea.cpp`, `view.cpp` and five panels.
  `tests/test_drawing_engine.py` gives what each entry draws: the C one is checked against it
  on the host, pixel for pixel.
- `src/Inf32.asm`, 925 lines, is a streaming inflate that stops when its input runs out and
  goes on at the next call (`Inf32BufSize`, `Inf32Decode`). zlib's or miniz's inflate behind
  the same two functions.
- `src/Drawarea_asm.asm`, 453 lines: `TDrawArea::PtrClear`, `PtrSpanCopy`, `OverlayMemCopy`.

### Windows, replaced by the machine

Counted in `src/*.cpp`:

- **DirectDraw** (4 files): the game draws into its own 8-bit pages. They become GPU memory
  and a palette in a Nano-X window, as `linux/tdawn/host.c` does.
- **GDI text**: `TextOut` and its kin 121 times in 19 files, `SelectObject` 68. The game's
  text goes through Windows' fonts. The trial's TrueType faces are rasterised when the image
  is made, at the sizes the game asks for, as `docs/system/fonts.md` does the image's fonts, and a
  small `TextOut` draws them into the 8-bit page.
- **Messages and input**: `WM_` 98 times in 13 files, `SendMessage`/`PostMessage` 49, the
  cursor 29. A message pump of our own, fed from Nano-X's events.
- **The clock**: `timeGetTime` and the like 292 times in 36 files. The machine's clock word,
  as Tiberian Dawn's clock reads it.
- **DirectPlay** (5 files) and Winsock: out. Single player only; the multiplayer screens say so.
- **DirectSound** (1 file): effects as voices pointed at their WAVs in the DRS, as Quake's are.
- **Music** is MCI (27 uses, CD audio and MIDI). The trial has MIDI only. Doom plays scores on
  the sound card's FM voices (`docs/machine/sound.md`); the trial's MIDI could go the same way, or wait.
- **The registry** (2 files): keys of the image's registry (`registry.h`).
- **`language.dll`**: its string table taken out when the image is made, and `LoadString`
  reads that.
- Files: `_open` and `fopen` 129 times, with Windows paths (`data2\empires.dat`). Backslashes
  and the case of names, as Tiberian Dawn's file search was fixed.

### The compiler

Visual C++ 5.0's dialect and the game's own `operator new` and globals: Tiberian Dawn needed
`-fno-lifetime-dse` and its globals made in source order (`docs/games/tdawn.md`); expect the same.
No `try`, `throw`, `dynamic_cast` or `typeid` in `src/`, so `cxxrt.cpp` over musl, as there.

## Steps

0. **The two licence decisions above.**
1. **On Windows, unchanged: does the game run on the trial's data?** Built as its README says,
   with the trial's files in `runtime/`. It refuses the dat until step 2 is done, so steps 1
   and 2 are one loop. This is the cheapest test of the data before any porting.
2. **The dat.** Either convert `VER 3.4` to `VER 3.7` once, when the image is made, with
   genieutils (it names an AoE beta version apart from 1.0; not checked that it reads 3.4), or teach `world.cpp`
   the older layout. The first leaves the game's code as it is.
3. **The assembly in C**, checked on the host against `tests/test_drawing_engine.py` and the
   original's inflate.
4. **A native Linux build with no window.** gcc, the Win32 calls behind stubs, the game started
   into the trial's scenario with the AI playing itself. Its frames and instructions are
   counted with a tool on the host, which tells whether step 6 is weeks or out of reach.
5. **The machine: `linux/aoe/build.sh`** as `linux/tdawn/build.sh` is (fetch at a commit,
   a patch, the platform files, static, LTO), the window, text, input, sound, the Start menu's
   `Games/Age of Empires`. Run on `rvc_cpu` first, then the card, and judged on a page.
6. **Speed.** The minimum was a Pentium 90 at 640x480; C&C's was a 386 at 320x200 and needed
   several rounds to reach 15 frames a second at 2 MIPS. The map and its sprites on the GPU,
   as Tiberian Dawn's map is; then the AI and path searches on worker cores, under the rules
   of `docs/machine/multicore.md` (`RVC_RACES`). Measured on `rvc_cpu --ips 2000000`, missions as
   they play.

## To find out first

- The **licence decisions** (step 0).
- Whether the decompilation **plays on the trial's data** once the dat says `VER 3.7`, or how
  far the two layouts differ (steps 1 and 2).
- **What a frame costs** natively (step 4), against Tiberian Dawn's.
- **Memory**: the game asked 16 MB on Windows; with the DRS files in the ROM it should fit, and
  `python linux/nommu/memory.py` must still pass with it in the chains.
- How much of the drawing **can move to the GPU**: whether the game reads back from its pages
  what it drew there, as Tiberian Dawn did (`Catch_Up`).
