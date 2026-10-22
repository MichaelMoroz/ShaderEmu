#!/bin/bash
# libc-test's functional and regression tests (and with MATH=1 its math tests) built by gpuvm-linux-cc and run under the
# kernel on gvm_cpu (gpu_vm/userland/README.md, "libc-test"). In WSL:
#   bash gpu_vm/userland/libc-test.sh
# libc-test is cloned into $LT (~/gpu_vm/dl/libc-test) when missing; its sources are only compiled, never run on the
# host. The programs go into an initramfs's /t with test/ltinit.c as /init, which runs each (20 s of the machine's
# time at most) and prints FAIL lines and a summary; the kernel is built in $KOUT as test.sh's. Results in
# $T/libc-test.log; programs that did not build are listed in $T/build-failures.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
LT="${LT:-$HOME/gpu_vm/dl/libc-test}"
T="${T:-$HOME/gpu_vm/libc-test-run}"
KOUT="${KOUT:-$HOME/gpu_vm/linux-build-ut}"
CC="$HERE/bin/gpuvm-linux-cc"
AR="$HERE/bin/gpuvm-linux-ar"
[ -d "$LT/src" ] || git clone -q https://repo.or.cz/libc-test.git "$LT"
rm -rf "$T"
mkdir -p "$T/common" "$T/t"

CFLAGS=(-O2 -std=c99 -D_POSIX_C_SOURCE=200809L -Wno-everything -fno-builtin -frounding-math -I"$LT/src/common")
for f in fdfill memfill mtest path print rand setrlim utf8 vmfill; do
    "$CC" "${CFLAGS[@]}" -c "$LT/src/common/$f.c" -o "$T/common/$f.o"
done
"$AR" rcs "$T/common/libtest.a" "$T/common"/*.o

dirs="functional regression"
[ "${MATH:-0}" = 1 ] && dirs="$dirs math"
: > "$T/build-failures"
built=0
for d in $dirs; do
    for f in "$LT/src/$d"/*.c; do
        n=$(basename "$f" .c)
        case "$n" in *dso*|*dlopen*|tls_get_new-dtv|tls_align|tls_init) continue ;; esac   # shared objects, dlopen
        if "$CC" "${CFLAGS[@]}" "$f" "$T/common/libtest.a" -lm -o "$T/t/$d-$n" > "$T/t/$d-$n.err" 2>&1; then
            built=$((built + 1)); rm -f "$T/t/$d-$n.err"
        else
            echo "$d-$n: $(grep -m1 -E 'error' "$T/t/$d-$n.err" || head -1 "$T/t/$d-$n.err")" >> "$T/build-failures"
            rm -f "$T/t/$d-$n" "$T/t/$d-$n.err"
        fi
    done
done
echo "libc-test.sh: $built programs built, $(wc -l < "$T/build-failures") did not (in $T/build-failures)"

"$CC" -O2 "$HERE/test/ltinit.c" -o "$T/init"
{
    echo "dir /t 0755 0 0"
    echo "dir /tmp 01777 0 0"
    for p in "$T"/t/*; do echo "file /t/$(basename "$p") $p 0755 0 0"; done
} > "$T/extra.list"

steps="sync initramfs image dtbs"
DEFCONFIG="$ROOT/gpu_vm/linux/arch/gpuvm/configs/gpuvm_defconfig"
[ -f "$KOUT/.config" ] && [ "$KOUT/.config" -nt "$DEFCONFIG" ] || steps="sync config initramfs image dtbs"
OUT="$KOUT" INIT="$T/init" INITRAMFS_EXTRA="$T/extra.list" bash "$ROOT/gpu_vm/linux/build.sh" $steps > "$T/kernel.log" 2>&1 \
    || { tail -30 "$T/kernel.log"; exit 1; }

make -s -C "$ROOT/gpu_vm/cpu" CC=gcc > /dev/null
timeout 3600 "$ROOT/build/gvm_cpu/gvm_cpu" --no-stdin --stats --dtb "$KOUT/arch/gpuvm/boot/dts/gvm.dtb" \
    "$KOUT/vmlinux" > "$T/boot.log" 2>&1 || true
sed -n '/Run \/init/,$p' "$T/boot.log" | grep -v '^\[ *[0-9]*\.[0-9]*\] ' > "$T/libc-test.log" || true
grep -E '^FAIL|^libc-test:' "$T/libc-test.log" || true
grep -q '^libc-test: .* passed' "$T/libc-test.log"
