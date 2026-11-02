#!/usr/bin/env bash
# Builds Command & Conquer: Tiberian Dawn (Vanilla Conquer, a Nano-X client here) for the image,
# and fetches the 1995 demo's game data, which is not kept in this repository (docs/games/tdawn.md).
#   wsl -- bash /mnt/c/.../linux/tdawn/build.sh        (after linux/nanox/build.sh)
# In the guest: nano-X -p & tdawn
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
VC=$WORK/src/vanilla-conquer
BUILD=$WORK/src/tdawn-build
OUT=${TDAWN_OUT:-$REPO/build/images/linux/root}	# (another image's folder for an experiment)
COMMIT=ce83b59
DEMO=${TDAWN_DEMO:-https://archive.org/download/CommandConquerDemo/cc1demo1.zip}
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
# a movie's block rows are libmedia's (docs/system/media.md, "VQA"; another build's with TDAWN_MEDIA=FILE)
MEDIA=${TDAWN_MEDIA:-$WORK/src/libmedia/libmedia.a}
[ -f "$MEDIA" ] || { echo "run linux/libmedia/build.sh first"; exit 1; }

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
cp "$HERE/cxxrt.cpp" "$HERE/shaderemu.cpp" "$HERE/soundio_shaderemu.cpp" "$HERE/host.c" "$HERE/host.h" "$HERE/td3d.h" "$VC/common/"
# the worker cores' library, the same for every program (programs/mc): host.c gives a core the scene
cp "$REPO/programs/mc/mc.h" "$REPO/programs/mc/mcw.h" "$REPO/programs/mc/mcw.c" "$REPO/programs/mc/mcwx.h" "$REPO/programs/mc/mcwx.c" "$VC/common/"
cp "$HERE/gl.cpp" "$VC/tiberiandawn/"
cp "$REPO/linux/libmedia/media.h" "$VC/common/"
# (the reset above renews every file's time, and the two games reset the same tree: each builds
# from a copy of its own, where a file keeps its time unless its bytes change)
SRC=$WORK/src/vanilla-conquer-td
rsync -rc --delete --exclude .git "$VC/" "$SRC/"
grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$SRC" "$BUILD/CMakeCache.txt" 2>/dev/null || rm -rf "$BUILD/CMakeCache.txt" "$BUILD/CMakeFiles"

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
rv32-cc -O2 -fno-pie -w -DNDEBUG -DSHADEREMU $GLINC -fno-common -fdata-sections -fno-toplevel-reorder -msmall-data-limit=0     -c "$REPO/programs/linux/gles.c" -o "$WORK/src/tdawn-gles.new.o"
riscv32-linux-objcopy --set-section-alignment '.data*=16' --set-section-alignment '.bss*=16' "$WORK/src/tdawn-gles.new.o"
# (kept with its time when its bytes are the same: a newer one links both games again, a minute)
cmp -s "$WORK/src/tdawn-gles.new.o" "$WORK/src/tdawn-gles.o" && rm "$WORK/src/tdawn-gles.new.o" || mv "$WORK/src/tdawn-gles.new.o" "$WORK/src/tdawn-gles.o"
# -fno-pie: a use of a global costs a load from a table otherwise (docs/games/doom.md)
# -fno-lifetime-dse: the game's operator new marks an object active before its constructor
# runs, a store the compiler may otherwise drop (a native -O3 build then has no overlays)
# The C++ is optimised as one program (docs/games/tdawn.md, "The whole program at once"): a partition
# a source file and nothing reordered, because the game's globals are made in the files' order.
LTO="-O3 -flto=$(nproc) -ffat-lto-objects -flto-partition=1to1 -fno-toplevel-reorder --param inline-unit-growth=2000 --param max-inline-insns-auto=400 --param max-inline-insns-single=1500 --param large-function-growth=1000 --param inline-min-speedup=5"
# What a played game ran, for the compiler (docs/games/tdawn.md, "What the compiler is told"):
# TDAWN_PROFILE=count builds the program that counts; otherwise the counts kept here are used.
PGO=""; COUNTS=""
if [ "${TDAWN_PROFILE:-}" = count ]; then
    # (the counters' functions are host.c's: an empty libgcov in the way of the toolchain's)
    mkdir -p "$WORK/src/nogcov" && riscv32-linux-ar rc "$WORK/src/nogcov/libgcov.a"
    PGO="-L$WORK/src/nogcov -fprofile-generate -fprofile-update=single"; COUNTS="-DSHADEREMU_PGO"
elif [ -f "$HERE/tdawn.profile" ]; then
    python3 "$HERE/profile.py" put "$HERE/tdawn.profile" "$BUILD"
    # The compiler reads the counts as it compiles, and make knows nothing of the .gcda files: the counts' sum in the flags
    # compiles every file again when they change (files with no change kept old counts).
    PGO="-fprofile-use -fprofile-partial-training -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch -DSHADEREMU_COUNTS=$(sha1sum "$HERE/tdawn.profile" | cut -c1-12)"
fi
# The compiler is given paths from where it runs. GCC knows a function that is not public, in the counts, by a sum with its source's
# path in it: with whole paths the counts held only in the folder they were made in (core 0 113.6 million elsewhere against 108.8).
TOP=$(dirname "$WORK")
cat > "$WORK/bin/tdawn-relative" <<EOF
#!/usr/bin/env bash
top=$TOP
rel=\$(realpath --relative-to="\$PWD" "\$top")
compiler=\$1; shift
exec "\$compiler" "\${@//\$top\//\$rel/}"
EOF
chmod +x "$WORK/bin/tdawn-relative"
cmake -S "$SRC" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$WORK/src/tdawn-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER_LAUNCHER="$WORK/bin/tdawn-relative" \
    -DSDL2=OFF -DOPENAL=OFF -DNETWORKING=OFF -DBUILD_VANILLARA=OFF -DSHADEREMU=ON \
    -DSHADEREMU_GLES="$WORK/src/tdawn-gles.o" -DSHADEREMU_NANOX="$MW/src/lib/libnano-X.a" \
    -DCMAKE_CXX_FLAGS="-O2 -fno-pie -fno-lifetime-dse -w -DSHADEREMU $GLINC $LTO $PGO ${TDAWN_XFLAGS:-}" -DCMAKE_CXX_FLAGS_RELEASE="-DNDEBUG" \
    -DCMAKE_C_FLAGS="-O2 -fno-pie -w -DSHADEREMU $COUNTS $GLINC" -DCMAKE_C_FLAGS_RELEASE="-DNDEBUG" \
    -DCMAKE_CXX_STANDARD_LIBRARIES="$MEDIA" > "$WORK/src/tdawn-configure.log" 2>&1 \
    || { tail -20 "$WORK/src/tdawn-configure.log"; exit 1; }
cmake --build "$BUILD" -j"$(nproc)" > "$WORK/src/tdawn-build.log" 2>&1 \
    || { grep -E "error|undefined reference" "$WORK/src/tdawn-build.log" | head -40; exit 1; }

# the game lists its program's folder for every file it looks for: a folder of its own in the ROM
mkdir -p "$OUT/usr/bin" "$OUT/usr/share" "$OUT/usr/lib/tdawn"
rm -f "$OUT/usr/bin/tdawn.bin"
riscv32-linux-strip -o "$OUT/usr/lib/tdawn/game" "$BUILD/vanillatd"
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
# the 3D view's data (docs/games/tdawn.md, "In 3D"): each map's heights from its own templates, read
# against the fetched tree's table of them, and the models made by script; 47 KB of the ROM
python3 "$HERE/td3d/heights.py" --source "$SRC/tiberiandawn" --mix "$WORK/src/tdawn-demo/DEMOL.MIX" \
    --mix "$WORK/src/tdawn-demo/DEMO.MIX" --out "$OUT/usr/share/tdawn-heights.dat" > "$WORK/src/tdawn-heights.log"
python3 "$HERE/models/build.py" "$OUT/usr/share/tdawn-models.dat" > "$WORK/src/tdawn-models.log"
# the sounds as the sound card plays them from the ROM (docs/machine/sound.md); made again when stale
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
# its settings and saved games go to the folder the host keeps (docs/system/persist.md)
HOME=/tmp/tdawn XDG_CONFIG_HOME=/persistence exec /usr/lib/tdawn/game "$@"
EOF
chmod +x "$OUT/usr/bin/tdawn"
echo "Games/Tiberian Dawn=tdawn" > "$OUT/usr/share/nxapps.60-tdawn"
ls -l "$OUT/usr/lib/tdawn/game" "$OUT/usr/share"/tdawn-*.MIX
