# Nano-X

The Linux image has a window system: Nano-X from [Microwindows](https://github.com/ghaerr/microwindows),
the server with its window manager built in, and a handful of its clients. It draws through
the machine's GPU and takes the machine's keyboard and pointer.

    / # nx                      # server, the desktop's picture, the bar, a terminal
    / # nxcalc &                # more clients: nxeyes, nxtetris, nxmine, nxroach, nxev, demo-*
    / # NANOX_SIZE=1024x600 nx  # another screen size (640x480 by default)

The harness window's pointer and keys are the machine's (`input.md`). The bar along the
bottom (`linux/nanox/nxbar.c`) has a Start menu with the programs in the image and a clock;
the machine's clock chip is fed the host's local time. `glxgears` runs in a window. The
desktop has a small set of programs of its own, listed under "The desktop's programs".

## Building it

    wsl -- bash /mnt/c/.../linux/kernel/build.sh    # once: cross compiler, kernel
    wsl -- bash /mnt/c/.../linux/nanox/build.sh     # about 10 s after the first time
    python tools\make_linux_image.py

- `linux/userland/toolchain.sh` builds the C library the programs link: musl, static, for
  RV32IMA without an FPU. The compiler's own runtime library assumes an FPU, so integer and
  soft-float helpers come from compiler-rt and the 128-bit `long double` ones from libgcc's
  soft-fp sources. `rv32-cc` is the resulting compiler driver.
- `linux/nanox/build.sh` fetches Microwindows at a fixed commit, applies
  `microwindows.patch`, adds the three drivers in that directory, builds with
  `linux/nanox/config` and leaves stripped binaries in `build/images/linux/root`, a tree the
  image builder copies into the root filesystem.
- Nothing fetched is kept in this repository.

## Drivers

- `kbd_shaderemu.c` and `mou_shaderemu.c` read the kernel's evdev devices (`input.md`): Linux
  key codes through a US layout, and an absolute pointer in display pixels.
- `scr_shaderemu.c` is the screen, described below.

## The screen is composed

Every top-level window (with its frame and everything inside it) has a buffer of its own in
GPU memory, and so has the desktop. Programs draw into their window's buffer whether or not
the window is covered. The screen is the display's layer mode (`display.md`): the driver keeps
a table with one entry per window, lowest first, and whatever shows the display composes them.
There is no composed framebuffer in RAM and nothing to redraw when a buffer changes.

So moving a window, raising it, or uncovering it by closing another costs a few words: no
pixels are copied by the CPU and no program is asked to draw again. A window keeps being
drawn past the right and bottom edges of the screen, so it comes back as it was. Coordinates
below zero are still clipped: a strip that was off the left or top edge is cleared and its
program asked to draw it again, which a terminal cannot do for text already printed.

Drawing into a buffer goes through the GPU too (`gpu.md`):

| Operation | GPU command |
|---|---|
| filled rectangle, horizontal and vertical line | a coloured rectangle |
| text and other one-bit bitmaps | a mask texture (fragment mode 3), over a coloured rectangle when the background is drawn too. Glyphs of the built-in fonts are copied into GPU memory once and stay there; other bitmaps are copied for each list |
| copy from a window (scrolling) | a rectangle textured with that window's buffer |
| a PPM file (`GrDrawImageFromFile`), at any size | the file's rows read straight into texture memory, as many as fit (1.8 MB), and a rectangle textured with them (fragment mode 4, three bytes a pixel); the GPU does the scaling |
| single pixels with nothing else queued, other images, blending, XOR drawing, reading pixels | software, into the buffer |

Commands are queued for one buffer at a time and submitted when drawing moves to another
buffer, when software needs the pixels, or when the server is about to wait for events. Each
list starts by drawing the buffer as a texture and is copied back into it, so software and GPU
drawing can be mixed freely. Submitting takes no system call (the lock word and `pause`,
`gpu.md`); the server waits until the copies counter says its picture is in RAM. A copy within a buffer reads RAM, which lacks what is still
queued, so it submits first if its source overlaps the queued area.

The cursor is not drawn at all: it is the display's cursor (`display.md`), an arrow by
default. The driver writes its position and, when the shape changes, a 32x32 image.

A program can also fill its window itself: `GrGetWindowInfo` returns the physical address of
the window's first pixel and the row length of its buffer, and `GrFlushWindow` tells the
server the pixels changed. The OpenGL driver does this.

GPU memory under Nano-X: window buffers in the first 16 MiB (`0x86000000`), then the control
words and the display's framebuffer, the server's command list, cursor, glyph cache and
scratch textures up to `0x87700000`; the last 4 MiB are left to one OpenGL program.

## The bar, and what a program adds to it

`nxbar` has the Start menu, a button for every open window (a click brings the window to the
front) and the clock. The menu is whatever the files `/usr/share/nxapps.*` list, one
`Label=command` a line, so a program gets into it by having its build put such a file in the
image: `linux/nanox/build.sh` writes `nxapps.10-nanox`, `doom.sh` writes `nxapps.50-doom`.

A window's frame has, left of its close box, a maximise box (the window fills the screen above
the bar, and goes back on the second click) and a minimise box (the window goes away; its
button on the bar, greyed meanwhile, brings it back).

