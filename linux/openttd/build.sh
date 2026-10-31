#!/usr/bin/env bash
# Builds OpenTTD 15.3 (GPL-2.0) with its open base sets, OpenGFX, OpenSFX and OpenMSX, for the
# Nano-X desktop (docs/games/openttd.md). Its C++ needs the whole of libstdc++: toolchain.sh's compiler.
#   wsl -- bash /mnt/c/.../linux/openttd/build.sh   (after linux/nanox's build.sh)
#   no MMU: bash linux/nommu/build.sh openttd/build.sh
# In the guest: nano-X -p & openttd
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/toolchain.sh"
MW=$WORK/src/microwindows
VER=15.3
SRC=${OPENTTD_SRC:-$WORK/src/openttd}
BUILD=$WORK/src/openttd-build
HOSTB=$WORK/src/openttd-host
DEPS=$WORK/src/openttd-deps
OUT=${OPENTTD_OUT:-$REPO/build/images/linux/root}	# (OPENTTD_OUT: a private image's overlay)
SETS="opengfx-releases/8.0/opengfx-8.0-all.zip opensfx-releases/1.0.3/opensfx-1.0.3-all.zip openmsx-releases/0.4.2/openmsx-0.4.2-all.zip"
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
T0=$(date +%s); step() { echo "== [$(( $(date +%s) - T0 )) s] $*"; }

if [ ! -d "$SRC/.git" ]; then
    step "OpenTTD $VER"
    git clone -q --depth 1 --branch "$VER" https://github.com/OpenTTD/OpenTTD.git "$SRC"
fi
# upstream's release plus our patch and files, put there again on every build (OPENTTD_TREE=keep:
# the tree as it stands, for writing that patch: git diff > linux/openttd/openttd.patch)
# Only files whose text changes are written, so a header the patch touches does not rebuild everything.
put() { cmp -s "$1" "$2" || cp "$1" "$2"; }
if [ "${OPENTTD_TREE:-}" != keep ]; then
    P=$(mktemp -d); OLD=$SRC/.shaderemu-files
    git -C "$SRC" apply --numstat "$HERE/openttd.patch" | cut -f3 > "$P.files"
    while read -r f; do mkdir -p "$P/$(dirname "$f")"; git -C "$SRC" show "$VER:$f" > "$P/$f"; done < "$P.files"
    (cd "$P" && git apply "$HERE/openttd.patch")
    while read -r f; do put "$P/$f" "$SRC/$f"; done < "$P.files"
    # files patched before and not now go back to upstream's
    for f in $( [ -f "$OLD" ] && grep -vxFf "$P.files" "$OLD" ); do git -C "$SRC" checkout -q "$VER" -- "$f"; done
    mv "$P.files" "$OLD"; rm -rf "$P"
fi
for f in video/shaderemu_v.cpp video/shaderemu_v.h video/shaderemu_gpu.cpp video/shaderemu_gpu.h video/shaderemu_t.cpp sound/shaderemu_s.cpp sound/shaderemu_s.h music/shaderemu_m.cpp music/shaderemu_m.h \
         mixer_shaderemu.cpp; do
    put "$HERE/$(basename "$f")" "$SRC/src/$f"
done

# zlib and liblzma, which saved games are packed with
mkdir -p "$DEPS"
if [ ! -f "$DEPS/lib/libz.a" ]; then
    step "zlib"
    [ -f "$WORK/src/zlib-1.3.1.tar.gz" ] || curl -sSfL https://zlib.net/fossils/zlib-1.3.1.tar.gz -o "$WORK/src/zlib-1.3.1.tar.gz"
    rm -rf "$DEPS/zlib" && mkdir -p "$DEPS/zlib" && tar -xzf "$WORK/src/zlib-1.3.1.tar.gz" -C "$DEPS/zlib" --strip-components=1
    (cd "$DEPS/zlib" && CC=ottd-cc CFLAGS=-O2 ./configure --static --prefix="$DEPS" > /dev/null && make -s -j"$(nproc)" libz.a > /dev/null && make -s install > /dev/null)
