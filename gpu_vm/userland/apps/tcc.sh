#!/usr/bin/env bash
# TinyCC for gpu_vm (gpu_vm/userland/apps/tcc/README.md): what linux/tcc/build.sh and linux/nommu/root.sh make of it for
# RISC-V, with gpu_vm's back end (tcc/gpuvm-gen.c and the rest, put into jrrk2's tree by tcc/gpuvm_hook.py). A compiler
# for this computer (the runtime, pack.py), the headers and libraries a program is compiled against (files of the ROM:
# /usr/include, /usr/lib), the guest's compiler, elf2flt, cc and example. Run by build.sh as ../gpu_vm/userland/apps/tcc.sh,
# after nanox, libmedia and hlsl.sh.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
source "$REPO/linux/userland/toolchain.sh"
T=$REPO/linux/tcc
SRC=$WORK/src/tinycc
MW=$WORK/src/microwindows/src
ROOT=$REPO/build/images/linux/root
GL=$REPO/programs/linux
PATHS="--sysincludepaths=/usr/include:/usr/lib/tcc/include --libpaths=/usr/lib:/usr/lib/tcc --crtprefix=/usr/lib --tccdir=/usr/lib/tcc"

[ -d "$SRC" ] || git clone -q -b riscv32 https://github.com/jrrk2/tinycc "$SRC"
git -C "$SRC" log --oneline -1
python3 "$T/fpu_hook.py" "$SRC"; python3 "$T/speed_hook.py" "$SRC"; python3 "$T/pass_hook.py" "$SRC"
python3 "$HERE/tcc/gpuvm_hook.py" "$SRC"

# config.h: gpu_vm as a variant of riscv32 (gpuvm_hook.py), no bounds checks or backtraces
gpuvm_config() {
    grep -q TCC_TARGET_GPUVM config.h || sed -i 's/^#define TCC_TARGET_RISCV32 1$/#define TCC_TARGET_GPUVM 1\n#define TCC_TARGET_RISCV32 1\n#define CONFIG_TCC_BCHECK 0\n#define CONFIG_TCC_BACKTRACE 0/' config.h
    grep -q TCC_TARGET_GPUVM config.h || { echo "tcc.sh: config.h has no TCC_TARGET_RISCV32 line"; exit 1; }
}

