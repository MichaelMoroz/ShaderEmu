#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# OpenCNC3D's brain (EA's GPL Tiberian Dawn logic, Vanilla Conquer ce83b59 with OpenCNC3D's patch) as one flat
# program for the machine, driven through its CNC_* interface by a host of ours with no picture
# (docs/games/opencnc3d.md, docs/plans/opencnc3d-plan.md step 1). Not in linux/nommu/build.sh's ALL: nothing of it
# goes into the shared image, and it downloads only OpenCNC3D's and Vanilla Conquer's sources.
#   bash linux/nommu/build.sh opencnc3d/build.sh       the machine's program, into OPENCNC3D_OUT (build/opencnc3d/out)
#   OPENCNC3D_HOST=1 bash linux/opencnc3d/build.sh      the build computer's own program, for the sums
#                                                       (WSL; OPENCNC3D_WORK, ~/ocnc-host)
#   OPENCNC3D_TREE=keep    the Vanilla Conquer tree as it stands, for writing our patch:
#                          (cd $WORK/src/vanilla-conquer-ocnc && git diff) > linux/opencnc3d/opencnc3d.patch
#                          (its index holds upstream with OpenCNC3D's patch: the diff is ours alone)
# The cartridge and the DOS data are not here: linux/opencnc3d/bake.py and data.py, into a cache outside the tree.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OCNC_COMMIT=248233af994ef331fb9cba596a8f84f172dbcf98     # OpenCNC3D v0.9.0
VC_COMMIT=ce83b59edd99cccac6cebaae054ca04962c744b7        # its brain/patches/UPSTREAM.txt; linux/tdawn's too
T0=$(date +%s); step() { echo "== [$(( $(date +%s) - T0 )) s] $*"; }

if [ "${OPENCNC3D_HOST:-}" = 1 ]; then
    WORK=${OPENCNC3D_WORK:-$HOME/ocnc-host}
    mkdir -p "$WORK/src"
    CC=gcc; CXX=g++; AR=ar; STRIP=strip
    # the machine's float rules (no fused multiply-add), so that the game computes the same
    OPT="-O2 -g -ffp-contract=off"
    BUILD=$WORK/build
else
    source "$HERE/../userland/toolchain.sh"      # $WORK, rv32-cc; rv32-c++ is linux/tdawn/build.sh's
    [ -x "$WORK/bin/rv32-c++" ] || { echo "run linux/tdawn/build.sh first (it makes rv32-c++)"; exit 1; }
    CC=rv32-cc; CXX=rv32-c++; AR=riscv32-linux-ar; STRIP=riscv32-linux-strip
    OPT="-O2"
    BUILD=$WORK/src/opencnc3d-build
fi
OCNC=$WORK/src/opencnc3d
VC=$WORK/src/vanilla-conquer-ocnc
# The engine is built where OpenCNC3D keeps it, brain/vanilla, because its patch includes two
# of OpenCNC3D's headers by relative path (../../fair_ai.h, ../../../game/sidebar_build_potential.h).
TOP=$WORK/src/opencnc3d-brain
SRC=$TOP/brain/vanilla

# OpenCNC3D at its commit, only the folders used here (its whole tree is a gigabyte)
if [ ! -d "$OCNC/.git" ]; then
    step "OpenCNC3D ${OCNC_COMMIT:0:8}"
    git clone -q --filter=blob:none --no-checkout https://github.com/freschism3000/OpenCNC3D "$OCNC"
fi
git -C "$OCNC" sparse-checkout init --no-cone
git -C "$OCNC" sparse-checkout set '/brain/patches/' '/brain/host/' '/brain/README.md' '/brain/fair_ai.h' \
    '/game/sidebar_build_potential.h' '/tools/romdump/' \
    '/tools/bakery/' '/game/bake*.py' '/game/missions/' '/game/*.json' '/data/autoheight/' \
    '/tools/heightmap-viewer/*.py' '/LICENSE' '/NOTICE.md' '/tools/bin_to_n64map.py' '/docs/re-findings.md'
git -C "$OCNC" checkout -q "$OCNC_COMMIT"

if [ ! -d "$VC/.git" ]; then
    step "Vanilla Conquer ${VC_COMMIT:0:7}"
    git clone -q https://github.com/TheAssemblyArmada/Vanilla-Conquer.git "$VC"
