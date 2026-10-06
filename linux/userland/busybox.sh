#!/usr/bin/env bash
# Builds busybox, static against our musl, into build/images/linux/root/bin, from where
# tools/make_linux_image.py puts it in the image in place of upstream's (which is linked
# dynamically against glibc: every command it started spent half its start-up in the loader).
#   wsl -- bash /mnt/c/.../linux/userland/busybox.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/toolchain.sh"
BUSYBOX=1.36.1
OUT=$REPO/build/images/linux/root/bin
cd "$WORK/src"
if [ ! -d busybox ]; then
    curl -sSL "https://busybox.net/downloads/busybox-$BUSYBOX.tar.bz2" -o busybox.tar.bz2
    mkdir busybox && tar -xf busybox.tar.bz2 -C busybox --strip-components=1 && rm busybox.tar.bz2
fi
cd busybox
make -s defconfig >/dev/null
# static; and without the applets that need headers or kernel features this machine lacks
set_option() { sed -i "s/^# CONFIG_$1 is not set\$/CONFIG_$1=y/; s/^CONFIG_$1=.*\$/CONFIG_$1=$2/" .config; }
set_option STATIC y
for off in TC FEATURE_TC_INGRESS NANDWRITE NANDDUMP UBIATTACH UBIDETACH UBIMKVOL UBIRMVOL UBIRSVOL UBIUPDATEVOL \
           FEATURE_UTMP FEATURE_WTMP SELINUX PAM FEATURE_HAVE_RPC FEATURE_INETD_RPC HWCLOCK; do
    sed -i "s/^CONFIG_$off=y\$/# CONFIG_$off is not set/" .config
done
{ yes "" || true; } | make -s oldconfig >/dev/null || true   # (yes ends with a broken pipe)
# (objects are joined with "$LD -r", which must not be the driver that adds the C start-up files)
make -s -j"$(nproc)" CC=rv32-cc LD="riscv32-linux-gcc $ARCH_FLAGS -nostdlib" HOSTCC=gcc AR=riscv32-linux-ar NM=riscv32-linux-nm STRIP=riscv32-linux-strip \
    OBJCOPY=riscv32-linux-objcopy SKIP_STRIP=y busybox 2>&1 | grep -E "error|undefined" || true
[ -f busybox ] || { echo "busybox was not built"; exit 1; }
mkdir -p "$OUT"
riscv32-linux-strip -o "$OUT/busybox" busybox
ls -l "$OUT/busybox"
