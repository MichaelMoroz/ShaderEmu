#!/usr/bin/env bash
# Builds Quake (id Software's GPL source, GLQuake, a Nano-X client here) for the image, and
# fetches the shareware episode's data, which is not kept in this repository (docs/games/quake.md).
#   wsl -- bash /mnt/c/.../linux/quake/build.sh        (after linux/nanox/build.sh)
# In the guest: nano-X -p & quake
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=${SE_MW:-$WORK/src/microwindows}
Q=${QUAKE_SRC:-$WORK/src/quake}	# (QUAKE_SRC: a tree of ones own, for a build beside another session's)
BUILD=${QUAKE_BUILD:-$WORK/src/quake-build}
DATA=$WORK/src/quake-data
OUT=${QUAKE_OUT:-$REPO/build/images/linux/root}
# (SE_PROGRAMS: a copy of programs/ to build against, for a comparison that keeps its libraries)
PROGRAMS=${SE_PROGRAMS:-$REPO/programs}
COMMIT=bf4ac42
SHAREWARE=${QUAKE_SHAREWARE:-https://ftp.netbsd.org/pub/pkgsrc/distfiles/quake106.zip}
LHASA=https://github.com/fragglet/lhasa/releases/download/v0.4.0/lhasa-0.4.0.tar.gz
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }

if [ ! -d "$Q/.git" ]; then
    echo "== Quake $COMMIT"
    git clone -q https://github.com/id-Software/Quake.git "$Q"
fi
# the tree is id's commit plus our patch, put there again on every build
# (QUAKE_TREE=keep: the tree as it stands, for writing that patch)
if [ "${QUAKE_TREE:-}" != keep ]; then
    git -C "$Q" checkout -q -f "$COMMIT" && git -C "$Q" clean -q -fd
    git -C "$Q" apply "$HERE/quake.patch"
fi

# GLQuake without the x86 assembly, with the null CD driver, the game's own network drivers
# (the loopback and UDP: docs/system/lan.md), and ours for
# the window, the system and the models' triangles.
SOURCES="cl_demo cl_input cl_main cl_parse cl_tent chase cmd common console crc cvar
    gl_draw gl_model gl_refrag gl_rlight gl_rmain gl_rmisc gl_rsurf gl_screen gl_warp
    host host_cmd keys menu mathlib net_loop net_main net_vcr net_bsd net_dgrm net_udp pr_cmds pr_edict pr_exec r_part sbar
    sv_main sv_phys sv_move sv_user zone view wad world cd_null"
OURS="scene_shaderemu vid_shaderemu sys_shaderemu mesh_shaderemu world_shaderemu fmath_shaderemu snd_shaderemu server_shaderemu"
# gles.c in its float build. (Texture names from 3,072 up are qgl.c's own, for models' poses.)
GLES="-DSEGL_FLOAT -DMAX_TEXTURES=4096"
GLINC="-I$HERE/include -idirafter $PROGRAMS/linux/include -I$MW/src/include"
# On top of the toolchain's flags (the machine's float instructions, docs/machine/fpu.md): no errno
# from sqrt, which is then one instruction, and a constant written without an f is a float
# too. As a double, as C has it, every "x * 0.5" in the source was a library call for a double
# multiply and two conversions.
FPU="-fno-math-errno -fsingle-precision-constant"
# -fsigned-char: it was written for x86, and reads floats as integers where it pleases
# (-fno-strict-aliasing).
# -fno-common -fdata-sections: a variable is a section of its own, which ALIGN16 below starts on
# 16 bytes. The server's frame runs on a worker core (server_shaderemu.c, docs/machine/multicore.md),
# and two cores must not store to the same 16 bytes: no variable of the server's shares them
# with one of the client's then. -fno-pie: a use of a global costs a load from a table otherwise.
# fmath.h before every file: the game's sin, cos, tan and atan in floats
# (QUAKE_DEFINES=-DSE_COUNT counts what the server does in a frame, for the quakestat: lines)
# -O3 without its vectors (the machine has none), with loops unrolled: 6% fewer instructions a
# frame of a timedemo than -O2 and 5% fewer to a level's first picture, for 190 KB of program.
# GCC inlining more (gpu_vm inlines 3 times its compiler's threshold): fewer calls, fewer prologue saves, which are stores
# (docs/plans/riscv-backport-plan.md, 2b). On rvc_cpu Quake's track 10 to 1200 2.1% fewer instructions on the first core,
# Quake III's 600 frames 1.9% fewer on all cores and 2.4% less guest time; Doom and the NES gained nothing.
OPT="-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops --param max-inline-insns-auto=60 --param max-inline-insns-single=400 --param inline-unit-growth=80"
CFLAGS="$OPT $FPU -fno-pie -fno-common -fdata-sections -fsigned-char -fno-strict-aliasing -w -DGLQUAKE -DSHADEREMU -include $HERE/fmath.h $GLINC -I$Q/WinQuake -I$PROGRAMS/mc ${QUAKE_DEFINES:-}"
ALIGN16="--set-section-alignment .data*=16 --set-section-alignment .sdata*=16 --set-section-alignment .bss*=16 --set-section-alignment .sbss*=16"
export CFLAGS Q HERE BUILD ALIGN16
mkdir -p "$BUILD" && cd "$BUILD"
rm -f net_none.o   # (of a build before the network: the link takes every object here)
{ for f in $SOURCES; do echo "$Q/WinQuake/$f.c"; done; for f in $OURS; do echo "$HERE/$f.c"; done; } |
    xargs -P "$(nproc)" -I{} sh -c 'o=$(basename {} .c).o; [ "$o" -nt {} ] && [ "$o" -nt "$HERE/quake.patch" ] && [ "$o" -nt "$HERE/include/GL/gl.h" ] && [ "$o" -nt "$HERE/build.sh" ] && [ "$o" -nt "$HERE/fmath.h" ] || { rv32-cc $CFLAGS -c {} -o "$o" && riscv32-linux-objcopy $ALIGN16 "$o"; }'