A window that changes size gets another buffer, which starts as a copy of the old one: what
was drawn stays, and only the area the window gained is cleared and its program asked to
draw. (A terminal's text survives, and the terminal takes as many columns and rows as now fit.) The
screen driver counts new buffers at `0x87000064`, and a program that draws into its window
itself (the OpenGL library) asks the server where its window is whenever the count moves.

The window manager's record of a window (`nanowm.h`) has our fields at its end. The build
does not recompile files for a changed header, so a field added in the middle leaves the
files the patch does not touch using the old layout: delete `obj/nanox/wm*.o` after such a
change.

## The desktop's programs

`linux/apps/` (built by `linux/apps/build.sh` after Nano-X, each one file on `ui.h`):

| Program | What it does |
|---|---|
| `nxedit FILE` | a text editor: arrows, Home, End, Page Up and Down, a click places the cursor; Ctrl+S saves, Ctrl+Q quits |
| `nxfiles [FOLDER]` | a file manager: a click selects, a second click (or Enter) opens: a folder, a picture in the viewer, a program, anything else in the editor. Up, Open, Edit, New file, Delete, Refresh |
| `nxpaint [FILE]` | pen, eraser, line, box, filled box in sixteen colours and three sizes; Save writes a PPM file (`/root/picture.ppm` unless a file was named) |
| `nxview FILE` | shows a picture as large as fits its window: PPM through the GPU, PGM, BMP, GIF and XPM through the engine's decoders |
| `nxsettings` | picks the desktop's picture or colour, and says what the machine is. `nxsettings apply` only puts the chosen desktop up (the `nx` script runs it) |
| `nxmon` | instructions a second over the last minute (from `rdcycle`), how busy Linux is, memory in use |
| `nxterm` | Microwindows' terminal, patched: it follows its window's size, and Shift+Page Up and Down look back through the last 400 lines |

**Pictures for the desktop** are `/usr/share/wallpaper-NAME.ppm`, made on the host by
`python tools\make_wallpaper.py [FOLDER ...]`: the Unsplash photographs listed in
`linux/apps/wallpapers.txt` (a name and the photo's id a line; fetched, never kept in this
repository) and every picture in the folders given. One 1920 or more across is brought down to
fit 1920x1080, one 1280 or more to fit 1280x720. Run it before `make_linux_image.py`. The
choice is kept in `/tmp/nxwallpaper`; a new image shows `wallpaper-fox.ppm`.

A picture is not decoded by anyone: a PPM file is already three bytes a pixel, so the screen
driver reads the rows it needs into GPU memory and the GPU samples them. The 1623x1080
desktop picture takes 0.2 s to put up on a 640x480 screen (drawing it pixel by pixel through
`GrArea` took 6 s). It is not clipped by child windows.

The server is started with `-p`: without it Nano-X ends when its last program has gone, and
the program that puts up the desktop's picture is the first to come and go.

## Changes to Microwindows

`linux/nanox/microwindows.patch`, applied by the build:

- **One read per client, not three system calls per request.** Stock Nano-X handles one
  request per trip around its main loop: a `select`, a `read` for the header, a `read` for
  the rest. On this machine a system call costs a couple of thousand instructions, so a
  button cost far more in system calls than in pixels, and the screen driver was asked to
  submit after every request. The server now takes everything a client has sent with one
  `read` and handles all of it before it waits again.
- **A cursor the screen driver shows.** With `gd_hwcursor` set the engine never draws, hides
  or saves under the cursor, so drawing no longer stops to step around it.
- **Partly hidden rectangles are filled by clip rectangle**, not one row at a time.
- **Composition** (`gd_compositor`): a window draws on the surface the screen driver gives its
  top-level window; windows no longer clip each other; moving, raising, lowering and
  unmapping a top-level window repaint nothing; the driver is shown the visible windows in
  order when it composes.
- The default cursor is the familiar arrow; new windows step across the screen instead of
  piling up at one place.
- The terminal is 80x24 (it was 50 rows, taller than the screen) and light on black, with
  the eight ANSI colours bright enough to read. It keeps what every character cell shows
  beside the drawing, which is what the look back through earlier lines is drawn from, and
  tells the program inside (`TIOCSWINSZ`) when its window changes size.
- **A driver may draw a picture file itself** (`gd_drawpicture`, asked first by
  `GdDrawImageFromFile`).

## Checking it

`NANOX_SOFTWARE=1` makes the same driver draw everything in software. The framebuffer in RAM
after a scene is then the reference for the same scene drawn with the GPU: the two were
identical pixel for pixel on three scenes (windows with text, a terminal, a game board; and
`nxbench`). Composition is done by the GPU in both.

`nxbench` (`linux/nanox/nxbench.c`) times drawing through the server, on an RTX 5090 with DXC:

| | software | GPU |
|---|---|---|
| 25 fills of 480x320 | 4.7 s | 0.19 s |
| 100 lines of text (5,400 characters) | 1.4 s | 0.64 s |
| 15 scrolls of the window by one line | 4.5 s | 0.22 s |
| 200 buttons (face, edges, label) | 1.4 s | 0.82 s |

`glxgears` in a 300x300 window runs at about 500 frames a second, with the emulator at
4.0 million instructions a second (`--stats-after S` prints both rates after a warm-up).

## Where the time goes

`rvc_harness --pc-log FILE` records the guest's pc once per emulator frame and
`tools/pc_profile.py` turns that into a profile by function, across the kernel, the server
and a client (give it `nm -n` output for each; link one of two static programs at another
address so they do not overlap). Use it before optimising: the first profile here was 57%
system calls and 20% one software text routine, neither of which was expected.

Still in software: images, and anything blended. Not done: resizing shows no outline (the
window manager draws it on the desktop, which is now behind the windows), and only one
OpenGL program can run at a time.
