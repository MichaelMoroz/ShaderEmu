#!/usr/bin/env bash
# The worker cores' tests (what linux/nommu/root.sh builds for the RISC-V machine: mctest, lanetest, steptest) into
# build/images/linux/root of build.sh's copy; run by build.sh as ../gpu_vm/userland/apps/mc.sh. No lanes on gpu_vm:
# lanetest and steptest check the plain calls against themselves, as on rvc_cpu (docs/machine/lanes.md).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
source "$REPO/linux/userland/toolchain.sh"
OUT=$REPO/build/images/linux/root/usr/bin
mkdir -p "$OUT" "$WORK/src/mc"
M=$REPO/programs/mc
rv32-cc -O2 -w -I"$M" "$M/mctest.c" "$M/mcw.c" "$M/mcwx.c" -o "$WORK/src/mc/mctest"
rv32-cc -O2 -w -I"$M" "$M/lanetest.c" "$M/mcw.c" -o "$WORK/src/mc/lanetest"
rv32-cc -O2 -w -I"$M" "$M/steptest.c" "$M/mcw.c" -o "$WORK/src/mc/steptest"
# gvmcmp: test_all.py's comparison of a float output with its reference (PSNR, SNR)
rv32-cc -O2 -Wall "$HERE/gvmcmp.c" -lm -o "$WORK/src/mc/gvmcmp"
for p in mctest lanetest steptest gvmcmp; do riscv32-linux-strip -o "$OUT/$p" "$WORK/src/mc/$p"; done
ls -l "$OUT/mctest" "$OUT/lanetest" "$OUT/steptest"
