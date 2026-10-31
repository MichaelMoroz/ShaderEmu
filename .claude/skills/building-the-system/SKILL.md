---
name: building-the-system
description: Build the ShaderX86 guest system - the harness (build.bat), the WSL toolchain and ~/nm tree, a private WSL home for parallel work, linux/nommu/setup.sh, kernel.sh, build.sh, root.sh, image.py, tools/make_image.ps1, private images, linux/prebuilt and --save-prebuilt, the kernel hook rules, relinking after a library change, cc-bytes, and remaking snapshots after a layout change. Use when a change must reach the image, when building in a worktree or beside other agents, when a kernel or hook changes, or when a build fails or seems to have no effect. Triggers on: build.sh, kernel.sh, root.sh, image.py, make_image, setup.sh, private image, prebuilt, save-prebuilt, hook, ~/nm, WSL build, build.bat, program not updated in the image, cc-bytes.
---

# Building the system

Depth: `docs/system/nommu.md` ("Building it", "Memory"), `README.md`, `linux/prebuilt/README.md`. The MMU build is not supported or developed for: do not add to it, though its tree and `build/images/linux/root` are still where everything starts.

## The chain

    linux/kernel/build.sh    once: cross compiler and the kernel tree, ~/shaderemu-linux
    linux/userland/build.sh  once: musl's sysroot, init
    tools/make_image.ps1     the shared image (from rvc/ and linux/prebuilt): build/images/linux/root
    linux/nommu/setup.sh     once: ~/nm/work, ~/nm/repo, flat drivers (rv32-cc, rv32-c++)
    linux/nommu/kernel.sh    build/nommu/Image
    linux/nommu/build.sh     programs, into the repo copy's root
    linux/nommu/root.sh      every program flat: build/nommu/flatroot
    linux/nommu/image.py     build/images/nommu       (tools/make_image.ps1 runs it, under the lock)

From the repository's folder, in PowerShell: `wsl -- bash linux/nommu/build.sh quake/build.sh`.

## One program

    wsl -- bash linux/nommu/build.sh NAME/build.sh    # a line: exit code and seconds; the log is ~/nm/logs/NAME-build.sh.log
    wsl -- bash linux/nommu/root.sh                   # or the image keeps the old program
    pwsh tools\make_image.ps1
    python tools\test_all.py TAG

`pwsh linux\nommu\all.ps1 [-Kernel] [-NoBuild] [-Cases "doom-1 nxray"]` is the whole round, with the icons and fonts. From another process its `-Builds a,b` does not split: call `build.sh a b`.

Read each `exit` in the output: `build.sh` goes on past a failed script, and a program that failed keeps its old copy in the root.

## Rebuilding what depends

| Changed | Also |
|---|---|
| musl, libm, `libcompiler_rt`, the drivers | all of `linux/nommu/build.sh`: every program links them |
| `gles.c`, `mcw.c`, `mcwx`, Nano-X's library, an example | `linux/tcc/build.sh`, and `cc-bytes` moves: set `CC_BYTES_NOMMU` in `tools/test_all.py` with the reason |
| Nano-X's library | every window program, by `nanox/build.sh` then its dependants |
| A kernel driver or hook | `kernel.sh`, then `image.py`. `tailtest`'s sum covers `/usr/share/rom-dates`: compare two kernels on one `rootfs.bin` |
| `slim.py`, `flat.py`, `elf2bflt.py` | `root.sh` and `image.py`. A saved prebuilt needs the new `slim.py` too |
| Layout (`layout.h`, `rvc_layout.h`) | the harness (`build.bat`), `kernel.sh`, and snapshots made again: old ones are refused |
| `linux/nommu/setup.sh` | run it, `build.sh`: it restarts `libc.a` from musl's |
| A big program's memory | `python linux/nommu/memory.py`; `BIG` in `flat.py` for a 1 MB stack |
| The fonts, icons | `python tools\make_fonts.py`, `python tools\make_icons.py`, before the image |

A game's script puts its tree back from its patch on each build: write `git diff` to the patch first, or build with `OPENTTD_TREE=keep`, `QUAKE_TREE=keep`, or edits are lost silently (it cost an OpenTTD rebuild in October).

## A private home, for parallel work

`~/nm` is shared: `build.sh` rsyncs the calling repository over `~/nm/repo` (a `flock` makes it one at a time, but your tree then replaces theirs). Beside other agents or in a worktree:

    wsl -- bash -c 'mkdir -p ~/agents/X && cp -a ~/nm ~/agents/X/nm && ln -s ~/shaderemu-linux ~/agents/X/shaderemu-linux'

