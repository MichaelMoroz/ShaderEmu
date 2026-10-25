#!/usr/bin/env bash
# The two images with wl in /usr/bin (programs/wl/README.md), after build.sh. In WSL:
#   bash programs/wl/image.sh gvm   -> build/wl/img: gpu_vm's img-cur (gpu_vm/bench_games/README.md) and wl
#   bash programs/wl/image.sh rv    -> build/wl/rvroot: linux/prebuilt/nommu's root and wl, for linux/nommu/image.py
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=$REPO/build/wl
case "${1:-}" in
gvm)
    BASE=${GVM_BASE:-$REPO/build/gvm_apps/img-cur}
    W=$OUT/img
    mkdir -p "$W"
    # (and hostwants where the base lacks it: img-cur has none, and rcS then starts the desktop, 127 having its bit 0 set)
    { grep -v '^file /usr/bin/wl ' "$BASE/rootfs.list"; echo "file /usr/bin/wl $OUT/gvm/wl 0755 0 0"
      grep -q '^file /usr/bin/hostwants ' "$BASE/rootfs.list" || echo "file /usr/bin/hostwants $REPO/build/gvm_apps/root/usr/bin/hostwants 0755 0 0"
    } > "$W/rootfs.list"
    python3 "$REPO/gpu_vm/userland/mkromfs.py" "$W/rootfs.list" "$W/rootfs.romfs"
    MIB=$(( ($(stat -c %s "$W/rootfs.romfs") + 1048575) / 1048576 ))
    sed -E "s|phram=root,0x40000000,[0-9]+Mi|phram=root,0x40000000,${MIB}Mi|" "$BASE/gvm.dts" > "$W/gvm.dts"
    "$HOME/gpu_vm/linux-build-apps/scripts/dtc/dtc" -q -I dts -O dtb -o "$W/gvm.dtb" "$W/gvm.dts"
    cp "$BASE/vmlinux" "$BASE/busybox" "$BASE/ram_mb" "$W/"
    echo "image.sh: $W ($MIB MB of ROM)"
    ;;
rv)
    R=$OUT/rvroot
    rm -rf "$R"; cp -a "$REPO/linux/prebuilt/nommu/root" "$R"
    cp "$OUT/rv/wl" "$R/usr/bin/wl"
    echo "image.sh: $R; now (Windows): set HALFLIFE_DATA=0 & python linux/nommu/image.py nommu_wl build/wl/rvroot linux/prebuilt/nommu/Image"
    ;;
*) echo "image.sh gvm|rv"; exit 1 ;;
esac