fi
# The tree is upstream's commit, OpenCNC3D's brain patch and its two copied files (as its
# tools/check-brain-checkout.sh makes it), then ours; put there again on every build.
if [ "${OPENCNC3D_TREE:-}" != keep ]; then
    git -C "$VC" checkout -q -f "$VC_COMMIT" && git -C "$VC" clean -q -fdx
    git -C "$VC" apply "$OCNC/brain/patches/vanilla-opencnc3d.patch"
    cp "$OCNC/brain/patches/wwkeyboard_null.cpp" "$VC/common/"
    cp "$OCNC/brain/patches/opencnc3d_compat.h" "$VC/tiberiandawn/"
    git -C "$VC" add -A
    [ ! -s "$HERE/opencnc3d.patch" ] || git -C "$VC" apply "$HERE/opencnc3d.patch"
fi
cp "$HERE/brain_shaderemu.cpp" "$VC/tiberiandawn/"
# built from a copy in which a file keeps its time unless its bytes change
mkdir -p "$SRC" "$TOP/game"
rsync -rc --delete --exclude .git "$VC/" "$SRC/"
rsync -c "$OCNC/brain/fair_ai.h" "$TOP/brain/"
rsync -c "$OCNC/game/sidebar_build_potential.h" "$TOP/game/"

step "the brain ($CXX)"
mkdir -p "$BUILD"
if [ -z "${OPENCNC3D_HOST:-}" ]; then
    cat > "$BUILD/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv32)
set(CMAKE_C_COMPILER $WORK/bin/rv32-cc)
set(CMAKE_CXX_COMPILER $WORK/bin/rv32-c++)
set(CMAKE_AR $(command -v riscv32-linux-ar))
set(CMAKE_RANLIB $(command -v riscv32-linux-ranlib))
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
EOF
    TC="-DCMAKE_TOOLCHAIN_FILE=$BUILD/toolchain.cmake"
else
    TC=""
fi
# -fno-lifetime-dse: the game's operator new marks an object active before its constructor runs
# (docs/games/tdawn.md); -fms-extensions, -fpermissive and msvc_gcc.h: dllinterface.h's MSVC forms
# (OpenCNC3D's brain/README.md, "The four portability patches", made for Clang)
MSVC="-fno-lifetime-dse -fms-extensions -fpermissive -include $HERE/msvc_gcc.h"
cmake -S "$SRC" -B "$BUILD" $TC -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_REMASTERTD=ON -DBUILD_VANILLATD=OFF -DBUILD_VANILLARA=OFF -DBUILD_REMASTERRA=OFF \
    -DSDL2=OFF -DOPENAL=OFF -DNETWORKING=OFF -DOPENCNC3D_STATIC=ON \
    -DCMAKE_CXX_FLAGS="$OPT $MSVC -w ${OPENCNC3D_XFLAGS:-}" -DCMAKE_CXX_FLAGS_RELEASE="-DNDEBUG" \
    -DCMAKE_C_FLAGS="$OPT -w" -DCMAKE_C_FLAGS_RELEASE="-DNDEBUG" > "$BUILD/configure.log" 2>&1 \
    || { tail -30 "$BUILD/configure.log"; exit 1; }
cmake --build "$BUILD" --target TiberianDawn -j"$(nproc)" > "$BUILD/build.log" 2>&1 \
    || { grep -E "error|undefined reference" "$BUILD/build.log" | head -40; exit 1; }

step "the host"
INC="-I$SRC -I$SRC/tiberiandawn -I$SRC/common"
DEFS="-DTIBERIAN_DAWN -DMEGAMAPS -DREMASTER_BUILD -D_USRDLL -DEIGHTPLAYERS"
LIBS="$BUILD/tiberiandawn/TiberianDawn.a $BUILD/common/libcommonr.a $BUILD/common/libcommon.a"
if [ -n "${OPENCNC3D_HOST:-}" ]; then
    $CXX $OPT $MSVC -w $DEFS $INC "$HERE/host.cpp" $LIBS -o "$WORK/opencnc3d-brain"
    ls -l "$WORK/opencnc3d-brain"
else
    $CXX $OPT $MSVC -w $DEFS $INC -c "$HERE/host.cpp" -o "$BUILD/host.o"
    $CXX $OPT -w -c "$REPO/linux/tdawn/cxxrt.cpp" -o "$BUILD/cxxrt.o"
    $CXX "$BUILD/host.o" "$BUILD/cxxrt.o" $LIBS -o "$BUILD/opencnc3d-brain"
    riscv32-linux-nm -n "$BUILD/opencnc3d-brain" > "$WORK/src/opencnc3d-brain.nm"
    OUT=${OPENCNC3D_OUT:-$REPO/build/opencnc3d/out}
    mkdir -p "$OUT/usr/bin"
    $STRIP -o "$OUT/usr/bin/opencnc3d-brain" "$BUILD/opencnc3d-brain"
    ls -l "$OUT/usr/bin/opencnc3d-brain"
fi
