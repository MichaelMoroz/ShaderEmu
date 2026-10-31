# Nano-X

The Linux image has a window system: Nano-X from [Microwindows](https://github.com/ghaerr/microwindows),
the server with its window manager built in, and a handful of its clients. It draws through
the machine's GPU and takes the machine's keyboard and pointer.

    / # nx                      # server, the desktop with its icons, the bar
    / # nxcalc &                # more clients: nxeyes, nxtetris, nxmine, nxclock, nxev, demo-*
    / # NANOX_SIZE=640x480 nx   # another screen size (800x600 by default)

The machine starts `nx` by itself when it boots, unless the host says not to (bit 0 of the host
flags, `gpu.md`; the image's init reads it). The harness asks for the desktop in its terminal
mode (`--no-desktop` leaves it at the shell) and not in runs with other arguments, which wait
for the shell prompt (`--desktop` asks for it there); a resumed shell gets `nx` typed for it.

The harness window's pointer and keys are the machine's (`input.md`). The bar along the
bottom (`linux/nanox/nxbar.c`) has a Start menu with the programs in the image and a clock;
the machine's clock chip is fed the host's local time. `glxgears` runs in a window. The
desktop has a small set of programs of its own, listed under "The desktop's programs". It has
two looks, Windows 7 (the default) and classic, chosen in Settings ("Looks").

### Where things are

| What | Where |
|---|---|
| The server's own changes: window manager, frames, events | `linux/nanox/microwindows.patch` (regenerate with `git diff -- src/nanox src/engine src/demos src/include` in a patched tree) |
| Frames and captions, both looks | `src/nanox/nxpaintnc.c` (in the patch): `nxPaintNCArea`, `paint_seven`, `caption_title`, `caption_icon` |
| A window's icon in its caption | `src/nanox/wmclients.c` `icon_of` (both indexes), `wmaction.c` hands it to the painter |
| Screen driver: GPU lists, pictures, ROM fonts, the look word | `linux/nanox/scr_shaderemu.c` (`gpu_picture`, `picture_alpha`, `romfont_*`, `nx_look`) |
| The bar | `linux/nanox/nxbar.c` |
| The desktop and Files | `linux/apps/nxfiles.c` |
| Settings | `linux/apps/nxsettings.c` (Look tab: `set_look`, `frames_again`) |
| Shared UI: widgets, looks, menus, icons, questions, starting programs | `linux/apps/ui.h`, `ui_look.h`, `ui_menu.h`, `ui_icons.h`, `ui_ask.h`, `ui_launch.h`, `ui_exec.h` |
| The desktop's start | `linux/nanox/nx` (a shell script) |
| Icons, look pictures, fonts | `tools/make_icons.py` (`linux/apps/icons.txt`, `icons7.txt`), `tools/make_fonts.py` |
| Programs of the Start menu | `/usr/share/nxapps.*`, written by each build script |

## Building it

    wsl -- bash linux/kernel/build.sh    # once: cross compiler, kernel's tree (docs/system/nommu.md)
    wsl -- bash linux/nommu/build.sh nanox/build.sh     # about 10 s after the first time
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1

- `linux/userland/toolchain.sh` builds the C library the programs link: musl, static, for
  RV32IMAF (`docs/machine/fpu.md`). The integer and soft-float helpers come from compiler-rt and the 128-bit `long double` ones from libgcc's
  soft-fp sources. `rv32-cc` is the resulting compiler driver.
- `linux/nanox/build.sh` fetches Microwindows at a fixed commit, applies
  `microwindows.patch`, adds the three drivers in that directory, builds with
  `linux/nanox/config` and leaves stripped binaries in the root `linux/nommu/root.sh` makes
  flat (`docs/system/nommu.md`).
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

A window minimised and shown again keeps its picture as well (`kept` in `GR_WINDOW`): it is
not cleared to its background, and its program is still asked to draw it, since what it drew
while unmapped was dropped: a program that does not draw again on an exposure (a game between frames, or one
paused, an OpenGL program) comes back as it was. A window moved, resized or reparented while unmapped, or a child
of it unmapped or destroyed meanwhile, is cleared.

Drawing into a buffer goes through the GPU too (`gpu.md`):

| Operation | GPU command |
|---|---|
| filled rectangle, horizontal and vertical line | a coloured rectangle |
| a string of a built-in font, all of it in view | one text command (`docs/machine/gpu.md`): the font's glyphs and their table are put into GPU memory the first time, and the engine hands the driver the whole string (`gd_drawstring`) instead of a bitmap a character |
| other text and one-bit bitmaps | a mask texture (fragment mode 3), over a coloured rectangle when the background is drawn too. Glyphs of the built-in fonts are copied into GPU memory once and stay there; other bitmaps are copied for each list |
| copy from a window (scrolling) | a rectangle textured with that window's buffer |
| a string of a font of the ROM (`docs/system/fonts.md`), any script, any of its sizes | one text command whose font table and glyphs are the font's file where it lies in the ROM: nothing is put into GPU memory but a word a character. Its glyphs are coverage and are blended, so what is queued after it in the list is drawn in the same pass |
| drawing on a pixmap of the screen's kind, and a copy from one (`GrNewPixmap`, `GrCopyArea`) | the pixmap's pixels are GPU memory when the pool has room: it is drawn on as a window is, and a copy from it is a rectangle textured with it (below, "A pixmap the GPU keeps") |
| a PPM file (`GrDrawImageFromFile`), at any size | one rectangle whose texture is the file where it lies in the ROM (fragment mode 4, three bytes a pixel); the GPU does the scaling and nothing is read. A file that is not in the ROM has its rows read into texture memory, as many as fit (1.8 MB) at a time |
| single pixels with nothing else queued, other images, blending, XOR drawing, reading pixels | software, into the buffer |

Commands are queued for one buffer at a time and submitted when drawing moves to another
buffer, when software needs the pixels, or when the server is about to wait for events. Each
list starts by drawing the buffer as a texture and is copied back into it, so software and GPU
drawing can be mixed freely. Submitting takes no system call (the lock word and `pause`,
`gpu.md`). A copy within a buffer reads RAM, which lacks what is still
queued, so it submits first if its source overlaps the queued area.

A list is drawn when the machine's pass ends and copied back in the commit of the pass after,
and waiting for either ends a pass with `pause`. So the driver waits only where the result is
needed (`drawn`, `settle` in `scr_shaderemu.c`):

- the list submitted before the server waits for events is not waited for at all: the
  server goes to sleep and its clients run;
- a list submitted in the middle of drawing is waited for until it is drawn (its commands
  and textures are written again next), not until it is copied back: the next list may be
  submitted with that copy still to come, and the copy is made before it is drawn;
- software that reads or draws on a buffer, and a buffer freed or resized, wait for the copy.
  `GrReadArea` is such a read: a program that writes its window's pixels itself reads one
  pixel after its drawing requests, and has its answer when all that was drawn is there.
- `GrGetWindowInfo` waits for nothing: where a window is, and where its pixels are, does not
  depend on what is drawn in it. The programs that want the wait (`nxmon`, `nxpaint`) read a pixel.

A program's own icon (`PICTURE_OWN`) is found in the ROM without reading the program's file:
the ROM's header at the file's number has its name and size, and the icon's mark is read
there (`rom_icon`), which brings no pages of it into the page cache.

The cursor is not drawn at all: it is the display's cursor (`display.md`), an arrow by
default. The driver writes its position and, when the shape changes, a 32x32 image; once the
pointer has moved, a worker core writes the position ("The pointer on a core of its own").

**A pixmap the GPU keeps** (`pixmap_new`, `pixmap_map` in `scr_shaderemu.c`). A pixmap of
the screen's kind (32 bits a pixel, and no larger than the GPU's target as the host's flags
say it: 2048 a side in the harness, 1280 x 720 in the Unity world, past which nothing is drawn)
takes its pixels from the pool the windows' buffers come from, eight of them at most, and is
a surface like a window's: fills, text, pictures and copies on it are commands, and a copy
from it to a window is one rectangle. A program draws something large once and shows parts
of it (the browser's page, `docs/apps/web.md`). When the pool has no room, or the pixmap is of
another kind, it is the server's memory, drawn on in software; the driver counts the ones the GPU keeps
at word 28 of its page of the control block (`WM_PIXMAPS`, `0x81001870`), so a program that
made one can tell. A new one is black, by a fill of the GPU's.

A program can also fill its window itself: `GrGetWindowInfo` returns the physical address of
the window's first pixel and the row length of its buffer, and `GrFlushWindow` tells the
server the pixels changed. The OpenGL driver does this. For a pixmap the GPU keeps it says
the same (0 for one in the server's memory): the browser submits lists of its own that are
copied into its pixmap, and keeps them in another (`docs/apps/web.md`, "Strips").

GPU memory under Nano-X: window buffers in the first 12 MiB (`0x80000000`; the worker cores'
mailbox pages above them, `0x80c00000`), then the control
words and the display's framebuffer, the server's command list, cursor, glyph cache and
scratch textures up to `0x81700000`; the last 4 MiB are left to one OpenGL program.

## The bar, and what a program adds to it

`nxbar` has, from the left: the Start button and menu; the programs pinned to it (an icon each,
the registry's `bar.pinned`, commands parted by commas, `nxfiles,nxterm,nxweb` by default; the
right button there unpins); a button for each program's windows and each that is starting ("The window manager and the bar");
four cells for the desktops; then the player's name, the
volume, the machine's speed, the clock and, at the right end, a strip that shows the desktop.

- A window's button: a click brings the window to the front, or puts it away when it is in
  front already (that button is drawn pressed: the window with the keyboard, from the
  activations the bar hears); the right button offers the window's menu; the middle button closes it (and it is watched:
  "A close that is not answered"); the wheel over the buttons goes from window to window. Several windows of a program: a list.
  Up to 32 windows and 16 buttons.
- The volume's button opens a slider (`/dev/sound` is opened only while it is up, so the card
  is not kept on), with a mute button under it; the wheel and Up/Down step by 5. It writes
  `sound.volume` as Settings does.
- The clock opens the month: arrows (or the wheel, or Left/Right) go from month to month,
  today is marked, the long date under it.
- The strip puts every window away, and a second click brings back those it put away.
- What the bar starts is watched (`ui_launch.h`): a program that fails is reported ("When a
  program fails to start").
- `nxbar --look` writes the registry's look into the window system's word and exits: `nx`
  runs it before the desktop starts.

The Windows key opens and closes the menu from anywhere (a key the bar grabs with
`GrGrabKey`). The menu is whatever the files `/usr/share/nxapps.*` list, one `Label=command` a
line, or `Folder/Label=command` for a program in a folder (a folder may be in a folder:
`A/B/Label=command`), so a program gets into it by having its build put such a file in the
image: `linux/nanox/build.sh` writes `nxapps.10-nanox`, `doom.sh` writes `nxapps.50-doom`. The
folders are Games, Utilities, Other and System, in the order the files first name them. A
folder's programs open in a menu of their own to its side, when the pointer is on it or with
Right or Enter. `NXBAR_SHOW=1` (or `=Games`) starts the bar with the menu open (and that
folder's beside it), for a test without a pointer: each level is a layer of its own.

The player's name (`$USER`, docs/system/persist.md) is in a box before the speed's when the desktop's
starter has one.

Settings has a Devices tab (`device_lines` in `nxsettings.c`; `nxsettings devices` prints it):
a list of the machine's devices and, for the one picked, what it is and how it runs now, drawn
again where it changed once a second. The processor's extensions are tried, not read (Linux's
`isa` line comes from the device tree): `linux/apps/machine.h` runs a float and a Zba/Zbb
instruction and says which trapped, and takes the MMU from `/proc/cpuinfo`, which names one
only when there is one (`docs/system/nommu.md`). Its cores and workers are the control words'
(`0x810003c0` on), the network's figures `/sys/class/net/lan0`'s, the host's kind and its
visitors the host's words (`0x81000360`).

Before the clock is the machine's speed in millions of instructions a second, the first core's
and all cores' (`0.3 / 43.7 M`: `speed_sample`, from the core's cycle counter and the workers'
counts in the control words, as `nxmon` reads them). It is sampled once a second and its box
alone drawn again when a figure is another tenth. An idle desktop, all of it, is 135 thousand
instructions a second on `rvc_cpu` (31 October 2026).

The bar looks for the windows (five questions to the window system each) when one came,
went or was put away: it selects the root's `GR_EVENT_MASK_CHLD_UPDATE` and looks within a
second of such an event, and every 60 s besides; a title is asked again when its window is
activated ("One program's windows are not every program's news"). Whose window has the keyboard is asked after an update and
every 15 s otherwise, and the program's name is read only when another window has it.

## The window manager and the bar

The window manager is in the server (`wm*.c`, `window_key` in `srvevent.c`) and knows frames; the bar (`nxbar`) knows
programs: which process a window is of, what is still starting, what was asked to close and has not. A frame's state is in
bits of its properties, which any program reads with `GrGetWindowInfo` (`GR_WM_PROPS_ONTOP`, `_SHADED`, `_MAXED`, `_TILED`,
`_AWAY`, the desktop in `GR_WM_PROPS_DESK`). No request was added and the client's library is as it was:

- **A program or the bar tells the window manager** with `GrSetWMProperties(window, flags GR_WM_FLAGS_COMMAND, props GR_WMC_*,
  background its argument)`: a frame to a zone, shaded, on top, to a desktop, a desktop shown, a window brought forward from
  wherever it is, a process that is starting.
- **The window manager tells the bar** with a hotkey event to whoever grabbed the Windows key (`wm_tell_bar`): `ch` is
  `GR_WM_TELL(what)`, `subwid` the frame, `rootx` and `rooty` the arguments. With no bar, Tab goes round as it did.

What it adds to the machine, before and after on `rvc_cpu` on 31 October 2026, the prebuilt root against the same with these programs (instructions of
the first core; a run gave the same count each of three times):

| | before | after |
|---|---|---|
| an idle desktop, thousands a second (30 s) | 122 | 125 |
| Calculator's start under `nx`, thousands: first window, later one | 1,192, 1,200 | 1,219, 1,241 |
| Terminal's | 1,169, 1,250 | 1,199, 1,271 |
| Files' | 738, 883 | 764, 938 |
| 16 s of a drag by the pointer's words, thousands (a worker moves the layer) | 3,719 | 3,890 (3,815 with `wm.remember` 0) |

glxgears' 5 s lines on the card's calibrated clock (`--pass-model` of `tools/card_model.json`, one run each), 953 frames a second with nothing else: the Start menu opened
and shut every 0.7 s, 900 before and 891 after (94 and 93%); three programs starting, 898 to 950 and 895 to 952. Nothing runs between a key and a click but the
bar's once-a-second look, while something starts or closes.

**Snapping.** A frame dragged to an edge goes to half the screen, to a corner to a quarter (48 pixels from it), to the top
to all of it; a frame that does not resize does not snap. Under the drag the bar shows where in one plain window under the
frame (blue, no buffer kept once it goes). The worker that follows the pointer still moves the layer: the quiet rectangle
is the screen less 8 pixels at its edges while a frame is dragged (`GsDragInset`), so the kernel reports only moves there
and the first core hears nothing in between. A frame dragged off a half or a quarter takes its old size back.

**Keys**, Ctrl+Alt held (by key code: Ctrl makes letters control characters):

| Key | |
|---|---|
| Left, Right | half of the screen |
| Up | all of it; Down: back, and from there minimised |
| U, I, J, K | the quarters: top left, top right, bottom left, bottom right |
| Shift + arrows | 16 pixels that way |
| S | rolled up to its caption, or down again |
| T | kept above the others, or not |
| D | the desktop shown, windows away; again and they come back |
| Space | the window's menu |
| 1 to 4 | that desktop; with Shift the window goes there |
| Page Up, Page Down | the desktop before or after |

**The caption.** A double press (under 500 ms, under 5 pixels) maximises and puts back; the right button opens the window's
menu (the bar's `show_task_menu`): Bring to front, Minimise, Maximise or Put back, Left half, Right half,
Roll up, Keep on top, Desktop (a submenu), New window, Pin to the bar, Close, End the program.

**Rolled up** (`GsShadeWindow`): the frame's buffer stays, the layer shows its first 28 rows (`gd_show_height`) and the rest does not
take the pointer. The program sees no change of size.

**Kept on top**: a frame raised is put back under those on top (`wm_raise_ontop`), a new one too. The cost is a loop in `GrRaiseWindow`
only while one exists.

**Desktops**, four: a frame has one in its properties and the others are unmapped by the window manager (`GR_WM_PROPS_AWAY`
says a frame is hidden by that and not minimised). The bar shows four cells left of the name: lit is the one shown, a line
under one is one with windows on it; a click or the wheel changes it. Its buttons, Tab and "show the desktop" are of the desktop
shown. A window brought forward shows its desktop.

**The switcher.** Ctrl or Alt + Tab with the key held is the bar's: the windows of this desktop, the one last with the keyboard
first, shown after 150 ms or a second Tab (a quick press goes to the last window with nothing drawn); letting the key go goes there.
Shift goes backward.

**Where a window was.** A window moved, resized or tiled by hand writes `wm.place.PROGRAM` =
`asked width,asked height,x,y,width,height,zone` in the registry, and the first window of that program with the same asked
size opens there (`place_found` in `wmclients.c`); a window that asks for a place (`GR_WM_PROPS_NOAUTOMOVE`) keeps it, a window that
cannot resize keeps its size. `wm.remember` 0 is off. Nothing is written until the frame is let go: the file is 96,000 packed bytes shared.

**A program's button.** The windows of one program on the desktop shown are one button with their count, showing the one that last had
the keyboard; a click or the right button lists them, with New window and Close all. A single window keeps Windows' click and the
full window menu. A pinned program that is up comes forward; the middle button or Shift starts another.

**Starting.** What the bar starts, or what a program says it started (`ui_launch_started` sends `GR_WMC_STARTING`), has a grey button
until a window of it, or of a program it started, shows; the note (`ui_launch.h`) stays too. After a minute (`NXBAR_START_MS`) nothing
has come: "has shown no window yet", Keep waiting or End it.

**A close that is not answered.** Alt+F4, the box, or the bar's Close tells the bar (`GR_WM_TELL_CLOSE`). After 5 s
(`NXBAR_CLOSE_MS`) it looks:

- the process has gone: nothing;
- a new window of it or of a child: it is asking about what is not saved: nothing;
- the frame is still there: "does not answer", End it or Wait (and asked again in 15 s). The question goes by itself if the window does;
- the frame has gone, no other window of it, and 5 s later it runs still: "closed its window and is still running", End it or Leave it.

End it is SIGTERM and, 2 s later, SIGKILL. "End the program" in the window menu asks the same without a wait. A process stopped (`kill -STOP`) or
hanging gets no different words: only its queue's answer is looked at.

**A program that dies.** Its windows go with its socket, and with them its button. Started from the bar or Files, a fault is said
("stopped working", which signal) with **Start again** (`ui_launch_report_again`), not only OK.

**One of a kind and the one drawer.** `ui_single_instance()` (`ui_single.h`): Settings without arguments and the Monitor bring the running one
forward, across desktops, and end. A program started by the launcher that ends at once because another draws with the GPU is not a dead end:
`nxask --gpu PID COMMAND` asks "Close NAME, start THIS" or "Go to NAME", closes the holder, kills it after a wait if it stays, and starts the program;
its own notice is left out under `UI_LAUNCHED`.

Tests: `python tools/test_all.py wm`, by keys and the pointer on the real desktop, with `nxwintest` (`linux/nanox/nxwintest.c`), which prints each
window's place and state and acts as a program that behaves badly: `hang`, `ignore`, `linger`, `crash`, `slow`, `two`. `NXBAR_PRINT=1` has the bar say what it
shows. `python tools/wm_shots.py --image build/images/NAME --out DIR` makes a picture of each feature (with no card time).

Not done: a window's picture in the switcher (a layer is one opaque buffer the composer cannot scale, so icons and a title); keys to move a
window between screens' edges beyond the nudge; `nxvideo`'s own GPU question stays the notice it was; restart of a program that was not started by a
launcher (from a shell, nothing watches it); a program that opens its window with no event ever taken has no frame, so its close cannot be asked.

## Menus

`linux/apps/ui_menu.h` is the menu the bar, the desktop and the explorer share: rows with a
16-pixel icon, a line between groups, greyed rows, and rows that open a menu of their own to
the side (on the other side when the screen ends). A level is a window that goes when the
menu closes. `GR_WM_PROPS_POPUP` on such a window has the window system ask every menu to
close (`GR_EVENT_TYPE_CLOSE_REQ`) when a button is pressed anywhere outside one; Escape closes
it too. Keys: Up, Down, Home, End, a letter, Right or Enter into a row's own menu, Left or
Backspace out of it. A program calls `ui_menu_open` and gives `ui_menu_event` every event
before it looks at it itself.

## The desktop

The desktop is `nxfiles --desktop`, started by `nx`: the screen's picture and, on it, the
icons of `/root/Desktop` (the root window is the display's own layer under every window, so
they take no window and no buffer).

- **A shortcut** is a file `NAME.app` of three lines, `name=`, `icon=` (a name of
  `linux/apps/icons.txt`; without it the program's own) and `run=` (a command). The image
  starts with one for every program of the Start menu but the System folder's, and for Home,
  Pictures, Music and Videos (`desktop_shortcuts` in `tools/make_linux_image.py`), a column a
  folder of the menu. `open` starts one too (`nxfiles --run FILE.app`).
- **Docs**, alone in the last column, opens Files on `/usr/share/doc/shaderemu`: the
  repository's `docs/` but for its plans, copied by every image build (`documentation()` in
  `make_linux_image.py`), so they are the tree's. A file there opens in the editor as a page
  ("The editor's reader"), `README.md` first, and its links go from file to file.
- **Places.** Each icon stands where it was last put, in whole cells of 76 x 70
  (`/root/Desktop/.places`: `x y name` a line). The left button held moves the selected
  icons; onto a folder it moves them into it. A new entry takes the first free cell, or the
  place it was dropped at.
- **Two clicks** within 0.7 s open an entry. Ctrl and Shift with a click select several.
  The right button brings the menu: an entry's (Open, Cut, Copy, Rename, Delete, Save to
  persistence, Properties) or the desktop's (Arrange icons, Refresh, Select all, Paste, New, Open in Files, Settings).
- **Keys**, once the desktop has been clicked: the arrows go to the nearest icon that way,
  Enter opens, Space selects, F2 renames (below, "A new name"), Delete, F5,
  Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V, letters for the entry that starts with them, and Shift+F10
  (or the Menu key) for the menu.
- It is in the overlay's memory, as everything a program writes is: what was moved or made
  is as the image has it again after power-off (but the icons' places: `nx` links `.places`
  to `/persistence/desktop/places`, which a host may keep, `docs/system/persist.md`).
- Settings draws a chosen picture on the root itself; the desktop sees the choice change
  within a second and draws picture and icons again. The choice is the registry's `desktop.wallpaper`.

A drag between windows: where the button is let go, the explorer asks the window system
which window is in front there (`GrQueryTree`, the one in front first). A Files window says
its folder in its title ("Files - /root/Pictures"), and no window at all is the desktop.
The entries are moved there with `mv` (copied with Ctrl held: `cp -a`); the other side sees
its folder's time change within a second and reads it again.

A box: the left button held from an empty place of the desktop or of a Files window (icons
or the list) and pulled selects every entry whose picture or name the box touches (a row of
the list from side to side; not the whole place an icon stands in); Ctrl keeps what was
selected before. The window system has no XOR drawing: the outline is four lines, taken away by
drawing what was under them again, clipped to the lines (`band_draw` in `nxfiles.c`). The
pointer's moves are asked for only while the box is held. A window's view (icons or list)
and the list's order are kept in the registry (`files.details`, `files.order`) for the next window.
Test: `NXFILES_PRINT=1 nxfiles /usr/share`, `nxkey drag:643,86:343,196` must say 8.

A click on one of several selected entries leaves them all selected while the button is held,
since they may be about to be dragged; let go where it went down, that entry alone is selected.
The keyboard's frame round the entry at the cursor is drawn inside its tile or row, where the next drawing of
the tile reaches it.

**A new name** (F2, the menu's Rename, a new folder or file) is typed over the entry's own, in
a box where the name was: under the icon on the desktop and in the tiles (as wide as the name,
up to two tiles), in the name's column of the list. The name is selected but for its ending
(`holiday` of `holiday.txt`; a folder's whole name), so a letter typed replaces it, as in
Windows. Left, Right, Home and End move the caret, with Shift they select, Ctrl+A selects all,
Backspace and Delete take away; a click in the box puts the caret there, a click elsewhere
gives the name, Enter gives it, Escape leaves the old one. A shortcut's name is its `name=`
line, and its file is renamed after it. A name that is there already, or that cannot be given,
is said in a window and the box stays. A file or folder of the image is renamed in place
(`linux/nommu/patch_ramfs.py`, `docs/system/nommu.md`).

**What went wrong is said** in a window of `nxask`'s with the error icon, which the desktop and
Files do not wait for (`report` in `nxfiles.c`), and in the status line: a folder that cannot
be opened, an entry that could not be copied, moved, deleted or renamed (with the first line
`cp`, `mv` or `rm` wrote to its errors, or the system's reason; "3 entries could not be moved,
the first ..."), a file or folder or shortcut that could not be made, the clipboard's file not
written, a shortcut with no `run=` line, a terminal or a properties window that could not be
started, and the desktop's picture gone (once a session: the desktop then shows a colour). A
program that cannot start is said too ("When a program fails to start").

**Save to persistence**, on an entry's menu (not inside `/persistence`), copies the selected
files and folders into `/persistence` (`cp -RL`: a link's file, not the link), asking first
before one of the same name there is replaced. When the folder would then hold more than
100,000 bytes, the 100 KB a player the host keeps (`docs/system/persist.md`), nothing is copied
and a window says the sizes: "Great Wave.jpg: 84.4 KB, and /persistence holds 16.6 KB already.
Together 101.1 KB: more than the 100 KB the host keeps." The bytes counted are the files' own,
before packing; a folder that packs to more than 96,000 is still refused by `nxpersist save`.
Once copied, a window says what the folder holds and offers to save it to the host now
(`nxpersist save`) or later. `NXFILES_PRINT=1` prints `nxfiles: saved to persistence: NAME`.

## Icons

Each look has its own set. The classic look's is below.
The Windows 7 look's are [Oxygen](https://github.com/KDE/oxygen-icons)'s (KDE, LGPL-3.0, fetched
at v6.30.0 into `build/fetch/Oxygen`), 16, 32 and 48 pixels, listed in `linux/apps/icons7.txt`
(the same names as `icons.txt`) and made into `icons7-16.ppm`, `icons7-32.ppm`, `icons7-48.ppm`
and `icons7.idx`. Each of those sheets has an `.rgba` twin (a word a pixel with its alpha: the
format is at the top of `tools/make_icons.py`): the screen driver finds the twin in the ROM
(`picture_alpha`) and draws a cell of it blended (fragment mode 1, the key flag on the clear
word), so the edges and shadows are smooth; the keyed `.ppm` is what software drawing uses.
In that look a program's icon is its sheet's cell when the list names it, its file's own only
when it does not. `ui_icons.h` and the window manager (`icon_of`, which keeps both cells of a
window) pick the index and the sheets by the look; a program that follows a change of look
reads its icons again (the bar, the desktop).

Two sheets in the ROM, 32 and 16 pixels a cell and 16 cells a row, and a list of which cell is
whose (`/usr/share/icons32.ppm`, `icons16.ppm`, `icons.idx`): `python tools\make_icons.py`
makes them from `linux/apps/icons.txt`, whose names are programs (as the kernel calls them),
kinds of file and places. The pictures are [Chicago95](https://github.com/grassmunk/Chicago95)'s
(GPL-3.0+/MIT), fetched once at one commit, and for what that set lacks the pictures in
`linux/apps/icons`, a file a name, any size: the games' own icons (Chocolate Doom's, Quake's,
ClassiCube's, Vanilla Conquer's two; its README says whose each is).
Every picture of the theme's `categories` folder is one and the same plain program group, so
programs name a picture of `apps` or `devices`; a file has an
icon by its ending where the theme has one of its own (`types[]` in `nxfiles.c`: a `.nes`
the controller, a game's data the game's, C sources, fonts, the kinds of picture; the
theme's zip is its tar and its css its html, so those stay); `nxgame GAME ...` has the
game's icon (`ui_icon_of_command`). A file with such an ending is opened with its program
though the image says every file can be run. `ICONS_OUT=FOLDER python tools\make_icons.py`
makes the sheets for an image of one's own (`tools/private_image.py` cuts the programs' icons
from the overlay's sheets when it has them).

- A cell is drawn by the GPU from the ROM where it lies: `GrDrawImageFromFile` with
  `GR_IMAGE_CELL(size, cell)` as its flags, which the screen driver takes as "this cell, and
  not its magenta" (fragment mode 4 with the key flag, `gpu.md`). One request and one
  rectangle an icon, as text is. `ui_icons.h` has `ui_icon_of(name)` and `ui_icon`.
- The driver keeps the last 64 files it found in the ROM (`PICTURES`): an icon costs no file calls.
- A program carries its own icon, at its file's end: 32 x 32 and 16 x 16 pixels of RGB (magenta
  for what is not the picture) and the eight bytes `NXICON1` and a zero, which change nothing
  for the loader. The image builder puts the sheets' cell there for every program of
  `icons.idx`, so the sheets are only where the build's icons come from; a program made or
  copied in the guest gets one with `nxicon PROGRAM PICTURE` (any kind `docs/system/media.md` reads, any size: it is scaled; what is see-through becomes the clear magenta),
  loses it with `--none`, and `nxicon PROGRAM` says whether it has one. Drawn with
  `GR_IMAGE_ICON(size)` as the flags: the same rectangle of the GPU's, read from the file's
  end. A program in memory (`/tmp`) is not in the ROM, and its icon is drawn by the first core.
- The caption, the bar's buttons, the Start menu, the desktop and the explorer all ask the
  program's file first (`/proc/PID/exe` for a window's) and the sheet by name after
  (`ui_icon_of_program`, `ui_icon_of_command` in `ui_icons.h`; `icon_of` in the window manager).

## Captions

A caption is a strip of colours stretched over it (`caption-on.ppm`, navy to light blue, and
`caption-off.ppm`, greys, both 256 x 1 and made by `tools/make_icons.py`): one more rectangle
of the GPU's, drawn from the ROM. The Windows 7 look's are 1 x 22 strips from top to bottom
(`caption7-on.ppm`, `caption7-off.ppm`; `bar7.ppm`, 1 x 26, is the bar's), and its caption
buttons and Start button are cells of 24 pixels of `look7.ppm` with an `.rgba` twin (`LOOK7` in
`make_icons.py`: minimise, maximise, restore, close, the same four for a window without the
keyboard, the Start button and the same pressed).

## Looks

Settings' Look tab chooses the windows' style, Windows 7 or classic, and the text, Selawik
("smooth", `docs/system/fonts.md`) or the window system's bitmap font. Choosing a style brings its own
text (classic is Chicago95's icons, grey frames and the bitmap font); the text
buttons then change only that. `nxsettings look seven|classic [smooth|classic]` does the same
from a script. The choice is the registry's `desktop.style` and `desktop.font` and, for the
running session, one word of the window manager's page of GPU memory (word 24 of `WM_OFFSET`,
`0x81001860`; `/dev/gpu` at offset `0x01001000` + `0x860`): `0x4c4b0000 | smooth << 8 | style`,
style 1 for Windows 7. 0 there (nobody set it) is the classic look, as a bare `nano-X` for a
test has it; `nx` writes the registry's look (Windows 7 unless it says otherwise) before the
desktop starts (`nxbar --look`). `linux/apps/ui_look.h` reads and writes it.

- **Frames** are painted in the look when they are painted (`nxPaintNCArea`): the geometry is
  the same in both (`CYCAPTION` 22, the buttons where the window manager's clicks look for
  them), so nothing but paint changes. Settings repaints every frame at once (`frames_again`:
  `GrClearWindow` with exposures on each decorated window).
- **The bar, the desktop and Files** read the word once a second and draw themselves again
  when it changed (`ui_restyle` in `ui.h`, `ui_menu_restyle` in `ui_menu.h`): the font, the
  icons of the other set, the bar's pinned programs and menu.
- **Other programs** read the look at their start (`ui_init`), and keep it until they are
  opened again; one that wants to follow calls `ui_restyle()` now and then and redraws.
- **The widgets** of `ui.h` (buttons, bevels, the focus box, scroll bars, sliders), menus and
  questions have a Windows 7 drawing: `UI_FACE`, `UI_SHADOW` and `UI_SELECTED` are its colours
  then (240 grey, 160 grey, 51/153/255 blue), so a program that uses them follows.
- With the smooth font, text is drawn a pixel higher (`ui_text`): Selawik 12's ascent is 12
  where the bitmap font's is 11, and the baseline stays where layouts expect it.
- After a change of look the desktop decodes its picture again: copying the decoded picture
  it keeps back over the screen did not cover the old names in two of three tries, and the
  names of both fonts stood one on the other. Not understood.
- The root window's client hears a press in any window it has none of its own in, the bar's
  among them (each client is told at the lowest window it selected): the desktop ignores
  a press whose `subwid` is not the root.

## A window before the work

A program that has seconds of work before it can show anything (a picture to decode, a film
to index, workers to start) maps its window first and says in it what it is doing: `nxview`,
`nxplay` and `nxvideo` do. The window manager frames a window as its program takes events,
so such a program calls `ui_framed()` (`ui.h`) after `GrMapWindow`, which takes what is
waiting; without it the window stands with no frame at the screen's corner until the work is
done. To check: the program, `sleep 2`, and the layer in a snapshot has the frame's size.

## Paint

`nxpaint` keeps its picture in its own memory (a word a pixel) and shows the part in view by
writing it into its window's buffer (`settle`, `present` in `linux/apps/nxpaint.c`), so that
a brush stroke costs the pixels it touches and nothing else. Its ten tools are buttons with a
picture each, 16 x 16 in a few colours (`tool_pictures` in `nxpaint.c`), the same in both looks.

- **Megabytes are moved by the worker cores** (`linux/apps/ui_bulk.h`: `bulk_flat`,
  `bulk_rows`, `bulk_rows_kept`). A new picture filled, its copy for taking changes back, and
  the view written into the window are a megabyte or two at the start, and a core keeps 6 KB
  of new stores a pass: a hundred passes each that end early. From 16 KB up a job goes to
  the workers in rows or pieces of 1 KB (up to 96 of them, as many as are through in the
  passes it takes to post to them: the square root of a 128th of the bytes); less the first
  core moves alone. The workers are borrowed (`mcw_lend`), so with another program at work on
  them the first core does it. The copy for taking changes back has the largest
  picture's room from the start.
- **A hand-over is not waited for** where nothing needs its result at once. Jobs asked for
  between `bulk_hold()` and `bulk_go(1)` are posted together and left with the workers;
  `bulk_wait()` comes before their memory is next used (any later job waits first by itself,
  an event that uses the picture waits in `handle`, a request to the window system waits when
  the workers may be writing the window: `ask_to_draw`). So a new picture is filled, with its
  copy, while the window system draws the panels; the view, the grey round it and the field
  of hues are one hand-over at the end of `draw_all`; a stroke's piece shown and the change
  kept are one, and the program is back at its events while they are made. Held jobs go to
  the workers from 4 KB. Jobs held together must be of memory apart (no 16 bytes of two): the
  grey beside the picture is the same rows' own end, not a job of its own.
- **Nothing is asked for that is there.** The status line and the tools are drawn when what
  they say changed, the field of hues is copied into the buffer once and then only its mark
  moves (the panel round it is filled in four pieces), and a line or an outline being
  dragged is taken out by drawing it again with the pixels from before (`undraw`), not by
  putting its whole rectangle back. A filled
  shape is still its rectangle.
- What still ends its passes early: the four passes `mcw_open` waits for its workers, the window system's wait
  for the GPU before the first write, and the kernel's page copies of a program's start. A zoomed view is written
  by the first core alone.
- **A window's buffer written by its program.** The window system draws by handing the
  buffer to the GPU and copying it back; what is written into it meanwhile is lost. A
  program that has asked for drawing (its panels) waits before it writes, with a request the
  window system answers only when the GPU is done: `GrReadArea` of one pixel.
  `GrGetWindowInfo` comes back sooner: it does not wait for the GPU ("The screen is composed"). What was
  written before a later request stays (the next list starts from the buffer as it is).
- **A stroke is its rows, not its dabs.** Dabs a step apart cover one run of a row between them, so the rows a
  line touches are found from where to where and each is written once (`stroke`). A span is written through a
  pointer kept in a register (`span`), since a store might otherwise be to the picture's width. **The flood** goes by
  runs that remember which way they came (`flood_runs`): a pixel is looked at once where it is filled, and the row a
  run came from only where the run is wider than its parent. `NXPAINT_DABS=1` and `NXPAINT_FLOOD=plain` are the plain
  ways: a script's sums must be the same with them.
- Tools (their letters): Select `s`, Pencil `p`, Brush `b`, Eraser `e`, Fill `f`, Pick `k`,
  Line `l`, Rect `r`, Oval `o`, Spray `y`; sizes 1 to 16 (`[`, `]`); shapes as an outline,
  filled, or both. The left button draws in the foreground's colour, the right in the
  background's. Fill is a flood: every pixel reached through pixels of the one clicked.
- Colours: a palette of 28, a field of every hue (paler downwards) with a slider for its
  lightness, the picked colour's number, and Pick takes one from the picture. The left
  button picks the foreground, the right the background, in the palette and the field both.
  A picture's colour is `0xAARRGGBB` and the window system's the other way round (`MWRGB`):
  a swatch is filled through `shown()`, or red and blue change places in it.
- A selection is dragged out with Select; dragged again it comes loose and moves, leaving
  the background's colour (a pasted piece floats the same way until something else is
  done). Edit has Cut, Copy, Paste, Delete, Select all; the copied piece is a file
  (`/tmp/nxpaint-clip.ppm`), so it passes between two paint windows. Delete and Cut take
  the piece away wherever it floats, and do nothing with nothing selected.
- A picture with changes is asked about before New, Open, Exit or the caption's box lets it
  go (Save, Don't save, Cancel: `ui_ask.h`); a test's script is asked nothing. Save on a picture opened from a PNG or JPEG opens the Save window.
- Open reads every kind libmedia reads (`docs/system/media.md`), decoded on the worker cores; a
  picture larger than 1,600 x 1,200 is made smaller to fit, and what is see-through is laid on
  white. Save writes the kind its name ends in, encoded on the worker cores: PNG (in
  segments, which libmedia decodes on every core), JPEG (quality 92, in restart intervals),
  BMP or PPM; a name with none of those endings gets `.png`, which the notice says. Save on a
  picture opened from another kind (GIF, TGA) opens the Save window. A picture with no file
  yet is asked what to be saved as, in a window with a name field (`ui_save_file` in
  `ui_files.h`) that starts in `/root/Pictures`, where Open starts, as `picture.png`: a name is
  taken in the folder shown, and a file that is there is asked about once. A file is written
  beside itself and renamed, and a write that fails says why.
- The window made larger (the caption's box, Ctrl+F10) has another buffer: the program maps
  it again when the driver's count at `0x81000064` moves, and draws everything when its
  size changed (`locate`).
- A window that takes the pointer's moves while a button is held (`ui_moves` in `ui.h`) asks
  for the button's release with them: the window system sends a held button's moves only to
  a window that would hear it let go. To test a bar: `nxkey
  drag:X,Y:X,Y` on it under `rvc_cpu`, the program printing its list's first row.
- File's New is a white picture of the present size at once; another size is Canvas size.
- Image: Canvas size (the picture kept at the top left), Scale, Crop to the selection, Flip,
  Turn, Invert, Clear. View: zoom 1 to 8 (`+`, `-`), arrows and the wheel move the view.
- Every change keeps the pixels it replaced (a rectangle of them, or the whole picture when
  its size changed): Ctrl+Z and Ctrl+Y, 48 deep.
- To test it with no pointer: `NXPAINT_SCRIPT="new:200x120 fg:ff0000 tool:Rect size:2
  drag:20,20:120,90 tool:Fill fg:00ff00 press:60,50 pixel:60,50 undo sum quit quit" nxpaint`
  under `rvc_cpu` with `nano-X -p &` (`tool:` `size:` `mode:` `fg:` `bg:` `drag:` `rdrag:`
  `press:` `new:` `resize:` `scale:` `open:` `saveas:` `pixel:` `sum` `probe`, and the
  menus' commands by name). A change taken back must give the sum from before it. Those
  coordinates are the picture's and go past the panels.
- The panels are tested by the window's coordinates, through what a pointer's events go
  through: `click:X,Y`, `rclick:X,Y`, `pull:X,Y:X,Y` and `rpull:` (a drag), each printing
  the tool, size, shape, zoom and the two colours after it; `hand` prints them alone.
  `swatch:N` reads a pixel of palette swatch N back from the window (28: the foreground's
  box, 29: the background's) and prints it beside the colour it stands for: `NOT the same`
  is a failure. That needs a server that draws: `NANOX_SOFTWARE=1 nano-X -p &` under
  `rvc_cpu`, or `rvc_harness --cpu`. In a 700 x 480 window swatch N is at
  `68 + (N % 14) * 18, 418 + (N / 14) * 20`, the tools from `3,24` (33 x 26 apart, two a
  row), the field at `324,409` (120 x 44) and the slider at `454,406`.
- Saving: `saveas:NAME` (a name with no folder goes to `/root/Pictures`), then `open:` of
  what the printed notice names and `sum`, which must be the sum from before (a PNG, a BMP and
  a PPM keep every pixel; a JPEG does not); `saveas:x` must say it wrote `x.png`, and
  `saveas:/nowhere/x` that it did not save. `saveas` alone
  and `save` on a new picture open the window, which a test types into from a shell job:
  `(nxkey 4000 t e s t enter) & NXPAINT_SCRIPT="new:40x30 saveas sum quit" nxpaint`.
- `events:MS` hands the window to the pointer and the keys for that long, and `where`
  prints its place on the screen, for `nxkey` to click in it or press Ctrl+F10:
  `(nxkey 5000 165,484 4000 ctrl+f10) & NXPAINT_SCRIPT="where events:6000 hand events:5000
  where fg:00ff00 tool:Fill press:2,2 probe swatch:17 quit quit" nxpaint`. After it `probe`'s
  two views must agree (the program's buffer is the one the window system shows).

## Frames a second

A window a program's frames are drawn in says how many a second and how many milliseconds
its mean frame took ("84 fps 11.9 ms"), in its caption left of the boxes: `programs/linux/fps_caption.h`, called where a frame is handed to the GPU
(`seglSwap`, `glXSwapBuffers`, ClassiCube's `Gfx_EndFrame`). The caption is rows of the
window's own buffer, just above its pixels, so the digits are written there once a second:
some hundreds of stores and no request (a title set through the window system cost a tenth
of a second of the core). The text is drawn on the caption's own pixels, dark on a light
caption and light on a dark one with a half-blended shadow, so it sits in either look; the
pixels it covers are kept and put back at the next second, and taken anew when the caption
was painted again in between (a window gaining or losing the keyboard). The
frames may go to a part of the program's window at its top left corner (the video player's
picture above its controls): the caption is looked for up to two windows above.

The text is right-aligned in a box of the widest text's room left of the boxes, so a shorter
text never leaves digits of a longer one beside it, and the box's caption pixels are put back
before each text (the window system's copy of a window's buffer puts an older caption back a
pass after a program has written it). The clock starts at the first frame, and a slow program
gets its mean once it has three frames or 2.5 s: under 10 frames a second with a tenth ("0.7
fps 1410 ms"), from 10 s a frame in seconds. A swap that shows part of a frame
(`seglNotAFrame()` before it: the ray tracer while its picture fills in) is not counted, and
`seglSwapAgain` (the two Command & Conquer games) counts as a frame.

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
  program. Ctrl+Alt with an arrow, a letter or a number tiles, moves and changes desktop ("The window manager and the bar").
- **The Start menu** has the keyboard while it is open, and opened by the Windows key it has
  its first row picked out: Up, Down, Home and End move, a letter goes to the next row that
  starts with it, Enter or Right opens a folder or starts a program, Left or Backspace leaves
  a folder, Escape leaves a folder and then closes the menu.
- **In a program** Tab and Shift+Tab go round its controls where it has several (a line round
  the one that has the keyboard: `ui_focus`, `ui_tab` in `ui.h`), Enter or Space presses a
  button, and a list takes the arrows, Page Up and Down, Home and End (`ui_list_key`).

`nxkey` presses keys for a test, through the keyboard's evdev device: `nxkey meta down enter
500 ctrl+tab` (names or characters, `ctrl+`, `alt+`, `shift+` before one, a number to wait
that many milliseconds, `X,Y` for a click of the left button there, `r:X,Y` for the right
one, `drag:X,Y:X,Y` for the left one held from the first place to the second, `wheel:X,Y:N`
for N notches of the wheel there, up positive). The first window a server gets has its
inside at 43,46; a second one is stepped across the screen, so a test that clicks starts
one program a server. A press and its release are two reports: the pointer's driver hands the window system one
state at a time once a button changes, so a click made while the server draws is seen.

A window's frame has, left of its close box, a maximise box (the window fills the screen above
the bar, and goes back on the second click) and a minimise box (the window goes away; its
button on the bar, greyed meanwhile, brings it back).
A window that fills the screen above the bar, maximised or made that large, is not moved by a
drag of its caption, nor resized from its corner while maximised (`wm_fills_screen` in
`wmaction.c`; `GsDragWindow` refuses such a surface too). Test: `nxterm`, `nxkey ctrl+f10`, a drag of its caption, and the frame
is at 0,0 still.

A window that changes size gets another buffer, which starts as a copy of the old one: what
was drawn stays, and only the area the window gained is cleared and its program asked to
draw. (A terminal's text survives, and the terminal takes as many columns and rows as now fit.) The
screen driver counts new buffers at `0x81000064`, and a program that draws into its window
itself (the OpenGL library) asks the server where its window is whenever the count moves.

The window manager's record of a window (`nanowm.h`) has our fields at its end. The build
does not recompile files for a changed header, so a field added in the middle leaves the
files the patch does not touch using the old layout: delete `obj/nanox/wm*.o` after such a
change.

## A program starting

A click that starts a program (two on a desktop icon or in Files, one in the Start menu) shows
at once that it did: the pointer gets an hourglass beside its arrow, and a note by it says
"Starting NAME" with the program's icon and, after a second, the seconds waited
(`linux/apps/ui_launch.h`). Both go when a window of the program, or of one it started, is
made active (a framed window's map is not told the root's listeners, its activation is) or,
for a window without a frame (Eyes, a game that fills the screen), found among the root's
mapped children by its number four times a second (a program's own numbers carry its
process: `nx_new_id`), when the program is gone, or after 30 s. They are made before the program is started, so
its first requests come after them. The pointer is the root window's: a top-level window shows
the root's pointer as it is (`GsNewWindow`).
`UI_LAUNCH=0` shows nothing (the same binaries), `UI_LAUNCH=print` prints when it ends and why.

Most of its cost is the questions asked (the note's text's width, the pointer's place) and the window updates the
desktop hears meanwhile, not the drawing. A game's window takes seconds, which is where the note matters.

## When a program fails to start

What the bar, the desktop and Files start is watched, and a window of `nxask` says what went
wrong (`--icon error --title "Could not start" TEXT OK`):

- **The exec failed** (no such program, not allowed, not a program, no memory): the child of
  the vfork writes `errno` into the launcher's memory before `_exit(127)` (`ui_exec_failed`
  in `ui_exec.h`; a vfork child shares that memory until its exec), and `ui_launch_started`
  reads it.
- **It ended before its window came** with a status other than 0, or by a signal: "stopped
  before its window opened", with the status (127 and 126 are said in words: a script's
  program not found or not runnable).
- **It crashed later** (SEGV, BUS, ILL, FPE, ABRT, SYS): "stopped working".

The launcher catches `SIGCHLD` (`ui_launch_watch`, in place of ignoring it): the handler reaps
into a ring and `ui_launch_tick` reads it in the program's loop; the client library turns an
interrupted wait into an event of type none, so the handler costs nothing while nothing ends. A
program that waits for a child of its own holds the handler off around the wait
(`ui_children_hold`, as `run_wait` in `nxfiles.c` does). `UI_LAUNCH=print` prints each report,
`UI_ASK_PRINT=1` the question window. Test: `nxreg set bar.pinned nosuch,false`, then click the
two pinned icons (`nxkey 49,587` and `73,587` on an 800 x 600 screen in the Windows 7 look).

`nxask [--icon NAME] [--title TITLE] TEXT [ANSWER ...]` shows an icon of the sheets (`error`,
`warning`, `information`) beside the text.

## The desktop's programs

Scrolling is shared: `ui.h` has the scroll bar (`ui_scrollbar`, `ui_scroll_to` for a click or
a drag on it) and `ui_wheel` for a notch of the wheel, which arrives as a button-down event.
A program that takes clicks must check `ui_wheel` first, or a notch is a click to it.

`linux/apps/` (built by `linux/apps/build.sh` after Nano-X, each one file on `ui.h`):

| Program | What it does |
|---|---|
| `nxedit [FILE...]` | a text editor, a file a tab (up to 16; Ctrl+Page Up and Down go between them, since Ctrl+Tab is the window manager's; the tab's cross or the middle button closes one), with File, Edit, Search and View menus (Alt+letter or F10; the right button brings Undo to Select all). Tabs and bytes are kept as they are, and so are CR LF and a missing last newline; a file is read in 256 KB blocks and its lines stay there until edited. Undo keeps 40,000 changes, a command's changes undone together. Find and Replace is a window that stays open (match case, whole word, wrap around, previous, replace all as one change, count); Ctrl+G goes to a line, Ctrl+B to the matching brace; Edit's Lines duplicate, move, indent, sort and trim. View has word wrap, line numbers, spaces and tabs shown, the cursor's line lit, colours for C (off for a file over 1 MB) and the tab width, kept in the registry (`nxedit.*`, with six recent files). Ctrl+Insert copies, Shift+Insert pastes, Shift+Delete cuts; a file is saved to a new file beside it and renamed over it. A text with changes is asked about before it is let go. View's Read (Ctrl+R) shows a Markdown file as a page, laid out by the browser's engine, links followed (below, "The editor's reader"); a file of the documentation opens so, `--read` opens any `.md` so |
| `nxfiles [FOLDER]` | the file explorer (and with `--desktop` the desktop, above): a tile with an icon for every entry (a name too long for one line goes on in a second, broken at a space when one is near, and only then is cut with two dots), or a list with sizes and kinds (View). Two clicks (or Enter) open: a folder, a shortcut's program, anything else through `open`. Ctrl and Shift with a click, Shift with the arrows and Ctrl+A select several; the selected entries are dragged onto a folder, into another Files window or onto the desktop (Ctrl held: copied). The right button, the Menu key or Shift+F10 bring a menu: Open, Edit, Cut, Copy, Paste, Rename (typed over the name, "A new name" above), Delete, Save to persistence, Shortcut on the desktop, Properties (a window of its own, `nxfiles --properties PATH`: the icon, the kind, where it is, its size or how much a folder holds, when it changed (the ROM keeps no dates: the image builder writes each file's into `/usr/share/rom-dates`, a line a file, and a file of the image has its date from there; `NXFILES_PRINT=1` prints the window's lines), what may be done with it, what a shortcut starts); on a folder also Paste into it and Terminal in it; on no entry View, Refresh, Select all, Paste, New (a folder, a text file), Terminal here (`nxterm` with its shell in the folder shown; on the desktop too), Properties. In the list a click on a column's heading puts the entries in that order (Name, Size largest first, Kind). Entries dragged to a folder that has one of the same name stay where they are: nothing is replaced unasked. Back, Forward, Up, Home, View, New folder and Terminal (a terminal in the folder shown) are keys with signs (`ui_glyph.h`; greyed when there is nowhere to go), with the folder's path beside them: a click on the path, or Ctrl+L, and another is typed there (Enter goes, Escape leaves). Down the left side are the places (Home, Desktop, Pictures, Music, Videos, Computer, and Network, which starts `nxnet`), the one shown picked out. The status line says how much can still be written. Keys: arrows, Page Up and Down, Home, End, letters typed one after another for the entry that starts so, Space to select, Backspace or Alt+Up for the folder above, Alt+Left and Alt+Right back and forward, F2, Delete (asked about first: "Delete these 3 entries?"; a folder goes with everything in it), F5, Ctrl+C, Ctrl+X, Ctrl+V (the entries are remembered in `/tmp/clipboard.files`, for any Files window), Escape. It reads the folder again when something else changes it. A name may have 127 letters and a folder 512 entries: what is over is counted in the status line ("3 more are not shown"), not shown cut short as an entry that cannot be opened |
| `nxweb [ADDRESS]` | the browser (`docs/apps/web.md`): pages with their style sheets, in the ROM's fonts and any script, tables and boxes side by side, forms that search; the page is drawn into a pixmap in strips ahead of the view, by worker cores, and scrolled as a copy. Menus (File, Edit, View, Go with the session's pages, Bookmarks, Help), a bar with Back, Forward, Reload or Stop, Home, Bookmark and the address, a status line, wheel and scroll bar; options and bookmarks in the registry. Keys: arrows and the paging keys scroll, Tab goes from link to link (its address in the status line) and Enter follows, Backspace or Alt+Left goes back, Alt+Right forward, Alt+Home home, F5 reloads, Escape stops, Ctrl+L or F6 opens the address, Ctrl+D bookmarks, Ctrl+F finds and F3 again, Ctrl+O, Ctrl+S, Ctrl+U open a file, save the page, show its source, F10 opens the menus |
| `nxpaint [FILE]` | the paint program (below, "Paint"): ten tools, a flood fill, a selection that is moved, copied and cropped to, pictures of any size up to 1,600 x 1,200, zoom, every change taken back and made again. Opens every kind libmedia reads (`docs/system/media.md`); saves PNG, JPEG, BMP or PPM by the name's ending, encoded on the worker cores |
| `nxview [FILE]` | shows a picture as Windows Photo Viewer does: Open, Edit and Set as desktop above it; zoom out and in, actual size or fit, the folder's picture before and after, a slide show, a quarter turn either way and Delete (asked first) on a row below; and a line saying its name, size, zoom and place in the folder. The wheel zooms round the pointer (fit to 1,600%, stopping at 100%), and a drag moves a picture larger than its room; zoomed or turned, only the part in view is made from its pixels, on the worker cores, into a pixmap the GPU keeps. Fit does not enlarge a small picture. Arrows, Page Up and Down, Home and End go through the folder; F11, Enter or two clicks show it over the whole screen; F5 starts a slide show (`NXVIEW_SLIDE=ms`, Space pauses). A file it cannot open is said in a question (`UI_ASK_PRINT`). `nxview --decode FILE OUT.ppm` decodes without a window and says what it took; `NXVIEW_WORKERS=N` asks for N workers, `NXVIEW_SUM=1` prints a sum of the result |
| `nxsettings` | the settings, a tab each: the desktop's picture (the image's, a picture file of any kind through the file chooser, or a colour), the screen's size (only those the host says it can show, in its flags word; the desktop is taken down whole and started again with it: `nx restart`), the sound card's volume, what the host keeps of `/persistence` (Storage: save now, load at start, clear; `docs/system/persist.md`), and what the machine is, every line of it asked of the machine as it is: the kernel, the cores (the geometry's words), float instructions (one is run, with a handler for the trap), the first core's speed since the last look (`rdcycle`), memory, the ROM's size, the screen, the sound card, the network's address, the time since power-on. A picture that is not a PPM is decoded on the worker cores where it is shown (below, "Pictures for the desktop"). From a script: `nxsettings apply` (the chosen desktop, as the `nx` script runs it), `size WxH`, `volume 0-100`, `choose [FOLDER]` (the file chooser alone; prints the path), `system` (the System tab's lines, printed; no desktop needed). Keys: Tab goes round the row of tabs and the tab's controls, arrows change the tab, the list's row or the slider; in the file chooser arrows, a letter, Enter, Backspace for the folder above, Escape |
| `nxmon` | the monitor, laid out as Windows 7's Task Manager. Applications: the desktop's windows (End Task closes, Switch To brings one forward, New Task runs a line through the shell). Processes: each program's share of the first core, its workers (`/proc/gpu`, `docs/machine/multicore.md`), GPU lists and memory, sorted by any column; End Process asks, then `SIGTERM`, and `SIGKILL` two seconds later; the right button for the process tree and the priority. Performance: core 0's and memory's meters, and their last 60 samples: the instructions of core 0 with all workers' stacked on them (or a cell a core of a 16 x 16 grid), memory in use. Networking (when there is `lan0`): its traffic. View sets the speed (0.5, 1 or 4 s, or paused); the graphs are pixels the worker cores write into the window's buffer once a sample, and a tab reads only what it shows (`/proc/PID` only on Processes). Kept in the registry: `nxmon.page`, `nxmon.cores`, `nxmon.speed`, `nxmon.kernel`. `NXMON_SHOW=applications\|processes\|performance\|cores\|networking` starts on one |
| `nxnet` | the other computers of the network, and what their owners share (`docs/system/lan.md`, "The Network program") |
| `nxterm [COMMAND \| -e PROGRAM ...]` | the terminal: an xterm-like screen (`TERM=xterm-256color`: scroll regions, insert and delete, 16, 256 and direct colours, bold, italic, underline, the alternate screen for vi and less, bracketed paste, the mouse for programs that ask) over a pseudo-terminal running the user's shell, started by vfork; a command given runs instead, and the window closes when it ends. 1,000 lines to look back through by the scroll bar, the wheel or Shift+Page Up and Down. The pointer selects (two clicks a word, three a line) and what is selected is on the clipboard; Ctrl+Shift+C and V, Shift+Insert and the middle button paste. The right button's menu: Copy, Paste, Select all, Clear, Reset, Text size (the ROM's Liberation Mono or the window system's font; Ctrl+= and Ctrl+-), Colours, New terminal; size and colours are the registry's `terminal.size` and `terminal.colours`. Only changed cells are drawn, a scroll is one copy, and box-drawing characters are drawn to fill their cells. SIGUSR1 prints the screen, a row a line after `nxterm\|`, for a test |
| `nxcalc` | the calculator, as Windows 7's: Standard (memory, percent, square root, left to right), Scientific (degrees, radians or grads, Inv, powers and roots, logarithms, factorials, brackets, with the operators' precedence) and Programmer (hex to binary in a word of 8 to 64 bits, the bitwise operators and shifts, a bit flipped by a click) from the View menu; a history of results (Ctrl+H) that a click recalls; the Windows calculator's keys; Ctrl+C copies the display and Ctrl+V types the clipboard's text as keys. Numbers are doubles, the compiler's software arithmetic, shown to 15 digits; the mode and options are the registry's (`nxcalc.*`). `nxcalc -e KEYS` types the keys without a window and prints the display |
| `nxclock` | the clock, a tab each: an analogue face as large as the window (drawn once a minute into a pixmap the GPU keeps; a second is a copy and the hand), the time in large figures with the date, a stopwatch with laps, a timer and an alarm that ring (a notice, and a beep through the sound card for up to a minute). It draws once a second, ten times while the stopwatch runs and is shown. Settings are the registry's (`nxclock.*`). `nxclock timer N` starts a timer of N seconds, `nxclock alarm HH:MM` sets the alarm; `NXCLOCK_PRINT=1` prints where its buttons are and when it rings |
| `nxtetris` | Tetris by the guideline: 7-bag, SRS turns with wall kicks, hold, five next, ghost, soft and hard drop, lock delay, levels 1 to 20, T-spins, combos, back-to-back. Only cells that change are drawn, a block four rectangles. The best five scores, the start level and the ghost are kept in the registry (`nxtetris.*`). Keys: arrows, Up or X and Z or Ctrl turn, Space drops, C or Shift holds, P or Escape pauses, F2 a new game, F10 the menus. `NXTETRIS_AUTO=N` plays N pieces itself and prints what it took (`NXTETRIS_SEED`) |
| `nxmine` | Minesweeper as Windows has it: the Game menu's three levels and a board of one's own, a first square that is never a mine and opens an area, the right button's flag and question mark, both buttons or the middle one on a number to open round it, and the face that starts a game again and shows what the pointer is doing. The squares, the counters' digits and the faces are drawn once into a pixmap and copied from it: a square is one copy, a row of covered ones one copy. The fastest time of each level is kept with a name (`nxmine.time.LEVEL`, `nxmine.name.LEVEL`). `NXMINE_SEED` lays the same mines each time and `NXMINE_PRINT=2` prints them, for a test played with `nxkey`'s arrows, Space and F |
| `nxpinball` | Pinball (`docs/apps/pinball.md`): two flippers, a plunger, pop bumpers, slingshots, lanes, drop targets and an orbit, three balls a game, the high score in the registry (`nxpinball.high`). The table and every look of what changes on it are drawn once into a pixmap; a frame is a copy for each thing that changed and the ball. Keys: Z, Left or left Ctrl and /, Right or right Ctrl the flippers, Space or Down the plunger, P or Escape pauses, F2 a new game. `PINBALL_DEMO=N` plays N frames itself and prints a sum |
| `nxeyes` | the eyes, two round windows without a frame on top of the desktop, with the desktop's own pixels copied into their corners (the window system's layers are opaque rectangles); a left drag moves them, the right button gives the menu (blink, three sizes, close), Escape or q closes them, and the size is the registry's `nxeyes.size`. They read the pointer from the machine's input words (`docs/machine/input.md`) and do not select its moves, so the quiet rectangle stays (below): a look every 66 ms while the pointer moves and every 250 to 500 ms while it is still, from a `select()` on the socket, and only the pupils are drawn. Blinking is the registry's `nxeyes.blink` |

**Pictures** are libmedia's (`docs/system/media.md`), for every program, on the worker cores.

**Fewer waits for the window system** (`linux/apps/ui_width.h`, which `ui.h` and `ui_menu.h`
include). The server draws what a program sent and then waits for the GPU's copy, each time
its socket runs empty; so what costs a program is how often it makes the server stop, not
how much it asks for.

- A text's width is worked out by the program (`ui_width`): the font's widths are asked for
  once. `GrGetGCTextSize` in the middle of a redraw is an answer waited for, and the half
  redraw drawn and waited for before it.
- The client library keeps 16 KB of requests before it writes them, so that a redraw is one write
  and one wake of the server, and reads a reply with one `read` ("Changes to Microwindows").
- A program that draws many labels measures with `ui_width` and asks the server nothing
  while it draws.

**A question in a window** (`linux/apps/ui_ask.h`): `ui_ask(title, text, answers, count, others)`
puts up a small window in the middle of the screen with the text (lines parted by `\n`) and a
button an answer, and comes back with the answer's number, or -1 for Escape or the caption's
box. The first answer has the keyboard; Tab and the arrows go round, Enter or Space presses, a
letter presses the answer that starts with it. The editor, Paint and the IDE ask it before a
changed text or picture is let go, Files before it deletes, and the video player says in one
why it does not play a file. `nxask TEXT [ANSWER ...]` is the
same for a script, which gets the answer's number as the exit status (255 for none), and
`nxask --closing` asks every program's windows to close as their caption's box does and waits
for them: a program with changes asks about them first, and a window low on the screen (which
never takes the keyboard from that question) offers "Don't wait" and "Cancel" meanwhile.
`UI_ASK_PRINT=1` prints each question, its answers with their places and where the window is,
for a test: `(nxkey 2500 n) & nxask 'Sure?' Yes No; echo $?` must print 1.

**The clipboard** is a file, `/tmp/clipboard` (`ui_clip_set` and `ui_clip_get` in `ui.h`; the
terminal reads and writes the same file): one piece of text that is there after the program
that left it has gone. The window system's own selections are a conversation between two
programs that both have to be running.

**The Start menu's System folder** (`nxapps.90-system`, written by `linux/apps/build.sh`) has
"Restart the desktop" (`nx restart ask`) and "Shut down" (`nxoff`): both ask first (`nxask`),
then ask the open programs to close (`nxask --closing`), so that a text or a picture with
changes is asked about and not lost; Cancel in either leaves everything as it was. Settings'
"Use the selected size" goes the same way (`nx restart ask SIZE`). `nx restart` and `nxoff now`
ask nothing, for a script. "Save and shut down" (`nxoff save`) saves `/persistence` between
the two (`nxpersist save`, `docs/system/persist.md`) and leaves the machine up if that fails. Both scripts move to a session of their own (`setsid`), so that one typed in a
terminal of the desktop outlives that terminal.
Then the desktop's programs and the window system are ended, and Linux powers off. The machine has no switch a program can
reach, so the kernel's power-off (`shaderemu_power_off` in `linux/kernel/shaderemu_input.c`)
stands still with interrupts off until the machine is switched off where it is shown.
When Linux has stopped, `nxoff` asks the host to switch the machine off (`nxpersist off`: the
address `power:off` of a fetch): the world does (`EmuMachine.FetchAsked`, as its Power key);
the harness and `rvc_cpu` do not, and there the machine stands still.

**Pictures for the desktop** are `/usr/share/wallpaper-NAME.jpg`, made on the host by
`python tools\make_wallpaper.py [FOLDER ...]`: the Unsplash photographs listed in
`linux/apps/wallpapers.txt` (a name and the photo's id a line; fetched, never kept in this
repository) and every picture in the folders given. One 1920 or more across is brought down to
fit 1920x1080, one 1280 or more to fit 1280x720. Each is a JPEG in some 512 restart intervals,
and beside it are its twins cut to the common screens, `wallpaper-NAME@800x600.jpg` (the
machine's own), `@1280x720` and `@1920x1080` (Settings does not list them). `--from-ppm` makes
the JPEGs of an older build's PPM pictures; `--twins` makes the twins missing from the pictures
there. Run it before `make_linux_image.py`. The choice is the registry's
`desktop.wallpaper` (`ui_wallpaper` in `ui.h`); a new image shows `wallpaper-fox.jpg`, and a
choice of an image's PPM that is now a JPEG is that JPEG. Settings keeps the picture file
itself, never a decoded copy: its chooser lists every kind libmedia decodes (JPEG, PNG, GIF,
BMP, TGA and the PNM family), a file that is not one is refused with libmedia's reason, a path
over the registry's 250 letters is refused too, and a choice the registry cannot keep is said.
It then leaves the drawing to the desktop, which sees the key change, so the picture is decoded once.
With no desktop running it draws the picture and lets its copy go.
When it opens, the choice in use is its selected row, a picture file of the user's with a row
of its own ("File: Great Wave.jpg"). The viewer's "Set as desktop" keeps the file too, and a
picture it shows turned is kept with its turns in `desktop.wallpaper.turn` ("1:/root/Pictures/Great
Wave.jpg", a quarter turn to the right), which the desktop applies when the path is its choice
(`ui_wallpaper_turns` in `ui.h`). Where the GPU has no room and the picture is drawn from a
copy in `/tmp`, that copy is not turned.

**While it starts** the desktop says so: `nx` runs `nxbar --splash TEXT` as soon as the server
takes a client and again before the desktop's programs, a panel in the middle of the screen
("Starting the desktop", "its picture and icons") that the picture then covers. A picture decoded for the screen or a window (`ui_picture_decoded_at`)
that takes more than 0.4 s has a panel over where it will be: its name, a bar from libmedia's
`media_progress` (the JPEG decoder's rows, a PNG's inflated bytes, the fitting's rows) and the
seconds, drawn every 0.25 s at most and covered by the picture.

**Decoded when the desktop starts** (`docs/system/media.md`): the desktop asks the
window system for a pixmap of the screen's size, which the GPU keeps (its pixels in the pool
the windows' buffers come from; `GrGetWindowInfo` says where a GPU pixmap's pixels are), and
libmedia decodes the picture into it on every worker core: its twin for the screen's size
as it is, another cropped or fitted as below. The pixmap stays the desktop's: drawing the
picture, or the part behind an icon that changed, is one copy of the GPU's. Where the pool has no room for the
pixmap, the picture is decoded into a PPM in `/tmp` and drawn from there. `UI_PICTURES_PRINT=1` says
what was decoded and how long it took.

A PPM file in the ROM is not decoded or copied by anyone. It is already three bytes a pixel, and
the root file system is romfs, which keeps every file in one piece in the ROM: the GPU samples
the file there. The screen driver finds it from the file's number, which romfs makes the
offset of the file's header (the overlay mounted over the ROM passes it on), and checks the
header's size and the file's first bytes through `/dev/mtd0` before trusting it. A file elsewhere (one Paint saved, in
RAM) has its rows read into GPU memory first. Pictures are not clipped by child windows.

The server is started with `-p`: without it Nano-X ends when its last program has gone, and
the program that puts up the desktop's picture is the first to come and go.

## The editor's reader

The editor shows a Markdown file as a page (`linux/apps/nxedit_read.h`): headings, emphasis,
strong and struck text, code in a line and in blocks (indented or fenced), lists in lists,
quotes, GitHub's tables with their alignment, links, rules; a picture is its description. View's
first item, Read (Markdown), or Ctrl+R goes between the page and the text, at the same place
in the file. A `.md` file under `/usr/share/doc/` opens as a page (the documentation: "The
desktop", Docs), `nxedit --read FILE` opens any so, and every other one as text.

- **The browser's engine lays it out**, with the ROM's fonts (`css.h`, `web.h`, `web_layout.h`;
  `docs/apps/web.md`): `nxedit_md.h` makes the text HTML (CommonMark's blocks, its rules for
  emphasis, a style sheet like GitHub's) and nothing of the window system is in it, so
  `tools/web_test.c FONTS FILE.md --sections` lays a file out on the host as the editor does and
  prints the same line as `NXEDIT_DUMP=1` does in the machine (nodes, size, texts, links, boxes,
  texts past the edge, and a sum of every item and character).
- **A section at a time** (`nxedit_page.h`), since the engine is about 300 instructions a byte. A section is the
  text from a heading at the start of a line, outside a fenced block, to a later one (2 KB at
  least for the first, 8 KB after), made HTML and laid out alone; its items are copied into
  the reader's own list under the sections before it, placed by an empty block at the end of
  the one before (where its next heading would have stood). The page is the whole page's to the
  pixel (`--sections` against a whole layout on the host, every file of the documentation).
  The first screen is there when its section is, and the rest is laid out while no event
  waits; the scroll bar and the status line ("Reading 12%", "Reading (laying out)") follow it.
  A file of 114 KB: the first screen in 362 ms, the whole in 5.6 s (`NXEDIT_TIME=1`, calibrated, 31 October 2026).
- **Keys and the pointer**: arrows, Page Up and Down, Space, Home and End, the wheel and the
  scroll bar move the page; a click on a link goes to a heading of the page (`#id`, GitHub's
  ids), to another `.md` file in the same tab (Backspace or Alt+Left come back; the right
  button's menu has Back and Edit the text), a file of another kind through `open`, an address
  through `open` too (the browser). A letter typed says that Ctrl+R edits. A command that
  changes the text goes back to the text first.
- The code's spaces at a line's start are no-break spaces: the engine starts a line at its
  first word, in a `<pre>` too.
- **What the editor's start pays for it**: the engine is 140 KB more of the program, which the kernel maps and
  copies at every start, 42 thousand instructions (3%, `rvc_cpu`, 24 October 2026). The engine's tables of names are
  names in place, not pointers, which would be relocations at every start (`web_entities`, `css.h`'s tags, colours,
  fonts, boxes and kinds), and its 60 KB of faces are allocated when a page is first laid out (`WEB_FACES_LATER`).

Test: `tools/test_all.py nxedit-read` (`/usr/share/md-test.md`, `linux/apps/md-test.md`, a page of
every kind, must give web_test's sum; a document opens as a page, a `.md` elsewhere as text).

## Changes to Microwindows

`linux/nanox/microwindows.patch`, applied by the build:

- **One read per client, not three system calls per request.** The server takes everything a client has sent with
  one `read` and handles all of it, each request where it lies in what was read, before it waits again.
- **A cursor the screen driver shows.** With `gd_hwcursor` set the engine never draws, hides
  or saves under the cursor, so drawing does not stop to step around it.
- **A program's own picture as the cursor.** A window's cursor of 32 x 1 whose mask is the
  words `0x5345`, `0x4355` is not a bitmap: its two words of bits are the address of 32 x 32
  colours (`0xAARRGGBB`) in the GPU's memory, and the driver gives the display that address
  with the cursor's hot spot (`gpu_cursor`). The picture is shown as it lies there, so the
  program changes it in place; no address is no cursor at all. Tiberian Dawn and Red Alert
  show their pointer this way (`docs/games/tdawn.md`).
- **Partly hidden rectangles are filled by clip rectangle**, not one row at a time.
- **Composition** (`gd_compositor`): a window draws on the surface the screen driver gives its
  top-level window; windows do not clip each other; moving, raising, lowering and
  unmapping a top-level window repaint nothing; the driver is shown the visible windows in
  order when it composes.
- The default cursor is the familiar arrow; new windows step across the screen instead of
  piling up at one place.
- **The client library** keeps 16 KB of requests before it writes them (`SZREQBUF`), and reads
  a reply with one `read` (`TypedReadBlock`: the type and the data leave the server together).
- **A driver may draw a picture file itself** (`gd_drawpicture`, asked first by
  `GdDrawImageFromFile`).
- **A new window's frame is painted when it is made**, not when its program first asks for events, though the window
  manager lives in the server and works from each program's own event queue: a game that takes a minute to start
  has its frame at once.
- **The keyboard always has a window** ("The keyboard" above): a press anywhere in a framed
  window activates it, the keyboard passes on when its window goes, and the server takes the window manager's keys
  before any program sees them (`window_key` in `srvevent.c`).
- **A colour is no reason to work a window's clip out again.** `GsPrepareDrawing` rebuilds the clip region when the
  window, the context's region or its clip mode changes; and a changed context tells the engine only the fields
  that differ from what it was told last.
- **A string is walked once.** The driver measures (`gd_measurestring`) and writes the command's words as
  it does; `gpu_drawstring` uses them when the same string is drawn next, which it is.
- **The index of icons is read once**, for the first window.
- **The server waits with `epoll`.** A client is watched from when it is accepted (`GsWatchFd`).
- **An answer is one write.** `GsWriteType` keeps the two-byte type back and `GsWrite` sends both (`writev`), so a
  program is not woken for two bytes; `GsFlushType` after every request and before the server waits.
- **The window manager.** A window dragged smaller than its frame stops at 96 x 24 (a corner dragged past the
  window's other corner leaves a layer of 102 x 53 in a snapshot, and the program running). A top-level window made
  smaller does not expose its parent: after a shrink the snapshot's desktop must be the picture there. A press on a
  frame's 3-pixel border leaves the keyboard with the window inside (`nxask` and a click at its border, then a
  letter: the letter must answer). A window manager's key held down acts once, not at the keyboard's repeat.
- **A system colour is asked for once** (`GrGetSysColor` in the client library keeps the answer).

## Fewer questions to the window system

A question to the server and its answer are many times a drawing request, nearly all of it the kernel's (two sockets,
two switches of process), so what a program of the desktop costs is mostly how often it and the server wake each
other. What a program asks often, it should keep.

- **A program waiting for an event is not answered with one the window manager took.** The
  window manager lives in the server and works from each program's queue. The server gives the next event of the
  queue or lets the program wait on (`GrGetNextEventWrapperFinish`).
- **The next event is asked for once.** The client library remembers that it has asked until the answer comes
  (`event_asked` in `client.c`).
- **A colour or a font a context already has is not sent again** (`ui_state.h`, under every
  `GrSetGCForeground` and `GrSetGCFont` of a program that includes `ui.h` or `ui_menu.h`).
  A program that changes a context's colour by another call must not rely on it.
- **The pointer's moves are selected while a button is held** (`ui_moves` in `ui.h`) and not
  otherwise: a window that selects them answers an event for every move over it.
- The screen's size is asked once (`ui_screen()`); a program with a tick of its own
  (`nxmon`'s frames) asks for events once with `GrPrepareSelect` and waits in its own
  `select()`.
- Launchers start a program with `vfork` and `exec` (`ui_run`, the bar, Files, the viewer,
  `emuinit`, `emumux`): no page of the launcher is copied for the child.

## One program's windows are not every program's news

Every program has the root's child updates selected, because the window manager works from
each program's queue, so nothing but these rules stands between any top-level window's map, activation, unmap and
destruction and a wake of every program.

- An undecorated top-level window (a menu, a panel) is no news to the root's listeners but
  for its being shown (`GsDeliverUpdateEvent`).
- A root child update reaches a program only if it selected them itself (`own_chld_update`,
  set in `GrSelectEvents`; the bar does), and then a frame's too, though the window manager
  took it. The window manager's own selection is marked by `wm_selecting`.
- The keyboard going to a program's own menu and back activates nothing (`GsMenuOf` in
  `GsSetFocus`), and the program's frame stays active under its menu (`GsFocusInPopupOf`).
  Leaving the menu for another program's window tells the menu's program's frames.
- A frame is painted again for an activation only when what it shows changed
  (`wm_activating`, `painted` in `nanowm.h`): its size, title, properties, icon, active or not.
- `nxbar` looks for its buttons when an update names a window it has no button for, asks one
  window's title when that window is activated, and the focus when any update came or 15 s
  went by (`linux/nanox/nxbar.c`); every 60 s it looks anyway. A frame's program window is asked once.
- `ui_window_origin` keeps a window's place from its last click (`ui_origin_note`, which sees
  every event of a program that opens menus by its window's place) until an update.

What a program's handling of input costs is measured over the same `nxkey` line sent to a
window that ignores it (`demo-hello`), which takes the tool's own sleeps and the kernel's
input path off. A letter in `nxterm` is the least that can be cut: key, terminal, shell, terminal and server are five
hand-overs of the kernel's.

## The pointer on a core of its own

A worker core (`docs/machine/multicore.md`) follows the pointer, and the first core hears of a move only where somebody
wants to.

- **The worker** (`wm_job` in `scr_shaderemu.c`) reads the pointer's words every pass and
  writes the display's cursor words, and the layer of a window that is being dragged. It
  touches GPU memory and a stack of its own and stores to texels nobody else does. A tenth
  of a second after the last change it sleeps on its job word, which the kernel's tick
  changes when the pointer moves (`shaderemu_gpu_wake`) and the driver when it has news.
- **It is started when the pointer first moves** (a machine nobody points at has none) and
  its core is only lent: its answer and "taken" words are kept the same as its job word,
  which to the kernel is a core with no job, so a program that keeps its workers has the
  core at once, and the driver takes the cursor back (`wm_lost`). It asks again when cores
  come free. `NANOX_POINTER_CORE=0` is the same binary without it.
- **What it is told** is texels in the layer table's page (`WM_OFFSET`), each with one
  writer: the cursor (how far its image's corner is from the pointer, the image) and a drag
  (a surface's buffer, how far its corner is from the pointer). The kernel's rectangle is in
  a free part of the worker cores' mailbox pages (`WM_QUIET_OFFSET`), which the kernel has
  mapped already: a boot and a program's first workers pay nothing for it.
- **A cursor turned off behind its back** (`cursor_check`): before the server
  waits it compares the display's "on" word with its own, and where a program that left (or
  the kernel at its exit) turned it off, the worker is woken to store it again, or the server
  stores it when there is no worker. The cursor's texel keeps one writer: the kernel clears that word at a
  program's exit only when no other program has `/dev/gpu` open.
- **The quiet rectangle** (`GsQuietRect` in `srvutil.c`): round the pointer, where the same
  window stays under it and nobody who would hear of a move has asked to. The kernel reports
  no move inside it (`pointer_hushed` in `shaderemu_input.c`); buttons and keys are reported, a button with the
  pointer's place. No rectangle while a button is held, over a window that selects the pointer's moves.
- **A drag** (`GsDragWindow`, from `wm_container_buttondown`): the worker moves the layer
  with the pointer until the button is let go, and the window manager moves the window once,
  to where the worker left it. Meanwhile the rectangle is the whole screen. While a window
  is dragged the driver looks at the table again a pass after it changed it: of two cores'
  stores to one texel one is lost.
- **The terminal** selects the pointer's moves only while a selection is held (`sel_moves` in `nxterm.c`).
- **The layer table is made when the scene changed** (`gd_scene_gen`, which goes up when a
  window comes, goes, moves, changes size or order; `layers_stale` for the driver's own
  buffers), not every time the server is about to wait.
- **The kernel's poll returns after its four reads** on a tick with nothing new.

The test is a drag with the first core busy (a shell's loop, a game): where the layer is against where the pointer has
it, pass by pass under `rvc_cpu`, must be every pixel and 1 behind; and `nxray` must still say 255 workers. A run's
instructions shift half a percent with what falls inside it: compare means of several timings, never one run.

Not done: resizing, menus and the frame's buttons are the window manager's, and a
window's program still has to take the press from its queue before a drag begins.

## Events that are sent, numbers a program chooses

- **Events are sent as they come.** A program says once that it wants its events
  (`GrNumPushEvents`, sent by the client library at the first call that looks for one), and
  the server writes each as it comes; looking for one is `recv` with `MSG_DONTWAIT` on the
  socket. The server never waits for a program to read: what a socket has no
  room for is held (`GsPushEvent`, `GsPushHeld` in `srvnet.c`) and nothing more is taken
  from that program's queue until it has gone. The window manager, which works from a
  program's queue, sees a press when it comes. `GrClose` forgets that it asked and drops the events it holds: a
  connection the program opens after it asks for its events again and keeps none of the old one's.
- **A window, a context and a font have numbers the program chose** (`nx_new_id` in
  `client.c`: bit 30, the process, a count; `GrNumNewWindowAt`, `GrNumNewGCAt`,
  `GrNumCreateFontExAt`). They are requests like any other and go out together. After
  32,767 of them a program asks the server again. A font that cannot be made is found out
  when it is used (the server's font then), not by a number of 0.
- **A reply is read with one `recv`, and written with one `write`** from one piece of memory
  (`nx_take`, `GsWrite`).
- **A request on the window and with the context of the one before looks nothing up**
  (`GsPrepareDrawing`'s first lines), and a colour set in the current context is told to the
  engine at once (`GsTellForeground`).
- **A frame is painted once when its window is made**, not again at its first exposure
  (`fresh` in `nanowm.h`); the list of icons is read without `sscanf` and a program's file is
  not opened to see whether it carries an icon: the screen driver looks (it keeps the ROM's
  device open and 64 files it found), and draws the sheet's cell for a file without
  (`GR_IMAGE_ICON_OR`).
- **`GdAllocBitmapRegion` makes a shaped window's region as it reads the bitmap**, not by a union a run.
- **The kernel does not wake a reader for room to write** (`linux/kernel/unixwake_hook.py`):
  when the server takes a question from the socket, the program asleep reading the answer sleeps on.
- The bar asks about the window an update named, not about every window (`note_window`);
  the explorer sorts a folder's numbers by a merge and clears no entry whole (`sort_entries`);
  the editor draws its status line's edge with the window, not at
  every key, a move through its text draws the text, its scroll bar and the status line, and a view that moved a few
  lines is a copy of what stays (`view_moved`, `NXEDIT_SCROLL=draw` draws it whole); the keyboard and the pointer are
  read once a wake; `PATH` has `/usr/bin` first.
- The Monitor's texts are made without `printf`, a line or a row of the statistics is drawn when it reads otherwise,
  and a process's `stat` is one `read`. To compare two builds of it, give both made-up samples: its numbers follow how
  fast everything ran. A `/proc/stat` comes in more than one piece, so it is not read with one `read`.
- `nxkey hold:X,Y to:X,Y drop:X,Y` is a drag a step at a time: a software server drops what comes faster than it
  draws, so a test's keys wait for it.

A program's start, thousands of the first core's instructions on `rvc_cpu` (31 October 2026; three seconds from the
command under a bare `nano-X -p`, less the same seconds without it, one run each):

| | first window of a session |
|---|---|
| Terminal | 1,175 |
| Calculator | 1,099 (a later one 787) |
| Editor | 740 |
| Files | 806 |
| Monitor | 1,932 |
| Settings | 1,546 |
| `demo-hello` | 445 |

Not done: pipes in place of the sockets, a program's pictures and fonts asked for in one piece.

## Checking it

`NANOX_SOFTWARE=1` makes the same driver draw everything in software. The framebuffer in RAM
after a scene is then the reference for the same scene drawn with the GPU. Composition is done by the GPU in both.

`nxbench` (`linux/nanox/nxbench.c`) times drawing through the server (`rvc_cpu`, 31 October 2026; the card's time
calibrated):

| | instructions, thousands | calibrated |
|---|---|---|
| 25 fills of 480x320, each waited for | 262 | 71 ms |
| 100 lines of text (5,400 characters) | 213 | 49 ms |
| 15 scrolls of the window by one line | 200 | 50 ms |
| 200 buttons (face, edges, label) | 900 | 185 ms |

## Where the time goes

`python tools/prof.py` on an `RVC_PROF` run (`docs/machine/profiling.md`). On the card's own run `rvc_harness --pc-log FILE`
records the guest's pc once per emulator frame and `tools/pc_profile.py` turns that into a profile by function, across
the kernel, the server and a client (give it `nm -n` output for each).

Still in software: images, and anything blended. Not done: resizing shows no outline (the
window manager draws it on the desktop, which is behind the windows), and only one
OpenGL program can run at a time: the GPU's memory for programs has fixed places, the kernel
gives it to the first that asks (`SHADEREMU_GPU_DRAW` on `/dev/gpu`, kept until the program
closes or ends), and a second one says whose it is ("the GPU is drawing for doom (process
41)...") and ends.
