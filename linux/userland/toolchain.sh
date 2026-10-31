#!/usr/bin/env bash
# Builds the C toolchain for programs in the Linux image: musl, static, for RV32IMAF (the
# machine's float instructions, docs/machine/fpu.md), with floats passed in integer registers. Sourced
# by the build scripts next to it, after linux/kernel/build.sh (docs/system/nanox.md).
# Leaves headers and libraries in $SYSROOT and the compiler driver $WORK/bin/rv32-cc.
set -euo pipefail
WORK=${WORK:-$HOME/shaderemu-linux}
SYSROOT=$WORK/sysroot
MUSL=1.2.5
LLVM=18.1.8
GCC=releases/gcc-13.3.0
export PATH="$WORK/bin:$WORK/tools/usr/bin:$WORK/toolchain/bin:$PATH"
# One date for every build (__DATE__, busybox's banner): a program whose sources did not change
# is then the same file again, and linux/prebuilt does not grow by it at every save.
export SOURCE_DATE_EPOCH=1704067200
# No stack guard: the compiler's default costs a stored and checked word in most functions.
# (Position-independent code stays the default: the guest's own linker, TinyCC's, needs the
# libraries that way. A program that wants its globals without a table asks with -fno-pie.)
# No fused multiply-add: it reads three registers and leaves the machine's fast step.
ARCH_FLAGS="-march=rv32imaf -mabi=ilp32 -fno-stack-protector -ffp-contract=off"
[ -d "$WORK/toolchain" ] && [ -d "$WORK/linux" ] || { echo "run linux/kernel/build.sh first"; exit 1; }
mkdir -p "$WORK/bin" "$WORK/src" "$SYSROOT/lib" && cd "$WORK/src"

if [ ! -f "$SYSROOT/include/linux/fb.h" ]; then
    echo "== kernel headers"
    make -s -C "$WORK/linux" ARCH=riscv headers_install INSTALL_HDR_PATH="$SYSROOT" >/dev/null
fi

# libraries built with other flags (before the float instructions) are built again
if [ "$(cat "$SYSROOT/lib/arch-flags" 2>/dev/null)" != "$ARCH_FLAGS" ]; then
    rm -f "$SYSROOT/lib/libc.a" "$SYSROOT/lib/libcompiler_rt.a"
    [ ! -d "$WORK/src/musl" ] || make -s -C "$WORK/src/musl" distclean >/dev/null 2>&1 || true
fi

if [ ! -f "$SYSROOT/lib/libc.a" ]; then
    echo "== musl $MUSL"
    if [ ! -d musl ]; then
        curl -sSL "https://musl.libc.org/releases/musl-$MUSL.tar.gz" -o musl.tar.gz
        mkdir musl && tar -xf musl.tar.gz -C musl --strip-components=1 && rm musl.tar.gz
    fi
    (cd musl && ./configure --target=riscv32-linux-musl --prefix="$SYSROOT" --disable-shared \
        CC=riscv32-linux-gcc CFLAGS="$ARCH_FLAGS -O2" AR=riscv32-linux-ar RANLIB=riscv32-linux-ranlib \
        >/dev/null && make -s -j"$(nproc)" >/dev/null 2>&1 && make -s install >/dev/null)
fi

# The compiler driver: gcc with musl's headers and libraries instead of its own C library.
cat > "$WORK/bin/rv32-cc" <<EOF
#!/usr/bin/env bash
link=1
for a in "\$@"; do case "\$a" in -c|-S|-E|-M|-MM) link=0 ;; esac; done
inc="-nostdinc -isystem $SYSROOT/include -isystem \$(riscv32-linux-gcc -print-file-name=include)"
if [ \$link = 1 ]; then
    exec riscv32-linux-gcc $ARCH_FLAGS \$inc -static -no-pie -nostdlib -L$SYSROOT/lib $SYSROOT/lib/crt1.o $SYSROOT/lib/crti.o \\
        "\$@" -Wl,--start-group -lc -lcompiler_rt -Wl,--end-group $SYSROOT/lib/crtn.o
fi
exec riscv32-linux-gcc $ARCH_FLAGS \$inc "\$@"
EOF
chmod +x "$WORK/bin/rv32-cc"

