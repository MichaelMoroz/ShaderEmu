# Nano-X

The Linux image has a window system: Nano-X from [Microwindows](https://github.com/ghaerr/microwindows),
the server with its window manager built in, and a handful of its clients. It draws through
the machine's GPU and takes the machine's keyboard and pointer.

    / # nx                      # server, the desktop's picture, the bar, a terminal
    / # nxcalc &                # more clients: nxeyes (the right button closes it), nxtetris, nxmine, nxev, demo-*
    / # NANOX_SIZE=640x480 nx   # another screen size (1280x720 by default, what the world shows)

The machine starts `nx` by itself when it boots, unless the host says not to (bit 0 of the host
flags, `gpu.md`; the image's init reads it). The harness asks for the desktop in its terminal
mode (`--no-desktop` leaves it at the shell) and not in runs with other arguments, which wait
for the shell prompt (`--desktop` asks for it there); a resumed shell gets `nx` typed for it.

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
| a string of a built-in font, all of it in view | one text command (`docs/gpu.md`): the font's glyphs and their table are put into GPU memory the first time, and the engine hands the driver the whole string (`gd_drawstring`) instead of a bitmap a character |
| other text and one-bit bitmaps | a mask texture (fragment mode 3), over a coloured rectangle when the background is drawn too. Glyphs of the built-in fonts are copied into GPU memory once and stay there; other bitmaps are copied for each list |
| copy from a window (scrolling) | a rectangle textured with that window's buffer |
| a PPM file (`GrDrawImageFromFile`), at any size | one rectangle whose texture is the file where it lies in the ROM (fragment mode 4, three bytes a pixel); the GPU does the scaling and nothing is read. A file that is not in the ROM has its rows read into texture memory, as many as fit (1.8 MB) at a time |
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
front) and the clock. The Windows key opens and closes the menu from anywhere (a key the bar
grabs with `GrGrabKey`). The menu is whatever the files `/usr/share/nxapps.*` list, one
`Label=command` a line, or `Folder/Label=command` for a program in a folder, so a program gets
into it by having its build put such a file in the image: `linux/nanox/build.sh` writes
`nxapps.10-nanox`, `doom.sh` writes `nxapps.50-doom`. The folders are Games, Utilities and
Other, in the order the files first name them; a click on one shows its programs in the
menu's place, with a way back as the bottom row. `NXBAR_SHOW=1` (or `=Games`) starts the bar
with the menu open, for a test without a pointer: the menu is the layer 150 pixels wide.

## The keyboard

Everything on the desktop can be worked without a pointer.

- **One window has the keyboard**, and its caption is the coloured one: a new window, the
  window a button is pressed in (anywhere in it, which also brings it to the front), or the
  one its button on the bar names. When that window closes or is minimised, the keyboard
  passes to the window then in front; it is never left with no window while one is shown.
- **The window manager's keys**, with Alt or Ctrl held (the harness's host takes Alt+Tab and
  Alt+F4 for itself, so use Ctrl there): Tab is the next window (the one in front goes to the
  back; with Shift the one at the back comes forward; a minimised window is shown again), F4
  closes the window, F9 minimises it, F10 maximises it and puts it back. These keys reach no
  program.
- **The Start menu** has the keyboard while it is open, and opened by the Windows key it has
  its first row picked out: Up, Down, Home and End move, a letter goes to the next row that
  starts with it, Enter or Right opens a folder or starts a program, Left or Backspace leaves
  a folder, Escape leaves a folder and then closes the menu.
- **In a program** Tab and Shift+Tab go round its controls where it has several (a line round
  the one that has the keyboard: `ui_focus`, `ui_tab` in `ui.h`), Enter or Space presses a
  button, and a list takes the arrows, Page Up and Down, Home and End (`ui_list_key`).

