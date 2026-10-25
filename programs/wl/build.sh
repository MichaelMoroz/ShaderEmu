#!/usr/bin/env bash
# wl for both machines, each with the compiler its games are built with (programs/wl/README.md). In WSL:
#   bash programs/wl/build.sh            -> build/wl/rv/wl (RISC-V, a flat program), build/wl/gvm/wl (gpu_vm, bFLT)
#   bash programs/wl/build.sh host       -> build/wl/host/wl (this computer, for trying a kernel)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=$REPO/build/wl
SRC=("$HERE"/wl.c "$HERE"/k_*.c "$REPO/programs/mc/mcwx.c")
# libmedia as each image has it (k_media.c): the RISC-V machine's prebuilt one, gpu_vm's from its apps' build
RV_MEDIA=$REPO/linux/prebuilt/nommu/root/usr/lib/libmedia.a
GVM_MEDIA=$REPO/build/gvm_apps/root/usr/lib/libmedia.a
# as the games: single-precision constants, no errno from the math (nxray's flags)
FLAGS="-O2 -fsingle-precision-constant -fno-math-errno -w -I$HERE -I$REPO/programs/mc -I$REPO/linux/libmedia"
what=${1:-all}
mkdir -p "$OUT/rv" "$OUT/gvm" "$OUT/host"
if [ "$what" = host ]; then
    # (no libmedia here: its kernels are left out)
    gcc -O2 -Wall -DWL_HOST -DWL_NO_MEDIA -I"$HERE" "$HERE"/wl.c $(ls "$HERE"/k_*.c | grep -v k_media.c) -lm -o "$OUT/host/wl"
    exit 0
fi
# the RISC-V machine: the no-MMU build's rv32-cc (GCC 13.3, PIE), made flat as linux/nommu/flat.py does
(
    export PATH="$HOME/nm/work/bin:$HOME/shaderemu-linux/toolchain/bin:$PATH"
    rv32-cc $FLAGS "${SRC[@]}" "$REPO/programs/mc/mcw.c" "$RV_MEDIA" -lm -o "$OUT/rv/wl.elf"
    riscv32-linux-strip -o "$OUT/rv/wl.strip" "$OUT/rv/wl.elf"
    python3 "$REPO/linux/nommu/elf2bflt.py" "$OUT/rv/wl.strip" "$OUT/rv/wl" 0x40000 | grep -v '^$' || true
)
# gpu_vm: the apps' toolchain (gpu_vm/userland/apps/toolchain.sh: clang for gpuvm, its math flags)
(
    source "$REPO/gpu_vm/userland/apps/toolchain.sh"
    rv32-cc $FLAGS "${SRC[@]}" "$REPO/programs/mc/mcw.c" "$GVM_MEDIA" -lm -o "$OUT/gvm/wl"
)
ls -l "$OUT/rv/wl" "$OUT/gvm/wl"
