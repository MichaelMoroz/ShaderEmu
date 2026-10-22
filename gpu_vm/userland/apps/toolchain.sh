#!/usr/bin/env bash
# gpu_vm's stand-in for linux/userland/toolchain.sh (gpu_vm/userland/apps/README.md): sourced by the RISC-V machine's
# build scripts, run from the copy of the repository that gpu_vm/userland/apps/build.sh makes, it gives them the names
# they call (rv32-cc, rv32-c++, riscv32-linux-ar, -strip, -nm, -objcopy, -ld ...) over gpu_vm's compiler
# (gpu_vm/userland/bin/gpuvm-linux-cc: clang --target=gpuvm-unknown-linux, musl, bFLT). A program comes out a bFLT
# file directly; its linked ELF is kept beside it as PROG.gdb (GVM_KEEP_ELF), which the nm here reads for a bFLT.
# Options of the RISC-V compiler that mean nothing here (-march, -mabi, gcc's --param and tree passes, -flto, a
# text segment's address) are dropped; -ffp-contract=off is dropped (gpu_vm's fmad rounds the product, as a separate
# fmul and fadd would: the same results); GVM_MATH is added to every compilation (gpu_vm/llvm/README.md).
set -euo pipefail
GVM_UL=${GVM_UL:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}       # gpu_vm/userland (of the copy)
WORK=${GVM_APPS_WORK:-$HOME/gpu_vm/apps-work}
SYSROOT=${GVM_SYSROOT:-$HOME/gpu_vm/sysroot}
LLVM_BIN=${LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}
export WORK SYSROOT LLVM_BIN
export SOURCE_DATE_EPOCH=1704067200
# the math the programs are compiled with (gpu_vm/llvm/README.md: -ffast-math takes the card's sin, cos, exp2, log2,
# rcp and rsqrt; a program whose own checks need IEEE results sets GVM_MATH= for itself)
GVM_MATH=${GVM_MATH:--ffp-contract=fast}
ARCH_FLAGS="$GVM_MATH"
# the C++ library's headers (templates only, as on the RISC-V machine: linux/tdawn/build.sh): libstdc++ 13.3's from
# the RISC-V toolchain, a 32-bit Linux's bits
CXXINC=${GVM_CXXINC:-$HOME/nm/work/toolchain/riscv32-buildroot-linux-gnu/include/c++/13.3.0}
export PATH="$WORK/bin:$GVM_UL/bin:$PATH"
[ -x "$LLVM_BIN/clang" ] || { echo "no gpu_vm clang in $LLVM_BIN (gpu_vm/llvm/build.sh)"; exit 1; }
[ -f "$SYSROOT/lib/libc.a" ] || { echo "no gpu_vm sysroot in $SYSROOT (gpu_vm/userland/build.sh)"; exit 1; }
mkdir -p "$WORK/bin" "$WORK/src"
# musl's sources, which some builds compile for the host (imgui's font atlas: qsort)
[ -e "$WORK/src/musl" ] || ln -s "$HOME/gpu_vm/musl" "$WORK/src/musl"

# the shared filter of compiler options: gcc's and RISC-V's that clang or gpu_vm has no use for
cat > "$WORK/bin/gvm-filter" <<'EOF'
# sourced: filters "$@" into the array args
args=()
skip=
for a in "$@"; do
    if [ -n "$skip" ]; then skip=; continue; fi
    case "$a" in
    -march=*|-mabi=*|-mcmodel=*|-mtune=*|-mcpu=*|-mno-relax|-mrelax|-msmall-data-limit=*|-mstrict-align|-mno-strict-align|\
    -mplt|-mno-plt|-mexplicit-relocs|-mno-explicit-relocs|-msave-restore|-mno-save-restore|-mriscv-attribute|-mno-riscv-attribute|\
    -mno-fdiv|-mfdiv|-mdiv|-mno-div) ;;
    --param|-param) skip=1 ;;
    --param=*|-fsched-pressure|-fno-sched-pressure|-ffat-lto-objects|-fno-fat-lto-objects|-flto|-flto=*|-fno-lto|\
    -fuse-linker-plugin|-fno-tree-*|-ftree-*|-fno-ipa-*|-fipa-*|-fno-gcse*|-fgcse*|-fno-schedule-insns*|-fschedule-insns*|\
    -fno-ira-*|-fira-*|-fno-code-hoisting|-fno-early-inlining|-fallow-store-data-races|-fno-allow-store-data-races|\
    -fno-semantic-interposition|-fno-tree-loop-distribute-patterns|-fno-inline-small-functions|-finline-small-functions|\
    -fno-reorder-blocks*|-freorder-blocks*|-fno-split-wide-types|-fno-var-tracking*|-fmerge-all-constants|-fno-peephole2|\
    -fno-move-loop-invariants|-fno-crossjumping|-fno-shrink-wrap|-fno-guess-branch-probability|-fno-ipa-cp|-fopt-info*|\
    -fno-pie|-fno-PIE|-fno-pic|-fno-PIC|-fpie|-fPIE|-fpic|-fPIC|-ffp-contract=off|-fexcess-precision=*|-fno-stack-clash-protection|\
    -fno-printf-return-value|-fno-isolate-erroneous-paths-*|-fvect-cost-model=*|-fno-ssa-phiopt|\
    -flimit-function-alignment|-falign-functions=*|-falign-loops=*|-falign-jumps=*|-falign-labels=*|-fno-align-*|\
    -Wl,-Ttext-segment=*|-Wl,-Ttext=*|-Wl,--relax|-Wl,--no-relax|-Wl,-z,norelro|-Wl,--hash-style=*|-Wl,--build-id*|-no-pie|-static-pie) ;;
    -Wl,--print-memory-usage|-Wl,-Map=*|-Wl,-Map,*) ;;
    *) args+=("$a") ;;
    esac
