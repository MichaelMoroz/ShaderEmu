#!/usr/bin/env bash
# Builds Quake (id Software's GPL source, GLQuake, a Nano-X client here) for the image, and
# fetches the shareware episode's data, which is not kept in this repository (docs/quake.md).
#   wsl -- bash /mnt/c/.../linux/quake/build.sh        (after linux/nanox/build.sh)
# In the guest: nano-X -p & quake
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
Q=$WORK/src/quake
BUILD=$WORK/src/quake-build
DATA=$WORK/src/quake-data
OUT=$REPO/build/images/linux/root
COMMIT=bf4ac42
SHAREWARE=${QUAKE_SHAREWARE:-https://ftp.netbsd.org/pub/pkgsrc/distfiles/quake106.zip}
LHASA=https://github.com/fragglet/lhasa/releases/download/v0.4.0/lhasa-0.4.0.tar.gz
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }

if [ ! -d "$Q/.git" ]; then
    echo "== Quake $COMMIT"
    git clone -q https://github.com/id-Software/Quake.git "$Q"
fi
# the tree is id's commit plus our patch, put there again on every build
git -C "$Q" checkout -q -f "$COMMIT" && git -C "$Q" clean -q -fd
git -C "$Q" apply "$HERE/quake.patch"

# GLQuake without the x86 assembly, with the null sound, CD and network drivers, and ours for
# the window, the system and the models' triangles.
SOURCES="cl_demo cl_input cl_main cl_parse cl_tent chase cmd common console crc cvar
    gl_draw gl_model gl_refrag gl_rlight gl_rmain gl_rmisc gl_rsurf gl_screen gl_warp
    host host_cmd keys menu mathlib net_loop net_main net_vcr net_none pr_cmds pr_edict pr_exec r_part sbar
    sv_main sv_phys sv_move sv_user zone view wad world cd_null snd_null"
OURS="vid_shaderemu sys_shaderemu mesh_shaderemu world_shaderemu fmath_shaderemu"
# gles.c in its float build. (Texture names from 3,072 up are qgl.c's own, for models' poses.)
GLES="-DSEGL_FLOAT -DMAX_TEXTURES=4096"
GLINC="-I$HERE/include -idirafter $REPO/programs/linux/include -I$MW/src/include"
# On top of the toolchain's flags (the machine's float instructions, docs/fpu.md): no errno
# from sqrt, which is then one instruction, and a constant written without an f is a float
# too. As a double, as C has it, every "x * 0.5" in the source was a library call for a double
# multiply and two conversions.
FPU="-fno-math-errno -fsingle-precision-constant"
# -fcommon: the source defines some variables in more than one file. -fsigned-char: it was
# written for x86, and reads floats as integers where it pleases (-fno-strict-aliasing). -fno-pie: a use of a global costs a load from a table otherwise.
# fmath.h before every file: the game's sin, cos, tan and atan in floats
# (QUAKE_DEFINES=-DSE_COUNT counts what the server does in a frame, for the quakestat: lines)
CFLAGS="-O2 $FPU -fno-pie -fcommon -fsigned-char -fno-strict-aliasing -w -DGLQUAKE -DSHADEREMU -include $HERE/fmath.h $GLINC -I$Q/WinQuake ${QUAKE_DEFINES:-}"
export CFLAGS Q HERE BUILD
mkdir -p "$BUILD" && cd "$BUILD"
{ for f in $SOURCES; do echo "$Q/WinQuake/$f.c"; done; for f in $OURS; do echo "$HERE/$f.c"; done; } |
    xargs -P "$(nproc)" -I{} sh -c 'o=$(basename {} .c).o; [ "$o" -nt {} ] && [ "$o" -nt "$HERE/quake.patch" ] && [ "$o" -nt "$HERE/include/GL/gl.h" ] && [ "$o" -nt "$HERE/build.sh" ] && [ "$o" -nt "$HERE/fmath.h" ] || rv32-cc $CFLAGS -c {} -o "$o"'
rv32-cc -O2 $FPU -fno-pie -Wall -c $GLINC "$HERE/qgl.c" -o qgl.o
rv32-cc -O2 -fno-pie -Wall -c $GLES $GLINC "$REPO/programs/linux/gles.c" -o gles.o
rv32-cc -O2 $FPU -w *.o "$MW/src/lib/libnano-X.a" -lm -o quake
mkdir -p "$OUT/usr/bin" "$OUT/usr/share"
riscv32-linux-strip -o "$OUT/usr/bin/quake.bin" quake
riscv32-linux-nm -n quake > "$WORK/src/quake.nm"

# The shareware episode: a zip holding an LHA archive, which a small extractor built here opens.
if [ ! -f "$DATA/id1/pak0.pak" ]; then
    echo "== the shareware episode's data"
    mkdir -p "$DATA" && cd "$DATA"
    if [ ! -x lhasa/src/lha ]; then
        curl -sSfL "$LHASA" -o lhasa.tar.gz
        mkdir -p lhasa && tar -xf lhasa.tar.gz -C lhasa --strip-components=1 && rm lhasa.tar.gz
        (cd lhasa && ./configure -q --disable-shared >/dev/null && make -s -j"$(nproc)" >/dev/null 2>&1)
    fi
    [ -f quake106.zip ] || curl -sSfL "$SHAREWARE" -o quake106.zip
    python3 -c "import zipfile; open('resource.1', 'wb').write(zipfile.ZipFile('quake106.zip').read('resource.1'))"
    lhasa/src/lha xq resource.1 id1/pak0.pak && rm resource.1
fi
[ "$(md5sum < "$DATA/id1/pak0.pak" | cut -d' ' -f1)" = 5906e5998fc3d896ddaf5e6a62e03abb ] || { echo "pak0.pak is not the 1.06 shareware one"; exit 1; }
# all in /usr/share: the image builder adds files to folders the ROM has, and makes none.
# The game wants its data in a folder id1 and writes its settings there: the starter makes one.
cp "$DATA/id1/pak0.pak" "$OUT/usr/share/quake-pak0.pak"
cat > "$OUT/usr/bin/quake" <<'EOF'
#!/bin/sh
mkdir -p /tmp/quake/id1 && cd /tmp/quake || exit 1
ln -sf /usr/share/quake-pak0.pak id1/pak0.pak
exec quake.bin "$@"
EOF
chmod +x "$OUT/usr/bin/quake"
echo "Quake=quake" > "$OUT/usr/share/nxapps.70-quake"
ls -l "$OUT/usr/bin/quake.bin" "$OUT/usr/share/quake-pak0.pak"