fi
if [ ! -f "$DEPS/lib/liblzma.a" ]; then
    step "liblzma"
    curl -sSfL https://github.com/tukaani-project/xz/releases/download/v5.8.1/xz-5.8.1.tar.gz -o "$DEPS/xz.tar.gz"
    rm -rf "$DEPS/xz" && mkdir -p "$DEPS/xz" && tar -xzf "$DEPS/xz.tar.gz" -C "$DEPS/xz" --strip-components=1
    (cd "$DEPS/xz" && ./configure --host=riscv32-linux-musl --prefix="$DEPS" CC=ottd-cc CFLAGS=-O2 --disable-shared --enable-static \
        --disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo --disable-lzma-links --disable-scripts --disable-doc \
        --disable-nls --enable-threads=no > /dev/null && make -s -j"$(nproc)" > /dev/null && make -s install > /dev/null)
fi

# strgen and settingsgen, which the build runs, for this computer
if [ ! -f "$HOSTB/.built-$VER" ] || [ "$HERE/openttd.patch" -nt "$HOSTB/.built-$VER" ]; then
    step "host tools"
    cmake -S "$SRC" -B "$HOSTB" -DOPTION_TOOLS_ONLY=ON -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build "$HOSTB" -j"$(nproc)" > /dev/null
    touch "$HOSTB/.built-$VER"
fi

INC="-I$MW/src/include -I$REPO/linux/userland -idirafter $REPO/programs/linux/include"
ottd-cc -O2 -w $INC -c "$REPO/programs/linux/gles.c" -o "$DEPS/gles.o"
# the worker cores' library (viewports are blitted in bands): a variable a section on 16 bytes
for m in mcw mcwx; do
    ottd-cc -O2 -w -fno-common -fdata-sections -I"$REPO/programs/mc" -c "$REPO/programs/mc/$m.c" -o "$DEPS/$m.o"
    "$CXXTC/bin/riscv32-linux-musl-objcopy" --set-section-alignment .data*=16 --set-section-alignment .sdata*=16         --set-section-alignment .bss*=16 --set-section-alignment .sbss*=16 "$DEPS/$m.o"
done
# a variable a section on 16 bytes in every object of the game (its tick and its drawing run on worker
# cores: docs/games/openttd.md, "The tick on a worker"), and 16 bytes of nothing after them in each kind
ALIGN16="--set-section-alignment .data*=16 --set-section-alignment .sdata*=16 --set-section-alignment .bss*=16 --set-section-alignment .sbss*=16"
cat > "$WORK/bin/ottd-align16.new" <<EOF
#!/usr/bin/env bash
"\$@" || exit \$?
out=; prev=
for a in "\$@"; do [ "\$prev" = -o ] && out=\$a; prev=\$a; done
case "\$out" in *.o) exec "$CXXTC/bin/riscv32-linux-musl-objcopy" $ALIGN16 "\$out" ;; esac
EOF
cmp -s "$WORK/bin/ottd-align16.new" "$WORK/bin/ottd-align16" && rm "$WORK/bin/ottd-align16.new" || mv "$WORK/bin/ottd-align16.new" "$WORK/bin/ottd-align16"
chmod +x "$WORK/bin/ottd-align16"
# the same on what a whole-program link compiles: its pieces reach no launcher, only the assembler (-B)
mkdir -p "$WORK/bin/ottd-as"
cat > "$WORK/bin/ottd-as/as.new" <<EOF
#!/usr/bin/env bash
"$CXXTC/$T/bin/as" "\$@" || exit \$?
out=; prev=
for a in "\$@"; do [ "\$prev" = -o ] && out=\$a; prev=\$a; done
exec "$CXXTC/bin/riscv32-linux-musl-objcopy" $ALIGN16 "\$out"
EOF
cmp -s "$WORK/bin/ottd-as/as.new" "$WORK/bin/ottd-as/as" && rm "$WORK/bin/ottd-as/as.new" || mv "$WORK/bin/ottd-as/as.new" "$WORK/bin/ottd-as/as"
chmod +x "$WORK/bin/ottd-as/as"
# The whole program at once (docs/games/openttd.md, "The compiler"): a partition a source file and nothing reordered, as
# Tiberian Dawn's, whose globals depend on the files' order as OpenTTD's factories may. OPENTTD_LTO=off: file by file
OPT=
[ "${OPENTTD_LTO:-on}" = off ] || OPT="-B$WORK/bin/ottd-as/ -fno-common -fdata-sections -flto=$(nproc) -flto-partition=1to1 -fno-toplevel-reorder --param inline-unit-growth=${OTTD_UG:-200} --param max-inline-insns-auto=${OTTD_AUTO:-60} --param max-inline-insns-single=${OTTD_SINGLE:-300} --param large-function-growth=${OTTD_LFG:-300} --param inline-min-speedup=${OTTD_SPEEDUP:-15}"
# What a played game ran, for the compiler (linux/userland/pgo/gcov.c; docs/games/tdawn.md, "What the compiler is told"):
# OPENTTD_PROFILE=count builds the program that counts, in a folder of its own; otherwise openttd.profile's counts.
GCOV=
if [ "${OPENTTD_PROFILE:-}" = count ]; then
    BUILD=$WORK/src/openttd-count-build
    mkdir -p "$WORK/src/nogcov" "$BUILD" && "$CXXTC/bin/riscv32-linux-musl-ar" rc "$WORK/src/nogcov/libgcov.a"
    # (a compiler without thread-local storage makes the counters' tuple a plain global)
    ottd-cc -O2 -w -D__thread= -DGCOV_ONCE -c "$REPO/linux/userland/pgo/gcov.c" -o "$DEPS/gcov.o"
    OPT="-B$WORK/bin/ottd-as/ -fprofile-generate -fprofile-update=single -L$WORK/src/nogcov"; GCOV="$DEPS/gcov.o"
