---
name: desktop-apps
description: Write or change a program for the ShaderX86 Nano-X desktop - where programs live, the window, events and the first frame, drawing cheaply, nxkey tests, captures, the registry, /persistence, the Start menu, icons, opening files, starting from the ROM, and test_all cases of the desktop kind. Use when adding or fixing a desktop application (nxfiles, nxedit, nxpaint, nxweb, nxview, nxmon, nxide, nxsettings), a window system change, or a program's start time. Triggers on: Nano-X, nx program, linux/apps, nxapps, nxopen, nxkey, GrOpen, ui.h, registry.h, nxreg, persist, desktop app, a window with no frame, capture of the desktop, a program's start.
---

# Desktop applications

Depth: `docs/system/nanox.md` (long: read "Where things are", "A window before the work", "The keyboard", "A program starting", "Events that are sent"), `docs/system/open.md`, `docs/system/persist.md`, `docs/apps/*.md`. A game: `porting-games`. Memory and the machine: `docs/system/nommu.md`.

## 1. Add a program

`linux/apps/` has the desktop's programs and `linux/apps/build.sh` compiles them. Small ones are one `.c` with headers: `ui.h` (widgets), `ui_menu.h`, `ui_icons.h`, `ui_files.h`, `ui_ask.h`, `ui_exec.h`, `ui_launch.h`, `registry.h`, `ui_bulk.h`. Start from the closest: `nxcalc.c` (small), `nxask.c` (a question), `nxmon.c` (workers shading a window's buffer).

Then, in the build script:

1. Start menu: a line `Category/Label=command` in `/usr/share/nxapps.NN-name` (`NN` orders; each file is written by its build script). The desktop's shortcut comes from it.
2. Files it opens: `/usr/share/nxopen.NN-name`, `kind endings first-bytes command %s`, and a row in `docs/system/open.md`.
3. Icon: `linux/apps/icons/NAME.png`, a line in **both** `linux/apps/icons.txt` and `icons7.txt` (names are programs as the kernel calls them), then `python tools\make_icons.py`; credit in `linux/apps/icons/README.md`.
4. Build: `building-the-system`, "One program". `README.md`'s table of programs and `docs/README.md` when it is new.

## 2. Things a window program must do

- **Map first, say what it is doing.** Seconds of work before anything shows: `GrMapWindow`, then `ui_framed()`, or the window stands with no frame at the screen's corner until the work ends. To check: the program, `sleep 2`, and the layer in a snapshot has the frame's size.
- **End the pass when idle.** `GrGetNextEventTimeout`, never a spin on the clock (the clock moves only between passes).
- **A first core with nothing to do** while workers work calls `mcw_rest()`. Workers lent, not kept: `mcw_lend()` (`nxmon`), so that a program wanting all 255 gets them.
- **Read a pixel first** (`GrReadArea`) before writing a window's buffer directly: `GrGetWindowInfo` does not wait for the GPU.
- **Fewer questions.** Events are pushed (`GrNumPushEvents`); `GrGetWindowInfo` and the like are round trips of two system calls and two task switches. What changes the window tree raises `gd_scene_gen`, or the layer table and quiet rectangle are stale. A window, GC and font's ids are the program's own (requests 127 to 129).
- A program with its own `select()` on the socket still gets one event a `GrServiceSelect`.
- A string cut by a pixmap's edge is dropped by the window system: draw into a scratch with rows over and under (a form in `nxweb` rendered blank because of this).
- `ui_exec.h` and `ui_launch.h` for starting others: exec failures and early deaths are reported with `nxask` (`UI_LAUNCH=print`).

## 3. Speed

A program's start is measured as the first window of a session and as a later one: the first pays for the icon list and the font. `python tools/prof.py report` with `--find nxNAME`.

