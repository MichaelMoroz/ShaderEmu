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
# -fno-common -fdata-sections, and ALIGN16 below: a variable is a section of its own that starts
# on 16 bytes. A worker core builds chunk meshes (MapRenderer.h, docs/multicore.md), and two
# cores must not store to the same 16 bytes.
CFLAGS="-O2 $FPU -fno-pie -fno-common -fdata-sections -fno-strict-aliasing -w -DPLAT_SHADEREMU -I$C/src -I$MW/src/include -I$REPO/programs/mc ${CLASSICUBE_DEFINES:-}"
ALIGN16="--set-section-alignment .data*=16 --set-section-alignment .sdata*=16 --set-section-alignment .bss*=16 --set-section-alignment .sbss*=16"
export CFLAGS HERE BUILD ALIGN16
mkdir -p "$BUILD" && cd "$BUILD"
{ ls "$C"/src/*.c; ls "$HERE"/*.c; } |
    xargs -P "$(nproc)" -I{} sh -c 'o=$(basename {} .c).o; [ "$o" -nt {} ] && [ "$o" -nt "$HERE/classicube.patch" ] && [ "$o" -nt "$HERE/build.sh" ] || { rv32-cc $CFLAGS -c {} -o "$o" && riscv32-linux-objcopy $ALIGN16 "$o"; }'
# Builder.c a second time, for the worker core: its own buffers, and its three names another's
rv32-cc $CFLAGS -DSE_SECOND -DBuilder_MakeChunk=Builder_MakeChunk_w -DBuilder_ApplyActive=Builder_ApplyActive_w     -DBuilder_Component=Builder_Component_w -c "$C/src/Builder.c" -o Builder_w.o
# the worker cores' library, the same for every program (programs/mc)
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -Wall -c -I"$REPO/programs/mc" "$REPO/programs/mc/mcw.c" -o mcw.o
# and 16 bytes of nothing after our variables in each kind of section: the libraries' follow
printf 'char se_pad_d[16] __attribute__((aligned(16))) = {1}; char se_pad_b[16] __attribute__((aligned(16)));
' > zz_pad.c
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -msmall-data-limit=0 -c zz_pad.c -o zz_pad_large.o
sed 's/se_pad_/se_pad_s/g' zz_pad.c > zz_pads.c
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -msmall-data-limit=64 -c zz_pads.c -o zz_pad_small.o
for o in Builder_w.o mcw.o zz_pad_large.o zz_pad_small.o; do riscv32-linux-objcopy $ALIGN16 "$o"; done
rv32-cc -O2 $FPU -w $(ls *.o | grep -v '^zz_') zz_pad_large.o zz_pad_small.o "$MW/src/lib/libnano-X.a" -lm -o classicube
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
