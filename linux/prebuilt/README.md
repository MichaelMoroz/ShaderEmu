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
| `root/usr/bin/nxfiles`, `nxweb`, `nxedit`, `nxpaint`, `nxview`, `nxmon`, `nxsettings`, `nxkey`, `root/usr/share/web-*` | `linux/apps/build.sh` | this repository |
| `root/usr/share/wallpaper-*.ppm` | `tools/make_wallpaper.py` | photographs from Unsplash |
| `root/usr/bin/doom.bin` | `linux/nanox/doom.sh` | the Doom in Microwindows' `contrib`, with the files in `linux/nanox` |
| `root/usr/bin/tcc`, `cc`, `example`, `root/usr/share/cc-sysroot.tar`, `example-*.c` | `linux/tcc/build.sh` | TinyCC, with musl's headers and libraries for it |
| `root/usr/bin/classicube.bin`, `root/usr/share/classicube-default.zip` | `linux/classicube/build.sh` | ClassiCube, with `linux/classicube/classicube.patch` |
| `root/usr/bin/imdemo` | `linux/imgui/build.sh` | Dear ImGui 1.91.5 |
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
| glxgears | MIT (Brian Paul) | https://gitlab.freedesktop.org/mesa/demos, tag `mesa-demos-8.4.0` |
| Drivers and programs of this repository | MIT (`LICENSE`); the kernel drivers GPL-2.0 | this repository |

The build scripts named above fetch exactly these sources and reproduce the binaries; that
is the corresponding source for the GPL and MPL parts.

## The games' data

Not free software, and here as their owners gave it out to be passed on: the shareware
episodes and the demos, unchanged but for being taken out of their archives (and, for the two
Command & Conquer demos, their sounds decoded into a pack beside them).

| Data | Whose | From |
|---|---|---|
| `doom1.wad` | id Software: Doom's shareware episode | https://archive.org/download/DoomsharewareEpisode/doom.ZIP |
| `quake-pak0.pak` | id Software: Quake's shareware episode, 1.06 | https://ftp.netbsd.org/pub/pkgsrc/distfiles/quake106.zip |
| `tdawn-*.MIX`, `tdawn-sound.pak` | Electronic Arts (Westwood): the Command & Conquer demo | https://archive.org/download/CommandConquerDemo/cc1demo1.zip |
| `ralert-*.MIX`, `ralert-sound.pak` | Electronic Arts (Westwood): the Red Alert demo | https://archive.org/download/CommandConquerRedAlert_1020/ra95demo.zip |
| `classicube-default.zip` | ClassiCube's default texture pack | the game's own site, fetched by `linux/classicube/build.sh` |
| `wallpaper-*.ppm` | their photographers, under the Unsplash licence | `tools/make_wallpaper.py` names each |
