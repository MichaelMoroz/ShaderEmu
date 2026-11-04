#!/usr/bin/env bash
# Quake III as ioquake3's own OpenGL renderer draws it, on Mesa (llvmpipe), on the build computer: a
# reference picture of a view to hold the machine's against (docs/games/quake3.md, "Checking it").
#
#   bash linux/quake3/mesa_reference.sh build
#   bash linux/quake3/mesa_reference.sh shot OUT.tga "+WORDS BEFORE THE MAP" "+WORDS AFTER IT"
#
# `build` compiles ioquake3 at linux/quake3/build.sh's commit, as it is upstream (no patch), with the
# GL1 renderer linked in, the game's three modules as libraries and nothing else (no sound library,
# no HTTP); no -dev package is wanted: SDL's headers are the ones ioquake3 bundles and the program links
# the installed libSDL2-2.0.so.0. `shot` loads q3dm17 with the demo's own pak (the one bake.py reads)
# after the first words (`+set cg_thirdperson 1`), then runs the second (`+wait 30 +setviewpos 192 -380
# 40 90`), waits, writes a screenshot and quits. A command that starts with + itself (`+attack`,
# `+lookdown`) cannot be a word here: put it in a file under $QUAKE3_REF/home/demoq3 and `+exec` that.
# It needs an X display (WSLg's) and draws with LIBGL_ALWAYS_SOFTWARE=1.
#
#   QUAKE3_REF=DIR     where the tree, the build and the game's folders go ($WORK/src/ioq3-mesa)
#   QUAKE3_DATA=DIR    where build.sh put the demo ($WORK/src/quake3-data)
#   QUAKE3_SRC=DIR     a clone of ioquake3 to take the commit from ($WORK/src/ioq3; else GitHub)
#   QUAKE3_WAIT=N      frames between the second words and the shot (150)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HOME/nm/work}
REF=${QUAKE3_REF:-$WORK/src/ioq3-mesa}
DATA=${QUAKE3_DATA:-$WORK/src/quake3-data}
SRC=${QUAKE3_SRC:-$WORK/src/ioq3}
COMMIT=$(sed -n 's/^COMMIT=//p' "$HERE/build.sh")
PAK=$DATA/demo/demoq3/pak0.pk3

build() {
    if [ ! -d "$REF/src/.git" ]; then
        mkdir -p "$REF"
        if [ -d "$SRC/.git" ]; then git clone -q "$SRC" "$REF/src"; else git clone -q https://github.com/ioquake/ioq3.git "$REF/src"; fi
    fi
    git -C "$REF/src" checkout -q -f "$COMMIT"
    local sdl lib
    sdl=$(ls -d "$REF/src/code/thirdparty"/SDL2-*/include | head -1)
    lib=$(ldconfig -p | sed -n 's/.*libSDL2-2\.0\.so\.0 (libc6,x86-64) => //p' | head -1)
    [ -n "$lib" ] || { echo "no libSDL2-2.0.so.0 installed"; exit 1; }
    mkdir -p "$REF/sdl2"
    # (find_package(SDL2) without the -dev package: its answer written by hand)
    cat > "$REF/sdl2/SDL2Config.cmake" <<EOF
set(SDL2_FOUND TRUE)
set(SDL2_INCLUDE_DIRS "$sdl")
set(SDL2_LIBRARIES "$lib")
EOF
    cmake -S "$REF/src" -B "$REF/build" -DCMAKE_BUILD_TYPE=Release -DSDL2_DIR="$REF/sdl2" -DUSE_INTERNAL_SDL=OFF \
        -DBUILD_SERVER=OFF -DBUILD_RENDERER_GL2=OFF -DUSE_RENDERER_DLOPEN=OFF -DBUILD_GAME_QVMS=OFF \
        -DUSE_OPENAL=OFF -DUSE_HTTP=OFF -DUSE_CODEC_VORBIS=OFF -DUSE_CODEC_OPUS=OFF -DUSE_VOIP=OFF -DUSE_MUMBLE=OFF > "$REF/cmake.log"
    cmake --build "$REF/build" -j "${JOBS:-8}" > "$REF/build.log" 2>&1 || { tail -30 "$REF/build.log"; exit 1; }
    find "$REF/build" -maxdepth 2 -name 'ioquake3*' -type f -perm -u+x
}

shot() {
    local out=$1 before=${2:-} after=${3:-}
    local exe base
    exe=$(find "$REF/build" -maxdepth 2 -name 'ioquake3*' -type f -perm -u+x | head -1)
    [ -n "$exe" ] || { echo "run: $0 build"; exit 1; }
    base=$REF/base
    # (the demo's pak as the demo has it, in demoq3: ioquake3 takes a game of its own as standalone, with no pak checks)
    mkdir -p "$base/demoq3" "$REF/home"
    ln -sf "$PAK" "$base/demoq3/pak0.pk3"
    for m in cgame qagame ui; do
        find "$REF/build" -name "${m}*.so" -exec cp {} "$base/demoq3/" \;
    done
    rm -f "$REF/home/demoq3/screenshots/"shot*.tga
    LIBGL_ALWAYS_SOFTWARE=1 SDL_AUDIODRIVER=dummy timeout "${QUAKE3_TIMEOUT:-55}" "$exe" \
        +set fs_basepath "$base" +set fs_homepath "$REF/home" +set com_basegame demoq3 +set vm_cgame 0 +set vm_game 0 +set vm_ui 0 \
        +set sv_pure 0 +set s_initsound 0 +set r_mode -1 +set r_customwidth 640 +set r_customheight 480 \
        +set r_fullscreen 0 +set in_nograb 1 +set com_introplayed 1 +set cl_motd 0 +set r_picmip 0 \
        $before +devmap q3dm17 $after +wait "${QUAKE3_WAIT:-150}" +screenshot +wait 5 +quit \
        > "$REF/shot.log" 2>&1 || true
    local got
    got=$(ls "$REF/home/demoq3/screenshots/"shot*.tga 2>/dev/null | head -1)
    [ -n "$got" ] || { tail -20 "$REF/shot.log"; echo "no screenshot"; exit 1; }
    cp "$got" "$out"
    echo "$out"
}

case "${1:-}" in
    build) build ;;
    shot) shift; shot "$@" ;;
    *) sed -n '2,20p' "$0"; exit 1 ;;
esac
