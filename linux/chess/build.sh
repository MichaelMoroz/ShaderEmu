#!/usr/bin/env bash
# Builds nxchess, chess on the desktop that searches on every worker core (docs/games/chess.md).
#   wsl -- bash linux/nommu/build.sh chess/build.sh        (after nanox/build.sh)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
OUT=${CHESS_OUT:-$REPO/build/images/linux/root}   # (CHESS_OUT: a private image's overlay)
BUILD=$WORK/src/chess-build
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
mkdir -p "$BUILD" "$OUT/usr/bin" "$OUT/usr/share"
INC="-I$MW/src/include -I$REPO/linux/userland -I$REPO/programs/mc -I$REPO/linux/apps -I$HERE"
rv32-cc -O3 -fno-tree-vectorize -fsched-pressure -Wall -Wno-unused-function $INC -c "$HERE/nxchess.c" -o "$BUILD/nxchess.o"
rv32-cc -O2 -w $INC -c "$REPO/programs/mc/mcw.c" -o "$BUILD/mcw.o"
rv32-cc -O2 -w $INC -c "$REPO/programs/mc/mcwx.c" -o "$BUILD/mcwx.o"
rv32-cc -O2 "$BUILD/nxchess.o" "$BUILD/mcwx.o" "$BUILD/mcw.o" "$MW/src/lib/libnano-X.a" -o "$BUILD/nxchess"
riscv32-linux-nm -n "$BUILD/nxchess" > "$WORK/src/nxchess.nm"
riscv32-linux-strip -o "$OUT/usr/bin/nxchess" "$BUILD/nxchess"
echo "Games/Chess=nxchess" > "$OUT/usr/share/nxapps.71-chess"
ls -l "$OUT/usr/bin/nxchess"
