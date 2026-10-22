#!/bin/bash
# gpu_vm's root file system on the ROM (gpu_vm/userland/README.md, "The root on the ROM"), as the lean RISC-V machine has
# it: rootfs.sh's tree as a romfs image read through MTD (phram over the ROM's addresses: a page is one memcpy, the
# commit's copy from 4 KB up), and no initramfs to unpack. In WSL, after rootfs.sh:
#   bash gpu_vm/userland/romroot.sh
#   bash gpu_vm/userland/systest.sh cpu --rom build/gvm_romroot/rootfs.romfs                 (with W=build/gvm_romroot)
#   W=build/gvm_romroot pwsh tools\gpu_queue.ps1 "C:\Program Files\Git\bin\bash.exe" gpu_vm/userland/systest.sh gpu --rom build/gvm_romroot/rootfs.romfs
# -> $W (build/gvm_romroot): rootfs.romfs (mkromfs.py of rootfs.sh's initramfs.list), vmlinux (gpuvm_defconfig with MTD,
# phram and romfs on MTD; its initramfs only /root, /dev and /dev/console, so the kernel mounts root=), gvm.dtb
# (bootargs `root=mtd:root rootfstype=romfs ro init=/init phram.phram=root,0x40000000,NMi`), busybox (for systest.sh).
# KOUT the kernel's build (~/gpu_vm/linux-build-romroot; configured and built whole the first time).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
R="${R:-$HOME/gpu_vm/rootfs-build}"
BB="${BB:-$HOME/gpu_vm/busybox}"
KOUT="${KOUT:-$HOME/gpu_vm/linux-build-romroot}"
KDIR="${KDIR:-$HOME/gpu_vm/linux}"
W="${W:-$ROOT/build/gvm_romroot}"
export PATH="${LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}:$PATH"
[ -f "$R/initramfs.list" ] || { echo "romroot.sh: no $R/initramfs.list (rootfs.sh)" >&2; exit 1; }
mkdir -p "$W"
python3 "$HERE/mkromfs.py" "$R/initramfs.list" "$W/rootfs.romfs"
MIB=$(( ($(stat -c %s "$W/rootfs.romfs") + 1048575) / 1048576 ))
printf 'dir /root 0700 0 0\ndir /dev 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\n' > "$W/initramfs.list"
{
    if [ ! -f "$KOUT/.config" ]; then
        OUT="$KOUT" bash "$ROOT/gpu_vm/linux/build.sh" sync config
        "$KDIR/scripts/config" --file "$KOUT/.config" -e MTD -e MTD_PHRAM -d ROMFS_BACKED_BY_BLOCK \
            -e ROMFS_BACKED_BY_BOTH -e ROMFS_ON_BLOCK -e ROMFS_ON_MTD
    fi
    OUT="$KOUT" INITRAMFS_LIST="$W/initramfs.list" bash "$ROOT/gpu_vm/linux/build.sh" sync initramfs image dtbs
} > "$W/kernel.log" 2>&1 || { tail -30 "$W/kernel.log"; exit 1; }
grep -q '^CONFIG_MTD_PHRAM=y' "$KOUT/.config" || { echo "romroot.sh: $KOUT/.config has no phram" >&2; exit 1; }
sed "s|bootargs = \"console=hvc0\";|bootargs = \"console=hvc0 root=mtd:root rootfstype=romfs ro init=/init phram.phram=root,0x40000000,${MIB}Mi\";|" \
    "$ROOT/gpu_vm/linux/arch/gpuvm/boot/dts/gvm.dts" > "$W/gvm.dts"
grep -q 'root=mtd:root' "$W/gvm.dts" || { echo "romroot.sh: gvm.dts's bootargs not found" >&2; exit 1; }
"$KOUT/scripts/dtc/dtc" -q -I dts -O dtb -o "$W/gvm.dtb" "$W/gvm.dts"
cp "$KOUT/vmlinux" "$W/vmlinux"; cp "$BB/busybox" "$W/busybox"
ls -l "$W/vmlinux" "$W/rootfs.romfs"
echo "romroot.sh: $W (the ROM: --rom $W/rootfs.romfs)"