elif [ -f "$HERE/openttd.profile.gz" ] && [ "${OPENTTD_PROFILE:-}" != off ]; then
    # (6 MB of text: counts.py says how they are made)
    mkdir -p "$BUILD"; gzip -dc "$HERE/openttd.profile.gz" > "$BUILD/openttd.profile"
    python3 "$REPO/linux/tdawn/profile.py" put "$BUILD/openttd.profile" "$BUILD"
    OPT="$OPT -fprofile-use -fprofile-partial-training -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch"
fi
printf 'char se_pad_d[16] __attribute__((aligned(16))) = {1}; char se_pad_b[16] __attribute__((aligned(16)));\n' > "$DEPS/zz_pad.c"
ottd-cc -O2 -fno-common -fdata-sections -msmall-data-limit=0 -c "$DEPS/zz_pad.c" -o "$DEPS/zz_pad_large.o"
sed 's/se_pad_/se_pad_s/g' "$DEPS/zz_pad.c" > "$DEPS/zz_pads.c"
ottd-cc -O2 -fno-common -fdata-sections -msmall-data-limit=64 -c "$DEPS/zz_pads.c" -o "$DEPS/zz_pad_small.o"
for o in zz_pad_large zz_pad_small; do "$CXXTC/bin/riscv32-linux-musl-objcopy" $ALIGN16 "$DEPS/$o.o"; done
# the tick's worker takes its memory from a heap of its own (shaderemu_t.cpp)
WRAP=; for f in malloc free calloc realloc aligned_alloc posix_memalign memalign malloc_usable_size; do WRAP="$WRAP;-Wl,--wrap=$f"; done
cat > "$WORK/src/openttd-toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv32)
set(CMAKE_C_COMPILER $WORK/bin/ottd-cc)
set(CMAKE_CXX_COMPILER $WORK/bin/ottd-c++)
set(CMAKE_AR $CXXTC/bin/riscv32-linux-musl-gcc-ar)
set(CMAKE_RANLIB $CXXTC/bin/riscv32-linux-musl-gcc-ranlib)
set(CMAKE_FIND_ROOT_PATH $DEPS)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$SRC" "$BUILD/CMakeCache.txt" 2>/dev/null && [ "$BUILD/CMakeCache.txt" -nt "$WORK/bin/ottd-c++" ] ||
    rm -rf "$BUILD/CMakeCache.txt" "$BUILD/CMakeFiles"
NONE=
for p in PNG LZO CURL Freetype SDL2 Allegro Fluidsynth Fontconfig Harfbuzz ICU OpusFile unofficial-breakpad OpenGL Grfcodec Doxygen Iconv Xaudio2; do
    NONE="$NONE -DCMAKE_DISABLE_FIND_PACKAGE_$p=ON"
