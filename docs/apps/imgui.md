# Dear ImGui

[Dear ImGui](https://github.com/ocornut/imgui) runs in the Linux image over the machine's
OpenGL: `imdemo` is a Nano-X window with a cube drawn through `programs/linux/gles.c` and
ImGui's windows on top of it. The library is as it comes (v1.91.5, fetched by the build, no
file of it changed); this repository has its platform and renderer, some 600 lines.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/tdawn/build.sh    (once: it makes the C++ driver)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/imgui/build.sh    (12 s the first time)
    python tools\make_linux_image.py                                 (then a new shell snapshot)

That is the image with the MMU. The world and `rvc_harness` with no arguments run Linux
without one, where the program is flat: `bash linux/nommu/build.sh imgui/build.sh`, then
`root.sh` and `image.py` (`docs/system/nommu.md`, "Building it").

In the guest: `nano-X -p & imdemo`, or "Dear ImGui" in the Start menu's Other.
`IMDEMO_SHOW=1` starts with ImGui's own demo window open and `IMDEMO_FRAMES=N` ends after N
frames with `imdemo: done`; every 30th frame prints an `imstat:` line.
`IMDEMO_HOLD=1` draws a picture the clock and the counts have no part in (for comparing two
builds: compose both screens from snapshots; only the caption's frames-a-second digits may
differ), `IMDEMO_SCROLL=N` makes the demo window low and N pixels down its list, so that
rows are cut by its edges, and `IMGUI_FONT_CHECK=1` builds the font's atlas as the library
would and compares it with the one written down.

## How it is drawn

ImGui hands over triangles with a position in pixels, a texture coordinate and a colour at
every vertex, blended in the order given, and a clip rectangle for each run of them.

