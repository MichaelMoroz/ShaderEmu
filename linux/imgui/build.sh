#!/usr/bin/env bash
# Builds imdemo: Dear ImGui (MIT) over OpenGL on the machine's GPU, a Nano-X client (docs/imgui.md).
#   wsl -- bash /mnt/c/.../linux/imgui/build.sh     (after linux/nanox/build.sh and linux/tdawn/build.sh,
#                                                    which makes the C++ driver)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
IMGUI=$WORK/src/imgui
BUILD=$WORK/src/imgui-build
OUT=$REPO/build/images/linux/root
TAG=v1.91.5
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
[ -x "$WORK/bin/rv32-c++" ] || { echo "run linux/tdawn/build.sh first"; exit 1; }

if [ ! -d "$IMGUI/.git" ]; then
    echo "== Dear ImGui $TAG"
    git clone -q --depth 1 --branch $TAG https://github.com/ocornut/imgui.git "$IMGUI"
fi

# Floats are the machine's instructions. The library is as it comes: no file of its own is changed.
FLAGS="-O2 -fno-pie -fno-math-errno -fno-strict-aliasing -w -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS"
INC="-I$IMGUI -I$HERE -I$MW/src/include -idirafter $REPO/programs/linux/include"
mkdir -p "$BUILD" && cd "$BUILD"
export FLAGS INC BUILD
{ ls "$IMGUI"/imgui*.cpp; ls "$HERE"/*.cpp "$REPO/linux/tdawn/cxxrt.cpp"; } |
    xargs -P "$(nproc)" -I{} sh -c 'o=$(basename {} .cpp).o; [ "$o" -nt {} ] && [ "$o" -nt "$HERE/build.sh" ] || rv32-c++ $FLAGS $INC -c {} -o "$o"'
# the OpenGL library with floats inside and a megabyte a frame: an interface is whole vertices,
# 64 bytes each (the two megabytes left are textures)
rv32-cc -O2 -fno-pie -w -DSEGL_FLOAT -DSET_SIZE=0x100000u -DUNIFORMS_IN=0x30000u -DVERTICES_IN=0x38000u -DMAX_COMMANDS=3072 \
    $INC -c "$REPO/programs/linux/gles.c" -o gles.o
# (linked away from the window system, so that a profile can tell the two apart: tools/pc_profile.py)
rv32-c++ -O2 -Wl,-Ttext-segment=0x2000000 *.o "$MW/src/lib/libnano-X.a" -o imdemo
mkdir -p "$OUT/usr/bin" "$OUT/usr/share"
riscv32-linux-strip -o "$OUT/usr/bin/imdemo" imdemo
riscv32-linux-nm -n imdemo > "$WORK/src/imdemo.nm"
echo "Other/Dear ImGui=imdemo" > "$OUT/usr/share/nxapps.80-imgui"
ls -l "$OUT/usr/bin/imdemo"