# 1. the compiler for this computer, and its runtime (TinyCC's builtins compiled by it)
echo "== cross compiler, and the runtime"
C=$WORK/build/tcc-gvm-cross
mkdir -p "$C" && cd "$C"
"$SRC/configure" --cpu=riscv32 --triplet=riscv32-linux-musl --config-musl $PATHS > configure.log
gpuvm_config
rm -f tcc tcc.o libtcc1.a ./*.o
make tcc ONE_SOURCE=yes > make.log 2>&1 || { tail -20 make.log; exit 1; }
make libtcc1.a ONE_SOURCE=yes RISCV32_O=builtin.o LIN_O= >> make.log 2>&1 || { tail -20 make.log; exit 1; }
# alloca: sp moves down by the size on 16 bytes, and what is below the old sp is the block (the caller's frame is
# addressed from its fp when it has allocas)
cat > "$C/alloca.s" <<'EOF'
	.text
	.globl alloca
	.type alloca, @function
alloca:
	addi t0, a0, 15
	andi t0, t0, -16
	sub sp, sp, t0
	addi a0, sp, 0
	jr ra
	.size alloca, .-alloca
EOF
"$LLVM_BIN/clang" --target=gpuvm-unknown-linux -c "$C/alloca.s" -o "$C/alloca.o"

# 2. what a program is compiled against: headers and libraries, files of the ROM
echo "== headers and libraries"
STAGE=$WORK/build/tcc-gvm-sysroot
rm -rf "$STAGE" && mkdir -p "$STAGE/usr/include" "$STAGE/usr/lib/tcc/include"
cp -r "$SYSROOT/include/." "$STAGE/usr/include/"
(cd "$STAGE/usr/include" && rm -rf linux drm rdma sound asm asm-generic mtd scsi video xen misc cxl)
cp "$SRC"/include/*.h "$STAGE/usr/lib/tcc/include/"
# TinyCC links its runtime last, after the C library: compiler-rt's builtins (64-bit division, double in software,
# which the C library needs too) go into that archive, with TinyCC's own and alloca
cp "$SYSROOT/lib/libclang_rt.builtins-gpuvm.a" "$STAGE/usr/lib/tcc/libtcc1.a"
(mkdir -p "$WORK/build/tcc1-gvm" && cd "$WORK/build/tcc1-gvm" && rm -f ./*.o && riscv32-linux-ar x "$C/libtcc1.a" &&
    for o in *.o; do mv "$o" "tcc1_$o"; done && cp "$C/alloca.o" tcc1_alloca.o && riscv32-linux-ar rs "$STAGE/usr/lib/tcc/libtcc1.a" tcc1_*.o)
cp "$SYSROOT"/lib/crt1.o "$SYSROOT"/lib/crti.o "$SYSROOT"/lib/crtn.o "$SYSROOT"/lib/libc.a "$SYSROOT"/lib/libm.a "$STAGE/usr/lib/"
cp "$MW/src/lib/libnano-X.a" "$STAGE/usr/lib/" 2>/dev/null || cp "$MW/lib/libnano-X.a" "$STAGE/usr/lib/"
cp "$MW/include/nano-X.h" "$MW/include/mwtypes.h" "$MW/include/mwconfig.h" "$STAGE/usr/include/" 2>/dev/null || true
mkdir -p "$STAGE/usr/include/GLES"
cp "$GL"/include/GLES/*.h "$STAGE/usr/include/GLES/"
rv32-cc -O2 -Wall -c -DSEGL_FLOAT -idirafter "$GL/include" -I"$MW/include" "$GL/gles.c" -o "$WORK/build/gles-tcc.o"
rm -f "$STAGE/usr/lib/libgles.a"; riscv32-linux-ar rcs "$STAGE/usr/lib/libgles.a" "$WORK/build/gles-tcc.o"
rv32-cc -O2 -Wall -c -I"$REPO/programs/mc" "$REPO/programs/mc/mcw.c" -o "$WORK/build/mcw-tcc.o"
rv32-cc -O2 -Wall -c -I"$REPO/programs/mc" "$REPO/programs/mc/mcwx.c" -o "$WORK/build/mcwx-tcc.o"
rm -f "$STAGE/usr/lib/libmcw.a"; riscv32-linux-ar rcs "$STAGE/usr/lib/libmcw.a" "$WORK/build/mcw-tcc.o" "$WORK/build/mcwx-tcc.o"
cp "$REPO/programs/mc/mcw.h" "$REPO/programs/mc/mcwx.h" "$REPO/programs/mc/mc.h" "$STAGE/usr/include/"
# the HLSL compiler's library (hlsl.sh, run before)
cp "$WORK/src/hlsl/libhlsl.a" "$STAGE/usr/lib/" && cp "$REPO/programs/hlsl/hlsl.h" "$STAGE/usr/include/"
if [ -f "$WORK/src/libmedia/libmedia.a" ]; then
    cp "$WORK/src/libmedia/libmedia.a" "$STAGE/usr/lib/" && cp "$REPO/linux/libmedia/media.h" "$STAGE/usr/include/"
fi
mkdir -p "$WORK/build/tcc-gvm-guest"
python3 "$T/pack.py" "$C/tcc" "$STAGE" "$T/examples" "$WORK/build/tcc-gvm-guest/packed.h"
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/share" "$ROOT/usr/lib"
rm -rf "$ROOT/usr/include"
(cd "$STAGE/usr/lib" && for f in *; do rm -rf "$ROOT/usr/lib/$f"; done)
cp -rL "$STAGE/usr/include" "$ROOT/usr/" && cp -rL "$STAGE/usr/lib/." "$ROOT/usr/lib/"

# 3. the same compiler for the machine (the libraries read from memory: mapped.c)
echo "== the guest's compiler"
G=$WORK/build/tcc-gvm-guest
cd "$G"
rv32-cc -O2 -Wall -I. -c "$T/mapped.c" -o mapped_files.o
"$SRC/configure" --cpu=riscv32 --triplet=riscv32-linux-musl --config-musl $PATHS \
    --cc=rv32-cc --ar=riscv32-linux-ar --extra-cflags="-O2" \
    "--extra-ldflags=-Wl,--wrap=open,--wrap=read,--wrap=lseek,--wrap=close $PWD/mapped_files.o" > configure.log
gpuvm_config
cp "$C/c2str.exe" . && cp "$C/tccdefs_.h" . && touch tccdefs_.h
rm -f tcc tcc.o libtcc.a libtcc.o tccpp.o tccgen.o tccelf.o tccasm.o tccdbg.o tccrun.o riscv32-*.o gpuvm-*.o
make tcc ONE_SOURCE=yes > make.log 2>&1 || { tail -20 make.log; exit 1; }
riscv32-linux-strip -o "$ROOT/usr/bin/tcc" tcc
rv32-cc -O2 -Wall "$HERE/tcc/elf2flt.c" -o "$G/elf2flt"
riscv32-linux-strip -o "$ROOT/usr/bin/elf2flt" "$G/elf2flt"
ls -l "$ROOT/usr/bin/tcc"; echo "$(find "$ROOT/usr/include" "$ROOT/usr/lib" -type f | wc -l) files, $(du -sh "$STAGE/usr" | cut -f1)"

# 4. cc (linux/nommu/cc for gpu_vm: musl's crt1.o has the flat program's start) and example
sed -e 's#/usr/lib/flatcrt.o /usr/lib/crti.o#/usr/lib/crt1.o /usr/lib/crti.o#' "$REPO/linux/nommu/cc" | tr -d '\r' > "$ROOT/usr/bin/cc"
grep -q '/usr/lib/crt1.o /usr/lib/crti.o' "$ROOT/usr/bin/cc" || { echo "tcc.sh: linux/nommu/cc's link line moved"; exit 1; }
tr -d '\r' < "$T/example" > "$ROOT/usr/bin/example"
chmod +x "$ROOT/usr/bin/cc" "$ROOT/usr/bin/example"
for f in "$T"/examples/*.c "$T"/examples/*.hlsl; do cp "$f" "$ROOT/usr/share/example-$(basename "$f")"; done
printf '%s\n' "Programming/Cube (C example)=nxide /usr/share/example-cube.c" "Programming/Home (C example)=nxide /usr/share/example-home.c" \
    "Programming/Shader on 255 cores (C example)=nxide /usr/share/example-shader.c" \
    "Programming/Fractal in HLSL (C example)=nxide /usr/share/example-fractal.c" > "$ROOT/usr/share/nxapps.30-examples"