done
step "configure"
cmake -S "$SRC" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$WORK/src/openttd-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DHOST_BINARY_DIR="$HOSTB" -DOPTION_INSTALL_FHS=ON -DCMAKE_INSTALL_PREFIX=/usr -DGLOBAL_DIR=/usr/share/openttd -DSHADEREMU=ON $NONE \
    -DSHADEREMU_INCLUDES="$MW/src/include;$REPO/linux/userland;$REPO/programs/linux/include;$REPO/programs/mc" \
    -DCMAKE_CXX_COMPILER_LAUNCHER="$WORK/bin/ottd-align16" -DCMAKE_C_COMPILER_LAUNCHER="$WORK/bin/ottd-align16" \
    -DCMAKE_CXX_FLAGS="$OPT" -DCMAKE_EXE_LINKER_FLAGS="$GCOV" \
    -DSHADEREMU_LIBS="$DEPS/gles.o;$DEPS/mcw.o;$DEPS/mcwx.o;$DEPS/zz_pad_large.o;$DEPS/zz_pad_small.o;$MW/src/lib/libnano-X.a$WRAP" > "$BUILD.configure.log" 2>&1 || { tail -30 "$BUILD.configure.log"; exit 1; }
step "compile"
cmake --build "$BUILD" -j"$(nproc)" --target openttd > "$BUILD.log" 2>&1 || { grep -E "error|Error" "$BUILD.log" | head -40; exit 1; }
step "install"

# the game, its English texts, its own pictures and scripts' libraries; then the open sets
D=/usr/share/openttd
rm -rf "$OUT$D" && mkdir -p "$OUT/usr/bin" "$OUT$D/lang" "$OUT$D/baseset"
riscv32-linux-musl-strip -o "$OUT/usr/bin/openttd.bin" "$BUILD/openttd" 2>/dev/null || "$CXXTC/bin/riscv32-linux-musl-strip" -o "$OUT/usr/bin/openttd.bin" "$BUILD/openttd"
"$CXXTC/bin/riscv32-linux-musl-nm" -n -C "$BUILD/openttd" > "$WORK/src/openttd.nm"
cp "$BUILD/lang/english.lng" "$OUT$D/lang/"
cp -r "$BUILD/baseset/." "$OUT$D/baseset/"
cp "$HERE/opntitle.dat" "$OUT$D/baseset/"	# (make_title.sh's small title game over upstream's)
cp "$SRC/media/baseset/opntitle.dat" "$OUT$D/busy.sav"	# upstream's, busy: OPENTTD_PLAY's game (docs/games/openttd.md, "Speed")
# no FreeType here, and the original game's sets, which are not open, are never there to describe
rm -f "$OUT$D/baseset/"*.ttf "$OUT$D/baseset/orig_"*.ob?
cp -r "$BUILD/ai" "$BUILD/game" "$OUT$D/"
cp "$SRC/COPYING.md" "$HERE/test.cfg" "$OUT$D/"
for s in $SETS; do
    z=$WORK/src/$(basename "$s")
    [ -f "$z" ] || curl -sSfL "https://cdn.openttd.org/$s" -o "$z"
    rm -rf "$BUILD/set" && mkdir -p "$BUILD/set" && python3 -m zipfile -e "$z" "$BUILD/set"
    # each zip holds one tar, which OpenTTD reads as it is; but a music set is not looked for in a tar
    case $s in
        openmsx*) find "$BUILD/set" -name '*.tar' -exec tar -xf {} -C "$OUT$D/baseset/" \; ;;
        *) find "$BUILD/set" -name '*.tar' -exec cp {} "$OUT$D/baseset/" \; ;;
    esac
done
# the music's instruments: DMXOPL's General MIDI bank for OPL (MIT), which shaderemu_m.cpp plays
GM=$WORK/src/dmxopl-e83c144a.op2
[ -f "$GM" ] || curl -sSfL "https://raw.githubusercontent.com/sneakernets/DMXOPL/e83c144a2e6d3c283e20f9d16d011126931892f5/GENMIDI.op2" -o "$GM"
cp "$GM" "$OUT$D/baseset/genmidi.op2"
cat > "$OUT/usr/bin/openttd" <<'EOF'
#!/bin/sh
# settings and saved games go to the folder the host keeps (docs/system/persist.md), else to /tmp
P=/persistence; mkdir -p $P/openttd 2>/dev/null || P=/tmp
XDG_DATA_HOME=$P XDG_CONFIG_HOME=$P exec openttd.bin "$@"
EOF
chmod 755 "$OUT/usr/bin/openttd"
echo "Games/OpenTTD=openttd" > "$OUT/usr/share/nxapps.73-openttd"
echo "openttd sav OTT openttd -g %s" > "$OUT/usr/share/nxopen.73-openttd"
ls -l "$OUT/usr/bin/openttd.bin"; du -sh "$OUT$D"
step done
