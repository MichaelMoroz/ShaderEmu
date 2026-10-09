# Dear ImGui

[Dear ImGui](https://github.com/ocornut/imgui) runs in the Linux image over the machine's
OpenGL: `imdemo` is a Nano-X window with a cube drawn through `programs/linux/gles.c` and
ImGui's windows on top of it. The library is as it comes (v1.91.5, fetched by the build, no
file of it changed); this repository has its platform and renderer, 270 lines.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/tdawn/build.sh    (once: it makes the C++ driver)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/imgui/build.sh    (12 s the first time)
    python tools\make_linux_image.py                                 (then a new shell snapshot)

In the guest: `nano-X -p & imdemo`, or "Dear ImGui" in the Start menu's Other.
`IMDEMO_SHOW=1` starts with ImGui's own demo window open and `IMDEMO_FRAMES=N` ends after N
frames with `imdemo: done`; every 30th frame prints an `imstat:` line.

## How it is drawn

ImGui hands over triangles with a position in pixels, a texture coordinate and a colour at
every vertex, blended in the order given, and a clip rectangle for each run of them.

- **Vertices** are the device's whole ones (`docs/gpu.md`: 64 bytes, vertex mode 0, whose
  positions are pixels), as floats. `seglScreenSpace()` gives room in the frame and
  `seglScreenUsed()` makes the draw: fragment mode 1 (a texture of words times the vertex's
  colour) in pass 5, blended by alpha with no depth test, so they lie over whatever OpenGL
  drew in the passes before, in list order.
- **The font** is ImGui's own atlas (512 x 64) as a texture of words, with the texel that is
  all white for everything that has no picture. One texture, so one fragment mode.
- **Indices** are followed by the program: the device draws triangle lists. A position and a
  texture coordinate are copied as the words they are, and a colour becomes four floats only
  when it differs from the vertex before.
- **Clipping**: the device has no scissor. A triangle wholly inside its clip rectangle is
  copied, one wholly outside is left out, and one that crosses an edge is cut against the
  four edges and drawn as a fan (a few a frame: the last line of a scrolled list).
- **Memory**: `gles.c` is built here with a megabyte a frame (`SET_SIZE`), room for some
  15,000 vertices, where its default has room for 2,500 of this size.

Input is Nano-X's events: the pointer, its buttons and wheel, and the keyboard, with the
arrow keys and Space working ImGui's own keyboard navigation.

## What it costs

On the harness (D3D11, about 2.7 million instructions a second with the desktop running):

| | instructions a frame | of them copying triangles | vertices |
|---|---|---|---|
| the demo's own window | 300,000 | 100,000 | 1,570 |
| with ImGui's demo window open beside it | 423,000 | 155,000 | 2,360 |

About six frames a second: usable, not smooth. The copy is some 43 instructions an index;
ImGui's own work (layout, text, building the lists) is the larger part and is the library's.
The counts are the processor's cycle counter, which also counts what the window system did
meanwhile.

Not done: quads for text (the device takes four corners for six vertices, which would cut a
third of the copy), a frame only when something changed, and pictures other than the font.
The pointer was not tried: `nxkey`'s click lets the button go wherever the host's pointer is,
and ImGui presses a button on release. The keyboard was (`nxkey down ... space` opens the
demo window through the checkbox).