# Our memcpy, memset, memmove and memcmp in place of musl's (memcpy.S, memset.c, memmove.c, memcmp.c beside
# this file): two pointers on word boundaries go sixteen words a turn with no stack frame; musl's own routine stays in the
# library as memcpy_any, for every other case. From 2 KB up both ask for the commit's copy or fill
# (uop.h). Put in again when the library or a file is newer.
OURS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
newer() { for f in memcpy.S memset.c memmove.c memcmp.c uop.h; do [ "$OURS/$f" -nt "$SYSROOT/lib/memcpy-ours" ] && return 0; done; return 1; }
if [ ! -f "$SYSROOT/lib/memcpy-ours" ] || newer || [ "$SYSROOT/lib/libc.a" -nt "$SYSROOT/lib/memcpy-ours" ]; then
    echo "== memcpy, memset"
    MEMOPS="-O2 -fPIC -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns"
    rv32-cc $MEMOPS -I"$OURS" -c "$OURS/memcpy.S" -o "$WORK/src/memcpy_ours.o"
    rv32-cc $MEMOPS -Dmemcpy=memcpy_any -c "$WORK/src/musl/src/string/memcpy.c" -o "$WORK/src/memcpy_any.o"
    rv32-cc $MEMOPS -c "$OURS/memset.c" -o "$WORK/src/memset_ours.o"
    # (memmove.c: the source's words read whole and shifted where the two pointers are at different places in a word)
    rv32-cc $MEMOPS -c "$OURS/memmove.c" -o "$WORK/src/memmove_ours.o"
    # (memcmp.c: memcmp a word a turn)
    rv32-cc $MEMOPS -c "$OURS/memcmp.c" -o "$WORK/src/memcmp_ours.o"
    riscv32-linux-ar d "$SYSROOT/lib/libc.a" memcpy.lo memcpy_ours.o memcpy_any.o memset.lo memset_ours.o memmove.lo memmove_ours.o \
        memcmp.lo memcmp_ours.o 2>/dev/null || true
    riscv32-linux-ar rs "$SYSROOT/lib/libc.a" "$WORK/src/memcpy_ours.o" "$WORK/src/memcpy_any.o" "$WORK/src/memset_ours.o" "$WORK/src/memmove_ours.o" \
        "$WORK/src/memcmp_ours.o" 2>/dev/null
    touch "$SYSROOT/lib/memcpy-ours"
fi

