#!/usr/bin/env bash
# A C++ compiler with the whole of libstdc++, exceptions and run-time types, for musl on RV32IMAF
# with floats in integer registers: GCC 13.3 built for riscv32-linux-musl over the toolchain's
# binutils and $SYSROOT's musl. Position-independent by default (flat programs need it), and with
# emulated thread-local storage (a flat program's loader sets up none). OpenTTD's (docs/games/openttd.md).
#   sourced by linux/openttd/build.sh; once, about ten minutes; leaves $CXXTC/bin/riscv32-linux-musl-g++
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../userland/toolchain.sh"
CXXTC=${CXXTC:-$HOME/shaderemu-linux/cxxtc}
CXXTC_GCC=13.3.0
CXXTC_LIBS="libgcc libstdc++-v3 libatomic"   # (libatomic: 64-bit atomics, which RV32 has no instructions for)
T=riscv32-linux-musl
if [ ! -x "$CXXTC/bin/$T-g++" ] || [ "$(cat "$CXXTC/stamp" 2>/dev/null)" != "$CXXTC_GCC $CXXTC_LIBS $ARCH_FLAGS" ]; then
    echo "== GCC $CXXTC_GCC for $T (once)"
    S=$HOME/shaderemu-linux/src
    mkdir -p "$S" "$CXXTC/bin" "$CXXTC/$T/bin" "$CXXTC/sysroot"
    [ -d "$S/gcc-$CXXTC_GCC" ] || { curl -sSfL "https://ftp.gnu.org/gnu/gcc/gcc-$CXXTC_GCC/gcc-$CXXTC_GCC.tar.xz" | tar -xJ -C "$S"; }
    (cd "$S/gcc-$CXXTC_GCC" && [ -e gmp ] || ./contrib/download_prerequisites > /dev/null)
    # the toolchain's binutils under the target's names
    for b in as ld ar ranlib nm objcopy objdump strip readelf; do
        ln -sfn "$(command -v riscv32-linux-$b)" "$CXXTC/bin/$T-$b"
        ln -sfn "$(command -v riscv32-linux-$b)" "$CXXTC/$T/bin/$b"
    done
    # musl's headers and libraries, as the toolchain built them (the sysroot's include and lib)
    ln -sfn "$HOME/shaderemu-linux/sysroot/include" "$CXXTC/sysroot/include"
    ln -sfn "$HOME/shaderemu-linux/sysroot/lib" "$CXXTC/sysroot/lib"
    B=$S/gcc-$CXXTC_GCC-build; rm -rf "$B"; mkdir -p "$B"
    TF="-O2 -fno-stack-protector -ffp-contract=off"
    (cd "$B" && "$S/gcc-$CXXTC_GCC/configure" --target=$T --prefix="$CXXTC" --with-sysroot="$CXXTC/sysroot" \
        --with-native-system-header-dir=/include --with-arch=rv32imaf --with-abi=ilp32 --disable-multilib \
        --enable-languages=c,c++ --disable-shared --enable-static --disable-tls --enable-default-pie --enable-threads=posix \
        --disable-libsanitizer --disable-libssp --disable-libquadmath --disable-libgomp --disable-libitm --disable-libvtv \
        --disable-libstdcxx-pch --disable-nls --disable-bootstrap --disable-werror --enable-libstdcxx-time=yes \
        CFLAGS_FOR_TARGET="$TF" CXXFLAGS_FOR_TARGET="$TF" > "$B/configure.log" 2>&1)
    make -s -C "$B" -j"$(nproc)" all-gcc $(printf ' all-target-%s' $CXXTC_LIBS) > "$B/make.log" 2>&1 || { tail -30 "$B/make.log"; exit 1; }
    make -s -C "$B" install-gcc $(printf ' install-target-%s' $CXXTC_LIBS) > "$B/install.log" 2>&1
    rm -rf "$B"
    echo "$CXXTC_GCC $CXXTC_LIBS $ARCH_FLAGS" > "$CXXTC/stamp"
fi

# The drivers, ottd-cc and ottd-c++: a flat program where the work folder is no-MMU Linux's
# (linux/nommu/setup.sh put flatcrt.o in its sysroot), else a static one; either way with the
# unwinder's marks round .eh_frame (ehframe.c) and musl's libc of $SYSROOT.
G=$CXXTC/bin/$T-
EH=$WORK/src/ottd-eh
mkdir -p "$EH"
OTTD_FLAGS="-fno-stack-protector -ffp-contract=off -fPIE \${RV32_MARCH:+-march=\$RV32_MARCH}"
# Every global hidden, declarations too (nommu_hidden.h, as rv32-cc): the program is one static image, so a global
# declared extern is reached PC-relative, not through the GOT, one load less at every use (the flat build alone)
HIDE=; [ -f "$SYSROOT/include/nommu_hidden.h" ] && HIDE="-include $SYSROOT/include/nommu_hidden.h"
for k in begin end; do
    D=; [ $k = begin ] && D=-DEH_BEGIN
    ${G}gcc -O2 -fPIE -fno-asynchronous-unwind-tables -fno-unwind-tables $D -c "$(dirname "${BASH_SOURCE[0]}")/ehframe.c" -o "$EH/eh$k.o"
done
if [ -f "$SYSROOT/lib/flatcrt.o" ]; then
    START="-static -Wl,-pie,--no-dynamic-linker,-z,text $SYSROOT/lib/flatcrt.o"; FORK="-include $SYSROOT/include/nommu_fork.h"
else
    START="-static -no-pie $SYSROOT/lib/crt1.o"; FORK=
fi
for d in cc c++; do
    # (C++ keeps fork: the header's vfork has no C linkage there, and OpenTTD forks only for a browser)
    C=gcc CXXLIB= F=$FORK; [ $d = c++ ] && C=g++ CXXLIB=-lstdc++ F=
    cat > "$WORK/bin/ottd-$d.new" <<EOF
#!/usr/bin/env bash
link=1; args=()
for a in "\$@"; do
    case "\$a" in -c|-S|-E|-M|-MM) link=0 ;; esac
    case "\$a" in -fno-pie|-fno-PIE|-no-pie|-fno-pic|-fno-PIC) ;; *) args+=("\$a") ;; esac
done
[ \$link = 1 ] && exec ${G}$C $OTTD_FLAGS $START -nostdlib -L$SYSROOT/lib $SYSROOT/lib/crti.o $EH/ehbegin.o "\${args[@]}" \\
    $CXXLIB -Wl,--start-group -lc -lgcc -latomic -lcompiler_rt -Wl,--end-group $SYSROOT/lib/crtn.o $EH/ehend.o
exec ${G}$C $OTTD_FLAGS $F $HIDE "\${args[@]}"
EOF
    # (kept with its time when the same: a newer driver has build.sh configure again)
    cmp -s "$WORK/bin/ottd-$d.new" "$WORK/bin/ottd-$d" && rm "$WORK/bin/ottd-$d.new" || mv "$WORK/bin/ottd-$d.new" "$WORK/bin/ottd-$d"
    chmod +x "$WORK/bin/ottd-$d"
done
