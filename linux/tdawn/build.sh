#!/usr/bin/env bash
# Builds Command & Conquer: Tiberian Dawn (Vanilla Conquer, a Nano-X client here) for the image,
# and fetches the 1995 demo's game data, which is not kept in this repository (docs/tdawn.md).
#   wsl -- bash /mnt/c/.../linux/tdawn/build.sh        (after linux/nanox/build.sh)
# In the guest: nano-X -p & tdawn
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
VC=$WORK/src/vanilla-conquer
BUILD=$WORK/src/tdawn-build
OUT=$REPO/build/images/linux/root
COMMIT=ce83b59
DEMO=${TDAWN_DEMO:-https://archive.org/download/CommandConquerDemo/cc1demo1.zip}
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }

# The C++ driver: g++ with libstdc++'s headers only (templates), over musl. The library itself
# is cxxrt.cpp; the code uses neither exceptions nor run-time type information.
CXXINC=$(dirname "$(riscv32-linux-g++ -print-file-name=libstdc++.a)")/../include/c++/13.3.0
cat > "$WORK/bin/rv32-c++" <<EOF
#!/usr/bin/env bash
link=1
for a in "\$@"; do case "\$a" in -c|-S|-E|-M|-MM) link=0 ;; esac; done
inc="-nostdinc -nostdinc++ -isystem $HERE/cxx -isystem $CXXINC -isystem $CXXINC/riscv32-buildroot-linux-gnu -isystem $SYSROOT/include -isystem \$(riscv32-linux-gcc -print-file-name=include)"
flags="$ARCH_FLAGS -fno-exceptions -fno-rtti -fno-threadsafe-statics -D__GLIBC_PREREQ(a,b)=0"
if [ \$link = 1 ]; then
    exec riscv32-linux-g++ \$flags \$inc -static -no-pie -nostdlib -L$SYSROOT/lib $SYSROOT/lib/crt1.o $SYSROOT/lib/crti.o \\
        "\$@" -Wl,--start-group -lc -lcompiler_rt -Wl,--end-group $SYSROOT/lib/crtn.o
fi
exec riscv32-linux-g++ \$flags \$inc "\$@"
EOF
chmod +x "$WORK/bin/rv32-c++"

if [ ! -d "$VC/.git" ]; then
    echo "== Vanilla Conquer $COMMIT"
    git clone -q https://github.com/TheAssemblyArmada/Vanilla-Conquer.git "$VC"
fi
# the tree is upstream's commit plus our patch and our files, put there again on every build
git -C "$VC" checkout -q -f "$COMMIT" && git -C "$VC" clean -q -fd
git -C "$VC" apply "$HERE/vanilla-conquer.patch"
cp "$HERE/cxxrt.cpp" "$HERE/shaderemu.cpp" "$HERE/soundio_shaderemu.cpp" "$HERE/host.c" "$HERE/host.h" "$VC/common/"
# the worker cores' library, the same for every program (programs/mc): host.c gives a core the scene
cp "$REPO/programs/mc/mc.h" "$REPO/programs/mc/mcw.h" "$REPO/programs/mc/mcw.c" "$VC/common/"
cp "$HERE/gl.cpp" "$VC/tiberiandawn/"

cat > "$WORK/src/tdawn-toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv32)
set(CMAKE_C_COMPILER $WORK/bin/rv32-cc)
set(CMAKE_CXX_COMPILER $WORK/bin/rv32-c++)
set(CMAKE_AR $(command -v riscv32-linux-ar))
set(CMAKE_RANLIB $(command -v riscv32-linux-ranlib))
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
EOF
GLINC="-idirafter $REPO/programs/linux/include -I$MW/src/include -I$REPO/linux/userland"
# The GPU's library with a section a variable, each starting on 16 bytes: a worker core makes
# the scene's commands with it (host.c), and two cores must not store to the same 16 bytes.
rv32-cc -O2 -fno-pie -w -DNDEBUG -DSHADEREMU $GLINC -fno-common -fdata-sections -fno-toplevel-reorder -msmall-data-limit=0     -c "$REPO/programs/linux/gles.c" -o "$WORK/src/tdawn-gles.o"
riscv32-linux-objcopy --set-section-alignment '.data*=16' --set-section-alignment '.bss*=16' "$WORK/src/tdawn-gles.o"
# -fno-pie: a use of a global costs a load from a table otherwise (docs/doom.md)
# -fno-lifetime-dse: the game's operator new marks an object active before its constructor
# runs, a store the compiler may otherwise drop (a native -O3 build then has no overlays)
cmake -S "$VC" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$WORK/src/tdawn-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DSDL2=OFF -DOPENAL=OFF -DNETWORKING=OFF -DBUILD_VANILLARA=OFF -DSHADEREMU=ON \
    -DSHADEREMU_GLES="$WORK/src/tdawn-gles.o" -DSHADEREMU_NANOX="$MW/src/lib/libnano-X.a" \
    -DCMAKE_CXX_FLAGS="-O2 -fno-pie -fno-lifetime-dse -w -DSHADEREMU $GLINC" -DCMAKE_CXX_FLAGS_RELEASE="-DNDEBUG" \
    -DCMAKE_C_FLAGS="-O2 -fno-pie -w -DSHADEREMU $GLINC" -DCMAKE_C_FLAGS_RELEASE="-DNDEBUG" > "$WORK/src/tdawn-configure.log" 2>&1 \
    || { tail -20 "$WORK/src/tdawn-configure.log"; exit 1; }