- **Vertices** are the device's tagged ones (`docs/machine/gpu.md`: 16 bytes, vertex mode 0, whose
  positions are pixels): x and y as ImGui's floats, the texture coordinates in 1,024ths, and
  a tag that names the vertex's colour in a table of the colours the frame has used (a new
  table after 256). `seglScreenCompactSpace()` gives room in the frame and
  `seglScreenCompactUsed()` makes the draw: fragment mode 1 (a texture of words times the
  vertex's colour) in pass 5, blended by alpha with no depth test, so they lie over whatever
  OpenGL drew in the passes before, in list order. Two triangles on corners 0 1 2 and 0 2 3,
  which is how ImGui makes its rectangles and letters, go over as the device's quad of four.
  (They were the device's whole vertices, 64 bytes for every index: the picture is the same,
  pixel for pixel, in a sixth of the memory.)
- **The font** is ImGui's own atlas (512 x 64) as a texture of words, with the texel that is
  all white for everything that has no picture. One texture, so one fragment mode.
- **Indices** are followed by the program: the device draws lists of triangles or quads.
- **Clipping**: the device has no scissor. A triangle wholly inside its clip rectangle is
  copied, one wholly outside is left out, and one that crosses an edge is cut against the
  four edges and drawn as a fan (a few a frame: the last line of a scrolled list).
- **Memory**: `gles.c` is built here with a megabyte a frame (`SET_SIZE`), room for some
  15,000 vertices, where its default has room for 2,500 of this size.

Input is Nano-X's events: the pointer, its buttons and wheel, and the keyboard, with the
arrow keys and Space working ImGui's own keyboard navigation.

## A frame and a start gone through (12 October 2026)

| | instructions a frame | of them copying triangles | start |
|---|---|---|---|
| the demo's own window | 222,000 (was 290,000) | 67,000 (93,000) | 1.9M (13.2M) |
| with ImGui's demo window | 326,000 (was 413,000) | 105,000 (147,000) | |

- **The font is written down at build time.** `host/bake_font.cpp` is the library's own
  atlas code run on the host (with musl's `qsort`, since the packer's order of equal
  rectangles is the sort's): pixels, glyphs and texture coordinates go into `font_baked.h`,
  floats as their bits, and `Init` puts them where the atlas would have built them.
  Rasterising the font was most of a start. A program with fonts or atlas settings of its
  own gets the atlas built as before; with the font written down nothing can build the
  atlas again (the font's file is not in the program). `nxpath` links the same objects.
- **The copy.** A corner costs about 30 instructions where it cost 60: the clip rectangle
  and the corners compared as the bits of their floats (the floats only when that says
  no); three small loops (quads of one colour, triangles alone among quads, a run of
  triangles) and one function for every other shape — one loop with everything in it
  carried more values than the compiler has registers for and was 10 to 20% slower each
  time it was tried; three colours remembered, the newest first; a letter's four texture
  coordinates converted once; up to 16 triangles in a row among quads written as quads
  whose last two corners are one, so that an arrow in a list is not two runs.
- **Text from numbers** is `imgui_format.h`: imgui.cpp is built with
  `IMGUI_DISABLE_DEFAULT_FORMAT_FUNCTIONS` and the backend has the two functions. The C
  library's `vsnprintf` is two passes and long doubles for a float (some 12,000
  instructions a call); the common formats are written exactly (a float to at most nine
  places, in integers, rounded as the library rounds) and anything else is the library's.
  After a change, compare with the library on random numbers, on the host and in the guest.
- **Events** are asked for when the machine's input words (0x81000020 to 0x2c) change, for
  eight frames after, and twice a second: a question with no answer was 9,300 instructions
  a frame. Keys from `nxkey` do not change those words and can be half a second late.
- **Flags.** `-O3 -flto` with more inlining (`TUNE` in `build.sh`) is 7% of a frame over
  `-O2`; the objects are fat, and a link that does not ask for LTO still does it, in one job.

A change to the copy is checked by summing the triangles it hands the device (in order,
leaving out those with two corners the same) against the build before, in the held scenes,
and by the pictures. Where a frame goes now: the copy 27%, `AddPolyline` 14% (the plot: 89
lines of two points, 340 instructions each, the next thing worth a special case),
`RenderText` 14%, the cube's two rotations 1.6% (doubles in `sinf`).

## The copy on worker cores (13 October 2026)

The copy is a third of a frame, all of it stores into the frame's memory, and nothing in
the next frame of ImGui waits for it. So it is four worker cores' (`docs/machine/multicore.md`),
and the first core goes on to the next frame:

| under `rvc_cpu` at 3 million instructions a second | the first core copies | workers copy |
|---|---|---|
| the demo's own window: the first core's instructions a frame | 220,000 | 153,000 |
| frames a second (300 frames and the start) | 12.0 | 16.5 |
| with ImGui's demo window | 322,000 | 214,000 |
| frames a second | 8.3 | 12.2 |

On the shader machine (D3D11, fxc2; 300 frames and the start, from power-on): 16.1 frames
a second with the build before, 15.8 with this one and `IMGUI_COPY=inline`, 21.3 with the
workers; with ImGui's demo window 11.2, 10.9 and 15.6. The sums below are the same there.

- `ImGui_ImplShaderEmu_RenderAhead()` in place of `RenderDrawData()`: ImGui's commands are
  cut into pieces, each worker's share of the indices a run of them (a command is cut where
  a share ends, at a whole triangle), and each worker gets room in the frame for its
  vertices (five corners an index: no cut triangle is more) and two colour tables of its
  own (`seglFrameSpace` in `gles.c`). ImGui's vertex and index buffers are taken from its
  lists (the lists get those of two frames ago, which they empty before they write), so
  ImGui may make its next frame at once.
- `ImGui_ImplShaderEmu_RenderFinish()`, before the program draws its next frame: waits for
  the workers, makes a command of each run they wrote (`seglScreenCompactAt`), and the
  program swaps. `ImGui_ImplShaderEmu_RenderReady()` says whether they have finished, and
  `imdemo` asks four times while it makes its next frame (`show` in `demo.cpp`), so that
  the frame before is shown a few passes into the next one (a frame of ImGui is 18 passes,
  the workers' shares two to five) and not at its end. From the pointer to the picture is
  then some 24 passes where it was 27, with frames 18 passes apart.
- A worker that needs a third table or more than 256 runs says so, and the first core
  copies the whole frame itself, as it also does (`RenderAhead()` answers false) with no
  workers, under 1,200 indices, with a callback among the commands, and with
  `IMGUI_COPY=inline`. The copy is one piece of code for both (`struct copier`): the first
  core's and each worker's have their own state.
- The colours remembered last had one left from the table before when a table began: it
  was found again with that table's tag. In a picture that stands still the tag was the
  same number and nobody saw; with a table a worker it was another colour's (the plot's
  line black). All three are the table's first colour now.

`IMGUI_SUM=1` prints an `imsum:` line a frame: the triangles handed to the device in order,
those with two corners the same left out, each corner with its colour for its tag. The
same for both ways of copying, every frame of 40, in `IMDEMO_HOLD=1` alone, with
`IMDEMO_SHOW=1` and with `IMDEMO_SCROLL=300` and `1000`.

## What it costs

On the harness (D3D11, about 2.7 million instructions a second with the desktop running), before
the sections above (12 and 13 October 2026) cut a frame and its copy:

| | instructions a frame | of them copying triangles | vertices |
|---|---|---|---|
| the demo's own window | 300,000 | 100,000 | 1,570 |
| with ImGui's demo window open beside it | 423,000 | 155,000 | 2,360 |

About six frames a second: usable, not smooth. The copy is some 43 instructions an index;
ImGui's own work (layout, text, building the lists) is the larger part and is the library's.
The counts are the processor's cycle counter, which also counts what the window system did
meanwhile.

Where a frame with the demo window goes (`--pc-log`, `tools\pc_profile.py` with the mangled
`nm -n` of `imdemo`: demangled names have spaces, which the tool does not read):

| | of the frame |
|---|---|
| `ImGui_ImplShaderEmu_RenderDrawData` (the copy) | 36% |
| `ImFont::RenderText` | 11% |
| `ImDrawList::AddPolyline` (anti-aliased lines) | 8% |
| `ImFont::CalcTextSizeA` | 3% |
| `ImDrawList::PrimReserve`, `AddLine`, `PlotEx`, `NewFrame`, `Begin`, `ItemAdd`, each | 1 to 2.5% |
| the kernel and the window system | 9% |

It is instructions that cost here, not stores. The tagged vertices and the quads took the
frame's vertex data from 380 KB to 64 KB and the passes a full write cache ended from 2,000
to 740 in 150 frames, and 150 frames took 24.2 to 25.4 s before and after: the copy is
still a test of every triangle against its clip rectangle and a walk along the indices
(154,000 instructions a frame before, 150,000 after).

The frame's list is not waited for (`seglSwapEarly`, `docs/machine/gpu.md`): the swap's `pause`
ended a pass of the machine a frame and lost what was left of it (`docs/machine/boot.md`, "Passes
that end early"). Nothing has to settle: a frame writes only its own triangles and colour
table, in the set the list being drawn does not read, and the font's texture is written
once. With the demo window open, 150 frames (`tools\tick_util.ps1`): 98.5% of the passes'
instructions with the wait (`IMDEMO_SWAP=wait`), 100.0% without, and 2.97 against 3.17
million instructions a second.

Not done, in the order of what they would give: a frame only when something changed (an
interface that waits is nine tenths of the time); the device reading ImGui's own buffers
(a vertex kind with an index buffer and a clip rectangle a command: the copy would be two
of the machine's copies, and it is the device's shader, its model and Unity's copy of it
that change); ImGui's lines without anti-aliasing (`AntiAliasedLines`, `AntiAliasedFill`:
a different picture); and pictures other than the font.
The pointer was not tried: `nxkey`'s click lets the button go wherever the host's pointer is,
and ImGui presses a button on release. The keyboard was (`nxkey down ... space` opens the
demo window through the checkbox).
