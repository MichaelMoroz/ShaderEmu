# Nano-X

The Linux image has a window system: Nano-X from [Microwindows](https://github.com/ghaerr/microwindows),
the server with its window manager built in, and a handful of its clients. It draws through
the machine's GPU and takes the machine's keyboard and pointer.

    / # nx                      # server, the bar, a terminal
    / # nxcalc &                # more clients: nxeyes, nxtetris, nxmine, nxroach, nxev, demo-*
    / # NANOX_SIZE=1024x600 nx  # another screen size (640x480 by default)

The harness window's pointer and keys are the machine's (`input.md`). The bar along the
bottom (`linux/nanox/nxbar.c`) has a Start menu with the programs in the image and a clock;
the machine's clock chip is fed the host's local time. `glxgears` runs in a window.

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
the window is covered. The screen is a GPU list with one textured rectangle per window, lowest
first, copied into the display's framebuffer whenever a buffer or the arrangement changed.

So moving a window, raising it, or uncovering it by closing another costs a few words: no
pixels are copied by the CPU and no program is asked to draw again. Only a window that was
partly off the screen is repainted when it comes back, because drawing is still clipped to the
screen.

Drawing into a buffer goes through the GPU too (`gpu.md`):

| Operation | GPU command |
|---|---|
| filled rectangle, horizontal and vertical line | a coloured rectangle |
| text and other one-bit bitmaps | a mask texture (fragment mode 3), over a coloured rectangle when the background is drawn too. Glyphs of the built-in fonts are copied into GPU memory once and stay there; other bitmaps are copied for each list |
| copy from a window (scrolling) | a rectangle textured with that window's buffer |
| single pixels with nothing else queued, images, blending, XOR drawing, reading pixels | software, into the buffer |

Commands are queued for one buffer at a time and submitted when drawing moves to another
buffer, when software needs the pixels, or when the server is about to wait for events. Each
list starts by drawing the buffer as a texture and is copied back into it, so software and GPU
drawing can be mixed freely. A copy within a buffer reads RAM, which lacks what is still
queued, so it submits first if its source overlaps the queued area.

The cursor is not drawn at all: it is the display's cursor (`display.md`), an arrow by
default. The driver writes its position and, when the shape changes, a 32x32 image.

A program can also fill its window itself: `GrGetWindowInfo` returns the physical address of
the window's first pixel and the row length of its buffer, and `GrFlushWindow` tells the
server the pixels changed. The OpenGL driver does this.

GPU memory under Nano-X: window buffers in the first 16 MiB (`0x86000000`), then the control
words and the display's framebuffer, the server's command list, cursor, glyph cache and
scratch textures up to `0x87700000`; the last 4 MiB are left to one OpenGL program.

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
  the eight ANSI colours bright enough to read.

## Checking it

`NANOX_SOFTWARE=1` makes the same driver draw everything in software. The framebuffer in RAM
after a scene is then the reference for the same scene drawn with the GPU: the two were
identical pixel for pixel on three scenes (windows with text, a terminal, a game board; and
`nxbench`). Composition is done by the GPU in both.

`nxbench` (`linux/nanox/nxbench.c`) times drawing through the server, on an RTX 5090 with DXC:

| | software | GPU |
|---|---|---|
| 25 fills of 480x320 | 6.4 s | 0.27 s |
| 100 lines of text (5,400 characters) | 1.5 s | 0.74 s |
| 15 scrolls of the window by one line | 4.4 s | 0.27 s |
| 200 buttons (face, edges, label) | 1.6 s | 0.98 s |

## Where the time goes

`rvc_harness --pc-log FILE` records the guest's pc once per emulator frame and
`tools/pc_profile.py` turns that into a profile by function, across the kernel, the server
and a client (give it `nm -n` output for each; link one of two static programs at another
address so they do not overlap). Use it before optimising: the first profile here was 57%
system calls and 20% one software text routine, neither of which was expected.

Still in software: images, and anything blended. Not done: resizing shows no outline (the
window manager draws it on the desktop, which is now behind the windows), and only one
OpenGL program can run at a time.