done
EOF

# rv32-cc: gpuvm-linux-cc with the options filtered; every link keeps its ELF (PROG.gdb, for nm and profiles)
cat > "$WORK/bin/rv32-cc" <<EOF
#!/usr/bin/env bash
. "$WORK/bin/gvm-filter"
export GVM_KEEP_ELF=1
exec "$GVM_UL/bin/gpuvm-linux-cc" -Wno-unknown-warning-option -Wno-ignored-optimization-argument -Qunused-arguments \
    -Wno-unused-command-line-argument -Wno-error=implicit-function-declaration -Wno-error=int-conversion \
    -Wno-error=incompatible-pointer-types -Wno-error=implicit-int $GVM_MATH "\${args[@]+"\${args[@]}"}"
EOF
# rv32-c++: C++ with libstdc++'s headers (templates only), no exceptions or run-time types, as the RISC-V machine's
# (a program brings its runtime: linux/tdawn/cxxrt.cpp); sources compiled here, the link gpuvm-linux-cc's
cat > "$WORK/bin/rv32-c++" <<EOF
#!/usr/bin/env bash
. "$WORK/bin/gvm-filter"
export GVM_KEEP_ELF=1
RES=\$("$LLVM_BIN/clang" -print-resource-dir)
CXX=("$LLVM_BIN/clang++" --target=gpuvm-unknown-linux -fPIE -nostdinc -nostdinc++ -isystem "\${GVM_CXX_EXTRA:-/nonexistent}"
     -isystem "$CXXINC" -isystem "$CXXINC/riscv32-buildroot-linux-gnu" -isystem "$CXXINC/backward" -isystem "$SYSROOT/include"
     -isystem "\$RES/include" -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit "-D__GLIBC_PREREQ(a,b)=0"
     -Wno-unknown-warning-option -Wno-ignored-optimization-argument -Qunused-arguments -Wno-unused-command-line-argument
     -Wno-register -Wno-c++11-narrowing $GVM_MATH)
link=1 out=
for a in "\${args[@]+"\${args[@]}"}"; do case "\$a" in -c|-S|-E|-M|-MM|-fsyntax-only) link=0 ;; esac; done
[ \$link = 0 ] && exec "\${CXX[@]}" "\${args[@]}"
# a link: C++ sources compiled first (each alone), then gpuvm-linux-cc links the objects
tmp=\$(mktemp -d); trap 'rm -rf "\$tmp"' EXIT
copts=() rest=() n=0 prev=
for a in "\${args[@]}"; do
    if [ -n "\$prev" ]; then
        case "\$prev" in -o|-Xlinker) rest+=("\$prev" "\$a") ;; *) copts+=("\$prev" "\$a") ;; esac; prev=; continue
    fi
    case "\$a" in
    -o|-Xlinker) prev=\$a ;;
    -I|-D|-U|-include|-isystem|-idirafter|-iquote|-x) prev=\$a ;;
    *.cpp|*.cc|*.cxx|*.C|*.c++) n=\$((n + 1)); srcs+=("\$a") ;;
    -l*|-L*|-Wl,*|-static|-nostdlib|-nostartfiles|-nodefaultlibs|-pthread|-s|-v) rest+=("\$a") ;;
    -*) copts+=("\$a") ;;
    *) rest+=("\$a") ;;
    esac
done
objs=() i=0
for s in "\${srcs[@]+"\${srcs[@]}"}"; do
    i=\$((i + 1)); o="\$tmp/\$i-\$(basename "\${s%.*}").o"
    "\${CXX[@]}" "\${copts[@]+"\${copts[@]}"}" -c "\$s" -o "\$o" || exit 1
    objs+=("\$o")
done
exec "$GVM_UL/bin/gpuvm-linux-cc" "\${copts[@]+"\${copts[@]}"}" "\${objs[@]+"\${objs[@]}"}" "\${rest[@]+"\${rest[@]}"}"
EOF
for t in gcc cc; do ln -sf rv32-cc "$WORK/bin/riscv32-linux-$t"; done
for t in g++ c++; do ln -sf rv32-c++ "$WORK/bin/riscv32-linux-$t"; done
for t in ar ranlib objcopy readelf size strings objdump; do
    printf '#!/bin/sh\nexec "%s/bin/gpuvm-linux-%s" "$@"\n' "$GVM_UL" "$t" > "$WORK/bin/riscv32-linux-$t"
done
# strip: a bFLT is copied unchanged (gpuvm-linux-strip), and its ELF goes with it as .gdb
cat > "$WORK/bin/riscv32-linux-strip" <<EOF
#!/bin/sh
exec "$GVM_UL/bin/gpuvm-linux-strip" "\$@"
EOF
# nm: of a bFLT, its ELF beside it (PROG.gdb)
cat > "$WORK/bin/riscv32-linux-nm" <<EOF
#!/bin/sh
last=; for a do last=\$a; done
if [ -f "\$last" ] && [ "\$(head -c 4 "\$last")" = bFLT ]; then
    n=\$#; i=0; for a do i=\$((i + 1)); [ \$i -lt \$n ] && set -- "\$@" "\$a"; done; shift \$n
    exec "$LLVM_BIN/llvm-nm" "\$@" "\$last.gdb"
fi
exec "$LLVM_BIN/llvm-nm" "\$@"
EOF
# ld: lld (a relocatable link, -r, is what the builds ask of it)
printf '#!/bin/sh\nexec "%s/ld.lld" "$@"\n' "$LLVM_BIN" > "$WORK/bin/riscv32-linux-ld"
chmod +x "$WORK/bin/"*
