#!/usr/bin/env bash
# OpenTTD for gpu_vm (docs/games/openttd.md): linux/openttd/build.sh, run in build.sh's copy of the repository with
# that copy's linux/openttd/toolchain.sh (a GCC of its own for the RISC-V machine) replaced by gpu_vm's: ottd-cc and
# ottd-c++ over gpuvm-linux-cc and its C++ mode (libc++, libc++abi and libunwind: gpu_vm/userland/cxx.sh), and the
# binutils' names it calls over LLVM's. Run by build.sh as ../gpu_vm/userland/apps/openttd.sh.
# The game's state must be the build computer's (linux/openttd/host_sum.sh): exact math (GVM_MATH empty), not the
# card's special functions.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
cat > "$REPO/linux/openttd/toolchain.sh" <<'EOF'
#!/usr/bin/env bash
# gpu_vm's (gpu_vm/userland/apps/openttd.sh): the apps' toolchain, and ottd-cc / ottd-c++ over it
export GVM_MATH=
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../userland/toolchain.sh"
[ -f "$SYSROOT/lib/libc++.a" ] || { echo "no C++ runtime in $SYSROOT (gpu_vm/userland/cxx.sh)"; exit 1; }
CXXTC=$WORK/ottd-tc
mkdir -p "$CXXTC/bin"
for t in objcopy strip nm readelf objdump ar ranlib; do ln -sf "$WORK/bin/riscv32-linux-$t" "$CXXTC/bin/riscv32-linux-musl-$t"; done
[ -e "$WORK/bin/riscv32-linux-nm" ] || ln -sf "$GVM_UL/bin/gpuvm-linux-nm" "$WORK/bin/riscv32-linux-nm"
ln -sf "$WORK/bin/riscv32-linux-ar" "$CXXTC/bin/riscv32-linux-musl-gcc-ar"
ln -sf "$WORK/bin/riscv32-linux-ranlib" "$CXXTC/bin/riscv32-linux-musl-gcc-ranlib"
ln -sf "$CXXTC/bin/riscv32-linux-musl-strip" "$WORK/bin/riscv32-linux-musl-strip"
for d in cc c++; do
    D=gpuvm-linux-$d
    # (C++: <cstdlib> first; fmt in OpenTTD asks libc++ for no transitive includes and then calls malloc)
    X=; [ $d = c++ ] && X="-include cstdlib"
    cat > "$WORK/bin/ottd-$d.new" <<EOT
#!/usr/bin/env bash
. "$WORK/bin/gvm-filter"
export GVM_KEEP_ELF=1
exec "$GVM_UL/bin/$D" -Wno-unknown-warning-option -Wno-ignored-optimization-argument -Qunused-arguments \
    -Wno-unused-command-line-argument $GVM_CFLAGS $X "\${args[@]+"\${args[@]}"}"
EOT
    cmp -s "$WORK/bin/ottd-$d.new" "$WORK/bin/ottd-$d" && rm "$WORK/bin/ottd-$d.new" || mv "$WORK/bin/ottd-$d.new" "$WORK/bin/ottd-$d"
    chmod +x "$WORK/bin/ottd-$d"
done
EOF
# no precompiled header (gpuvm-linux-cc compiles sources, not headers)
sed -i 's/-DSHADEREMU=ON \$NONE/-DSHADEREMU=ON -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON $NONE/' "$REPO/linux/openttd/build.sh"
grep -q CMAKE_DISABLE_PRECOMPILE_HEADERS "$REPO/linux/openttd/build.sh" || { echo "openttd.sh: build.sh's cmake line moved"; exit 1; }
exec bash "$REPO/linux/openttd/build.sh"
