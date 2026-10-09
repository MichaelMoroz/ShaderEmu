#!/usr/bin/env bash
# Builds Command & Conquer: Red Alert (Vanilla Conquer, a Nano-X client here) for the image, and
# fetches the demo's game data, which is not kept in this repository (docs/ralert.md).
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
OUT=$REPO/build/images/linux/root
COMMIT=ce83b59
DEMO=${RALERT_DEMO:-https://archive.org/download/CommandConquerRedAlert_1020/ra95demo.zip}
# Tiberian Dawn's build makes the C++ driver and the toolchain file, and fetches the source
[ -x "$WORK/bin/rv32-c++" ] && [ -f "$WORK/src/tdawn-toolchain.cmake" ] || { echo "run linux/tdawn/build.sh first"; exit 1; }

# the tree is upstream's commit plus Tiberian Dawn's patch (the machine's side is shared), ours
# on top of it, and the files of both, put there again on every build
git -C "$VC" checkout -q -f "$COMMIT" && git -C "$VC" clean -q -fd
git -C "$VC" apply "$TD/vanilla-conquer.patch"
[ ! -s "$HERE/vanilla-conquer.patch" ] || git -C "$VC" apply "$HERE/vanilla-conquer.patch"
cp "$TD/cxxrt.cpp" "$TD/shaderemu.cpp" "$TD/soundio_shaderemu.cpp" "$TD/host.c" "$TD/host.h" "$VC/common/"
cp "$HERE/gl.cpp" "$VC/redalert/"
# The worker cores' side (docs/multicore.md): mcw.c is the library every program has for them,
# workers.c the game's jobs over it.
cp "$HERE/workers.c" "$HERE/workers.h" "$REPO/programs/mc/mc.h" "$REPO/programs/mc/mcw.h" "$REPO/programs/mc/mcw.c" "$VC/redalert/"

GLINC="-idirafter $REPO/programs/linux/include -I$MW/src/include -I$REPO/linux/userland"
# (the flags are Tiberian Dawn's: docs/tdawn.md)
cmake -S "$VC" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$WORK/src/tdawn-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DSDL2=OFF -DOPENAL=OFF -DNETWORKING=OFF -DBUILD_VANILLATD=OFF -DSHADEREMU=ON \
    -DSHADEREMU_GLES="$REPO/programs/linux/gles.c" -DSHADEREMU_NANOX="$MW/src/lib/libnano-X.a" \
    -DCMAKE_CXX_FLAGS="-O2 -fno-pie -fno-lifetime-dse -w -DSHADEREMU -DSHADEREMU_RA $GLINC" -DCMAKE_CXX_FLAGS_RELEASE="-DNDEBUG" \
    -DCMAKE_C_FLAGS="-O2 -fno-pie -w -DSHADEREMU -DSHADEREMU_RA $GLINC" -DCMAKE_C_FLAGS_RELEASE="-DNDEBUG" > "$WORK/src/ralert-configure.log" 2>&1 \
    || { tail -20 "$WORK/src/ralert-configure.log"; exit 1; }
cmake --build "$BUILD" -j"$(nproc)" > "$WORK/src/ralert-build.log" 2>&1 \
    || { grep -E "error|undefined reference" "$WORK/src/ralert-build.log" | head -40; exit 1; }

mkdir -p "$OUT/usr/bin" "$OUT/usr/share"
riscv32-linux-strip -o "$OUT/usr/bin/ralert.bin" "$BUILD/vanillara"
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
# The sounds as the sound card plays them from the ROM (docs/sound.md), by Tiberian Dawn's tool.
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
ini=.config/vanilla-conquer/vanillara/redalert.ini
[ -f $ini ] || { mkdir -p ${ini%/*} && printf '[Intro]\nPlayIntro=no\n' > $ini; }
# the game reads the whole of its program's folder for every file it looks for: not /usr/bin
[ -x game ] || cp /usr/bin/ralert.bin game
HOME=/tmp/ralert exec ./game "$@"
EOF
chmod +x "$OUT/usr/bin/ralert"
echo "Games/Red Alert=ralert" > "$OUT/usr/share/nxapps.65-ralert"
ls -l "$OUT/usr/bin/ralert.bin" "$OUT/usr/share"/ralert-*.MIX
