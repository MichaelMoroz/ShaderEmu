# Prebuilt Linux kernel and programs

Binaries for the machine (RV32IMAF), so that `python tools\make_linux_image.py` can
build the Linux image without a compiler. They are replaced by
`python tools\make_linux_image.py --save-prebuilt` after a build. It is the whole image: every
program the build scripts make, and the data the games play, so that the machine can be
started and everything on its desktop run without building anything.

| File | Built by | From |
|---|---|---|
| `Image` | `linux/kernel/build.sh` | Linux 5.17.11, pimaker's fork, plus the drivers and hooks in `linux/kernel` |
| `root/usr/bin/nano-X`, `nx*`, `demo-*` | `linux/nanox/build.sh` | Microwindows, with `linux/nanox/microwindows.patch` and the drivers and programs in `linux/nanox` |
| `root/usr/bin/glxgears` | `linux/nanox/build.sh` | `glxgears.c` from Mesa's demos, unmodified, on `programs/linux/gl.c` |
| `root/usr/bin/nx` | | the script `linux/nanox/nx` |
| `root/emuinit`, `root/usr/bin/emumux`, `fptest`, `sndtest` | `linux/userland/build.sh` | this repository |
| `root/bin/busybox` | `linux/userland/busybox.sh` | BusyBox 1.36.1 |
| `root/usr/bin/nc`, `telnetd` | `linux/userland/build.sh` | two-line scripts that start BusyBox's applets of those names (`docs/lan.md`) |
| `root/usr/bin/nxfiles`, `nxweb`, `nxedit`, `nxpaint`, `nxview`, `nxmon`, `nxsettings`, `nxkey`, `nxoff`, `root/usr/share/web-*`, `nxapps.90-system`, `open`, `nxopen-archive`, `nxopen-run`, `nxopen.20-apps` (what opens which kind of file, `docs/open.md`), `picture-fox.png`, `picture-fox.jpg` (cut from the desktop's picture by `tools/make_test_pictures.py`) | `linux/apps/build.sh` | this repository |
| `root/usr/bin/nxray`, `nxpath`, `root/usr/share/nxapps.82-raytrace`, `nxapps.83-pathtrace` | `linux/raytrace/build.sh`, `linux/pathtrace/build.sh` | this repository; `nxpath` with Dear ImGui (below) |
| `root/usr/bin/nxplay`, `root/usr/share/play-test.wav`, `.mp3`, `.flac`, `.mod` (made by `linux/play/make_test.py`), `nxapps.84-play`, `nxopen.84-play` | `linux/play/build.sh` | this repository, with minimp3 and dr_flac (below) |
| `root/usr/share/wallpaper-*.ppm` | `tools/make_wallpaper.py` | photographs from Unsplash |
| `root/usr/bin/doom.bin`, `root/usr/share/nxopen.50-doom` | `linux/nanox/doom.sh` | the Doom in Microwindows' `contrib`, with the files in `linux/nanox` |
| `root/usr/bin/tcc`, `cc`, `example`, `root/usr/share/cc-sysroot.tar`, `example-*.c` | `linux/tcc/build.sh` | TinyCC, with musl's headers and libraries for it |
| `root/usr/bin/classicube.bin`, `root/usr/share/classicube-default.zip` | `linux/classicube/build.sh` | ClassiCube, with `linux/classicube/classicube.patch` |
| `root/usr/bin/imdemo` | `linux/imgui/build.sh` | Dear ImGui 1.91.5 |
| `root/usr/bin/nes.bin`, `nes`, `root/usr/share/nxopen.85-nes` | `linux/nes/build.sh` | Nofrendo, with `linux/nes/nofrendo.patch` and `osd_shaderemu.c` |
| `root/usr/share/nes-thwaite.nes`, `nes-nova.nes` | `linux/nes/build.sh` | two NES games (release files) |
| `root/usr/share/doom1.wad` | `linux/nanox/doom.sh` | Doom's shareware episode |
| `root/usr/share/quake-pak0.pak` | `linux/quake/build.sh` | Quake's shareware episode (1.06) |
| `root/usr/share/tdawn-*.MIX`, `tdawn-sound.pak` | `linux/tdawn/build.sh` | the Command & Conquer demo; the pack is its sounds decoded (`tools/make_tdawn_sound.py`) |
| `root/usr/share/ralert-*.MIX`, `ralert-sound.pak` | `linux/ralert/build.sh` | the Red Alert demo; the pack is its sounds decoded |
| `root/usr/bin/quake.bin` | `linux/quake/build.sh` | id Software's Quake (GLQuake), with `linux/quake/quake.patch` and the files in `linux/quake` |
| `root/usr/bin/tdawn.bin`, `ralert.bin` | `linux/tdawn/build.sh`, `linux/ralert/build.sh` | Vanilla Conquer, with the `vanilla-conquer.patch` of each and the files in `linux/tdawn` and `linux/ralert` |

Every program is statically linked, so each also contains the parts of the C library and
compiler runtime it uses.

## Licences and sources

| Component | Licence | Source |
|---|---|---|
| Linux kernel | GPL-2.0 | https://github.com/pimaker/linux-rvc, branch `rvcnet-5.17.11`; configuration: upstream rvc's `linux.config` plus `linux/kernel/config.extra`; our changes: `linux/kernel` |
| Microwindows / Nano-X | MPL-1.1, or GPL-2.0 at the recipient's choice | https://github.com/ghaerr/microwindows, commit `a53c319`; our changes: `linux/nanox/microwindows.patch` |
| musl 1.2.5 | MIT | https://musl.libc.org |
| compiler-rt 18.1.8 builtins | Apache-2.0 with LLVM exception | https://github.com/llvm/llvm-project |
| soft-fp (128-bit float routines, from GCC 13.3.0's libgcc) | LGPL-2.1-or-later with the soft-fp linking exception | https://github.com/gcc-mirror/gcc, `libgcc/soft-fp` |
| Quake | GPL-2.0 | https://github.com/id-Software/Quake, commit `bf4ac42`; our changes: `linux/quake/quake.patch` |
| Vanilla Conquer (Tiberian Dawn, Red Alert) | GPL-3.0 with EA's additional terms | https://github.com/TheAssemblyArmada/Vanilla-Conquer, the commit `linux/tdawn/build.sh` names; our changes: `linux/tdawn/vanilla-conquer.patch`, `linux/ralert/vanilla-conquer.patch` |
| libstdc++ (headers, from GCC 13.3.0) | GPL-3.0 with the GCC runtime library exception | https://github.com/gcc-mirror/gcc |
| BusyBox 1.36.1 | GPL-2.0 | https://busybox.net/downloads/busybox-1.36.1.tar.bz2; configuration: `linux/userland` |
| Doom | GPL-2.0 | Microwindows' `src/contrib/doom` at the commit above; our changes: `linux/nanox` |
| TinyCC | LGPL-2.1 | https://github.com/jrrk2/tinycc, branch `riscv32`; our changes: `linux/tcc` |
| ClassiCube | BSD-3-Clause | https://github.com/ClassiCube/ClassiCube, commit `d41c3f7`; our changes: `linux/classicube/classicube.patch` |
| Dear ImGui 1.91.5 | MIT | https://github.com/ocornut/imgui, tag `v1.91.5` |
| Nofrendo | LGPL-2.0 | https://github.com/espressif/esp32-nesemu, commit `693e378`; our changes: `linux/nes/nofrendo.patch` |
| minimp3 | CC0-1.0 | https://github.com/lieff/minimp3, commit `ea99364`; our changes: `linux/play/minimp3.patch` |
| dr_flac 0.13.4 | public domain (Unlicense) or MIT No Attribution | https://github.com/mackron/dr_libs, commit `dfe8377` |
| glxgears | MIT (Brian Paul) | https://gitlab.freedesktop.org/mesa/demos, tag `mesa-demos-8.4.0` |
| Drivers and programs of this repository | MIT (`LICENSE`); the kernel drivers GPL-2.0 | this repository |

The build scripts named above fetch exactly these sources and reproduce the binaries; that
is the corresponding source for the GPL and MPL parts.

## Size

The build scripts give every program one build date (`SOURCE_DATE_EPOCH` in
`linux/userland/toolchain.sh`, `KBUILD_BUILD_TIMESTAMP` for the kernel), so a program whose
sources did not change is the same file after a rebuild and adds nothing to the repository
when this folder is saved again. The files are kept as they are, not packed: git packs them
itself (the 100 MB here are 60 in the repository), and packing them better by hand (xz: 51)
would only save anything in a history written again without the files as they are now.
(The games' data as one solid archive would be 33 MB where git keeps 53; that was weighed in
October 2026 and the files were left as they are.)

## The games' data

Not free software, and here as their owners gave it out to be passed on: the shareware
episodes and the demos, unchanged but for being taken out of their archives (and, for the two
Command & Conquer demos, their sounds decoded into a pack beside them).

| Data | Whose | From |
|---|---|---|
| `doom1.wad` | id Software: Doom's shareware episode | https://archive.org/download/DoomsharewareEpisode/doom.ZIP |
| `quake-pak0.pak` | id Software: Quake's shareware episode, 1.06 | https://ftp.netbsd.org/pub/pkgsrc/distfiles/quake106.zip |
| `nes-thwaite.nes` | Damian Yerrick: Thwaite 0.04 (GPL-3.0) | https://github.com/pinobatch/thwaite-nes/releases/download/v0.04/thwaite.nes |
| `nes-nova.nes` | NovaSquirrel: Nova the Squirrel 1.0.6a (GPL-3.0) | https://github.com/NovaSquirrel/NovaTheSquirrel/releases/download/v1.0.6a/nova.nes |
| `tdawn-*.MIX`, `tdawn-sound.pak` | Electronic Arts (Westwood): the Command & Conquer demo | https://archive.org/download/CommandConquerDemo/cc1demo1.zip |
| `ralert-*.MIX`, `ralert-sound.pak` | Electronic Arts (Westwood): the Red Alert demo | https://archive.org/download/CommandConquerRedAlert_1020/ra95demo.zip |
| `classicube-default.zip` | ClassiCube's default texture pack | the game's own site, fetched by `linux/classicube/build.sh` |
| `wallpaper-*.ppm` | their photographers, under the Unsplash licence | `tools/make_wallpaper.py` names each |