`nxkey` presses keys for a test, through the keyboard's evdev device: `nxkey meta down enter
500 ctrl+tab` (names or characters, `ctrl+`, `alt+`, `shift+` before one, a number to wait
that many milliseconds, `X,Y` for a click of the left button there).

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

Scrolling is shared: `ui.h` has the scroll bar (`ui_scrollbar`, `ui_scroll_to` for a click or
a drag on it) and `ui_wheel` for a notch of the wheel, which arrives as a button-down event.
A program that takes clicks must check `ui_wheel` first, or a notch is a click to it.

`linux/apps/` (built by `linux/apps/build.sh` after Nano-X, each one file on `ui.h`):

| Program | What it does |
|---|---|
| `nxedit FILE` | a text editor: arrows, Home, End, Page Up and Down move and with Shift select, a click places the cursor and the pointer held down selects, the wheel and a scroll bar move through the text. Ctrl+C, Ctrl+X and Ctrl+V copy, cut and paste through the desktop's clipboard, Ctrl+A selects everything, Ctrl+Z undoes (512 changes back; letters typed one after another are one), Ctrl+F asks in the status line what to find and F3 finds the next. Ctrl+S saves; Ctrl+Q quits, and asks once more when the text was not saved |
| `nxfiles [FOLDER]` | a file manager, a tile with an icon for every entry: a click selects, a second click (or Enter) opens: a folder, a picture in the viewer, a page in the browser, a program, anything else in the editor. Wheel and scroll bar. Up, Open, Edit, New (a file) and Folder, which ask for the new entry's name in the status line, Rename (F2), Copy, Cut and Paste (Ctrl+C, Ctrl+X, Ctrl+V: the entry is remembered in `/tmp/clipboard.files`, so it can be pasted in another Files window, under a free name: `name-2.txt`), Delete (a folder only when it is empty). Keys: arrows, Page Up and Down, Home, End, a letter for the next name that starts with it, Backspace for the folder above, Delete, F5 reads the folder again; Tab goes to the buttons and back |
| `nxweb [ADDRESS]` | a browser for HTML and the plainer part of CSS (`docs/fetch.md`): Back, Home, Reload, an address to type, wheel and scroll bar. Keys: arrows and the paging keys scroll, Tab goes from link to link (its address in the status line) and Enter follows, Backspace goes back, F5 reloads, Ctrl+L opens the address |
| `nxpaint [FILE]` | pen, eraser, line, box, filled box in sixteen colours and three sizes; Save writes a PPM file (`/root/picture.ppm` unless a file was named). Keys: a tool's first letter, 1 to 3 for the size, [ and ] for the colour, Ctrl+S |
| `nxview FILE` | shows a picture as large as fits its window: PPM through the GPU; PNG and JPEG decoded on the worker cores into a PPM file in memory, which is then shown the same way (below); PGM, BMP, GIF and XPM through the engine's decoders. Escape or q closes it. `nxview --decode FILE OUT.ppm` decodes without a window and says what it took; `NXVIEW_WORKERS=N` asks for N workers (3), `NXVIEW_SUM=1` prints a sum of the result |
| `nxsettings` | the settings, a tab each: the desktop's picture (the image's, any PPM file through the file chooser, or a colour), the screen's size (only those the host says it can show, in its flags word; the desktop is taken down whole and started again with it: `nx restart`), the sound card's volume, and what the machine is. From a script: `nxsettings apply` (the chosen desktop, as the `nx` script runs it), `size WxH`, `volume 0-100`, `choose [FOLDER]` (the file chooser alone; prints the path). Keys: Tab goes round the row of tabs and the tab's controls, arrows change the tab, the list's row or the slider; in the file chooser arrows, a letter, Enter, Backspace for the folder above, Escape |
| `nxmon` | three plots of the last two minutes, a sample a second: instructions a second (from `rdcycle`; the plot's top doubles as needed), how busy Linux is (kernel and programs stacked), memory in use (with the files written since the start, which live in memory). On a machine with worker cores (`docs/multicore.md`) what they run is stacked on the first plot, and a line says how many of them work, sleep, are idle and are parked. Under them: time running, load, processes, files kept in memory, task switches and interrupts a second, and the three busiest programs (looked at every third second). The plots are pixels the worker cores write into the window's buffer, some sixteen times a second (`docs/multicore.md`): they slide between samples, and the window system draws nothing of them. Under that a list of the programs, busiest first: a click or the arrows choose one, and End program (or Delete) asks it to go; pressed again for the same program it is made to (`SIGTERM`, then `SIGKILL`). The first program and the monitor itself are not in the list. Escape or q closes it |
| `nxterm` | Microwindows' terminal, patched: it follows its window's size, and Shift+Page Up and Down or the wheel look back through the last 400 lines, with a mark at the right edge for how far. The pointer held down selects text, from the cell it went down on to the cell it is over, and letting go copies it to the clipboard; the right button, Shift+Insert and Ctrl+Shift+V type the clipboard's text |

**PNG and JPEG pictures** (`linux/apps/ui_image.h`) are decoded by the program that shows
them, on the machine's worker cores (`docs/multicore.md`), into a PPM file the display then
draws. What costs is not the arithmetic but the writing: a core keeps about 6 KB of new
stores a pass and a picture is megabytes, so each stage writes memory of its own and the
pixels are written once, into the file's own pages, by as many cores as there are. For a
PNG, three workers follow each other: one inflates the stream, one takes the filters off
the rows that are there, one writes those rows as pixels. For a JPEG the first worker reads
the Huffman codes into a list of the coefficients that are not zero (three bytes each, where
a block would be 128 written), and the others take strips of 16 rows, do the transform and
the colours on their stacks and write only pixels; a strip begins and ends on 16 bytes of
the picture whatever its width, so two cores never write the same 16.

| 640 x 480 | Instructions, on one core | One core | Three workers | Seven (four of them small) |
|---|---|---|---|---|
| a photograph as PNG (308 KB) | 53 million | 11.5 s | 8.6 s | |
| the same as JPEG (33 KB) | 31 million | 6.5 s | 3.6 s | 2.4 s |

A PNG gains least: more than half of it is the inflating, which is one stream. Not read:
interlaced PNG, progressive and arithmetic JPEG, four-colour JPEG; the viewer says which.
The JPEG's colours at half size are repeated, not smoothed, so its picture differs from
other decoders' at coloured edges (4% of its bytes by more than two levels); the PNG is
exact. `python tools/make_test_pictures.py --check` builds the same decoder for the host,
compares both with Pillow and prints the sums the guest must print
(`NXVIEW_SUM=1 nxview --decode /usr/share/picture-fox.png /tmp/a.ppm`: `72ae6c0d`; the JPEG:
`95b8fd40`), with any number of workers.

**The clipboard** is a file, `/tmp/clipboard` (`ui_clip_set` and `ui_clip_get` in `ui.h`; the
terminal reads and writes the same file): one piece of text that is there after the program
that left it has gone. The window system's own selections are a conversation between two
programs that both have to be running.

**The Start menu's System folder** (`nxapps.90-system`, written by `linux/apps/build.sh`) has
"Restart the desktop" (`nx restart`) and "Shut down" (`nxoff`): the desktop's programs and the
window system are ended, and Linux powers off. The machine has no switch a program can
reach, so the kernel's power-off (`shaderemu_power_off` in `linux/kernel/shaderemu_input.c`)
stands still with interrupts off until the machine is switched off where it is shown; before
that hook, power-off returned and a shell went on running.

**Pictures for the desktop** are `/usr/share/wallpaper-NAME.ppm`, made on the host by
`python tools\make_wallpaper.py [FOLDER ...]`: the Unsplash photographs listed in
`linux/apps/wallpapers.txt` (a name and the photo's id a line; fetched, never kept in this
repository) and every picture in the folders given. One 1920 or more across is brought down to
fit 1920x1080, one 1280 or more to fit 1280x720. Run it before `make_linux_image.py`. The
choice is kept in `/tmp/nxwallpaper`; a new image shows `wallpaper-fox.ppm`.

A picture is not decoded or copied by anyone. A PPM file is already three bytes a pixel, and
the root file system is romfs, which keeps every file in one piece in the ROM: the GPU samples
the file there. The screen driver finds it from the file's number, which romfs makes the
offset of the file's header (the overlay mounted over the ROM passes it on), and checks the
header's size and the file's first bytes through `/dev/mtd0` before trusting it. Putting up the
1623x1080 desktop picture costs the server about 15 ms. A file elsewhere (one Paint saved, in
RAM) has its rows read into GPU memory first, which takes about 1.2 s for a picture that size;
drawing it pixel by pixel through `GrArea` took 6 s. Pictures are not clipped by child windows.

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
- **A program's own picture as the cursor.** A window's cursor of 32 x 1 whose mask is the
  words `0x5345`, `0x4355` is not a bitmap: its two words of bits are the address of 32 x 32
  colours (`0xAARRGGBB`) in the GPU's memory, and the driver gives the display that address
  with the cursor's hot spot (`gpu_cursor`). The picture is shown as it lies there, so the
  program changes it in place; no address is no cursor at all. Tiberian Dawn and Red Alert
  show their pointer this way (`docs/tdawn.md`).
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
- **A new window's frame is painted when it is made.** The window manager lives in the
  server but works from each program's own event queue, so a frame was painted when its
  program first asked for events: a game that takes a minute to start showed what the last
  window left in the buffer it was given, the other game's caption included.
- **The keyboard always has a window** ("The keyboard" above): a press anywhere in a framed
  window activates it (stock Nano-X did so only on the caption), the keyboard passes on when
  its window goes (it went to the desktop, where no key did anything until a caption was
  clicked), and the server takes the window manager's keys before any program sees them
  (`window_key` in `srvevent.c`; stock `wm_key_down` is a FIXME).

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
OpenGL program can run at a time: the GPU's memory for programs has fixed places, the kernel
gives it to the first that asks (`SHADEREMU_GPU_DRAW` on `/dev/gpu`, kept until the program
closes or ends), and a second one says whose it is ("the GPU is drawing for doom (process
41)...") and ends. Before that both drew into the same memory.
