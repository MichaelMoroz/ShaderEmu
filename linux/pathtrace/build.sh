#!/usr/bin/env bash
# Builds nxpath, the mesh viewer whose scenes the worker cores path trace (docs/apps/raytrace.md), its window Dear ImGui's,
# and puts its example meshes into /root/Models of the image.
#   wsl -- bash /mnt/c/.../linux/pathtrace/build.sh        (after linux/imgui/build.sh)
# In the guest: nano-X -p & nxpath
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
IMGUI=$WORK/src/imgui
LIB=$WORK/src/imgui-build        # Dear ImGui and its backend, as linux/imgui built them: for the handles only
BUILD=$WORK/src/pathtrace-build
OUT=${PATHTRACE_OUT:-$REPO/build/images/linux/root}   # (PATHTRACE_OUT: a private image's overlay)
[ -f "$LIB/imgui_panel.o" ] || { echo "run linux/imgui/build.sh first"; exit 1; }
mkdir -p "$BUILD" "$OUT/usr/bin" "$OUT/usr/share/pathtrace" "$OUT/root/Models"
# the example meshes: the Utah teapot as Stanford's course has it, the Stanford rabbit at its
# third size (fetched once), and a knot made here
DATA=$WORK/src/pathtrace-data
mkdir -p "$DATA"
[ -f "$DATA/teapot.obj" ] || curl -sSfL -o "$DATA/teapot.obj" https://graphics.stanford.edu/courses/cs148-10-summer/as3/code/as3/teapot.obj
if [ ! -f "$DATA/bunny.ply" ]; then
    curl -sSfL -o "$DATA/bunny.tar.gz" http://graphics.stanford.edu/pub/3Dscanrep/bunny.tar.gz
    tar -xzf "$DATA/bunny.tar.gz" -C "$DATA" bunny/reconstruction/bun_zipper_res3.ply
    mv "$DATA/bunny/reconstruction/bun_zipper_res3.ply" "$DATA/bunny.ply"
fi
python3 "$HERE/make_knot.py" "$DATA/knot.obj"
cp "$DATA/teapot.obj" "$DATA/bunny.ply" "$DATA/knot.obj" "$OUT/root/Models/"
# the example scenes and the Cornell box's parts (docs/apps/raytrace.md, "Example scenes")
cp "$HERE"/scenes/*.scene "$HERE"/scenes/*.obj "$OUT/usr/share/pathtrace/"
# ImGuizmo, at the commit of Dear ImGui 1.91.5's time
GIZMO=$WORK/src/ImGuizmo
GIZMO_AT=e552f632bbb17a0ebf5a91a22900f6f68bac6545
if [ ! -d "$GIZMO/.git" ]; then
    git clone -q --filter=blob:none --no-checkout https://github.com/CedricGuillemet/ImGuizmo.git "$GIZMO"
fi
[ "$(git -C "$GIZMO" rev-parse HEAD)" = "$GIZMO_AT" ] || git -C "$GIZMO" checkout -q "$GIZMO_AT"
INC="-I$IMGUI -I$REPO/linux/imgui -I$HERE -I$MW/src/include -I$REPO/programs/mc -I$REPO/linux/apps -I$REPO/linux/userland -idirafter $REPO/programs/linux/include"
CXX="-DNDEBUG -fno-pie -fno-math-errno -fsingle-precision-constant -fno-strict-aliasing -w -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -DIMGUI_DISABLE_DEFAULT_FORMAT_FUNCTIONS"
# Three of ImGuizmo's circles take ImGui's own count of sides, not a stated 32 or 64: a stated
# count is a sine and a cosine a side from the C library, which works them out in doubles.
sed -e 's/\(CenterCircleSize, colors\[0\]\), 32);/\1);/' -e 's/\(mRadiusSquareCenter, colors\[0\]\), 64, /\1, 0, /' \
    "$GIZMO/ImGuizmo.cpp" > "$BUILD/ImGuizmo_ours.cpp"
[ "$(grep -c 'CenterCircleSize, colors\[0\]);' "$BUILD/ImGuizmo_ours.cpp")" = 2 ] || { echo "ImGuizmo.cpp: the middle's circles were not found"; exit 1; }
grep -q 'mRadiusSquareCenter, colors\[0\], 0, ' "$BUILD/ImGuizmo_ours.cpp" || { echo "ImGuizmo.cpp: the turn handle's circle was not found"; exit 1; }
# (its own sines and cosines, a ring's, are gizmo.cpp's: in floats)
rv32-c++ -O3 $CXX $INC -I"$GIZMO" -Dsinf=gizmo_sin -Dcosf=gizmo_cos -c "$BUILD/ImGuizmo_ours.cpp" -o "$BUILD/ImGuizmo.o"
rv32-c++ -O3 $CXX $INC -I"$GIZMO" -c "$HERE/gizmo.cpp" -o "$BUILD/gizmo.o"
# the window's menus, panel and line of figures, by Dear ImGui over the view (docs/apps/raytrace.md, "The window")
rv32-c++ -O2 $CXX $INC -c "$HERE/panel.cpp" -o "$BUILD/panel.o"
# the OpenGL library with floats inside and its own frame size (not linux/imgui's megabyte: the
# meshes are drawn where they lie, and the memory is theirs)
rv32-cc -O2 -fno-pie -w -DSEGL_FLOAT $INC -c "$REPO/programs/linux/gles.c" -o "$BUILD/gles.o"
# single-precision constants (the machine has no doubles), and its own square root; nothing
# that changes what a float comes to (the sums are the test)
TUNE="-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops -fira-loop-pressure"
for f in tracer bvh mesh; do
    rv32-cc $TUNE -fno-pie -Wall -fsingle-precision-constant -fno-math-errno $INC -c "$HERE/$f.c" -o "$BUILD/$f.o"
done
rv32-cc -O2 -fno-pie -Wall -Wno-unused-function -fsingle-precision-constant -fno-math-errno $INC -c "$HERE/nxpath.c" -o "$BUILD/nxpath.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/mc/mcw.c" -o "$BUILD/mcw.o"
rv32-c++ $TUNE --param=max-inline-insns-auto=60 --param=inline-unit-growth=80 -flto=auto -w "$BUILD/nxpath.o" "$BUILD/tracer.o" "$BUILD/bvh.o" "$BUILD/mesh.o" \
    "$BUILD/gizmo.o" "$BUILD/panel.o" "$LIB/imgui_panel.o" "$BUILD/ImGuizmo.o" "$BUILD/mcw.o" "$LIB/imgui.o" "$LIB/imgui_draw.o" "$LIB/imgui_tables.o" "$LIB/imgui_widgets.o" \
    "$LIB/imgui_impl_shaderemu.o" "$LIB/cxxrt.o" "$BUILD/gles.o" "$MW/src/lib/libnano-X.a" -lm -o "$BUILD/nxpath"
riscv32-linux-strip -o "$OUT/usr/bin/nxpath" "$BUILD/nxpath"
echo "Other/Path tracer (every core)=nxpath" > "$OUT/usr/share/nxapps.83-pathtrace"
# a mesh anybody opens is shown (docs/system/open.md)
printf '%s\n' 'mesh obj,ply - nxpath %s' 'scene scene - nxpath %s' > "$OUT/usr/share/nxopen.83-pathtrace"
ls -l "$OUT/usr/bin/nxpath" "$OUT/root/Models" "$OUT/usr/share/pathtrace"
