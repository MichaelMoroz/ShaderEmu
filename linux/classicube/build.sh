#!/usr/bin/env bash
# Builds ClassiCube (BSD-3, a Nano-X client here) for the image (docs/classicube.md).
#   wsl -- bash /mnt/c/.../linux/classicube/build.sh        (after linux/nanox/build.sh)
# In the guest: nano-X -p & classicube
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
C=$WORK/src/ClassiCube
BUILD=$WORK/src/classicube-build
OUT=$REPO/build/images/linux/root
COMMIT=d41c3f7eef2038f59702b58bdb373483fb0d28f9
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }

if [ ! -d "$C/.git" ]; then
    echo "== ClassiCube $COMMIT"
    git clone -q https://github.com/ClassiCube/ClassiCube.git "$C"
fi
# the tree is that commit plus our patch, put there again on every build
git -C "$C" checkout -q -f "$COMMIT" && git -C "$C" clean -q -fd
git -C "$C" apply "$HERE/classicube.patch"

# Every file of the game leaves itself out by the platform's defines; ours are the window and
# the graphics. Floats are the machine's instructions; a constant without an f is a float too.
FPU="-fno-math-errno -fsingle-precision-constant"
CFLAGS="-O2 $FPU -fno-pie -fno-strict-aliasing -w -DPLAT_SHADEREMU -I$C/src -I$MW/src/include ${CLASSICUBE_DEFINES:-}"
export CFLAGS HERE BUILD
mkdir -p "$BUILD" && cd "$BUILD"
{ ls "$C"/src/*.c; ls "$HERE"/*.c; } |
    xargs -P "$(nproc)" -I{} sh -c 'o=$(basename {} .c).o; [ "$o" -nt {} ] && [ "$o" -nt "$HERE/classicube.patch" ] && [ "$o" -nt "$HERE/build.sh" ] || rv32-cc $CFLAGS -c {} -o "$o"'
rv32-cc -O2 $FPU -w *.o "$MW/src/lib/libnano-X.a" -lm -o classicube
mkdir -p "$OUT/usr/bin" "$OUT/usr/share"
riscv32-linux-strip -o "$OUT/usr/bin/classicube.bin" classicube
riscv32-linux-nm -n classicube > "$WORK/src/classicube.nm"

# all in /usr/share: the image builder adds files to folders the ROM has, and makes none.
# The game looks for texpacks/default.zip where it runs and writes its settings and maps there.
# Its textures go in uncompressed (store_zip.py): inflating them was 7 seconds of every start.
python3 "$HERE/store_zip.py" "$C/misc/cc_textures.zip" "$OUT/usr/share/classicube-default.zip"
cat > "$OUT/usr/bin/classicube" <<'EOF'
#!/bin/sh
mkdir -p /tmp/classicube/texpacks && cd /tmp/classicube || exit 1
ln -sf /usr/share/classicube-default.zip texpacks/default.zip
# (the frame rate in the corner is a line of text drawn again every second: 7% of the machine)
[ -f options.txt ] || echo "gui-showfps=false" > options.txt
[ $# = 0 ] && set -- --singleplayer
exec classicube.bin "$@"
EOF
chmod +x "$OUT/usr/bin/classicube"
echo "Games/ClassiCube=classicube" > "$OUT/usr/share/nxapps.75-classicube"
ls -l "$OUT/usr/bin/classicube.bin"
