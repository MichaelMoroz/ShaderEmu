# Dear ImGui

[Dear ImGui](https://github.com/ocornut/imgui) runs in the Linux image over the machine's
OpenGL: `imdemo` is a Nano-X window with a cube drawn through `programs/linux/gles.c` and
ImGui's windows on top of it. The library is as it comes (v1.91.5, fetched by the build, no
file of it changed); this repository has its platform and renderer, some 600 lines.

    wsl -- bash linux/nommu/build.sh tdawn/build.sh    (once: it makes the C++ driver)
    wsl -- bash linux/nommu/build.sh imgui/build.sh
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1                                 (then a new shell snapshot)

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

## What it costs

`rvc_cpu`, 31 October 2026, from the frame after `imstat: frame 30` to the end of 300 frames; the
frames a second are calibrated, 31 October 2026, so programs' clocks saw the card's pace:

| | the first core's instructions a frame | of them copying triangles | vertices | frames a second |
|---|---|---|---|---|
| the demo's own window, the first core copies (`IMGUI_COPY=inline`) | 220,000 | 66,000 | 1,568 | 24.5 |
| the demo's own window, workers copy | 156,000 | | 1,564 | 34.4 |
| with ImGui's demo window, the first core copies | 320,000 | 106,000 | 2,352 | 17.0 |
| with ImGui's demo window, workers copy | 217,000 | | 2,344 | 24.8 |

An interface that waits still draws every frame: a frame only when something changes is what
would give the most.

- **The font is written down at build time.** `host/bake_font.cpp` is the library's own
  atlas code run on the host (with musl's `qsort`, since the packer's order of equal
  rectangles is the sort's): pixels, glyphs and texture coordinates go into `font_baked.h`,
  floats as their bits, and `Init` puts them where the atlas would have built them.
  Rasterising the font would be most of a start. A program with fonts or atlas settings of its
  own gets the atlas built by the library; with the font written down nothing can build the
  atlas again (the font's file is not in the program). `nxpath` links the same objects.
- **Text from numbers** is `imgui_format.h`: imgui.cpp is built with
  `IMGUI_DISABLE_DEFAULT_FORMAT_FUNCTIONS` and the backend has the two functions. The C
  library's `vsnprintf` is two passes and long doubles for a float; the common formats are
  written exactly (a float to at most nine places, in integers, rounded as the library rounds)
  and anything else is the library's. After a change, compare with the library on random
  numbers, on the host and in the guest.
- **Events** are asked for when the machine's input words (0x81000020 to 0x2c) change, for
  eight frames after, and twice a second. Keys from `nxkey` do not change those words and can
  be half a second late.
- **Flags.** `-O3 -flto` with more inlining (`TUNE` in `build.sh`); the objects are fat, and a
  link that does not ask for LTO still does it, in one job.
- **The swap.** The frame's list is not waited for (`seglSwapEarly`, `docs/machine/gpu.md`; waiting
  would end a pass of the machine a frame, `docs/machine/boot.md`, "Passes that end early"). Nothing
  has to settle: a frame writes only its own triangles and colour table, in the set the list
  being drawn does not read, and the font's texture is written once. `IMDEMO_SWAP=wait` waits.

## The copy

A corner is tested fast: the clip rectangle and the corners are compared as the bits of their
floats (the floats only when that says no). There are three small loops (quads of one colour,
triangles alone among quads, a run of triangles) and one function for every other shape: one
loop with everything in it carries more values than the compiler has registers for. Three colours
are remembered, the newest first (all three the table's first colour when a table begins, or a
tag from the table before is found again); a letter's four texture coordinates are converted
once; and up to 16 triangles in a row among quads are written as quads whose last two corners are
one, so that an arrow in a list is not two runs.

A change to the copy is checked by summing the triangles it hands the device (in order,
leaving out those with two corners the same) against the build before, in the held scenes,
and by the pictures.

## The copy on worker cores

The copy is a third of a frame, all of it stores into the frame's memory, and nothing in
the next frame of ImGui waits for it. So it is four worker cores' (`docs/machine/multicore.md`),
and the first core goes on to the next frame.

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
  the frame before is shown a few passes into the next one and not at its end.
- A worker that needs a third table or more than 256 runs says so, and the first core
  copies the whole frame itself, as it also does (`RenderAhead()` answers false) with no
  workers, under 1,200 indices, with a callback among the commands, and with
  `IMGUI_COPY=inline`. The copy is one piece of code for both (`struct copier`): the first
  core's and each worker's have their own state.

`IMGUI_SUM=1` prints an `imsum:` line a frame: the triangles handed to the device in order,
those with two corners the same left out, each corner with its colour for its tag. It must be
the same for both ways of copying, every frame of 40, in `IMDEMO_HOLD=1` alone, with
`IMDEMO_SHOW=1` and with `IMDEMO_SCROLL=300` and `1000`.

## Not done

A frame only when something changed; the device reading ImGui's own buffers (a vertex kind with
an index buffer and a clip rectangle a command: the copy would be two of the machine's copies,
and it is the device's shader, its model and Unity's copy of it that change); ImGui's lines
without anti-aliasing (`AntiAliasedLines`, `AntiAliasedFill`: a different picture); and pictures
other than the font.

The pointer is not tried: `nxkey`'s click lets the button go wherever the host's pointer is, and
ImGui presses a button on release. The keyboard is (`nxkey down ... space` opens the demo window
through the checkbox).
