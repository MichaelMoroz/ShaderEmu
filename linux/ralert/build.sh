#!/usr/bin/env bash
# Builds Command & Conquer: Red Alert (Vanilla Conquer, a Nano-X client here) for the image, and
# fetches the demo's game data, which is not kept in this repository (docs/games/ralert.md).
#   wsl -- bash /mnt/c/.../linux/ralert/build.sh        (after linux/tdawn/build.sh)
# In the guest: nano-X -p & ralert
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
TD=$HERE/../tdawn
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
VC=$WORK/src/vanilla-conquer
BUILD=$WORK/src/ralert-build
OUT=${RALERT_OUT:-$REPO/build/images/linux/root}	# (another image's folder for an experiment)
COMMIT=ce83b59
DEMO=${RALERT_DEMO:-https://archive.org/download/CommandConquerRedAlert_1020/ra95demo.zip}
# Tiberian Dawn's build makes the C++ driver and the toolchain file, and fetches the source
[ -x "$WORK/bin/rv32-c++" ] && [ -f "$WORK/src/tdawn-toolchain.cmake" ] || { echo "run linux/tdawn/build.sh first"; exit 1; }
# a movie's rows and pointers are libmedia's (docs/system/media.md, "Films")
MEDIA=${RALERT_MEDIA:-$WORK/src/libmedia/libmedia.a}
[ -f "$MEDIA" ] || { echo "run linux/libmedia/build.sh first"; exit 1; }

# the tree is upstream's commit plus Tiberian Dawn's patch (the machine's side is shared), ours
# on top of it, and the files of both, put there again on every build
git -C "$VC" checkout -q -f "$COMMIT" && git -C "$VC" clean -q -fd
git -C "$VC" apply "$TD/vanilla-conquer.patch"
[ ! -s "$HERE/vanilla-conquer.patch" ] || git -C "$VC" apply "$HERE/vanilla-conquer.patch"
cp "$TD/cxxrt.cpp" "$TD/shaderemu.cpp" "$TD/soundio_shaderemu.cpp" "$TD/host.c" "$TD/host.h" "$VC/common/"
cp "$REPO/programs/mc/mc.h" "$REPO/programs/mc/mcw.h" "$REPO/programs/mc/mcw.c" "$REPO/programs/mc/mcwx.h" "$REPO/programs/mc/mcwx.c" "$VC/common/"
cp "$HERE/gl.cpp" "$VC/redalert/"
# The worker cores' side (docs/machine/multicore.md): mcw.c is the library every program has for them,
# workers.c the game's jobs over it.
cp "$HERE/workers.c" "$HERE/workers.h" "$REPO/programs/mc/mc.h" "$REPO/programs/mc/mcw.h" "$REPO/programs/mc/mcw.c" "$REPO/programs/mc/mcwx.h" "$REPO/linux/libmedia/media.h" "$VC/redalert/"
# (the reset above renews every file's time, and the two games reset the same tree: each builds
# from a copy of its own, where a file keeps its time unless its bytes change)
SRC=$WORK/src/vanilla-conquer-ra
rsync -rc --delete --exclude .git "$VC/" "$SRC/"
grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$SRC" "$BUILD/CMakeCache.txt" 2>/dev/null || rm -rf "$BUILD/CMakeCache.txt" "$BUILD/CMakeFiles"

GLINC="-idirafter $REPO/programs/linux/include -I$MW/src/include -I$REPO/linux/userland"
# (the flags are Tiberian Dawn's: docs/games/tdawn.md)
LTO="-O3 -flto=$(nproc) -ffat-lto-objects -flto-partition=1to1 -fno-toplevel-reorder --param inline-unit-growth=2000 --param max-inline-insns-auto=400 --param max-inline-insns-single=1500 --param large-function-growth=1000 --param inline-min-speedup=5"
# What a played game ran, for the compiler (docs/games/tdawn.md, "What the compiler is told"):
# TDAWN_PROFILE=count builds the program that counts; otherwise the counts kept here are used.
PGO=""; COUNTS=""
if [ "${TDAWN_PROFILE:-}" = count ]; then
    # (the counters' functions are host.c's: an empty libgcov in the way of the toolchain's)
    mkdir -p "$WORK/src/nogcov" && riscv32-linux-ar rc "$WORK/src/nogcov/libgcov.a"
    PGO="-L$WORK/src/nogcov -fprofile-generate -fprofile-update=single -fprofile-exclude-files=/common/"; COUNTS="-DSHADEREMU_PGO"
elif [ -f "$HERE/ralert.profile" ]; then
    python3 "$TD/profile.py" put "$HERE/ralert.profile" "$BUILD"
    # (the counts' sum in the flags: make knows nothing of the .gcda files, linux/tdawn/build.sh)
    PGO="-fprofile-use -fprofile-partial-training -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch -DSHADEREMU_COUNTS=$(sha1sum "$HERE/ralert.profile" | cut -c1-12)"
fi
# (paths from where the compiler runs, as Tiberian Dawn's: the counts of a function that is not public then hold in any folder)
[ -x "$WORK/bin/tdawn-relative" ] || { echo "run linux/tdawn/build.sh first"; exit 1; }
cmake -S "$SRC" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$WORK/src/tdawn-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER_LAUNCHER="$WORK/bin/tdawn-relative" \
    -DSDL2=OFF -DOPENAL=OFF -DNETWORKING=OFF -DBUILD_VANILLATD=OFF -DSHADEREMU=ON \
    -DSHADEREMU_GLES="$WORK/src/tdawn-gles.o" -DSHADEREMU_NANOX="$MW/src/lib/libnano-X.a" \
    -DCMAKE_CXX_FLAGS="-O2 -fno-pie -fno-lifetime-dse -w -DSHADEREMU -DSHADEREMU_RA $GLINC $LTO $PGO" -DCMAKE_CXX_FLAGS_RELEASE="-DNDEBUG" \
    -DCMAKE_C_FLAGS="-O2 -fno-pie -w -DSHADEREMU -DSHADEREMU_RA $COUNTS $GLINC" -DCMAKE_C_FLAGS_RELEASE="-DNDEBUG" \
    -DCMAKE_CXX_STANDARD_LIBRARIES="$MEDIA" > "$WORK/src/ralert-configure.log" 2>&1 \
    || { tail -20 "$WORK/src/ralert-configure.log"; exit 1; }
cmake --build "$BUILD" -j"$(nproc)" > "$WORK/src/ralert-build.log" 2>&1 \
    || { grep -E "error|undefined reference" "$WORK/src/ralert-build.log" | head -40; exit 1; }

# the game lists its program's folder for every file it looks for: a folder of its own in the ROM
mkdir -p "$OUT/usr/bin" "$OUT/usr/share" "$OUT/usr/lib/ralert"
rm -f "$OUT/usr/bin/ralert.bin"
riscv32-linux-strip -o "$OUT/usr/lib/ralert/game" "$BUILD/vanillara"
riscv32-linux-nm -n "$BUILD/vanillara" > "$WORK/src/ralert.nm"

if [ ! -f "$WORK/src/ralert-demo/MAIN.MIX" ]; then
    echo "== the demo's data"
    mkdir -p "$WORK/src/ralert-demo"
    [ -f "$WORK/src/ralert-demo.zip" ] || curl -sSL "$DEMO" -o "$WORK/src/ralert-demo.zip"
    python3 - "$WORK/src/ralert-demo.zip" "$WORK/src/ralert-demo" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for name in ('REDALERT.MIX', 'MAIN.MIX'):
    open(sys.argv[2] + '/' + name, 'wb').write(z.read('ra95demo/INSTALL/' + name))
PY
fi
# The sounds as the sound card plays them from the ROM (docs/machine/sound.md), by Tiberian Dawn's tool.
# They are in MIX files inside the two MIX files, behind an encrypted index: the source's own
# tool, built for this computer, takes them out.
PAK="$WORK/src/ralert-demo/ralert-sound.pak"
if [ ! "$PAK" -nt "$WORK/src/ralert-demo/MAIN.MIX" ] || [ ! "$PAK" -nt "$REPO/tools/make_tdawn_sound.py" ] || [ ! -d "$WORK/src/ralert-demo/x/scores" ]; then
    TOOLS=$WORK/src/vanillamix-build
    if [ ! -x "$TOOLS/vanillamix" ]; then
        cmake -S "$VC" -B "$TOOLS" -DCMAKE_BUILD_TYPE=Release -DBUILD_TOOLS=ON -DBUILD_VANILLATD=OFF -DBUILD_VANILLARA=OFF \
            -DSDL2=OFF -DOPENAL=OFF -DNETWORKING=OFF > "$TOOLS.log" 2>&1 && cmake --build "$TOOLS" -j"$(nproc)" >> "$TOOLS.log" 2>&1 \
            || { tail -20 "$TOOLS.log"; exit 1; }
    fi
    X=$WORK/src/ralert-demo/x
    rm -rf "$X" && mkdir -p "$X/main" "$X/redalert"
    "$TOOLS/vanillamix" -x -d "$X/main" "$WORK/src/ralert-demo/MAIN.MIX" > /dev/null
    "$TOOLS/vanillamix" -x -d "$X/redalert" "$WORK/src/ralert-demo/REDALERT.MIX" > /dev/null
    for m in main/sounds main/allies main/russian main/conquer main/scores redalert/speech; do
        mkdir -p "$X/$(basename $m)" && "$TOOLS/vanillamix" -x -d "$X/$(basename $m)" "$X/$m.mix" > /dev/null
    done
    python3 "$REPO/tools/make_tdawn_sound.py" "$PAK" "$X"/{sounds,allies,russian,conquer,scores,speech}
fi
cp "$PAK" "$OUT/usr/share/ralert-sound.pak"
# the demo's two tunes: the game finds them by name, the card plays them from the pack, so
# only how each begins has to be there (as files of their own, which is where a tune is looked for)
TUNES="CRUS226M HELL226M"
for f in $TUNES; do head -c 64 "$WORK/src/ralert-demo/x/scores/$(echo $f | tr 'A-Z' 'a-z').aud" > "$OUT/usr/share/ralert-$f.AUD"; done
# all in /usr/share: the image builder adds files to folders the ROM has, and makes none.
# The game looks for its files by name in one folder, which the starter makes of links.
for f in REDALERT MAIN; do cp "$WORK/src/ralert-demo/$f.MIX" "$OUT/usr/share/ralert-$f.MIX"; done
cat > "$OUT/usr/bin/ralert" <<'EOF'
#!/bin/sh
mkdir -p /tmp/ralert && cd /tmp/ralert || exit 1
for f in REDALERT MAIN; do ln -sf /usr/share/ralert-$f.MIX $f.MIX; done
for f in CRUS226M HELL226M; do ln -sf /usr/share/ralert-$f.AUD $f.AUD; done
# a first run "from the install" goes past the menu into the Soviet mission: say it has been run
# (its settings and saved games are of the folder the host keeps: docs/system/persist.md)
ini=/persistence/vanilla-conquer/vanillara/redalert.ini
[ -f $ini ] || { mkdir -p ${ini%/*} && printf '[Intro]\nPlayIntro=no\n' > $ini; }
HOME=/tmp/ralert XDG_CONFIG_HOME=/persistence exec /usr/lib/ralert/game "$@"
EOF
chmod +x "$OUT/usr/bin/ralert"
echo "Games/Red Alert=ralert" > "$OUT/usr/share/nxapps.65-ralert"
ls -l "$OUT/usr/lib/ralert/game" "$OUT/usr/share"/ralert-*.MIX
