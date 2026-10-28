#!/usr/bin/env bash
# The no-MMU kernel: the MMU kernel's tree (linux/kernel/build.sh, its hooks in) copied, fourteen patches,
# and its configuration without the MMU; the ROM is its root, as the MMU kernel's. Into build/nommu/Image.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
SH=$HOME/shaderemu-linux
K=$HOME/nm/ksrc
O=$HOME/nm/k-rom; mkdir -p "$O" "$REPO/build/nommu"
[ -f "$SH/linux/arch/riscv/boot/Image" ] || { echo "run linux/kernel/build.sh first"; exit 1; }
rsync -a --delete --exclude .git "$SH/linux/" "$K/"
export PATH="$SH/tools/usr/bin:$SH/toolchain/bin:$PATH" BISON_PKGDATADIR="$SH/tools/usr/share/bison" M4="$SH/tools/usr/bin/m4"
make -C "$K" ARCH=riscv mrproper > /dev/null    # (the copy was built in place; this one is built beside)
python3 "$HERE/patch_kconfig.py" "$K" && python3 "$HERE/patch_signal.py" "$K" && python3 "$HERE/patch_drivers.py" "$K" && python3 "$HERE/patch_ovl.py" "$K" && python3 "$HERE/patch_ramfs.py" "$K" && python3 "$HERE/patch_console.py" "$K" && python3 "$HERE/patch_procmaps.py" "$K" && python3 "$HERE/patch_uaccess.py" "$K" && python3 "$HERE/patch_memmap.py" "$K" && python3 "$HERE/patch_fastrun.py" "$K" && python3 "$HERE/patch_memcpy.py" "$K" && python3 "$HERE/patch_flatreloc.py" "$K" && python3 "$HERE/patch_sysfs.py" "$K" && python3 "$HERE/patch_zeroread.py" "$K" || exit 1
{ cat "$SH/all.config"; echo "# CONFIG_MMU is not set"; echo "CONFIG_BINFMT_FLAT=y"; echo "CONFIG_OVERLAY_FS_REDIRECT_DIR=y"
  echo "# CONFIG_VM_EVENT_COUNTERS is not set"
  echo "# CONFIG_KALLSYMS is not set"; echo "# CONFIG_AIO is not set"; echo "# CONFIG_IO_URING is not set"; echo "# CONFIG_COREDUMP is not set"; echo "CONFIG_LOG_BUF_SHIFT=14"
  echo "CONFIG_RANDOM_TRUST_BOOTLOADER=y"; } > "$O/all.config"
export KBUILD_BUILD_TIMESTAMP="Mon Jan  1 00:00:00 UTC 2024" KBUILD_BUILD_USER=shaderemu KBUILD_BUILD_HOST=shaderemu KBUILD_BUILD_VERSION=1
M="make -C $K O=$O ARCH=riscv CROSS_COMPILE=riscv32-linux- -j$(nproc)"
$M KCONFIG_ALLCONFIG="$O/all.config" allnoconfig > "$O/config.log" 2>&1
grep -c "MMU is not set" "$O/.config" > /dev/null || { echo "CONFIG_MMU is still set"; exit 1; }
$M Image > "$O/kernel.log" 2>&1; echo "make exit $?"
grep -E "error|Error" "$O/kernel.log" | head
cp "$O/arch/riscv/boot/Image" "$REPO/build/nommu/Image" && ls -l "$REPO/build/nommu/Image"
