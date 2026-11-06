# Dear ImGui

[Dear ImGui](https://github.com/ocornut/imgui) runs in the Linux image over the machine's
OpenGL: `imdemo` is a Nano-X window with a cube drawn through `programs/linux/gles.c` and
ImGui's windows on top of it. The library is v1.91.5, fetched by the build, with `imgui.patch`
("Drawn later", "The library's own costs"; `host/make_patch_edits.py` makes it on a clean tree:
`git diff` there is the patch); this repository has its platform and renderer. `nxpath`'s window
is ImGui's too (`docs/apps/raytrace.md`, "The window").

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

The first core's instructions a frame, `IMDEMO_HOLD=1`, the mean from `imstat: frame 30` to frame 150 (`RVC_PROF`'s
count, the kernel's and the window system's instructions on that core in it), 6 November 2026, against the
committed code of the day before built with the held picture's count of vertices (the same sums, frame by frame):

| | before | now | |
|---|---|---|---|
| the program's own window | 155,172 | 56,718 | 2.74 times fewer |
| with ImGui's demo window | 218,521 | 87,260 | 2.50 times fewer |

The busiest of the six copying workers: 41,600 and 52,800 a frame. What gave it, each measured on the demo window:
drawn later (218,500 to 151,000), the plot's segments as one record and the plot on the workers (135,700 to
114,000), the counts (to 101,000), the shares cut by work (the busiest worker 103,000 to 53,000), the fallback
window bare, the keys' walks skipped, the resize grips quiet, a text's width in a tight loop, a run a texture,
more inlining and the counts made again (to 88,200), a style colour's last words kept (to 87,260).

The table of 31 October 2026, before all that (`rvc_cpu`, from the frame after `imstat: frame 30` to the end of
300 frames; the frames a second calibrated, so programs' clocks saw the card's pace):

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

## Drawn later

With worker cores (`RenderAhead`), a list's text, lines and filled shapes are not made by the first core: the
library hands them to the backend's hooks (`GImDrawListDefer`, `imgui.patch`), which keep what each was asked with
(the text, the points) in the list's `_Deferred` words and put in its place a triangle on one vertex three times
(x all ones, y the record's place, u the indices it can make at most, v its work). The worker that copies the list
meets that triangle in `one_shape()` and makes the record by the library's own function (`ImFont::RenderText`,
`AddPolyline`, `AddConvexPolyFilled`) on a list of its own (`struct scratch`: 2,048 vertices, the shared data a copy
of the context's with its own temporary points), and copies those triangles: the same triangles in the same order,
so `IMGUI_SUM` is the same as the first core's copy and as the library unchanged.

- `AddLine`'s one-segment lines of one colour and width with nothing between them in one command are one record
  (a plot's grid), and `PlotLines()` hands over its values and scale (`PlotEx` in the patch) in parts of 12
  segments, each a marker of its own: a part goes through the segments before it for their floats alone.
- What reads back the vertices it made (the colour wheel's hue ring, angled table headers, a rounded picture)
  draws with the flag off.
- The shares are cut by work, not indices: a command keeps its records' work and corners in `UserCallbackData` and
  `UserCallbackDataSize` (the patch adds them where commands are made one), a marker's are in its vertex. A
  worker out of room, or a third colour table, gives the frame to the first core (`failed`).
- A run of the device's is a texture's, not a command's: the clip rectangles are the copy's.
- `IMGUI_LATER=0`: the first core draws them, as before; `IMGUI_SHARES=1` prints each worker's instructions every
  30 frames. The scratch lists are 16 bytes apart (a worker's each).

## The library's own costs

The rest of `imgui.patch`: a window's first place in floats (the soft doubles were two conversions and four sums a
window); the fallback window (`Debug##Default`) begun with no title bar, background, resizing or scroll bars after a
frame with nothing in it (it is not drawn then); the 154 named keys' two walks skipped while no key is down, owned
or locked and none was touched (`KeysIdle`, cleared wherever a key's state or owner is written); a window's resize
grip or border that the pointer is not over and that is not held left out of `ItemAdd()` and `ButtonBehavior()`
(`ResizeHandleQuiet`); a text's width a run of plain characters at a time at the font's own size; a style colour's words kept with the four floats
they were made of, bit for bit (`GetColorU32`).

## Played counts

The library and the backend are compiled with GCC's counts of four held runs (`imgui.profile`, `IMGUI_PROFILE=off`
none): `IMGUI_PROFILE=count` builds the counting program in its own folder, and `counts.py` runs it and gathers the
counts (its docstring has the steps). The sources are named from the build's folder, so the counts hold wherever
the tree lies. After a change to the patch or the backend, make them again: a function whose code changed has none.
`IMGUI_AUTO`/`IMGUI_GROWTH` set the inlining (120 and 200; 60 and 80 were 1.2% more, 250 and 500 the same).

## A panel over a program's picture

`nxpath`, `nxray` and `nxphys` draw their controls with ImGui over what they draw with OpenGL, through
`imgui_panel.h` (`linux/imgui/imgui_panel.cpp`, built into the library's folder with the backend). It feeds ImGui the
program's own window's events (`imp_event`: a move or a press over the program's picture is not told to ImGui, which
would want frames for it; a move that leaves ImGui's windows is, once), holds a press until a frame has had the
pointer where it is (a tab, which other items may lie over, takes a click only where the pointer was the frame
before), and says when ImGui wants a frame (`imp_busy`: an input settling, or the program's `imp_wake` when what
the panel shows changed, its figures once a second).

`nxpath` makes a frame of ImGui's every time it draws (`imp_begin`, `imp_end`). `nxray` and `nxphys` show pictures
far more often than their panels change, so `imp_frame` makes one only when it is wanted: in between the frame made
last is put into each picture's list again, `ImGui_ImplShaderEmu_RenderAgain()`. With `KeepFrames(true)` the
backend keeps what `RenderDrawData()` gave the device, its runs' tables and corners, in one of two blocks of GPU
memory of its own in turn (96 KB each, taken by `imp_keep()` before the program takes the rest; a list still being
drawn reads the other), and a picture's list then draws them where they lie: no copy and no frame of ImGui's. A
frame that does not fit there is kept in the program's memory and copied into the frame. A frame for an input is
followed by one more (what it opens, a popup, is laid out in one and drawn in the next); one for the program's
figures is not. Both programs make it on the first core, without workers (theirs trace and move boxes): one
frame a second of figures cost `nxray` 7 to 13 thousand of the first core's instructions a picture (36 to 42
thousand before, 43 to 55 after, 255 workers, 320 x 240; rays a second within their spread), and `nxphys` its
pictures a second from 27 to 25 or 26 (the heap, 255 workers; substeps kept at 60 a second). The copy of the kept
words into each list, tried first, cost nothing that showed: those instructions were ImGui's frames, two a second
until a frame for figures had no second one. A program whose OpenGL library is 16.16 builds `gles.c` with floats
(`SEGL_FLOAT`), as the backend writes ImGui's floats: its own 16.16 calls are taken as they were.

## Not done

A frame only when something changed for `imdemo` and `nxpath`; the device reading ImGui's own buffers (a vertex kind with
an index buffer and a clip rectangle a command: the copy would be two of the machine's copies,
and it is the device's shader, its model and Unity's copy of it that change); ImGui's lines
without anti-aliasing (`AntiAliasedLines`, `AntiAliasedFill`: a different picture); and pictures
other than the font.

The pointer is not tried: `nxkey`'s click lets the button go wherever the host's pointer is, and
ImGui presses a button on release. The keyboard is (`nxkey down ... space` opens the demo window
through the checkbox).