- Whole files under 32 pages map whole; a read costs 8,000 instructions however small. Map data.
- One core keeps about 5.25 KB of texels a pass. Do not clear a table per page or frame; decoded pieces are made where they lie; megabytes go to borrowed workers and are not waited for until the memory is next used (`ui_bulk.h`).
- Pictures, sound and films through libmedia, on the workers (`docs/system/media.md`). JPEG in restart intervals or PNG in segments for what a tool writes.
- Whether the desktop lets others run is measured by another program's frames: glxgears' 5 s lines under `rvc_cpu` while the load runs, as a share of its lines with nothing else (AGENTS.md).

## 4. Settings and saves

- A setting: `reg_get`/`reg_set` (`registry.h`) as `program.name`, set at once, read at start; `nxreg get|set|unset|list` for scripts; a row in `docs/system/persist.md`'s table. Not a file of its own. A test that needs the defaults must not read it (`RAY_FRAMES` is how).
- A file to keep: under `/persistence/NAME` (a starter's `cd`, `HOME` or link). 96,000 packed bytes for everything; save is asked for, never by itself.
- Without `/persistence` a set fails: the program goes on with what it has.

## 5. Test it

**Scripted input.** `nxkey`, through the keyboard's device: `nxkey meta down enter 500 ctrl+tab`; a number waits; `X,Y` clicks; `r:X,Y`, `drag:X,Y:X,Y`, `wheel:X,Y:N`, `hold:X,Y to:X,Y drop:X,Y` for a server that drops fast moves. The first window of a server has its inside at 43,46; a second is stepped across, so a test that clicks starts one program a server. Characters need their keys: `'shift+;'` for a colon.

Programs print state for tests: `NXFILES_PRINT=1`, `NXWEB_DUMP=1`, `NXPAINT_SCRIPT="new:200x120 ... sum quit"`, `UI_ASK_PRINT=1`, `NXMON_PRINT=1`, `MICROPOLIS_REPORT=1` (places of windows). Give a new program such a switch from the start, plus a sum of what it made.

**A case.** `tools/test_all.py`:

    case('nxask', 'desktop', X + "(nxkey 2500 n) & nxask 'Sure?' Yes No; echo ASK=$?", has=[r'^ASK=1']),

`X` starts `nano-X -p`. Sleeps are in guest seconds, cheap. Watch ports: `nxweb-http` uses fixed ports, and a case run beside another on them fails. See `running-tests`.

**A capture.** `rvc_cpu` has no GPU or display. Through the queue, no card time:

    pwsh tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_opt --image linux-net --cpu --warp --no-desktop --expect "/ # " --send "nano-X -p & sleep 2; CMD; sleep 5; echo LX-''DONE\n" --until LX-DONE --save-state F.snap
    python tools\snapshot_screen.py F.snap OUT.png

A GPU list that reads memory a program wrote a frame before wants `--cpu-fresh`. Do not capture from `--screen-log`. Look at the picture: a blank form passed every count.

**The real desktop** is `nx` (`--desktop`), not bare `nano-X -p`: the bar, the desktop and `nxpersist load` are there. OpenTTD's close-and-it-stays bug showed only so. Test closing: Alt+F4, then that `cat /proc/gpu` holds nothing of it; "One program at a time draws" makes a program that stayed fail the next.

**The pointer**, with the first core busy: drag and compare where the layer is, pass by pass (`nxray` must still say 255 workers: `docs/system/nanox.md`, "The pointer on a core of its own").

## 6. The browser, the IDE, the HLSL

Each has its own rules: `nxweb` draws in strips (`strip_job` draws what `draw_item` does: change both; `NXWEB_CHECK=1` in the harness with `--cpu --warp`), its engine has no window code (`tools/web_test.c`, `tools/web_net_test.c` on the host, `test_all.py web`). The IDE: `docs/apps/ide.md`, `nxide-*` cases. HLSL: `programs/hlsl/test/hlsl_test.c`, `test_all.py hlsl`. A `test_all` value depending on fonts: `python tools\make_fonts.py` comes before the image.
