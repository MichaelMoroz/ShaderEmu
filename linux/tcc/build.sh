#!/usr/bin/env bash
# Builds TinyCC for the machine itself: a C compiler that runs in the guest and makes programs
# for it (32-bit RISC-V, soft float), from jrrk2's riscv32 branch. Puts the compiler, and an
# archive of the C library, headers and our window and OpenGL libraries, into the image.
#   wsl -- bash /mnt/c/.../linux/tcc/build.sh       (after linux/nanox/build.sh)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
SRC=$WORK/src/tinycc
MW=$WORK/src/microwindows/src
ROOT=$REPO/build/images/linux/root
GL=$REPO/programs/linux
PATHS="--sysincludepaths=/usr/include:/usr/lib/tcc/include --libpaths=/usr/lib:/usr/lib/tcc --crtprefix=/usr/lib --tccdir=/usr/lib/tcc"

if [ ! -d "$SRC" ]; then
    echo "== TinyCC source"
    git clone -q -b riscv32 https://github.com/jrrk2/tinycc "$SRC"
fi
git -C "$SRC" log --oneline -1
# its float instructions as this machine has them: single precision only (docs/machine/fpu.md)
python3 "$HERE/fpu_hook.py" "$SRC"
# numbers with a point read without the C library's long doubles, and an archive's symbols
# hashed once (docs/system/cc.md)
python3 "$HERE/speed_hook.py" "$SRC"
# and its stores in bursts, which ended the machine's passes early (docs/system/cc.md)
python3 "$HERE/pass_hook.py" "$SRC"

# 1. a compiler that runs here and makes code for the machine: it builds TinyCC's own runtime
echo "== cross compiler, and the runtime"
mkdir -p "$WORK/build/tcc-cross" && cd "$WORK/build/tcc-cross"
"$SRC/configure" --cpu=riscv32 --triplet=riscv32-linux-musl --config-musl $PATHS > configure.log
make -j"$(nproc)" > make.log 2>&1 || { tail -20 make.log; exit 1; }

