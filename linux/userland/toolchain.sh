#!/usr/bin/env bash
# Builds the C toolchain for programs in the Linux image: musl, static, for RV32IMAF (the
# machine's float instructions, docs/fpu.md), with floats passed in integer registers. Sourced
# by the build scripts next to it, after linux/kernel/build.sh (docs/nanox.md).
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
echo "$ARCH_FLAGS" > "$SYSROOT/lib/arch-flags"
