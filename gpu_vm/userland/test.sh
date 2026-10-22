#!/bin/bash
# The userland's tests, run under the kernel on gvm_cpu (gpu_vm/userland/README.md, "Tests"). In WSL:
#   bash gpu_vm/userland/test.sh [-O LEVEL]
# Builds test/*.c with gpuvm-linux-cc into $T, an initramfs with them in /bin and test/init.c as /init, a kernel in a
# build folder of its own ($KOUT: the first run configures and builds it whole, later ones relink), boots it on
# gvm_cpu and looks for init's summary: every program passed. The console's output is $T/boot.log.
# Needs build.sh's sysroot (musl, builtins). T (~/gpu_vm/userland-test), KOUT (~/gpu_vm/linux-build-ut).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
T="${T:-$HOME/gpu_vm/userland-test}"
KOUT="${KOUT:-$HOME/gpu_vm/linux-build-ut}"
OPT=-O2
if [ "${1:-}" = -O ]; then OPT=-O$2; shift 2; fi
CC="$HERE/bin/gpuvm-linux-cc"
mkdir -p "$T"

progs="hello libc files proc signals timeenv threads mathf init"
for p in $progs; do
    out=$p; [ $p = mathf ] && out=math
    extra=; [ $p = mathf ] && extra=-lm; [ $p = threads ] && extra=-pthread
    "$CC" $OPT -Wall -Wno-unused-variable -Wno-format-truncation $extra "$HERE/test/$p.c" -o "$T/$out"
done

{
    echo "dir /bin 0755 0 0"
    echo "dir /etc 0755 0 0"
    echo "dir /tmp 01777 0 0"
    for p in $progs; do
        out=$p; [ $p = mathf ] && out=math
        [ $p = init ] || echo "file /bin/$out $T/$out 0755 0 0"
    done
    echo "file /etc/test.txt $T/test.txt 0644 0 0"
} > "$T/extra.list"
echo "a file of the initramfs" > "$T/test.txt"

steps="sync initramfs image dtbs"
[ -f "$KOUT/.config" ] || steps="sync config initramfs image dtbs"
OUT="$KOUT" INIT="$T/init" INITRAMFS_EXTRA="$T/extra.list" bash "$ROOT/gpu_vm/linux/build.sh" $steps > "$T/kernel.log" 2>&1 \
    || { tail -30 "$T/kernel.log"; exit 1; }

make -s -C "$ROOT/gpu_vm/cpu" CC=gcc > /dev/null
timeout 600 "$ROOT/build/gvm_cpu/gvm_cpu" --no-stdin --stats --dtb "$KOUT/arch/gpuvm/boot/dts/gvm.dtb" "$KOUT/vmlinux" \
    > "$T/boot.log" 2>&1 || true
sed -n '/Run \/init/,$p' "$T/boot.log"
n=$(echo $progs | wc -w); n=$((n - 1))
if grep -q "gvm-userland-test: $n passed, 0 failed" "$T/boot.log"; then
    echo "test.sh: all $n programs passed ($OPT)"
else
    echo "test.sh: FAILED ($OPT; the console's output: $T/boot.log)"
    exit 1
fi
