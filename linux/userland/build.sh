#!/usr/bin/env bash
# Builds the image's init (emuinit.c) into build/images/linux/root, from where
# tools/make_linux_image.py puts it in the image and makes it the kernel's init.
#   wsl -- bash /mnt/c/.../linux/userland/build.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/toolchain.sh"
OUT=$REPO/build/images/linux/root
mkdir -p "$OUT"
rv32-cc -Os -Wall ${EMUINIT_FLAGS:-} "$HERE/emuinit.c" -o "$WORK/src/emuinit"
riscv32-linux-strip -o "$OUT/emuinit" "$WORK/src/emuinit"
ls -l "$OUT/emuinit"
