# Prebuilt Linux kernel and programs

Binaries for the machine (RV32IMAF), so that `python tools\make_linux_image.py` can
build the Linux image without a compiler. They are replaced by
`python tools\make_linux_image.py --save-prebuilt` after a build, which leaves the games' data
out: the games are here, and what they play (Quake's shareware episode, the Command & Conquer
demos) is fetched by their build scripts and is not this repository's to keep. A game from
here without its data says so and ends.

| File | Built by | From |
|---|---|---|
| `Image` | `linux/kernel/build.sh` | Linux 5.17.11, pimaker's fork, plus the drivers and hooks in `linux/kernel` |
| `root/usr/bin/nano-X`, `nx*`, `demo-*` | `linux/nanox/build.sh` | Microwindows, with `linux/nanox/microwindows.patch` and the drivers and programs in `linux/nanox` |
| `root/usr/bin/glxgears` | `linux/nanox/build.sh` | `glxgears.c` from Mesa's demos, unmodified, on `programs/linux/gl.c` |
| `root/usr/bin/nx` | | the script `linux/nanox/nx` |
| `root/emuinit`, `root/usr/bin/emumux`, `fptest`, `sndtest` | `linux/userland/build.sh` | this repository |
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
| glxgears | MIT (Brian Paul) | https://gitlab.freedesktop.org/mesa/demos, tag `mesa-demos-8.4.0` |
| Drivers and programs of this repository | MIT (`LICENSE`); the kernel drivers GPL-2.0 | this repository |

The build scripts named above fetch exactly these sources and reproduce the binaries; that
is the corresponding source for the GPL and MPL parts.
