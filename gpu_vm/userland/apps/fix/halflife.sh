#!/usr/bin/env bash
# Half-Life for gpu_vm (gpu_vm/userland/apps/build.sh runs this on its copy, $R): Xash3D's library_suffix (a
# submodule, in no patch) names gpu_vm among its architectures as the RISC-V build's own (floats in integer
# registers: the same library names), and git leaves the submodule's change out of the tree's comparison.
set -euo pipefail
B="$R/linux/halflife/build.sh"
grep -q 'gpu_vm/userland/apps/fix/halflife.sh' "$B" && exit 0
sed -i '/^git -C "\$H" submodule update -q --init freevgui$/a\
# (gpu_vm/userland/apps/fix/halflife.sh: library_suffix knows no gpu_vm)\
git -C "$X" config submodule.3rdparty/library_suffix.ignore dirty\
LS=$X/3rdparty/library_suffix/include/build.h\
grep -q __gpuvm__ "$LS" || sed -i "s/^#elif defined __riscv\\$/#elif defined __gpuvm__\\n\\t#define XASH_RISCV 1\\n\\t#define XASH_RISCV_SOFTFP 1\\n&/" "$LS"\
grep -q __gpuvm__ "$LS" || { echo "library_suffix: no __riscv line"; exit 1; }' "$B"
grep -q 'gpu_vm/userland/apps/fix/halflife.sh' "$B" || { echo "fix/halflife.sh: build.sh's freevgui line not found"; exit 1; }
# the stack the RISC-V image gives it (linux/nommu/flat.py's BIG: 1 MB; gpuvm-linux-cc's default 64 KB ran into the
# heap's metadata after the bss: a return address overwritten by mallocng, a jump into the heap)
sed -i 's|^    "\$MEDIA/libmedia.a" mcw.o mcwx.o "\$MW/src/lib/libnano-X.a" -lm -o halflife \\$|    "$MEDIA/libmedia.a" mcw.o mcwx.o "$MW/src/lib/libnano-X.a" -lm -o halflife -Wl,-z,stack-size=1048576 \\|' "$B"
grep -q 'stack-size=1048576' "$B" || { echo "fix/halflife.sh: build.sh's link line not found"; exit 1; }
