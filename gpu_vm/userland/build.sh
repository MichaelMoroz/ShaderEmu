#!/bin/bash
# The userland toolchain for gpu_vm's Linux (gpu_vm/userland/README.md). Run in WSL.
#
#   build.sh [STEP ...]    steps, in order: musl mfloat builtins (default: all three)
#     syscalls   bits/syscall.h.in again from the kernel's headers (gpu_vm/linux/build.sh headers first)
#     musl       musl 1.2.5 from its tarball into $MUSL with the port (musl/) laid over it; libc.a, crt1.o, crti.o,
#                crtn.o and the headers installed into $SYSROOT
#     mfloat     $SYSROOT/lib/libmfloat.a: double math functions at float precision (musl/mfloat/README; -lmfloat)
#     builtins   compiler-rt's generic builtins as $SYSROOT/lib/libclang_rt.builtins-gpuvm.a
#
# MUSL_CFLAGS (-O2) musl's optimization. SYSROOT (~/gpu_vm/sysroot), MUSL the tree musl is built in (~/gpu_vm/musl), DL the downloads (~/gpu_vm/dl),
# LLVM_BIN (~/gpu_vm/llvm-build/bin), LLVM_SRC (~/gpu_vm/llvm-project), KHDR the kernel's installed headers
# (~/gpu_vm/linux-build/usr/include), JOBS.
#
# The downloaded sources are data: nothing is run from inside them but musl's own configure and Makefile.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYSROOT="${SYSROOT:-$HOME/gpu_vm/sysroot}"
MUSL="${MUSL:-$HOME/gpu_vm/musl}"
DL="${DL:-$HOME/gpu_vm/dl}"
LLVM_BIN="${LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}"
LLVM_SRC="${LLVM_SRC:-$HOME/gpu_vm/llvm-project}"
KHDR="${KHDR:-$HOME/gpu_vm/linux-build/usr/include}"
JOBS="${JOBS:-$(nproc)}"
MUSL_VER=1.2.5
MUSL_SHA256=a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
TARGET=gpuvm-unknown-linux
CC="$LLVM_BIN/clang --target=$TARGET"