cmake --build "$BUILD" -j"$(nproc)" > "$WORK/src/tdawn-build.log" 2>&1 \
    || { grep -E "error|undefined reference" "$WORK/src/tdawn-build.log" | head -40; exit 1; }

mkdir -p "$OUT/usr/bin" "$OUT/usr/share"
riscv32-linux-strip -o "$OUT/usr/bin/tdawn.bin" "$BUILD/vanillatd"
riscv32-linux-nm -n "$BUILD/vanillatd" > "$WORK/src/tdawn.nm"

if [ ! -f "$WORK/src/tdawn-demo/DEMO.MIX" ] || [ ! -f "$WORK/src/tdawn-demo/MAP1.AUD" ]; then
    echo "== the demo's data"
    mkdir -p "$WORK/src/tdawn-demo"
    curl -sSL "$DEMO" -o "$WORK/src/tdawn-demo.zip"
    python3 - "$WORK/src/tdawn-demo.zip" "$WORK/src/tdawn-demo" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for name in ('DEMO.MIX', 'DEMOL.MIX', 'SOUNDS.MIX', 'SPEECH.MIX', 'MAP1.AUD', 'WIN1.AUD'):
    open(sys.argv[2] + '/' + name, 'wb').write(z.read(name))
PY
fi
# all in /usr/share: the image builder adds files to folders the ROM has, and makes none.
# The game looks for its files by name in one folder, which the starter makes of links.
for f in DEMO DEMOL SOUNDS SPEECH; do cp "$WORK/src/tdawn-demo/$f.MIX" "$OUT/usr/share/tdawn-$f.MIX"; done
# the sounds as the sound card plays them from the ROM (docs/sound.md); made again when stale
PAK="$WORK/src/tdawn-demo/tdawn-sound.pak"
if [ ! "$PAK" -nt "$WORK/src/tdawn-demo/SOUNDS.MIX" ] || [ ! "$PAK" -nt "$REPO/tools/make_tdawn_sound.py" ]; then
    python3 "$REPO/tools/make_tdawn_sound.py" "$PAK" "$WORK/src/tdawn-demo"/{SOUNDS,SPEECH,DEMO,DEMOL}.MIX \
        "$WORK/src/tdawn-demo"/{MAP1,WIN1}.AUD
fi
cp "$PAK" "$OUT/usr/share/tdawn-sound.pak"
# the demo's two tunes (the map's and the score's): the game finds them by name, the card plays
# them from the pack, so only how each begins has to be there
for f in MAP1 WIN1; do head -c 64 "$WORK/src/tdawn-demo/$f.AUD" > "$OUT/usr/share/tdawn-$f.AUD"; done
cat > "$OUT/usr/bin/tdawn" <<'EOF'
#!/bin/sh
mkdir -p /tmp/tdawn && cd /tmp/tdawn || exit 1
for f in DEMO DEMOL SOUNDS SPEECH; do ln -sf /usr/share/tdawn-$f.MIX $f.MIX; done
for f in MAP1 WIN1; do ln -sf /usr/share/tdawn-$f.AUD $f.AUD; done
# the game reads the whole of its program's folder for every file it looks for: not /usr/bin
[ -x game ] || cp /usr/bin/tdawn.bin game
HOME=/tmp/tdawn exec ./game "$@"
EOF
chmod +x "$OUT/usr/bin/tdawn"
echo "Games/Tiberian Dawn=tdawn" > "$OUT/usr/share/nxapps.60-tdawn"
ls -l "$OUT/usr/bin/tdawn.bin" "$OUT/usr/share"/tdawn-*.MIX
