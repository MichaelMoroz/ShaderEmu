#!/bin/bash
# The real-work bench's programs (gpu_vm/bench_real/README.md), the same sources by the same clang 23.1.4 for both
# machines. In WSL:
#   bash gpu_vm/bench_real/build.sh [rv] [gvm] [bare] [kernel]     (default: all)
# rv:     CoreMark and zlib's minigzip for the RISC-V machine (rv32imaf ilp32, as rv32-cc), compiled by clang, linked
#         by rv32-cc's driver (the shader machine's musl), made flat by linux/nommu/elf2bflt.py -> $OUT/rv
# gvm:    the same for gpu_vm's Linux by gpuvm-linux-cc (bFLT), and this folder's init.c -> $OUT/gvm
# bare:   CoreMark bare-metal for gvm_gpu (gpu_vm/llvm/test/bench/run.sh) at ITER1 and ITER2 iterations -> $OUT/bare
# kernel: a kernel ($KOUT) whose initramfs has the gvm programs, the input and init.c as /init
# The input of gzip: zlib's own C sources, joined, cut to GZ_BYTES. OUT (build/bench_real of the tree), KOUT
# (~/gpu_vm/linux-build-bench), ITER1 / ITER2 (CoreMark's iterations, 60 and 180), COREMARK, THIRD.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${OUT:-$ROOT/build/bench_real}"
KOUT="${KOUT:-$HOME/gpu_vm/linux-build-bench}"
BIN="$HOME/gpu_vm/llvm-build/bin"
COREMARK="${COREMARK:-$HOME/gpu_vm/coremark-src/coremark}"
Z="${THIRD:-$HOME/gpu_vm/third}/zlib"
ITER1="${ITER1:-60}"; ITER2="${ITER2:-180}"
GZ_BYTES="${GZ_BYTES:-262144}"
RVSYS="$HOME/nm/work/sysroot"
steps=("$@"); [ ${#steps[@]} -gt 0 ] || steps=(rv gvm bare kernel)
mkdir -p "$OUT"
CM_SRC="core_list_join core_main core_matrix core_state core_util"
Z_SRC="adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil"
cat "$Z"/*.c | head -c "$GZ_BYTES" > "$OUT/input"

# build DIR "CC..." : CoreMark and minigzip's objects into DIR
objs() {
    local d=$1; shift
    mkdir -p "$d"
    for f in $CM_SRC; do
        "$@" -O2 -I"$HERE/coremark" -I"$COREMARK" -DPERFORMANCE_RUN=1 -DFLAGS_STR='"-O2"' -w -c "$COREMARK/$f.c" -o "$d/$f.o"
    done
    "$@" -O2 -I"$HERE/coremark" -I"$COREMARK" -w -c "$HERE/coremark/core_portme.c" -o "$d/core_portme.o"
    for f in $Z_SRC; do "$@" -O2 -DZ_HAVE_UNISTD_H -I"$Z" -w -c "$Z/$f.c" -o "$d/z_$f.o"; done
    "$@" -O2 -DZ_HAVE_UNISTD_H -I"$Z" -w -c "$Z/test/minigzip.c" -o "$d/minigzip.o"
}

for s in "${steps[@]}"; do
    case "$s" in
    rv)
        d="$OUT/rv"
        RVCC=("$BIN/clang" --target=riscv32-unknown-linux-musl -march=rv32imaf -mabi=ilp32 -fPIE -fno-stack-protector
              -ffp-contract=off -nostdinc -isystem "$RVSYS/include" -isystem "$("$BIN/clang" -print-resource-dir)/include"
              -include nommu_fork.h)
        objs "$d" "${RVCC[@]}"
        export PATH="$HOME/nm/work/bin:$HOME/shaderemu-linux/toolchain/bin:$PATH"
        ( cd "$d"
          rv32-cc -o coremark.elf $(for f in $CM_SRC core_portme; do echo $f.o; done)
          rv32-cc -o minigzip.elf minigzip.o z_*.o
          python3 "$ROOT/linux/nommu/elf2bflt.py" coremark.elf coremark 0x40000
          python3 "$ROOT/linux/nommu/elf2bflt.py" minigzip.elf minigzip 0x40000 )
        ;;
    gvm)
        d="$OUT/gvm"
        CC="$ROOT/gpu_vm/userland/bin/gpuvm-linux-cc"
        objs "$d" "$CC"
        ( cd "$d"
          GVM_STACK=262144 "$CC" -o coremark $(for f in $CM_SRC core_portme; do echo $f.o; done)
          GVM_STACK=262144 "$CC" -o minigzip minigzip.o z_*.o
          "$CC" -O2 -DITER1="\"$ITER1\"" -DITER2="\"$ITER2\"" "$HERE/init.c" -o init )
        ;;
    bare)
        for n in "$ITER1" "$ITER2"; do
            OUT="$OUT/bare/$n" ITER=$n bash "$ROOT/gpu_vm/llvm/test/bench/run.sh" -O2
        done
        ;;
    kernel)
        d="$OUT/gvm"
        { echo "dir /bin 0755 0 0"; echo "dir /tmp 01777 0 0"; echo "dir /bench 0755 0 0"
          echo "file /bin/coremark $d/coremark 0755 0 0"; echo "file /bin/minigzip $d/minigzip 0755 0 0"
          echo "file /bench/input $OUT/input 0644 0 0"; } > "$d/extra.list"
        steps_k="sync initramfs image dtbs"
        [ -f "$KOUT/.config" ] || steps_k="sync config initramfs image dtbs"
        OUT="$KOUT" INIT="$d/init" INITRAMFS_EXTRA="$d/extra.list" bash "$ROOT/gpu_vm/linux/build.sh" $steps_k \
            > "$OUT/kernel.log" 2>&1 || { tail -30 "$OUT/kernel.log"; exit 1; }
        cp "$KOUT/vmlinux" "$KOUT/arch/gpuvm/boot/dts/gvm.dtb" "$d/"
        ;;
    *) echo "build.sh: no step $s" >&2; exit 1 ;;
    esac
done
ls -l "$OUT"/rv "$OUT"/gvm 2>/dev/null | grep -v '\.o$' || true
