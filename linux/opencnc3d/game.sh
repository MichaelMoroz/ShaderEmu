#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# OpenCNC3D's own program for the machine: its app (app/opencnc3d.cpp, the menus and the state machine), its
# renderer (game/cnc_eyes.cpp at tier 1) and their C files, with the brain linked in, over a layer of ours
# for what SDL2 does (sdl_nanox.c) and Half-Life's GL shim (gl_ocnc.c). docs/games/opencnc3d.md, "The game".
# Run by build.sh after the brain (the machine's build only); OpenTTD's C++ compiler, for all of libstdc++.
#   in: $WORK, $HERE, $REPO, $OCNC (OpenCNC3D's tree), $BUILD (the brain's build), $OUT
set -euo pipefail
source "$HERE/../openttd/toolchain.sh"           # ottd-cc, ottd-c++ ($CXXTC)
S=$WORK/src
G=$S/opencnc3d-game-build
MW=$S/microwindows
P=$REPO/programs
DEPS=$S/openttd-deps                              # zlib, as OpenTTD built it (linux/openttd/build.sh)
[ -f "$DEPS/lib/libz.a" ] || { echo "run linux/openttd/build.sh first (zlib)"; exit 1; }
[ -f "$S/curl/lib/.libs/libcurl.a" ] || { echo "run linux/ports/curl.sh first"; exit 1; }
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
mkdir -p "$G/ours" "$G/renamed"

# SDL2's headers only (zlib licence): the types, constants and the GL declarations (SDL_opengl.h) the
# program is written against. None of SDL's code is built: sdl_nanox.c is the part the program calls.
SDLV=2.30.9
SDLSUM=24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4
if [ ! -f "$S/SDL2-$SDLV/include/SDL.h" ]; then
    curl -sSfL "https://github.com/libsdl-org/SDL/releases/download/release-$SDLV/SDL2-$SDLV.tar.gz" -o "$S/SDL2-$SDLV.tar.gz"
    echo "$SDLSUM  $S/SDL2-$SDLV.tar.gz" | sha256sum -c --quiet
    tar -xzf "$S/SDL2-$SDLV.tar.gz" -C "$S" "SDL2-$SDLV/include" "SDL2-$SDLV/LICENSE.txt"
fi
# the version header the build scripts make (tools/version.sh)
sh "$OCNC/tools/version.sh" --header "$G/opencnc3d_build.h.new" > /dev/null
cmp -s "$G/opencnc3d_build.h.new" "$G/opencnc3d_build.h" && rm "$G/opencnc3d_build.h.new" || mv "$G/opencnc3d_build.h.new" "$G/opencnc3d_build.h"

HDR=$TOP/brain/vanilla/tiberiandawn
INC="-I$HERE/compat -I$S/SDL2-$SDLV/include -I$OCNC/game -I$OCNC/menu -I$OCNC/video -I$OCNC/audio -I$OCNC/app -I$HDR \
     -I$DEPS/include -I$G -I$S/curl/include"
CXXF="-std=c++14 -O2 -fms-extensions -fpermissive -w -include $HERE/msvc_gcc.h -include $G/opencnc3d_build.h"
cd "$G"
# (an object is made again when its source, a header of ours or this script is newer)
newer() { [ ! -f "$2" ] || [ "$1" -nt "$2" ] || [ "$HERE/game.sh" -nt "$2" ]; }
cc_one() {
    local std=$1 f=$2 o; o=$(basename "$f" | sed 's/\.[a-z]*$//').o
    newer "$f" "$o" || return 0
    ottd-cc -std="$std" -O2 -w $INC -c "$f" -o "$o" 2> "$o.err" || { cat "$o.err"; exit 1; }
}
# the C files of tools/win/sources.sh, as its build-win.sh compiles them
for f in game/dosbar.c game/hud640.c game/dosopt.c game/dossave.c audio/sosadpcm.c audio/wsadpcm.c audio/wsaud.c \
         audio/mixfile.c audio/sndbank.c audio/mixer.c audio/sfxtable.c audio/sfxname.c audio/cncaudio.c audio/wavio.c \
         audio/audiotap.c audio/audioboot.c net/lockstep.c; do cc_one c99 "$OCNC/$f"; done
