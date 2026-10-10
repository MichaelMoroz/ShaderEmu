#!/usr/bin/env bash
# Builds a NES for the image (docs/nes.md): Nofrendo (LGPL-2), as Espressif keep it for a small
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
OUT=$REPO/build/images/linux/root
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
INC="-I$HERE -I$N -I$N/cpu -I$N/nes -I$N/sndhrdw -I$N/libsnss -I$N/mappers -I$MW/src/include -I$REPO/programs/mc -idirafter $REPO/programs/linux/include"
# (asm: the one line of the ESP32's own in these sources, a breakpoint where an assertion fails;
# 240: the PPU draws every line into a picture the sources make 224 high)
CFLAGS="-O2 -fno-pie -std=gnu99 -fgnu89-inline -fcommon -w -Dasm(x)=abort() -DNES_VISIBLE_HEIGHT=240 -DHOST_LITTLE_ENDIAN -DNES6502_JUMPTABLE $INC"
export CFLAGS BUILD HERE
mkdir -p "$BUILD" && cd "$BUILD"
{ find "$N" -name '*.c'; ls "$HERE"/*.c "$REPO/programs/linux/gles.c" "$REPO/programs/mc/mcw.c"; } |
    xargs -P "$(nproc)" -I{} sh -c 'o=$(basename {} .c).o; [ "$o" -nt {} ] && [ "$o" -nt "$HERE/build.sh" ] && [ "$o" -nt "$HERE/nofrendo.patch" ] && [ "$o" -nt "$HERE/rc6502.h" ] || rv32-cc $CFLAGS -c {} -o "$o"'
rv32-cc -O2 *.o "$MW/src/lib/libnano-X.a" -lm -o nes
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
# a .nes file anybody opens is this program's (docs/open.md)
printf '%s\n' 'nes nes NES\x1a nes %s' > "$OUT/usr/share/nxopen.85-nes"
printf '%s\n' "Games/Thwaite (NES)=nes thwaite" "Games/Nova the Squirrel (NES)=nes nova" > "$OUT/usr/share/nxapps.85-nes"
ls -l "$OUT/usr/bin/nes.bin" "$OUT/usr/share"/nes-*.nes
