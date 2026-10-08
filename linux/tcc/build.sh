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
# its float instructions as this machine has them: single precision only (docs/fpu.md)
python3 "$HERE/fpu_hook.py" "$SRC"

# 1. a compiler that runs here and makes code for the machine: it builds TinyCC's own runtime
echo "== cross compiler, and the runtime"
mkdir -p "$WORK/build/tcc-cross" && cd "$WORK/build/tcc-cross"
"$SRC/configure" --cpu=riscv32 --triplet=riscv32-linux-musl --config-musl $PATHS > configure.log
make -j"$(nproc)" > make.log 2>&1 || { tail -20 make.log; exit 1; }

# 2. the same compiler, compiled for the machine
echo "== the guest's compiler"
mkdir -p "$WORK/build/tcc-guest" && cd "$WORK/build/tcc-guest"
"$SRC/configure" --cpu=riscv32 --triplet=riscv32-linux-musl --config-musl $PATHS \
    --cc=rv32-cc --ar=riscv32-linux-ar --extra-cflags="-O2" > configure.log
# (a header the build makes by running a small program: both taken from the build that ran
# here, the program first, so that the header is the newer and is not made again)
cp "$WORK/build/tcc-cross/c2str.exe" . && cp "$WORK/build/tcc-cross/tccdefs_.h" . && touch tccdefs_.h
make tcc > make.log 2>&1 || { tail -20 make.log; exit 1; }
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/share"
riscv32-linux-strip -o "$ROOT/usr/bin/tcc" tcc

# 3. what a program is compiled against. The ROM's builder cannot make folders, so this goes
# in as one archive, which `cc` unpacks over the root (an overlay in RAM) the first time.
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
rv32-cc -O2 -Wall -c -DSEGL_FLOAT -I"$GL/include" -I"$MW/include" "$GL/gles.c" -o "$WORK/build/gles.o"
rm -f "$STAGE/usr/lib/libgles.a"
riscv32-linux-ar rcs "$STAGE/usr/lib/libgles.a" "$WORK/build/gles.o"
riscv32-linux-strip -g "$STAGE"/usr/lib/*.a "$STAGE"/usr/lib/*.o 2>/dev/null || true
tar -C "$STAGE" -cf "$ROOT/usr/share/cc-sysroot.tar" usr
ls -l "$ROOT/usr/bin/tcc" "$ROOT/usr/share/cc-sysroot.tar"

# 4. `cc FILES...` compiles with all of that; `example NAME` builds and runs /usr/share/example-NAME.c
tr -d '\r' < "$HERE/cc" > "$ROOT/usr/bin/cc"
tr -d '\r' < "$HERE/example" > "$ROOT/usr/bin/example"
chmod +x "$ROOT/usr/bin/cc" "$ROOT/usr/bin/example"
for f in "$HERE"/examples/*.c; do cp "$f" "$ROOT/usr/share/example-$(basename "$f")"; done
printf '%s\n' "Other/Cube (C, built here)=example cube" "Other/Home (C, built here)=example home" > "$ROOT/usr/share/nxapps.30-examples"
