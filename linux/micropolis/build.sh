#!/usr/bin/env bash
# Builds Micropolis, the city simulator of 1989 (MicropolisCore's engine, GPLv3 with EA's terms;
# a Nano-X front end here), with its tiles, sprites, tools and cities (docs/games/micropolis.md).
#   wsl -- bash /mnt/c/.../linux/micropolis/build.sh   (after linux/nanox, linux/tdawn and linux/libmedia's build.sh)
# In the guest: nano-X -p & nxmicropolis
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
MP=${MICROPOLIS_SRC:-$WORK/src/micropolis}
BUILD=$WORK/src/micropolis-build
OUT=${MICROPOLIS_OUT:-$REPO/build/images/linux/root}	# (MICROPOLIS_OUT: a private image's overlay)
COMMIT=9d5b400b2d3beaf81e4103056f6c7f503dfde6e1
GCC=releases/gcc-13.3.0
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
# (the C++ driver over musl is Tiberian Dawn's)
[ -x "$WORK/bin/rv32-c++" ] || { echo "run linux/tdawn/build.sh first"; exit 1; }

# only the engine and the game's data of the repository (its web app and documents are 200 MB)
if [ ! -d "$MP/.git" ]; then
    echo "== MicropolisCore $COMMIT"
    git clone -q --filter=blob:none --no-checkout https://github.com/SimHacker/MicropolisCore.git "$MP"
fi
git -C "$MP" sparse-checkout set --no-cone /packages/micropolis-engine/src/ /content/micropolis/cities/ /content/micropolis/data/ \
    /content/micropolis/images/ /content/micropolis/sounds/ /LICENSE /gpl-3.0.txt /MicropolisGPLLicenseNotice.md /MicropolisPublicNameLicense.md
# upstream's commit plus our patch, put there again on every build (MICROPOLIS_TREE=keep: the tree
# as it stands, for writing that patch: git diff -- packages/micropolis-engine/src)
if [ "${MICROPOLIS_TREE:-}" != keep ]; then
    git -C "$MP" checkout -q -f "$COMMIT" && git -C "$MP" clean -q -fd
    git -C "$MP" apply "$HERE/micropolis.patch"
fi
ENGINE=$MP/packages/micropolis-engine/src
mkdir -p "$BUILD/guest" "$BUILD/host"
# std::map's tree and std::list's links, which libstdc++'s headers leave to its library (Tiberian
# Dawn's cxxrt.cpp has the rest)
for f in tree list; do
    [ -f "$BUILD/$f.cc" ] || curl -sSfL "https://raw.githubusercontent.com/gcc-mirror/gcc/$GCC/libstdc++-v3/src/c++98/$f.cc" -o "$BUILD/$f.cc"
done