# The float libm (libm/ beside this file, flm_libm.h): sinf, cosf, sincosf, tanf, expf, exp2f, logf, log2f, log10f,
# powf and asinf in float instructions alone, and the doubles' floor, ceil, trunc and round on their bits, in place
# of musl's, which work in software doubles. Each object replaces musl's member of its name where it lies in libc.a
# (the archive's order stays). Put in again when the library or a file is newer.
if [ ! -f "$SYSROOT/lib/libm-ours" ] || [ -n "$(find "$OURS/libm" -newer "$SYSROOT/lib/libm-ours")" ] || [ "$SYSROOT/lib/libc.a" -nt "$SYSROOT/lib/libm-ours" ]; then
    echo "== libm"
    M=$WORK/src/musl
    rm -rf "$M/src/math/shaderemu" "$WORK/src/libm-obj" && mkdir -p "$M/src/math/shaderemu" "$WORK/src/libm-obj"
    cp "$OURS"/libm/* "$M/src/math/shaderemu/"
    MUSLF="-O2 -std=c99 -ffreestanding -fexcess-precision=standard -frounding-math -fno-strict-aliasing -D_XOPEN_SOURCE=700 -w
           -I$M/arch/riscv32 -I$M/arch/generic -I$M/obj/src/internal -I$M/src/include -I$M/src/internal -I$M/obj/include -I$M/include"
    for f in "$M"/src/math/shaderemu/*.c; do
        rv32-cc $MUSLF -c "$f" -o "$WORK/src/libm-obj/$(basename "$f" .c).lo"
    done
    riscv32-linux-ar r "$SYSROOT/lib/libc.a" "$WORK"/src/libm-obj/*.lo
    touch "$SYSROOT/lib/memcpy-ours" "$SYSROOT/lib/libm-ours"   # (the memcpy above is still in: no turn of each again)
fi

# A double's %e, %f, %g and %a formatted in doubles, not in 128-bit software long doubles (vfprintf_double.py beside
# this file); musl's vfprintf.lo replaced where it lies in libc.a. Put in again when the library or the script is newer.
if [ ! -f "$SYSROOT/lib/vfprintf-ours" ] || [ "$OURS/vfprintf_double.py" -nt "$SYSROOT/lib/vfprintf-ours" ] || [ "$SYSROOT/lib/libc.a" -nt "$SYSROOT/lib/vfprintf-ours" ]; then
    echo "== vfprintf"
    M=$WORK/src/musl
    MUSLF="-O2 -std=c99 -ffreestanding -fexcess-precision=standard -frounding-math -fno-strict-aliasing -D_XOPEN_SOURCE=700 -w
           -I$M/arch/riscv32 -I$M/arch/generic -I$M/obj/src/internal -I$M/src/include -I$M/src/internal -I$M/obj/include -I$M/include"
    mkdir -p "$WORK/src/vfprintf-obj"
    python3 "$OURS/vfprintf_double.py" "$M/src/stdio/vfprintf.c" "$M/src/stdio/vfprintf_double.c"
    rv32-cc $MUSLF -c "$M/src/stdio/vfprintf_double.c" -o "$WORK/src/vfprintf-obj/vfprintf.lo"
    riscv32-linux-ar r "$SYSROOT/lib/libc.a" "$WORK/src/vfprintf-obj/vfprintf.lo"
    touch "$SYSROOT/lib/memcpy-ours" "$SYSROOT/lib/libm-ours" "$SYSROOT/lib/vfprintf-ours"   # (the others are still in)
fi

# A group's freed memory kept for the next group of its length (malloc_keep.py beside this file), the members of
# mallocng's malloc.c and free.c out of libc.a (free.lo is also src/malloc/free.c's: the one with __libc_free goes).
if [ ! -f "$SYSROOT/lib/malloc-ours" ] || [ "$OURS/malloc_keep.py" -nt "$SYSROOT/lib/malloc-ours" ] || [ "$SYSROOT/lib/libc.a" -nt "$SYSROOT/lib/malloc-ours" ]; then
    echo "== malloc"
    M=$WORK/src/musl
    K=$WORK/src/malloc-obj; rm -rf "$K"; mkdir -p "$K"
    python3 "$OURS/malloc_keep.py" "$M/src/malloc/mallocng" "$K"
    MUSLF="-O2 -std=c99 -ffreestanding -fexcess-precision=standard -frounding-math -fno-strict-aliasing -D_XOPEN_SOURCE=700 -w
           -I$K -I$M/src/malloc/mallocng -I$M/arch/riscv32 -I$M/arch/generic -I$M/obj/src/internal -I$M/src/include -I$M/src/internal -I$M/obj/include -I$M/include"
    rv32-cc $MUSLF -c "$K/malloc_keep.c" -o "$K/malloc_keep.lo"
    rv32-cc $MUSLF -c "$K/free_keep.c" -o "$K/free_keep.lo"
    (cd "$K" && riscv32-linux-ar d "$SYSROOT/lib/libc.a" malloc.lo &&
        for n in 1 2; do riscv32-linux-ar xN $n "$SYSROOT/lib/libc.a" free.lo; riscv32-linux-nm free.lo | grep -q ' T __libc_free' && { riscv32-linux-ar dN $n "$SYSROOT/lib/libc.a" free.lo; break; }; done &&
        riscv32-linux-ar d "$SYSROOT/lib/libc.a" malloc_keep.lo free_keep.lo; riscv32-linux-ar rs "$SYSROOT/lib/libc.a" malloc_keep.lo free_keep.lo)
    touch "$SYSROOT/lib/memcpy-ours" "$SYSROOT/lib/libm-ours" "$SYSROOT/lib/vfprintf-ours" "$SYSROOT/lib/malloc-ours"
fi

# The compiler's own runtime is built for a CPU with an FPU, so the integer and soft-float
# routines come from compiler-rt, and the 128-bit long double ones (which compiler-rt lacks
# on a 32-bit CPU) from libgcc's soft-fp.
if [ ! -f "$SYSROOT/lib/libcompiler_rt.a" ]; then
    echo "== compiler runtime"
    if [ ! -d compiler-rt ]; then
        curl -sSL "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM/compiler-rt-$LLVM.src.tar.xz" -o rt.tar.xz
        mkdir compiler-rt && tar -xf rt.tar.xz -C compiler-rt --strip-components=1 && rm rt.tar.xz
    fi
    if [ ! -d soft-fp ]; then
        mkdir soft-fp
        base=https://raw.githubusercontent.com/gcc-mirror/gcc/$GCC
        for f in soft-fp.h op-1.h op-2.h op-4.h op-8.h op-common.h single.h double.h quad.h \
                 addtf3.c subtf3.c multf3.c divtf3.c negtf2.c eqtf2.c getf2.c letf2.c unordtf2.c \
                 extendsftf2.c extenddftf2.c trunctfsf2.c trunctfdf2.c fixtfsi.c fixtfdi.c \
                 fixunstfsi.c fixunstfdi.c floatsitf.c floatditf.c floatunsitf.c floatunditf.c; do
            curl -sSfL "$base/libgcc/soft-fp/$f" -o "soft-fp/$f"
        done
        curl -sSfL "$base/libgcc/config/riscv/sfp-machine.h" -o soft-fp/sfp-machine.h
        curl -sSfL "$base/include/longlong.h" -o soft-fp/longlong.h
    fi
    rm -rf rt-obj && mkdir rt-obj
    skipped=
    for f in compiler-rt/lib/builtins/*.c; do
        n=$(basename "$f" .c)
        case $n in
            *tf[0-9]|*tf|*tfsi|*tfdi|*tfti|*xf*|*bf*|atomic*|emutls|enable_execute_stack|eprintf|gcc_personality_v0|\
            clear_cache|cpu_model|os_version_check|trampoline_setup|apple_versioning|mingw_fixfloat) continue ;;
        esac
        rv32-cc -O2 -w -fno-builtin -c "$f" -o "rt-obj/$n.o" 2>/dev/null || skipped="$skipped $n"
    done
    for f in soft-fp/*.c; do
        rv32-cc -O2 -w -fno-builtin -Isoft-fp -c "$f" -o "rt-obj/sfp_$(basename "$f" .c).o"
    done
    [ -z "$skipped" ] || echo "   not built:$skipped"
    rm -f "$SYSROOT/lib/libcompiler_rt.a"
    riscv32-linux-ar rcs "$SYSROOT/lib/libcompiler_rt.a" rt-obj/*.o
fi
# Faster 64-bit division and doubles in place of compiler-rt's members (rt/ beside this file), each measured in
# instructions a call under qemu and checked bit for bit against the host (rt/test_rt.c, rt/test_df.c, rt/fpcheck.c):
# - rt/divdi3.c: __udivdi3, __divdi3, __umoddi3, __moddi3, __udivmoddi4, __divmoddi4 by two steps of the 32-bit divide
#   (compiler-rt's go a bit a turn: a 64-bit value by a 32-bit one 1,028 instructions, now 80);
# - rt/dfast.c: a double's sum, difference and product on 32-bit words for normal operands (a sum 155 instructions in
#   compiler-rt, 96 in libgcc's soft-fp, some 80 here; a product 107, 81), everything else by the slow routines:
#   soft-fp's sum (__adddf3_slow) and compiler-rt's product (__muldf3_slow); and an int or unsigned made a double
#   (__floatsidf, __floatunsidf: compiler-rt's some 70, these some 20);
# - libgcc's soft-fp for the comparisons and the conversions to and from 64-bit integers (compiler-rt: __floatdidf
#   513, __fixdfdi 368, __fixsfdi 324; soft-fp: 99, 29, 25), built with no rounding mode read (-U__riscv_flen: round
#   to nearest, as compiler-rt). compiler-rt's quotient and the other conversions stay, being the faster.
# Put in again when the library or a file is newer.
RT_SFP="eqdf2 gedf2 ledf2 unorddf2 fixdfdi fixunsdfdi floatdidf floatundidf fixsfdi fixunssfdi floatdisf floatundisf adddf3"
RT_THEIRS="udivdi3.o umoddi3.o divdi3.o moddi3.o udivmoddi4.o divmoddi4.o adddf3.o subdf3.o muldf3.o floatsidf.o floatunsidf.o comparedf2.o fixdfdi.o
           fixunsdfdi.o floatdidf.o floatundidf.o fixsfdi.o fixunssfdi.o floatdisf.o floatundisf.o sfp_subdf3.o"
if [ ! -f "$SYSROOT/lib/rt-ours" ] || [ -n "$(find "$OURS/rt" -newer "$SYSROOT/lib/rt-ours")" ] || [ "$SYSROOT/lib/libcompiler_rt.a" -nt "$SYSROOT/lib/rt-ours" ]; then
    echo "== 64-bit division, doubles"
    base=https://raw.githubusercontent.com/gcc-mirror/gcc/$GCC/libgcc/soft-fp
    mkdir -p soft-fp-df   # (not in soft-fp/, whose every file the runtime's build above takes)
    for f in $RT_SFP; do
        [ -f "soft-fp-df/$f.c" ] || curl -sSfL "$base/$f.c" -o "soft-fp-df/$f.c"
    done
    rm -rf rt-ours && mkdir rt-ours
    rv32-cc -O2 -c "$OURS/rt/divdi3.c" -o rt-ours/divdi3.o
    rv32-cc -O2 -c "$OURS/rt/dfast.c" -o rt-ours/dfast.o
    for f in $RT_SFP; do
        rv32-cc -O2 -w -fno-builtin -U__riscv_flen -D__adddf3=__adddf3_slow -Isoft-fp -c "soft-fp-df/$f.c" -o "rt-ours/sfp_$f.o"
    done
    rv32-cc -O2 -w -fno-builtin -D__muldf3=__muldf3_slow -c compiler-rt/lib/builtins/muldf3.c -o rt-ours/slow_muldf3.o
    riscv32-linux-ar d "$SYSROOT/lib/libcompiler_rt.a" $RT_THEIRS $(cd rt-ours && ls) 2>/dev/null || true
    riscv32-linux-ar rs "$SYSROOT/lib/libcompiler_rt.a" rt-ours/*.o
    touch "$SYSROOT/lib/rt-ours"
fi
echo "$ARCH_FLAGS" > "$SYSROOT/lib/arch-flags"
