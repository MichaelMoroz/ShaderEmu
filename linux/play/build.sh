#!/usr/bin/env bash
# Builds nxplay, the audio player (docs/play.md), and puts it and its test files in the image.
#   wsl -- bash /mnt/c/.../linux/play/build.sh        (after linux/nanox/build.sh)
# In the guest: nxplay /usr/share/play-test.mp3
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
OUT=$REPO/build/images/linux/root
BUILD=$WORK/src/play
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
mkdir -p "$BUILD/fetch" "$OUT/usr/bin" "$OUT/usr/share"

# The decoders are one header each, fetched at one commit and not kept in the repository:
# minimp3 is CC0, dr_flac public domain or MIT No Attribution (each says so at its top).
fetch() {
    [ -f "$BUILD/fetch/$1" ] || curl -sSfL "$2" -o "$BUILD/fetch/$1"
    echo "$3  $BUILD/fetch/$1" | sha256sum -c --quiet -
}
fetch minimp3.h https://raw.githubusercontent.com/lieff/minimp3/ea99364f61c14656440e8d77e9c233ccf3124633/minimp3.h \
    57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad
fetch dr_flac.h https://raw.githubusercontent.com/mackron/dr_libs/dfe8377631000664666519fdb83da193fd8037f4/dr_flac.h \
    111144e778f55738db6851cb226015c419e00d04b916a09506d4856d9cff945c
# minimp3 with a half rate and with frames that only fill the bit reservoir (minimp3.patch)
cp "$BUILD/fetch/minimp3.h" "$BUILD/minimp3.h"
cp "$BUILD/fetch/dr_flac.h" "$BUILD/dr_flac.h"
tr -d '\r' < "$HERE/minimp3.patch" | patch -s "$BUILD/minimp3.h"

# single-precision constants: minimp3 compares a float with 32766.5, which is a double
# comparison otherwise, and the machine has no doubles
FLOAT="-fsingle-precision-constant -fno-math-errno"
INC="-I$BUILD -I$HERE -I$REPO/linux/apps -I$MW/src/include -I$REPO/linux/userland -I$REPO/programs/mc"
rv32-cc -O2 -fno-pie -Wall -Wno-unused-function $INC -c "$HERE/nxplay.c" -o "$BUILD/nxplay.o"
rv32-cc -O2 -fno-pie -w $FLOAT -I"$BUILD" -c "$HERE/mp3.c" -o "$BUILD/mp3full.o"
rv32-cc -O2 -fno-pie -w $FLOAT -DMP3D_HALF -I"$BUILD" -c "$HERE/mp3.c" -o "$BUILD/mp3half.o"
rv32-cc -O2 -fno-pie -w $FLOAT -I"$BUILD" -I"$HERE" -c "$HERE/flac.c" -o "$BUILD/flac.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/mc/mcw.c" -o "$BUILD/mcw.o"
rv32-cc -O2 -no-pie "$BUILD/nxplay.o" "$BUILD/mp3full.o" "$BUILD/mp3half.o" "$BUILD/flac.o" "$BUILD/mcw.o" \
    "$MW/src/lib/libnano-X.a" -lm -o "$BUILD/nxplay"
riscv32-linux-nm -n "$BUILD/nxplay" > "$WORK/src/nxplay.nm"
riscv32-linux-strip -o "$OUT/usr/bin/nxplay" "$BUILD/nxplay"
# its test files (make_test.py made them); /usr/share, since the image builder makes no folders
cp "$HERE"/test/play-test.* "$OUT/usr/share/"
echo "Other/Music=nxplay" > "$OUT/usr/share/nxapps.84-play"
# what `open` starts for a sound file (linux/apps/nxopen.conf has the columns)
printf '%s\n' 'sound wav,mp3,flac,mod ID3,fLaC,\xff\xfb nxplay %s' > "$OUT/usr/share/nxopen.84-play"
ls -l "$OUT/usr/bin/nxplay"
