#!/usr/bin/env bash
# Builds nxphys, the rigid boxes that every worker core moves its share of (docs/apps/physics.md),
# for the image, its panel Dear ImGui's.   wsl -- bash /mnt/c/.../linux/physics/build.sh        (after linux/imgui/build.sh)
# In the guest: nano-X -p & nxphys
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
IMGUI=$WORK/src/imgui
LIB=$WORK/src/imgui-build        # Dear ImGui and its backend, as linux/imgui built them
OUT=${PHYS_OUT:-$REPO/build/images/linux/root}   # (PHYS_OUT: a private image's overlay)
BUILD=$WORK/src/physics-build
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
[ -f "$LIB/imgui_panel.o" ] || { echo "run linux/imgui/build.sh first"; exit 1; }
mkdir -p "$BUILD" "$OUT/usr/bin" "$OUT/usr/share"
INC="-idirafter $REPO/programs/linux/include -I$MW/src/include -I$REPO/linux/userland -I$REPO/programs/mc -I$REPO/linux/apps -I$REPO/linux/imgui -I$HERE"
# single-precision constants (the machine has no doubles), and the machine's own square root
FLOATS="-fsingle-precision-constant -fno-math-errno"
# a substep is a hundred floats alive at once: scheduled to keep them in registers, 5% fewer instructions
# (and loops unrolled: 157.9 to 157.5 million instructions of 120 substeps on the first core, the same sum;
# docs/system/flags.md)
OPT="-O3 -fsched-pressure -fno-tree-vectorize -funroll-loops"
rv32-cc $OPT -fno-pie -Wall -Wno-unused-function $FLOATS $INC -c "$HERE/physics.c" -o "$BUILD/physics.o"
rv32-cc $OPT -fno-pie -Wall -Wno-unused-function $FLOATS $INC -c "$HERE/nxphys.c" -o "$BUILD/nxphys.o"
# the OpenGL library with floats: a box's corners are vertices as they are
rv32-cc -O2 -fno-pie -w -DSEGL_FLOAT $INC -c "$REPO/programs/linux/gles.c" -o "$BUILD/gles.o"
# the bar under the picture and the window of the boxes, by Dear ImGui (phys_panel.cpp; docs/apps/physics.md, "The panel")
rv32-c++ -O2 -fno-pie -w -DNDEBUG $FLOATS -fno-strict-aliasing -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS \
    -DIMGUI_DISABLE_DEFAULT_FORMAT_FUNCTIONS -I"$IMGUI" $INC -c "$HERE/phys_panel.cpp" -o "$BUILD/phys_panel.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/mc/mcw.c" -o "$BUILD/mcw.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/mc/mcwx.c" -o "$BUILD/mcwx.o"
rv32-c++ -O2 -no-pie "$BUILD/nxphys.o" "$BUILD/physics.o" "$BUILD/phys_panel.o" "$LIB/imgui_panel.o" "$LIB/imgui.o" "$LIB/imgui_draw.o" \
    "$LIB/imgui_tables.o" "$LIB/imgui_widgets.o" "$LIB/imgui_impl_shaderemu.o" "$LIB/cxxrt.o" "$BUILD/gles.o" "$BUILD/mcwx.o" "$BUILD/mcw.o" \
    "$MW/src/lib/libnano-X.a" -lm -o "$BUILD/nxphys"
riscv32-linux-nm -n "$BUILD/nxphys" > "$WORK/src/nxphys.nm"
riscv32-linux-strip -o "$OUT/usr/bin/nxphys" "$BUILD/nxphys"
echo "Other/Physics (every core)=nxphys" > "$OUT/usr/share/nxapps.87-physics"
ls -l "$OUT/usr/bin/nxphys"