for f in audio/audio_sdl.c menu/dosmenu.c menu/dosops.c menu/doslobby.c menu/dosmp.c menu/dosmenu_shell.c menu/mpbrowse.c \
         video/vqaplay.c video/movieplay.c video/moviesnd.c video/pngwrite.c app/campaign.c app/logo3d.c net/net_udp.c \
         net/netmatch.c net/netbeacon.c net/roomcode.c launcher/lnet.c launcher/ljson.c; do cc_one gnu99 "$OCNC/$f"; done
cxx_one() {
    local f=$1 o=$2; shift 2
    # (or our patch, whose part under opencnc3d/ changes their headers)
    newer "$f" "$o" || [ "$HERE/msvc_gcc.h" -nt "$o" ] || [ "$HERE/opencnc3d.patch" -nt "$o" ] || return 0
    ottd-c++ $CXXF $INC "$@" -c "$f" -o "$o" 2> "$o.err" || { grep -E "error" "$o.err" | head -20; exit 1; }
}
cxx_one "$OCNC/menu/enhanced_menu.cpp" enhanced_menu.o
cxx_one "$OCNC/game/cnc_eyes.cpp" cnc_eyes.o -DOPENCNC3D_NO_MAIN
cxx_one "$OCNC/app/opencnc3d.cpp" opencnc3d.o

# ours
ottd-cc -O2 -w -I$S/SDL2-$SDLV/include -I$MW/src/include -c "$HERE/sdl_nanox.c" -o ours/sdl_nanox.o
ottd-cc -O2 -w -c "$HERE/brain_link.c" -o ours/brain_link.o
ottd-cc -O2 -w -DSEGL_FLOAT -DMAX_TEXTURES=64 -idirafter $P/linux/include -I$MW/src/include -I$P/linux -I$REPO/linux/quake3 \
    -c "$REPO/linux/quake3/gpu_shaderemu.c" -o ours/gpu_shaderemu.o
ottd-cc -O2 -w -I$REPO/linux/quake3 -I$P/mc -c "$HERE/gl_ocnc.c" -o ours/gl_ocnc.o
ottd-cc -O2 -fno-common -fdata-sections -w -I$P/mc -c "$P/mc/mcw.c" -o ours/mcw.o
ottd-cc -O2 -fno-common -fdata-sections -w -I$P/mc -c "$P/mc/mcwx.c" -o ours/mcwx.o

# the program's gl calls renamed for gl_ocnc.c (gles.c, under the shim, has glX of its own)
for o in *.o; do
    syms=$(riscv32-linux-nm -u "$o" | awk '$2 ~ /^gl[A-Z]/ {print "--redefine-sym " $2 "=ocnc_" $2}')
    riscv32-linux-objcopy $syms "$o" "renamed/$o"
done
# brain_link.c's names are weak, and a weak reference pulls nothing out of an archive: each the
# brain defines is asked for by name (-u), so its member comes in and dlsym finds it
asks=$(riscv32-linux-nm -g --defined-only "$BUILD/tiberiandawn/TiberianDawn.a" 2>/dev/null |
       awk '$3 ~ /^(CNC_|OpenCNC3D_)/ {print "-Wl,-u," $3}' | sort -u)
echo "   the brain's entry points asked for: $(echo $asks | wc -w)"
ottd-c++ -o opencnc3d $asks renamed/*.o ours/*.o "$BUILD/tiberiandawn/TiberianDawn.a" "$BUILD/common/libcommonr.a" \
    "$BUILD/common/libcommon.a" "$S/curl/lib/.libs/libcurl.a" "$S/curl-deps/lib/libmbedtls.a" "$S/curl-deps/lib/libmbedx509.a" \
    "$S/curl-deps/lib/libmbedcrypto.a" "$DEPS/lib/libz.a" "$MW/src/lib/libnano-X.a" -lm
riscv32-linux-nm -n -C opencnc3d > "$S/opencnc3d.nm"
mkdir -p "$OUT/usr/bin"
riscv32-linux-strip -o "$OUT/usr/bin/opencnc3d" opencnc3d
riscv32-linux-size opencnc3d
ls -l "$OUT/usr/bin/opencnc3d"
