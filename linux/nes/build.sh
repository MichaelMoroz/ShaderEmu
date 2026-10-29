#!/usr/bin/env bash
# Builds a NES for the image (docs/games/nes.md): Nofrendo (LGPL-2), as Espressif keep it for a small
# machine, with this directory's side of it, and two free games. In the guest: nes thwaite
#   wsl -- bash /mnt/c/.../linux/nes/build.sh        (after linux/nanox/build.sh)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
SRC=$WORK/src/esp32-nesemu
ROMS=$WORK/src/nes-roms
BUILD=$WORK/src/nes-build
OUT=${NES_OUT:-$REPO/build/images/linux/root}   # (NES_OUT: a private image's overlay)
COMMIT=693e378
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }

if [ ! -d "$SRC/.git" ]; then
    echo "== Nofrendo (esp32-nesemu $COMMIT)"
    git clone -q https://github.com/espressif/esp32-nesemu "$SRC"
fi
git -C "$SRC" checkout -q -f "$COMMIT"
# two games anyone may pass on (GPL-3): Thwaite by Damian Yerrick, Nova the Squirrel by NovaSquirrel
mkdir -p "$ROMS"
[ -f "$ROMS/thwaite.nes" ] || curl -sSfL -o "$ROMS/thwaite.nes" https://github.com/pinobatch/thwaite-nes/releases/download/v0.04/thwaite.nes
[ -f "$ROMS/nova.nes" ] || curl -sSfL -o "$ROMS/nova.nes" https://github.com/NovaSquirrel/NovaTheSquirrel/releases/download/v1.0.6a/nova.nes

N=$SRC/components/nofrendo
# ours: the first frame owed at the start, and the loop a game waits in counted, not run
git -C "$SRC" apply "$HERE/nofrendo.patch"
INC="-I$HERE -I$N -I$N/cpu -I$N/nes -I$N/sndhrdw -I$N/libsnss -I$N/mappers -I$MW/src/include -I$REPO/programs/mc -I$REPO/linux/userland -idirafter $REPO/programs/linux/include"
# (asm: the one line of the ESP32's own in these sources, a breakpoint where an assertion fails;
# 240: the PPU draws every line into a picture the sources make 224 high)
# (5 to 7% fewer instructions a frame than -O2, with the same sums)
OPT="-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops"
CFLAGS="$OPT -flto -fno-pie -std=gnu99 -fgnu89-inline -fcommon -w -Dasm(x)=abort() -DNES_VISIBLE_HEIGHT=240 -DHOST_LITTLE_ENDIAN -DNES6502_JUMPTABLE $INC"
# What played games ran, for the compiler (linux/userland/pgo/gcov.c; docs/games/tdawn.md, "What the compiler is told"):
# NES_PROFILE=count builds the program that counts, in a folder of its own; otherwise nes.profile's counts.
PGO=""; COUNTS=""
if [ "${NES_PROFILE:-}" = count ]; then
    BUILD=$WORK/src/nes-count-build
    mkdir -p "$WORK/src/nogcov" && riscv32-linux-ar rc "$WORK/src/nogcov/libgcov.a"
    PGO="-fprofile-generate -fprofile-update=single"; COUNTS="-L$WORK/src/nogcov"
elif [ -f "$HERE/nes.profile" ] && [ "${NES_PROFILE:-}" != off ]; then
    python3 "$REPO/linux/tdawn/profile.py" put "$HERE/nes.profile" "$BUILD"
    PGO="-fprofile-use -fprofile-partial-training -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch"
fi
CFLAGS="$CFLAGS $PGO"
export CFLAGS BUILD HERE
mkdir -p "$BUILD" && cd "$BUILD"
[ "${NES_PROFILE:-}" = count ] && rv32-cc -O2 -fno-pie -Wall -c "$REPO/linux/userland/pgo/gcov.c" -o gcov.o
{ find "$N" -name '*.c'; ls "$HERE"/*.c "$REPO/programs/mc/mcw.c" "$REPO/programs/mc/mcwx.c"; } |
    xargs -P "$(nproc)" -I{} sh -c 'o=$(basename {} .c).o; [ "$o" -nt {} ] && [ "$o" -nt "$HERE/build.sh" ] && [ "$o" -nt "$HERE/nofrendo.patch" ] && [ "$o" -nt "$HERE/rc6502.h" ] && [ "$o" -nt "$HERE/nes.profile" ] || rv32-cc $CFLAGS -c {} -o "$o"'
# The GPU's library apart, a section a variable and each on 16 bytes (as linux/tdawn/build.sh
# has it): a worker core makes the lists with it while this one emulates, and two cores must
# not store to the same 16 bytes (docs/machine/multicore.md). Optimised with the rest, its variables
# lay among the emulator's, by chance of the build.
[ gles.o -nt "$REPO/programs/linux/gles.c" ] && [ gles.o -nt "$HERE/build.sh" ] || {
    rv32-cc $OPT -fno-pie -w $INC -fno-common -fdata-sections -fno-toplevel-reorder -msmall-data-limit=0 -c "$REPO/programs/linux/gles.c" -o gles.o
    riscv32-linux-objcopy --set-section-alignment '.data*=16' --set-section-alignment '.bss*=16' gles.o
}
rv32-cc $OPT $PGO $COUNTS -flto -flto-partition=one *.o "$MW/src/lib/libnano-X.a" -lm -o nes
mkdir -p "$OUT/usr/bin" "$OUT/usr/share"
riscv32-linux-strip -o "$OUT/usr/bin/nes.bin" nes
riscv32-linux-nm -n nes > "$WORK/src/nes.nm"
# all in /usr/share: the image builder adds files to folders the ROM has, and makes none
cp "$ROMS/thwaite.nes" "$OUT/usr/share/nes-thwaite.nes"
cp "$ROMS/nova.nes" "$OUT/usr/share/nes-nova.nes"
# nes NAME is the game /usr/share/nes-NAME.nes; nes FILE any other
cat > "$OUT/usr/bin/nes" <<'EOF'
#!/bin/sh
[ -f "/usr/share/nes-$1.nes" ] && exec nes.bin "/usr/share/nes-$1.nes"
exec nes.bin "$@"
EOF
chmod +x "$OUT/usr/bin/nes"
# a .nes file anybody opens is this program's (docs/system/open.md)
printf '%s\n' 'nes nes NES\x1a nes %s' > "$OUT/usr/share/nxopen.85-nes"
printf '%s\n' "Games/Thwaite (NES)=nes thwaite" "Games/Nova the Squirrel (NES)=nes nova" > "$OUT/usr/share/nxapps.85-nes"
ls -l "$OUT/usr/bin/nes.bin" "$OUT/usr/share"/nes-*.nes