Then in every WSL command: `export HOME=~/agents/X PYTHONUSERBASE=/home/micha/.local` (numpy lives in the real home's user site). Run `linux/nommu/setup.sh` under it from your worktree, as the drivers hold paths. Traps:

- `setup.sh` deletes the `.a` and `.o` files of `$W/src`: then everything is relinked and built. For one program redirect the copy's drivers (`~/agents/X/nm/work/bin`) to the private tree instead, and rebuild it alone.
- `$HOME` inside a command: use `bash -c '...'` so WSL expands it, and check `readlink ~/agents/X/shaderemu-linux`.
- WSL has no sudo. `ffmpeg` is wanted once for ClassiCube's tune and Half-Life's data, and caches copied with `~/nm` have them. The teapot: `linux/prebuilt/root/root/Models/teapot.obj` into `~/nm/work/src/pathtrace-data/`.
- Bytes must not go by the folder: `-ffile-prefix-map=$NM=/nm` is in the drivers, so `cc-bytes` holds under any `HOME`. A new `__FILE__` or path in a library breaks that.
- A worktree has no `bin\`, `build\` or `rvc\`. `build.bat` makes the harness; copy `D:\Development\ShaderX86\rvc`, or `git clone https://github.com/pimaker/rvc rvc`. Never write into the main checkout's `build\`.

Builds copy no `.claude` (`--exclude /.claude`): a worktree there held 15 GB.

## Private images

`linux/nommu/image.py [NAME [ROOT [KERNEL]]]` makes `build/images/NAME` from a flat root and a kernel, on the shared image, and leaves `build/images/nommu` alone. It **replaces** `NAME/overlay` with ROOT each time, and adds Uplink's data unless `HALFLIFE_DATA=0`. To try one program beside a baseline:

    cp -a build/nommu/flatroot ROOT                              # a copy of the baseline's root
    (the flat program over its copy in ROOT/usr/bin)             # from root.sh in a private home, or flat.py on a folder
    python linux/nommu/image.py PRIVATE ROOT build/nommu/Image
    bin\rvc_cpu.exe --image build/images/PRIVATE ...

- Its shared part is `build/images/linux`: a worktree makes it with `pwsh tools\make_image.ps1` first, from `rvc\` and `linux\prebuilt`.
- `tools/private_image.py NAME` is the part under it, and reads the shared root each time: a "before" made again after a rebuild is not before. `linux/prebuilt/root` of the last commit is, or keep the image folder itself.
- `NAME_OUT` in a game's `build.sh` (`CLASSICUBE_OUT`, `HALFLIFE_OUT`, `MICROPOLIS_OUT`, `NES_OUT`, `OPENTTD_OUT`, `PATHTRACE_OUT`, `PHYS_OUT`, `QUAKE_OUT`) installs elsewhere than the shared root: what lands there is still to be made flat (`linux/openttd/counts.py` has an example).
- Delete private images when done: each is hundreds of MB.

## The kernel's hooks

`linux/kernel/*_hook.py` run in `linux/kernel/build.sh`'s order, and `kernel.sh` installs the repository's drivers and hooks into its copy of the tree (`~/nm/ksrc`), then `linux/nommu/patch_*.py`, into `~/nm/k-rom`.

- A hook skips a file with its marker. After editing a hook: `git checkout` the files it patches in the kernel tree first, or nothing happens and nothing says so.
- A hook that replaces another's text must keep that hook's mark. Run `build.sh` twice after adding one.
- `kernel.sh` stops at a failed make (`make exit N`; errors in `~/nm/k-rom/kernel.log`). Before it did, a failed link copied the old `Image`, and a whole baseline tested a 3-day-old kernel. Confirm `build/nommu/Image`'s time.
- Kernel text patched by a hook that no-MMU does not compile (`faultaround_hook.py`, `icache_hook.py`) does nothing here.
- Upstream's path, a new driver call: patch in `linux/nommu/patch_*.py` and add it to `kernel.sh`'s chain.

## The shared image and the prebuilt

- `pwsh tools\make_image.ps1` only, one writer at a time (`build\image.lock`; it takes the lock after 180 s). A run reading an image being written fails oddly.
- `pwsh tools\make_image.ps1 --save-prebuilt` keeps a build in `linux\prebuilt` (the no-MMU build too). `linux/prebuilt/nommu` holds the current kernel and files over `linux/prebuilt/root`. The MMU part stays old by design, and `make_linux_image.py --save-prebuilt` can stop with "Image is missing": `image.py --save-prebuilt` is the part that matters then. Keep each file under 90 MB; check a fresh checkout makes an image from it. Commit it only when asked: it is 340 MB.
- Data that is not ours to pass on is fetched, never kept: `linux/prebuilt/README.md` has the licences of what is.

## The harness

`build.bat` (Windows, CMake, Visual Studio): `bin\rvc_harness.exe`, `bin\rvc_harness_dxc.exe`, `bin\rvc_cpu.exe`, with fxc2's `d3dcompiler_47.dll`. A running `rvc_cpu` holds its exe: wait, in a shared `bin\`.

## In WSL

`$NAME` is the computer's name, so no build variable by that name. `set -euo pipefail` with a guard for prerequisites. Unchanged programs keep their bytes (`SOURCE_DATE_EPOCH`); write only files whose text changed, since a touched header rebuilt OpenTTD whole (210 s against 10).
