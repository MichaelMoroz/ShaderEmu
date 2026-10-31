# Prebuilt Linux kernel and programs

Binaries for the machine (RV32IMAF), so that `pwsh tools\make_image.ps1` can
build the Linux image without a compiler. They are replaced by
`pwsh tools\make_image.ps1 --save-prebuilt` after a build. It is the whole image: every
program the build scripts make, and the data the games play, so that the machine can be
started and everything on its desktop run without building anything.

| File | Built by | From |
|---|---|---|
| `Image` | `linux/kernel/build.sh` | Linux 5.17.11 with the MMU, pimaker's fork, plus the drivers and hooks in `linux/kernel`; not run: `nommu/Image` is the machine's kernel |
| `root/usr/bin/nano-X`, `nx*`, `demo-*` | `linux/nanox/build.sh` | Microwindows, with `linux/nanox/microwindows.patch` and the drivers and programs in `linux/nanox` |
| `root/usr/bin/glxgears` | `linux/nanox/build.sh` | `glxgears.c` from Mesa's demos, unmodified, on `programs/linux/gl.c` |
| `root/usr/bin/nx` | | the script `linux/nanox/nx` |
| `root/emuinit`, `root/usr/bin/emumux`, `fptest`, `sndtest` | `linux/userland/build.sh` | this repository |
| `root/bin/busybox` | `linux/userland/busybox.sh` | BusyBox 1.36.1 |
| `root/usr/bin/nc`, `telnetd` | `linux/userland/build.sh` | two-line scripts that start BusyBox's applets of those names (`docs/system/lan.md`) |
| `root/usr/bin/nxfiles`, `nxweb`, `nxedit`, `nxpaint`, `nxview`, `nxmon`, `nxsettings`, `nxkey`, `nxicon` (a program's own icon, `docs/system/nanox.md`), `nxoff`, `root/usr/share/web-*`, `nxapps.90-system`, `open`, `nxopen-archive`, `nxopen-run`, `nxopen.20-apps` (what opens which kind of file, `docs/system/open.md`), `picture-fox.png`, `picture-fox.jpg` (cut from the desktop's picture by `tools/make_test_pictures.py`) | `linux/apps/build.sh` | this repository |
| `root/usr/bin/nxray`, `nxpath`, `root/usr/share/nxapps.82-raytrace`, `nxapps.83-pathtrace` | `linux/raytrace/build.sh`, `linux/pathtrace/build.sh` | this repo; `nxpath` with Dear ImGui (below) and, in `root/Models`, the Utah teapot (Martin Newell) and the Stanford Bunny (Stanford Computer Graphics Laboratory) |
| `root/usr/bin/nxplay`, `root/usr/share/play-test.wav`, `.mp3`, `.flac`, `.mod` (made by `linux/play/make_test.py`), `nxapps.84-play`, `nxopen.84-play` | `linux/play/build.sh` | this repository, with minimp3 and dr_flac (below) |
| `root/usr/share/wallpaper-*.ppm` | `tools/make_wallpaper.py` | photographs from Unsplash |
| `root/usr/bin/doom.bin`, `root/usr/share/nxopen.50-doom` | `linux/nanox/doom.sh` | the Doom in Microwindows' `contrib`, with the files in `linux/nanox` |
| `root/usr/bin/tcc`, `cc`, `example`, `root/usr/include`, `root/usr/lib` (the C library's, Nano-X's, OpenGL ES's and the worker cores' headers and libraries), `root/usr/share/example-*.c` | `linux/tcc/build.sh` | TinyCC, with musl's headers and libraries for it |
| `root/usr/bin/nxide`, `root/usr/share/nxapps.28-ide`, `nxopen.15-ide` | `linux/ide/build.sh` | this repository (the IDE, `docs/apps/ide.md`) |
| `root/usr/bin/nxmicropolis`, `root/usr/share/micropolis/` (its pictures, sounds, cities and texts, and its licences), `nxapps.72-micropolis`, `nxopen.72-micropolis` | `linux/micropolis/build.sh` | Micropolis (MicropolisCore's engine), with `linux/micropolis/micropolis.patch` and the front end in `linux/micropolis` |
| `root/usr/bin/classicube.bin`, `root/usr/share/classicube-default.zip`, `classicube-sound.pak`, `classicube-music.mp3` | `linux/classicube/build.sh` | ClassiCube, with `linux/classicube/classicube.patch` |
| `root/usr/bin/imdemo` | `linux/imgui/build.sh` | Dear ImGui 1.91.5 |
| `root/usr/bin/nes.bin`, `nes`, `root/usr/share/nxopen.85-nes` | `linux/nes/build.sh` | Nofrendo, with `linux/nes/nofrendo.patch` and `osd_shaderemu.c` |
| `root/usr/share/nes-thwaite.nes`, `nes-nova.nes` | `linux/nes/build.sh` | two NES games (release files) |
| `root/usr/share/doom1.wad` | `linux/nanox/doom.sh` | Doom's shareware episode |
| `root/usr/share/quake-pak0.pak` | `linux/quake/build.sh` | Quake's shareware episode (1.06) |
| `root/usr/share/tdawn-*.MIX`, `tdawn-sound.pak` | `linux/tdawn/build.sh` | the Command & Conquer demo; the pack is its sounds decoded (`tools/make_tdawn_sound.py`) |
| `root/usr/share/ralert-*.MIX`, `ralert-sound.pak` | `linux/ralert/build.sh` | the Red Alert demo; the pack is its sounds decoded |
| `root/usr/bin/quake.bin` | `linux/quake/build.sh` | id Software's Quake (GLQuake), with `linux/quake/quake.patch` and the files in `linux/quake` |
| `root/usr/bin/quake3.bin`, `root/usr/share/quake3-se.dat`, `nxapps.71-quake3` | `linux/quake3/build.sh` | ioquake3, with the patch and files in `linux/quake3` (`docs/games/quake3.md`) |
| `root/usr/lib/tdawn/game`, `root/usr/lib/ralert/game` | `linux/tdawn/build.sh`, `linux/ralert/build.sh` | Vanilla Conquer, with the `vanilla-conquer.patch` of each and the files in `linux/tdawn` and `linux/ralert` |
| `nano`, `htop`, `mc`, `mg`, `dialog`, `file`, `sl` | `linux/terminal/build.sh` | upstream's releases, `docs/system/terminal.md` |
| `qjs`, `qjsc`; `micropython` | `linux/quickjs/build.sh`; `linux/micropython/build.sh` | QuickJS; MicroPython (`docs/system/scripts.md`) |
| `dropbearmulti` and its links, `sftp`, `curl`, `socat`, `thttpd`, `pcregrep`, `dotlockfile`, `at`, iproute2's | `linux/ports/*.sh` | upstream's releases, `docs/system/ports.md` |
| `root/usr/bin/openttd.bin`, `openttd`, `root/usr/share/openttd/` | `linux/openttd/build.sh` | OpenTTD 15.3 with the open base sets, `docs/games/openttd.md` |
| `nommu/root/usr/bin/halflife.bin`, `halflife`, `root/usr/share/halflife/` (its own files, the HLSDK's licence), `nxapps.72-halflife` | `linux/halflife/build.sh` | Xash3D FWGS and hlsdk-portable, with `linux/halflife/xash3d.patch`, `hlsdk.patch` and the files in `linux/halflife`; Uplink's data is not here: `linux/halflife/data.py` fetches it when an image is made (`docs/games/halflife.md`) |

Every program is statically linked, so each also contains the parts of the C library and
compiler runtime it uses.

## Linux without the MMU (`nommu/`)

The machine the harness and the world run (`docs/system/nommu.md`). `nommu/Image` is the kernel of
`linux/nommu/kernel.sh`: the same Linux and drivers, built without `CONFIG_MMU`, with
`linux/nommu`'s patches. `nommu/root` holds only the files that differ from `root`: every
program built by `linux/nommu/build.sh` and made flat by `linux/nommu/root.sh` (the same sources
as above, position-independent, in the bFLT format), and `elf2flt`, `mctest`, `flatcrt.o` and
`nommu_fork.h`, which only that build has. `linux/nommu/image.py` lays them over the image of
`root`; `--save-prebuilt` replaces them after a build.

## Licences and sources

| Component | Licence | Source |
|---|---|---|
| Linux kernel | GPL-2.0 | https://github.com/pimaker/linux-rvc, branch `rvcnet-5.17.11`; configuration: upstream rvc's `linux.config` plus `linux/kernel/config.extra`; our changes: `linux/kernel` |
| Microwindows / Nano-X | MPL-1.1, or GPL-2.0 at the recipient's choice | https://github.com/ghaerr/microwindows, commit `a53c319`; our changes: `linux/nanox/microwindows.patch` |
| musl 1.2.5 | MIT | https://musl.libc.org |
| compiler-rt 18.1.8 builtins | Apache-2.0 with LLVM exception | https://github.com/llvm/llvm-project |
| soft-fp (128-bit float routines, from GCC 13.3.0's libgcc) | LGPL-2.1-or-later with the soft-fp linking exception | https://github.com/gcc-mirror/gcc, `libgcc/soft-fp` |
| Quake | GPL-2.0 | https://github.com/id-Software/Quake, commit `bf4ac42`; our changes: `linux/quake/quake.patch` |
| ioquake3 (Quake III Arena) | GPL-2.0 | https://github.com/ioquake/ioq3, the commit `linux/quake3/build.sh` names |
| Vanilla Conquer (Tiberian Dawn, Red Alert) | GPL-3.0 with EA's additional terms | https://github.com/TheAssemblyArmada/Vanilla-Conquer, the commit `linux/tdawn/build.sh` names; our changes: `linux/tdawn/vanilla-conquer.patch`, `linux/ralert/vanilla-conquer.patch` |
| libstdc++ (headers, from GCC 13.3.0) | GPL-3.0 with the GCC runtime library exception | https://github.com/gcc-mirror/gcc |
| BusyBox 1.36.1 | GPL-2.0 | https://busybox.net/downloads/busybox-1.36.1.tar.bz2; configuration: `linux/userland` |
| Doom | GPL-2.0 | Microwindows' `src/contrib/doom` at the commit above; our changes: `linux/nanox` |
| TinyCC | LGPL-2.1 | https://github.com/jrrk2/tinycc, branch `riscv32`; our changes: `linux/tcc` |
| Xash3D FWGS | GPL-3.0-or-later | https://github.com/FWGS/xash3d-fwgs, the commit `linux/halflife/build.sh` names; our changes: `linux/halflife/xash3d.patch` |
| hlsdk-portable (Half-Life's client and server) | Half-Life 1 SDK licence (free distribution only; its text at `usr/share/halflife/LICENSE-hlsdk`) | https://github.com/FWGS/hlsdk-portable, the commit `linux/halflife/build.sh` names; our changes: `linux/halflife/hlsdk.patch` |
| Micropolis (MicropolisCore's engine and its cities, pictures, texts and sounds) | GPL-3.0 with EA's additional terms; "Micropolis" under the Micropolis Public Name License | https://github.com/SimHacker/MicropolisCore, the commit `linux/micropolis/build.sh` names; our changes: `linux/micropolis/micropolis.patch` |
| ClassiCube | BSD-3-Clause | https://github.com/ClassiCube/ClassiCube, commit `d41c3f7`; our changes: `linux/classicube/classicube.patch` |
| Dear ImGui 1.91.5 | MIT | https://github.com/ocornut/imgui, tag `v1.91.5` |
| Nofrendo | LGPL-2.0 | https://github.com/espressif/esp32-nesemu, commit `693e378`; our changes: `linux/nes/nofrendo.patch` |
| minimp3 | CC0-1.0 | https://github.com/lieff/minimp3, commit `ea99364`; our changes: `linux/libmedia/minimp3.patch` |
| dr_flac 0.13.4 | public domain (Unlicense) or MIT No Attribution | https://github.com/mackron/dr_libs, commit `dfe8377` |
| OpenTTD 15.3, OpenGFX, OpenSFX, OpenMSX | GPL-2.0; OpenSFX CC BY-SA 3.0 (`usr/share/openttd/COPYING.md`) | https://www.openttd.org; `linux/openttd/build.sh` names each; our changes: `linux/openttd/openttd.patch` |
| QuickJS; MicroPython | MIT | the commits `linux/quickjs/build.sh` and `linux/micropython/build.sh` name |
| nano, htop, mc, mg, dialog, file, sl; Dropbear, OpenSSH's sftp, curl, Mbed TLS, socat, thttpd, pcre, lockfile-progs, at, iproute2 | each its own | the releases and sha256 sums in `linux/terminal/build.sh` and `linux/ports/*.sh` |
| glxgears | MIT (Brian Paul) | https://gitlab.freedesktop.org/mesa/demos, tag `mesa-demos-8.4.0` |
| Drivers and programs of this repository | MIT (`LICENSE`); the kernel drivers GPL-2.0 | this repository |

The build scripts named above fetch exactly these sources and reproduce the binaries; that
is the corresponding source for the GPL and MPL parts.

## Size

The build scripts give every program one build date (`SOURCE_DATE_EPOCH` in
`linux/userland/toolchain.sh`, `KBUILD_BUILD_TIMESTAMP` for the kernel), so a program whose
sources did not change is the same file after a rebuild and adds nothing to the repository
when this folder is saved again. The files are kept as they are, not packed: git packs them
itself, and packing them better by hand would only save anything in a history written again without
the files as they are now. This folder is 322 MB on 31 October 2026, 215 of the shared root and 103 of
`nommu`.

## The games' data

Not free software, and here as their owners gave it out to be passed on: the shareware
episodes and the demos, unchanged but for being taken out of their archives (and, for the two
Command & Conquer demos, their sounds decoded into a pack beside them).

| Data | Whose | From |
|---|---|---|
| `doom1.wad` | id Software: Doom's shareware episode | https://archive.org/download/DoomsharewareEpisode/doom.ZIP |
| `quake-pak0.pak` | id Software: Quake's shareware episode, 1.06 | https://ftp.netbsd.org/pub/pkgsrc/distfiles/quake106.zip |
| `nes-thwaite.nes` | Damian Yerrick: Thwaite 0.04 (GPL-3.0) | https://github.com/pinobatch/thwaite-nes/releases/download/v0.04/thwaite.nes |
| `root/root/Models/teapot.obj`, `bunny.ply` | the Utah teapot, as Stanford's CS148 course had it (its address no longer answers: this is the copy to build from, `docs/system/nommu.md`); the Stanford Bunny | `linux/pathtrace/build.sh` |
| `nes-nova.nes` | NovaSquirrel: Nova the Squirrel 1.0.6a (GPL-3.0) | https://github.com/NovaSquirrel/NovaTheSquirrel/releases/download/v1.0.6a/nova.nes |
| `quake3-pak0.pk3`, and `quake3-se.dat` made from it | id Software: the Quake III Arena demo's data | `linux/quake3/build.sh` names the address |
| `tdawn-*.MIX`, `tdawn-sound.pak` | Electronic Arts (Westwood): the Command & Conquer demo | https://archive.org/download/CommandConquerDemo/cc1demo1.zip |
| `ralert-*.MIX`, `ralert-sound.pak` | Electronic Arts (Westwood): the Red Alert demo | https://archive.org/download/CommandConquerRedAlert_1020/ra95demo.zip |
| `classicube-default.zip` | ClassiCube's default texture pack | the game's own site, fetched by `linux/classicube/build.sh` |
| `classicube-sound.pak`, `classicube-music.mp3` | Mojang: Minecraft's dig and step sounds and the tune `calm3`, as ClassiCube fetches them | `resources.download.minecraft.net` by the SHA-1s in ClassiCube's `Resources.c`, `linux/classicube/sound/make_sound.py` |
| `wallpaper-*.ppm` | their photographers, under the Unsplash licence | `tools/make_wallpaper.py` names each |
| `icons32.ppm`, `icons16.ppm`, `icons.idx` | the Chicago95 icon theme (GPL-3.0+/MIT), the cells `linux/apps/icons.txt` names; and the games' own icons: Chocolate Doom's (GPL-2.0), Quake's and Quake III's (GPL-2.0), ClassiCube's (BSD-3-Clause), Vanilla Conquer's two (GPL-3.0), Micropolis's (GPL-3.0) | https://github.com/grassmunk/Chicago95 at the commit in `tools/make_icons.py`; `linux/apps/icons/README.md` |
| `icons7-*`, `icons7.idx` | Oxygen's icons (KDE, LGPL-3.0), the cells `linux/apps/icons7.txt` names | https://github.com/KDE/oxygen-icons; `tools/make_icons.py` |
| `caption-on.ppm`, `caption-off.ppm`, `caption7-*.ppm` | ours: a window caption's colours | `tools/make_icons.py` |
| `font-*.sef`, `font-*.msdf` | the image's fonts, Selawik (Microsoft) among them | `tools/make_fonts.py`; `docs/system/fonts.md` |
| `root/Pictures/*.jpg` | public domain: NASA (Earthrise, Blue Marble, Deep Field), Hokusai, van Gogh | Wikimedia Commons, each named with its SHA-1 in `linux/media/media.txt` |
| `root/Music/Grieg - *.mp3` | public domain: Musopen Symphony Orchestra's recordings, 50 s each | Wikimedia Commons (`linux/media/media.txt`) |
| `root/Music/Beethoven - *.mp3` | public domain: the Moonlight Sonata's first movement, and the Fifth Symphony's by the Fulda Symphonic Orchestra (Simon Schindler), 50 s each | Wikimedia Commons (`linux/media/media.txt`) |
| `root/Music/Bach - Goldberg Aria.flac` | CC0: Kimiko Ishizaka's recording (Musopen), 20 s | Wikimedia Commons (`linux/media/media.txt`) |
| `root/Videos/Big Buck Bunny.mpg` | (c) copyright 2008, Blender Foundation / www.bigbuckbunny.org, CC BY 3.0: 20 s of the trailer | Wikimedia Commons, encoded by `tools/make_video.py` |