# 2. what a program is compiled against: headers and libraries, files of the ROM where the
# compiler looks for them (they were one archive unpacked at the first build: half a minute).
echo "== headers and libraries"
STAGE=$WORK/build/tcc-sysroot
rm -rf "$STAGE" && mkdir -p "$STAGE/usr/include" "$STAGE/usr/lib/tcc/include"
cp -r "$SYSROOT/include/." "$STAGE/usr/include/"
# the kernel's own headers are most of that, and the C library's need none of them
(cd "$STAGE/usr/include" && rm -rf linux drm rdma sound asm asm-generic mtd scsi video xen misc cxl)
cp "$SRC"/include/*.h "$STAGE/usr/lib/tcc/include/"
# TinyCC links its runtime last, after the C library, which is where the C library's own needs
# (64-bit division, soft float) must come from: compiler-rt's builtins go into that archive
cp "$SYSROOT/lib/libcompiler_rt.a" "$STAGE/usr/lib/tcc/libtcc1.a"
(mkdir -p "$WORK/build/tcc1" && cd "$WORK/build/tcc1" && rm -f ./*.o && riscv32-linux-ar x "$WORK/build/tcc-cross/libtcc1.a" &&
    for o in *.o; do mv "$o" "tcc1_$o"; done && riscv32-linux-ar rs "$STAGE/usr/lib/tcc/libtcc1.a" tcc1_*.o)
cp "$SYSROOT"/lib/crt1.o "$SYSROOT"/lib/crti.o "$SYSROOT"/lib/crtn.o "$SYSROOT"/lib/libc.a "$SYSROOT"/lib/libm.a \
    "$STAGE/usr/lib/"
# the window system's client library, and our OpenGL ES on the machine's GPU
cp "$MW/src/lib/libnano-X.a" "$STAGE/usr/lib/" 2>/dev/null || cp "$MW/lib/libnano-X.a" "$STAGE/usr/lib/"
cp "$MW/include/nano-X.h" "$MW/include/mwtypes.h" "$MW/include/mwconfig.h" "$STAGE/usr/include/" 2>/dev/null || true
mkdir -p "$STAGE/usr/include/GLES"
cp "$GL"/include/GLES/*.h "$STAGE/usr/include/GLES/"
# (the library's float build: what a program here hands it is floats)
rv32-cc -O2 -Wall -c -DSEGL_FLOAT -idirafter "$GL/include" -I"$MW/include" "$GL/gles.c" -o "$WORK/build/gles.o"
rm -f "$STAGE/usr/lib/libgles.a"
riscv32-linux-ar rcs "$STAGE/usr/lib/libgles.a" "$WORK/build/gles.o"
# the worker cores' library (docs/machine/multicore.md), for a program that shares its work out
rv32-cc -O2 -Wall -c -I"$REPO/programs/mc" "$REPO/programs/mc/mcw.c" -o "$WORK/build/mcw.o"
rv32-cc -O2 -Wall -c -I"$REPO/programs/mc" "$REPO/programs/mc/mcwx.c" -o "$WORK/build/mcwx.o"
rm -f "$STAGE/usr/lib/libmcw.a"
riscv32-linux-ar rcs "$STAGE/usr/lib/libmcw.a" "$WORK/build/mcw.o" "$WORK/build/mcwx.o"
cp "$REPO/programs/mc/mcw.h" "$REPO/programs/mc/mcwx.h" "$REPO/programs/mc/mc.h" "$STAGE/usr/include/"
# the HLSL compiler, which makes a shader the lanes' code at run time (docs/system/hlsl.md)
bash "$REPO/programs/hlsl/build.sh" "$WORK/build/hlsl" > /dev/null
cp "$WORK/build/hlsl/libhlsl.a" "$STAGE/usr/lib/" && cp "$REPO/programs/hlsl/hlsl.h" "$STAGE/usr/include/"
# and the IDE's for a .hlsl file (docs/apps/ide.md): hlslc checks it, hlslview (the fractal example) runs it
mkdir -p "$ROOT/usr/bin"
rv32-cc -O2 -Wall -I"$REPO/programs/hlsl" "$REPO/programs/hlsl/hlslc.c" -L"$STAGE/usr/lib" -lhlsl -lmcw -o "$WORK/build/hlsl/hlslc"
rv32-cc -O2 -Wall -idirafter "$STAGE/usr/include" "$HERE/examples/fractal.c" -L"$STAGE/usr/lib" -lgles -lnano-X -lhlsl -lmcw -lm \
    -o "$WORK/build/hlsl/hlslview"
riscv32-linux-strip -o "$ROOT/usr/bin/hlslc" "$WORK/build/hlsl/hlslc"
riscv32-linux-strip -o "$ROOT/usr/bin/hlslview" "$WORK/build/hlsl/hlslview"
# libmedia's (linux/libmedia/build.sh, run before): kept, since usr/lib and usr/include are replaced below
if [ -f "$WORK/src/libmedia/libmedia.a" ]; then
    cp "$WORK/src/libmedia/libmedia.a" "$STAGE/usr/lib/" && cp "$REPO/linux/libmedia/media.h" "$STAGE/usr/include/"
fi
riscv32-linux-strip -g "$STAGE"/usr/lib/*.a "$STAGE"/usr/lib/*.o 2>/dev/null || true
# what the examples take of each archive goes first in it, for the ROM (pack.py, docs/system/cc.md);
# the compiler is told where that is (mapped.c), so it is built after
mkdir -p "$WORK/build/tcc-guest"
python3 "$HERE/pack.py" "$WORK/build/tcc-cross/tcc" "$STAGE" "$HERE/examples" "$WORK/build/tcc-guest/packed.h"
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/share"
# (only what the stage has: other folders of usr/lib are other programs')
rm -rf "$ROOT/usr/include" "$ROOT/usr/share/cc-sysroot.tar"
(cd "$STAGE/usr/lib" && for f in *; do rm -rf "$ROOT/usr/lib/$f"; done)
mkdir -p "$ROOT/usr/lib" && cp -rL "$STAGE/usr/include" "$ROOT/usr/" && cp -rL "$STAGE/usr/lib/." "$ROOT/usr/lib/"

# 3. the same compiler, compiled for the machine
echo "== the guest's compiler"
mkdir -p "$WORK/build/tcc-guest" && cd "$WORK/build/tcc-guest"
# the libraries it links are read from memory (mapped.c, under its open, read, lseek and close)
rv32-cc -O2 -Wall -I. -c "$HERE/mapped.c" -o mapped_files.o
"$SRC/configure" --cpu=riscv32 --triplet=riscv32-linux-musl --config-musl $PATHS \
    --cc=rv32-cc --ar=riscv32-linux-ar --extra-cflags="-O2" \
    "--extra-ldflags=-Wl,--wrap=open,--wrap=read,--wrap=lseek,--wrap=close $PWD/mapped_files.o" > configure.log
# (a header the build makes by running a small program: both taken from the build that ran
# here, the program first, so that the header is the newer and is not made again)
cp "$WORK/build/tcc-cross/c2str.exe" . && cp "$WORK/build/tcc-cross/tccdefs_.h" . && touch tccdefs_.h
# (as one source file: the compiler's own calls are then the host compiler's to fold. The
# objects of a build before this one are of other flags: they go first)
rm -f tcc tcc.o libtcc.a libtcc.o tccpp.o tccgen.o tccelf.o tccasm.o tccdbg.o tccrun.o riscv32-*.o
make tcc ONE_SOURCE=yes > make.log 2>&1 || { tail -20 make.log; exit 1; }
riscv32-linux-strip -o "$ROOT/usr/bin/tcc" tcc
ls -l "$ROOT/usr/bin/tcc"; echo "$(find "$ROOT/usr/include" "$ROOT/usr/lib" -type f | wc -l) files, $(du -sh "$STAGE/usr" | cut -f1)"

# 4. `cc FILES...` compiles with all of that; `example NAME` builds and runs /usr/share/example-NAME.c
tr -d '\r' < "$HERE/cc" > "$ROOT/usr/bin/cc"
tr -d '\r' < "$HERE/example" > "$ROOT/usr/bin/example"
chmod +x "$ROOT/usr/bin/cc" "$ROOT/usr/bin/example"
for f in "$HERE"/examples/*.c "$HERE"/examples/*.hlsl; do cp "$f" "$ROOT/usr/share/example-$(basename "$f")"; done
# (each opens in the IDE, to be read, changed and run from there; `example NAME` still builds and runs one)
printf '%s\n' "Programming/Cube (C example)=nxide /usr/share/example-cube.c" "Programming/Home (C example)=nxide /usr/share/example-home.c" \
    "Programming/Shader on 255 cores (C example)=nxide /usr/share/example-shader.c" \
    "Programming/Fractal in HLSL (C example)=nxide /usr/share/example-fractal.c" > "$ROOT/usr/share/nxapps.30-examples"
