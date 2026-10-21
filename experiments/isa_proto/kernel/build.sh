#!/usr/bin/env bash
# The no-MMU kernel for the machine's slots: linux/nommu/kernel.sh's tree and configuration, patch_native.py,
# the medany code model (code addresses are pc-relative), then xlat_kernel.c makes vmlinux the machine's.
# Into ~/isa (not the shared ~/nm): ksrc, k-isa, and build/isa/kernel.bin of the repo.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
SH=$HOME/shaderemu-linux
K=$HOME/isa/ksrc
O=$HOME/isa/k-isa; mkdir -p "$O" "$REPO/build/isa"
NM="$REPO/linux/nommu"
if [ "${1:-}" != "--again" ]; then   # (--again: the tree as it is, built where it was)
  rsync -a --delete --exclude .git "$SH/linux/" "$K/"
  export PATH="$SH/tools/usr/bin:$SH/toolchain/bin:$PATH" BISON_PKGDATADIR="$SH/tools/usr/share/bison" M4="$SH/tools/usr/bin/m4"
  make -C "$K" ARCH=riscv mrproper > /dev/null
  for p in kconfig signal drivers ovl ramfs console procmaps; do python3 "$NM/patch_$p.py" "$K" > /dev/null || exit 1; done
  python3 "$HERE/patch_native.py" "$K" || exit 1
  { cat "$SH/all.config"; echo "# CONFIG_MMU is not set"; echo "CONFIG_BINFMT_FLAT=y"; echo "CONFIG_OVERLAY_FS_REDIRECT_DIR=y";
    echo "CONFIG_CMODEL_MEDANY=y"; } > "$O/all.config"
fi
export PATH="$SH/tools/usr/bin:$SH/toolchain/bin:$PATH" BISON_PKGDATADIR="$SH/tools/usr/share/bison" M4="$SH/tools/usr/bin/m4"
export KBUILD_BUILD_TIMESTAMP="Mon Jan  1 00:00:00 UTC 2024" KBUILD_BUILD_USER=shaderemu KBUILD_BUILD_HOST=shaderemu KBUILD_BUILD_VERSION=1
M="make -C $K O=$O ARCH=riscv CROSS_COMPILE=riscv32-linux- -j$(nproc)"
[ "${1:-}" != "--again" ] && { $M KCONFIG_ALLCONFIG="$O/all.config" allnoconfig > "$O/config.log" 2>&1; }
grep -q "MMU is not set" "$O/.config" && grep -q "CONFIG_CMODEL_MEDANY=y" "$O/.config" || { echo "configuration: MMU or code model"; exit 1; }
$M vmlinux > "$O/kernel.log" 2>&1 || { grep -E "error|Error" "$O/kernel.log" | head; exit 1; }
gcc -O2 -Wall -o "$HOME/isa/xlat" "$HERE/../xlat.c" "$HERE/../xlat_flat.c" "$HERE/../xlat_kernel.c" "$HERE/../xlat_main.c" || exit 1
"$HOME/isa/xlat" kernel "$O/vmlinux" "$REPO/build/isa/kernel.bin" "$REPO/build/isa/kernel.map"
