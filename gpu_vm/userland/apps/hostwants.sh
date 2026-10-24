#!/usr/bin/env bash
# hostwants (hostwants.c): rcS asks it whether the host wants the desktop at boot, as emuinit.c does on RISC-V.
# Run by build.sh as ../gpu_vm/userland/apps/hostwants.sh.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
source "$REPO/linux/userland/toolchain.sh"
OUT=$REPO/build/images/linux/root/usr/bin
mkdir -p "$WORK/src" "$OUT"
rv32-cc -Os -Wall "$HERE/hostwants.c" -o "$WORK/src/hostwants"
riscv32-linux-strip -o "$OUT/hostwants" "$WORK/src/hostwants"
ls -l "$OUT/hostwants"
