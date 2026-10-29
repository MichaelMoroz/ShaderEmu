#!/usr/bin/env bash
# Builds ClassiCube (BSD-3, a Nano-X client here) for the image (docs/games/classicube.md).
#   wsl -- bash /mnt/c/.../linux/classicube/build.sh        (after linux/nanox/build.sh)
# In the guest: nano-X -p & classicube
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
C=$WORK/src/ClassiCube
BUILD=$WORK/src/classicube-build
# (CLASSICUBE_OUT: a private image's overlay instead of the shared root)
OUT=${CLASSICUBE_OUT:-$REPO/build/images/linux/root}
COMMIT=d41c3f7eef2038f59702b58bdb373483fb0d28f9
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
# (the texture pack's PNGs are decoded by libmedia: png_shaderemu.c, docs/system/media.md)
MEDIA=${MEDIA_LIB:-$WORK/src/libmedia/libmedia.a}
[ -f "$MEDIA" ] || { echo "run linux/libmedia/build.sh first"; exit 1; }

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
# on 16 bytes. A worker core builds chunk meshes (MapRenderer.h, docs/machine/multicore.md), and two
# cores must not store to the same 16 bytes.
CFLAGS="-O2 $FPU -DCC_API= -DCC_VAR= -fno-pie -fno-common -fdata-sections -fno-strict-aliasing -w -DPLAT_SHADEREMU -I$C/src -I$MW/src/include -I$REPO/programs/mc -I$REPO/linux/libmedia -idirafter $REPO/programs/linux/include ${CLASSICUBE_DEFINES:-}"
ALIGN16="--set-section-alignment .data*=16 --set-section-alignment .sdata*=16 --set-section-alignment .bss*=16 --set-section-alignment .sbss*=16"
# The files a profile is full of get more of the compiler (no vectors: the machine has none).
# With it a frame is 3% fewer instructions, a chunk's mesh 14% and digging 20%; the meshes are the same.
HOT="-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops"
HOTFILES=" Builder Generator Graphics_ShaderEmu MapRenderer Lighting Particle ExtMath Vectors Animations Entity EntityComponents Physics World Block Bitmap Drawer2D "
export CFLAGS HERE BUILD ALIGN16 HOT HOTFILES
mkdir -p "$BUILD" && cd "$BUILD"
{ ls "$C"/src/*.c; ls "$HERE"/*.c; } |
    xargs -P "$(nproc)" -I{} sh -c 'o=$(basename {} .c).o; [ "$o" -nt {} ] && [ "$o" -nt "$HERE/classicube.patch" ] && [ "$o" -nt "$HERE/build.sh" ] || { b=$(basename {} .c); f=; case "$HOTFILES" in *" $b "*) f=$HOT;; esac; rv32-cc $CFLAGS $f -c {} -o "$o" && riscv32-linux-objcopy $ALIGN16 "$o"; }'
# Builder.c again for each worker core that makes meshes (SE_COPIES in MapRenderer.h): its own
# buffers, and its four names another's
rm -f Builder_w.o
for k in 1 2 3 4 5 6 7; do
    rv32-cc $CFLAGS $HOT -DSE_SECOND -DBuilder_MakeChunk=Builder_MakeChunk_w$k -DBuilder_ApplyActive=Builder_ApplyActive_w$k \
        -DBuilder_Component=Builder_Component_w$k -DBuilder_Job_w=Builder_Job_w$k -c "$C/src/Builder.c" -o Builder_w$k.o &
done
wait
# the worker cores' library, the same for every program (programs/mc)
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -Wall -c -I"$REPO/programs/mc" "$REPO/programs/mc/mcw.c" -o mcw.o
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -Wall -c -I"$REPO/programs/mc" "$REPO/programs/mc/mcwx.c" -o mcwx.o
# and 16 bytes of nothing after our variables in each kind of section: the libraries' follow
printf 'char se_pad_d[16] __attribute__((aligned(16))) = {1}; char se_pad_b[16] __attribute__((aligned(16)));
' > zz_pad.c
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -msmall-data-limit=0 -c zz_pad.c -o zz_pad_large.o
sed 's/se_pad_/se_pad_s/g' zz_pad.c > zz_pads.c
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -msmall-data-limit=64 -c zz_pads.c -o zz_pad_small.o
for o in Builder_w[1-7].o mcw.o mcwx.o zz_pad_large.o zz_pad_small.o; do riscv32-linux-objcopy $ALIGN16 "$o"; done
rv32-cc -O2 $FPU -w $(ls *.o | grep -v '^zz_') zz_pad_large.o zz_pad_small.o "$MEDIA" "$MW/src/lib/libnano-X.a" -lm -o classicube
mkdir -p "$OUT/usr/bin" "$OUT/usr/share"
riscv32-linux-strip -o "$OUT/usr/bin/classicube.bin" classicube
riscv32-linux-nm -n classicube > "$WORK/src/classicube.nm"

# all in /usr/share: the image builder adds files to folders the ROM has, and makes none.
# The game looks for texpacks/default.zip where it runs and writes its settings and maps there.
# Its zip's entries are stored, and its pictures one pack of PNGs that libmedia decodes on the worker
# cores while the game starts (store_zip.py, png_shaderemu.c).
python3 "$HERE/store_zip.py" "$C/misc/cc_textures.zip" "$OUT/usr/share/classicube-default.zip"
# its dig and step sounds and one tune, Mojang's, fetched once (sound/make_sound.py, docs/games/classicube.md "Sound")
SND=$WORK/src/classicube-sound
mkdir -p "$SND"
[ "$SND/adpcm_encode" -nt "$HERE/sound/adpcm_encode.c" ] || gcc -O2 -o "$SND/adpcm_encode" "$HERE/sound/adpcm_encode.c"
if [ ! "$OUT/usr/share/classicube-sound.pak" -nt "$HERE/sound/make_sound.py" ] || [ ! "$OUT/usr/share/classicube-sound.pak" -nt "$SND/adpcm_encode" ] ||
   [ ! -f "$OUT/usr/share/classicube-music.mp3" ]; then
    python3 "$HERE/sound/make_sound.py" "$OUT/usr/share" "$C/src/Resources.c" "$WORK/src/libmedia/media_test-host" "$SND/adpcm_encode" "$SND/cache"
fi
cat > "$OUT/usr/bin/classicube" <<'EOF'
#!/bin/sh
mkdir -p /tmp/classicube/texpacks /persistence/classicube && cd /tmp/classicube || exit 1
ln -sf /usr/share/classicube-default.zip texpacks/default.zip
# its settings are of the folder the host keeps (docs/system/persist.md); its maps are too large for it
# (the frame rate in the corner is a line of text drawn again every second: 7% of the machine)
[ -f /persistence/classicube/options.txt ] || echo "gui-showfps=false" > /persistence/classicube/options.txt
ln -sf /persistence/classicube/options.txt options.txt
# the player's name, the desktop's (docs/system/persist.md), as the launcher's would be
if [ -n "$USER" ]; then
    grep -v '^launcher-cc-username=' options.txt > /tmp/classicube/options.new
    echo "launcher-cc-username=$USER" >> /tmp/classicube/options.new
    cat /tmp/classicube/options.new > /persistence/classicube/options.txt
fi
[ $# = 0 ] && set -- --singleplayer
exec classicube.bin "$@"
EOF
chmod +x "$OUT/usr/bin/classicube"
echo "Games/ClassiCube=classicube" > "$OUT/usr/share/nxapps.75-classicube"
ls -l "$OUT/usr/bin/classicube.bin"