# the engine for the guest, and for this computer: the test's sums must be the same on both.
# -fwrapv: the engine's sums overflow 32 bits in a large city, which C leaves undefined and
# the compiler then worked out differently for each machine (Haight-Ashbury's sums differed).
SOURCES=$(cd "$ENGINE" && ls *.cpp | grep -v -e '^emscripten.cpp$' -e '^callback.cpp$')
# The guest's engine at -O3 with loops unrolled: 355.8 to 229.5 million instructions of the two sums' runs,
# the same sums (rvc_cpu, 3 November 2026; docs/system/flags.md). Objects of other flags are made again.
CXXFLAGS="-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops -fwrapv -fno-pie -w -I$ENGINE -I$HERE"
[ "$(cat "$BUILD/guest/flags" 2>/dev/null)" = "$CXXFLAGS" ] || { rm -f "$BUILD"/guest/*.o; echo "$CXXFLAGS" > "$BUILD/guest/flags"; }
export ENGINE BUILD CXXFLAGS HERE
{ for f in $SOURCES; do echo "$ENGINE/$f"; done; echo "$HERE/engine.cpp"; echo "$REPO/linux/tdawn/cxxrt.cpp"; echo "$BUILD/tree.cc"; echo "$BUILD/list.cc"; } |
    xargs -P "$(nproc)" -I{} sh -c 'o=$BUILD/guest/$(basename {} | sed "s/\.c[cp]*$//").o; [ "$o" -nt {} ] && [ "$o" -nt "$HERE/micropolis.patch" ] && [ "$o" -nt "$HERE/mp.h" ] || rv32-c++ $CXXFLAGS -c {} -o "$o"'
{ for f in $SOURCES; do echo "$ENGINE/$f"; done; echo "$HERE/engine.cpp"; } |
    xargs -P "$(nproc)" -I{} sh -c 'o=$BUILD/host/$(basename {} .cpp).o; [ "$o" -nt {} ] && [ "$o" -nt "$HERE/micropolis.patch" ] && [ "$o" -nt "$HERE/mp.h" ] || g++ -O2 -fwrapv -w -I$ENGINE -I$HERE -c {} -o "$o"'
printf '#include "mp.h"\nint main(int c, char **v) { return c > 1 && mp_sum_test(v[1]) == 0 ? 0 : 1; }\n' > "$BUILD/host/sum.c"
g++ -O2 -I"$HERE" -x c++ "$BUILD/host/sum.c" -x none "$BUILD"/host/*.o -o "$BUILD/micropolis-sum"

INC="-I$HERE -I$MW/src/include -I$REPO/linux/apps -I$REPO/linux/userland -idirafter $REPO/programs/linux/include"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/linux/gles.c" -o "$BUILD/gles.o"
rv32-cc -O2 -fno-pie -Wall -Wno-unused-function $INC -c "$HERE/nxmicropolis.c" -o "$BUILD/nxmicropolis.o"
rv32-c++ -O2 "$BUILD/nxmicropolis.o" "$BUILD"/guest/*.o "$BUILD/gles.o" "$MW/src/lib/libnano-X.a" -lm -o "$BUILD/nxmicropolis"
mkdir -p "$OUT/usr/bin" "$OUT/usr/share/micropolis/cities" "$OUT/usr/share/micropolis/data"
riscv32-linux-strip -o "$OUT/usr/bin/nxmicropolis" "$BUILD/nxmicropolis"
riscv32-linux-nm -n "$BUILD/nxmicropolis" > "$WORK/src/micropolis.nm"

# the pictures as the GPU reads them, the cities, the texts, and the licences that go with them
CONTENT=$MP/content/micropolis
# the sounds the engine and the tools ask for, decoded by libmedia's build for this computer
DECODE=$WORK/src/libmedia/media_test-host
rm -rf "$BUILD/sounds" && mkdir -p "$BUILD/sounds"
if [ -x "$DECODE" ]; then
    for s in ExplosionHigh ExplosionLow FogHornLow HeavyTraffic HonkHonkHigh HonkHonkLow HonkHonkMed Monster Siren Sorry UhUh \
             build bulldozer; do
        "$DECODE" audio "$CONTENT/sounds/$s.mp3" "$BUILD/sounds/$s.raw" -1 > /dev/null
    done
else
    echo "(no sounds: run linux/libmedia/build.sh first)"
fi
python3 "$HERE/pack.py" "$CONTENT" "$OUT/usr/share/micropolis" "$BUILD/sounds"
cp "$CONTENT"/cities/*.cty "$OUT/usr/share/micropolis/cities/"
cp "$CONTENT"/data/stri.*.txt "$OUT/usr/share/micropolis/data/"
cp "$MP/MicropolisGPLLicenseNotice.md" "$MP/MicropolisPublicNameLicense.md" "$MP/gpl-3.0.txt" "$OUT/usr/share/micropolis/"
cat "$MP/MicropolisGPLLicenseNotice.md" > "$OUT/usr/share/micropolis/COPYING"
echo "Games/Micropolis=nxmicropolis" > "$OUT/usr/share/nxapps.72-micropolis"
# a city anybody opens is played (docs/system/open.md)
echo "city cty - nxmicropolis %s" > "$OUT/usr/share/nxopen.72-micropolis"
"$BUILD/micropolis-sum" "$CONTENT/cities/haight.cty,1000"
ls -l "$OUT/usr/bin/nxmicropolis"; du -sh "$OUT/usr/share/micropolis"