rv32-cc $OPT $FPU -fno-pie -fno-common -fdata-sections -Wall -c $GLINC "$HERE/qgl.c" -o qgl.o
# gles.c with what a thing drawn again asks of it (gpu_shaderemu.c includes it)
rv32-cc $OPT -fno-pie -fno-common -fdata-sections -Wall -c $GLES $GLINC -I"$PROGRAMS/linux" "$HERE/gpu_shaderemu.c" -o gles.o
# the worker cores' library and its second layer, the same for every program (programs/mc)
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -Wall -c -I"$PROGRAMS/mc" "$PROGRAMS/mc/mcw.c" -o mcw.o
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -Wall -c -I"$PROGRAMS/mc" "$PROGRAMS/mc/mcwx.c" -o mcwx.o
# and 16 bytes of nothing after our variables in each kind of section: the libraries' follow
printf 'char se_pad_d[16] __attribute__((aligned(16))) = {1}; char se_pad_b[16] __attribute__((aligned(16)));\n' > zz_pad.c
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -msmall-data-limit=0 -c zz_pad.c -o zz_pad_large.o
sed 's/se_pad_/se_pad_s/g' zz_pad.c > zz_pads.c
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -msmall-data-limit=64 -c zz_pads.c -o zz_pad_small.o
for o in qgl.o gles.o mcw.o mcwx.o zz_pad_large.o zz_pad_small.o; do riscv32-linux-objcopy $ALIGN16 "$o"; done
rv32-cc -O2 $FPU -w $(ls *.o | grep -v '^zz_') zz_pad_large.o zz_pad_small.o "$MW/src/lib/libnano-X.a" -lm -o quake
mkdir -p "$OUT/usr/bin" "$OUT/usr/share"
riscv32-linux-strip -o "$OUT/usr/bin/quake.bin" quake
riscv32-linux-nm -n quake > "${QUAKE_NM:-$WORK/src/quake.nm}"

# The shareware episode: a zip holding an LHA archive, which a small extractor built here opens.
if [ ! -f "$DATA/id1/pak0.pak" ]; then
    echo "== the shareware episode's data"
    mkdir -p "$DATA" && cd "$DATA"
    if [ ! -x lhasa/src/lha ]; then
        curl -sSfL "$LHASA" -o lhasa.tar.gz
        mkdir -p lhasa && tar -xf lhasa.tar.gz -C lhasa --strip-components=1 && rm lhasa.tar.gz
        # (without the flags exported above for the game: they are another machine's, and configure
        # then says it "cannot run C compiled programs")
        (cd lhasa && env -u CFLAGS ./configure -q --disable-shared >/dev/null && env -u CFLAGS make -s -j"$(nproc)" >/dev/null 2>&1)
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
# id1, with the settings and saved games, is of the folder the host keeps (docs/system/persist.md)
mkdir -p /persistence/quake /tmp/quake && cd /tmp/quake || exit 1
ln -sfn /persistence/quake id1
ln -sf /usr/share/quake-pak0.pak id1/pak0.pak
# (the player's name, the desktop's: docs/system/persist.md)
exec quake.bin +name "${USER:-player}" "$@"
EOF
chmod +x "$OUT/usr/bin/quake"
# (from the menu Quake opens with its own menu up, over the demos it plays)
echo "Games/Quake=quake +menu_main" > "$OUT/usr/share/nxapps.70-quake"
ls -l "$OUT/usr/bin/quake.bin" "$OUT/usr/share/quake-pak0.pak"