steps=("$@")
[ ${#steps[@]} -gt 0 ] || steps=(musl mfloat builtins)

syscalls() {
    # every __NR_ the kernel's asm/unistd.h defines for gpuvm, as a number
    local t; t=$(mktemp -d)
    echo '#include <asm/unistd.h>' > "$t/a.c"
    $CC -nostdinc -I"$KHDR" -E -dM "$t/a.c" | awk '$1 == "#define" && $2 ~ /^__NR_/ && $2 !~ /^__NR3264/ { print $2 }' \
        | sort -u > "$t/names"
    { echo '#include <asm/unistd.h>'; while read -r n; do echo "@@ \"$n\" $n"; done < "$t/names"; } > "$t/b.c"
    $CC -nostdinc -I"$KHDR" -E -P "$t/b.c" | awk '$1 == "@@" { n = $2; gsub(/"/, "", n); $1 = $2 = ""; if ($0 ~ /^[0-9() +]+$/) print n, $0 }' \
        | while read -r n v; do echo "$n $(( v ))"; done | sort -k2 -n \
        | awk '$1 != "__NR_syscalls" && $1 != "__NR_arch_specific_syscall" { printf "#define %s %s\n", $1, $2 }' > "$t/out"
    {
        echo "/* From the kernel's asm/unistd.h for gpuvm (gpu_vm/userland/build.sh syscalls). 32-bit with 64-bit"
        echo "   times only: futex is futex_time64. musl names llseek _llseek, and its clock_nanosleep falls back on"
        echo "   nanosleep (101, which the kernel does not have: ENOSYS) only when clock_nanosleep_time64 is ENOSYS, as"
        echo "   riscv32's list has it. */"
        cat "$t/out"
        echo '#define __NR_futex __NR_futex_time64'
        echo '#define __NR__llseek __NR_llseek'
        echo '#define __NR_nanosleep 101'
        echo '/* newer than the 5.17 kernel, named by musl 1.2.5 (ENOSYS here; musl falls back) */'
        echo '#define __NR_cachestat 451'
        echo '#define __NR_fchmodat2 452'
    } > "$HERE/musl/arch/gpuvm/bits/syscall.h.in"
    echo "syscalls: $(wc -l < "$t/out") numbers into musl/arch/gpuvm/bits/syscall.h.in"
    rm -rf "$t"
}

musl() {
    local tgz="$DL/musl/musl-$MUSL_VER.tar.gz"
    if [ ! -f "$tgz" ]; then
        mkdir -p "$DL/musl"
        wget -q -O "$tgz" "https://musl.libc.org/releases/musl-$MUSL_VER.tar.gz"
    fi
    echo "$MUSL_SHA256  $tgz" | sha256sum -c --quiet -
    # a clean tree each time: the tarball, then the port's files (arch/gpuvm and src/*/gpuvm) over it
    rm -rf "$MUSL"
    mkdir -p "$MUSL"
    tar -xzf "$tgz" -C "$MUSL" --strip-components=1
    (cd "$HERE/musl" && find . -type f) | while read -r f; do
        mkdir -p "$MUSL/$(dirname "$f")"
        cp "$HERE/musl/$f" "$MUSL/$f"
    done
    # configure knows the arch by the target's name
    sed -i 's/^riscv64\*) ARCH=riscv64 ;;/gpuvm*) ARCH=gpuvm ;;\n&/' "$MUSL/configure"
    grep -q 'ARCH=gpuvm' "$MUSL/configure"
    cd "$MUSL"
    ./configure --target=gpuvm --prefix="$SYSROOT" --syslibdir="$SYSROOT/lib" --disable-shared --enable-static \
        CC="$CC" AR="$LLVM_BIN/llvm-ar" RANLIB="$LLVM_BIN/llvm-ranlib" CFLAGS="${MUSL_CFLAGS:--O2} ${GVM_PIC:--fPIE}" > configure.log
    make -j"$JOBS" > make.log 2>&1 || { tail -30 make.log; exit 1; }
    make install > install.log 2>&1 || { tail -30 install.log; exit 1; }
    # the kernel's headers (linux/, asm/, asm-generic/) beside libc's, for programs that include them
    if [ -d "$KHDR/linux" ]; then
        cp -r "$KHDR"/{linux,asm,asm-generic,mtd} "$SYSROOT/include/" 2>/dev/null || true
    fi
    echo "musl: $(ls -la "$SYSROOT/lib/libc.a" | awk '{print $5}') bytes of libc.a in $SYSROOT/lib"
}

mfloat() {
    local O; O=$(mktemp -d)
    for f in "$HERE"/musl/mfloat/*.c; do
        $CC -O2 ${GVM_PIC:--fPIE} -ffreestanding -nostdinc -isystem "$SYSROOT/include" -c "$f" -o "$O/$(basename "$f" .c).o"
    done
    rm -f "$SYSROOT/lib/libmfloat.a"
    "$LLVM_BIN/llvm-ar" rcs "$SYSROOT/lib/libmfloat.a" "$O"/*.o
    echo "mfloat: $(ls "$O" | wc -l) functions into $SYSROOT/lib/libmfloat.a"
    rm -rf "$O"
}

builtins() {
    # compiler-rt's generic builtins (lib/builtins: the C files, none of an architecture's own): i64 division,
    # double and float in software (soft float's sf/df ops), conversions, i64 shifts and compares, __atomic_* for 8
    # bytes. A file that compiles to nothing (128-bit long double, tf) is left out by being empty.
    local B="$LLVM_SRC/compiler-rt/lib/builtins" O; O=$(mktemp -d)
    local skip=" emutls enable_execute_stack clear_cache gcc_personality_v0 eprintf os_version_check cpu_model trampoline_setup apple_versioning "
    local n=0 f name inc
    # gpu_vm's floats have no rounding modes or exceptions: the soft double's adds and the like read the mode inline
    # (fp_mode.h's guard taken first), not through calls of fp_mode.c's __fe_getround and __fe_raise_inexact
    printf '%s\n' '#define FP_MODE_H' 'typedef enum { CRT_FE_TONEAREST, CRT_FE_DOWNWARD, CRT_FE_UPWARD, CRT_FE_TOWARDZERO } CRT_FE_ROUND_MODE;' \
        'static inline CRT_FE_ROUND_MODE __fe_getround(void) { return CRT_FE_TONEAREST; }' \
        'static inline int __fe_raise_inexact(void) { return 0; }' > "$O/fp_mode_inline.h"
    for f in "$B"/*.c; do
        name=$(basename "$f" .c)
        case "$skip" in *" $name "*) continue ;; esac
        case "$name" in *xf*|*xc3|*bf2|crtbegin|crtend) continue ;; esac   # x87's long double, bfloat16, crt objects
        inc=(-include "$O/fp_mode_inline.h"); [ "$name" = fp_mode ] && inc=()
        if $CC -O2 ${GVM_PIC:--fPIE} -fno-builtin -ffreestanding -nostdinc -isystem "$SYSROOT/include" -I"$B" "${inc[@]}" \
               -c "$f" -o "$O/$name.o" 2> "$O/$name.err"; then
            n=$((n + 1))
        else
            echo "builtins: $name did not compile ($(head -1 "$O/$name.err"))"
            rm -f "$O/$name.o"
        fi
    done
    mkdir -p "$SYSROOT/lib"
    rm -f "$SYSROOT/lib/libclang_rt.builtins-gpuvm.a" "$O/fp_mode_inline.h"
    "$LLVM_BIN/llvm-ar" rcs "$SYSROOT/lib/libclang_rt.builtins-gpuvm.a" "$O"/*.o
    echo "builtins: $n files into $SYSROOT/lib/libclang_rt.builtins-gpuvm.a"
    rm -rf "$O"
}

for s in "${steps[@]}"; do
    case "$s" in
    syscalls) syscalls ;;
    musl) musl ;;
    mfloat) mfloat ;;
    builtins) builtins ;;
    *) echo "build.sh: no step $s" >&2; exit 1 ;;
    esac
done
