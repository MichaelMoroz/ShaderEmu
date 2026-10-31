#!/usr/bin/env bash
# A build folder and a copy of the repository for no-MMU Linux: the same build scripts, with compiler
# drivers that make every program position-independent and link it to be loaded as a flat program.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
NM=$HOME/nm
SH=$HOME/shaderemu-linux
W=$NM/work
mkdir -p "$NM" "$W/bin" "$W/src"
T0=$(date +%s); step() { echo "== [$(( $(date +%s) - T0 )) s] $*"; }

step "repository copy"
rsync -a --delete --exclude /build --exclude /.git --exclude /experiments --exclude /logs --exclude /unity --exclude /rvc "$REPO/" "$NM/repo/"
mkdir -p "$NM/repo/build/images/linux"
rsync -a --delete "$REPO/build/images/linux/root/" "$NM/repo/build/images/linux/root/"
# the toolchain's own drivers are written under other names: ours below are the ones on the path
sed -i 's|cat > "\$WORK/bin/rv32-cc" <<EOF|cat > "$WORK/bin/rv32-cc.mmu" <<EOF|; s|chmod +x "\$WORK/bin/rv32-cc"$|chmod +x "$WORK/bin/rv32-cc.mmu"|' "$NM/repo/linux/userland/toolchain.sh"
sed -i 's|cat > "\$WORK/bin/rv32-c++" <<EOF|cat > "$WORK/bin/rv32-c++.mmu" <<EOF|; s|chmod +x "\$WORK/bin/rv32-c++"$|chmod +x "$WORK/bin/rv32-c++.mmu"|' "$NM/repo/linux/tdawn/build.sh"
grep -c 'rv32-cc.mmu' "$NM/repo/linux/userland/toolchain.sh"; grep -c 'rv32-c++.mmu' "$NM/repo/linux/tdawn/build.sh"

step "build folder"
for d in toolchain tools linux; do ln -sfn "$SH/$d" "$W/$d"; done
[ -d "$W/sysroot" ] || cp -a "$SH/sysroot" "$W/sysroot"
for d in busybox microwindows ioq3 micropolis micropolis-build ClassiCube classicube-sound imgui ImGuizmo esp32-nesemu nes-roms tinycc \
         vanilla-conquer vanilla-conquer-td vanilla-conquer-ra tdawn-demo ralert-demo zlib-1.3.1 soft-fp compiler-rt musl libmedia quake quake-data \
         quake3-data pathtrace-data tinybvh bvhc play video ide apps emuinit emumux fptest sndtest InfoNES; do
    [ -e "$SH/src/$d" ] && [ ! -e "$W/src/$d" ] && cp -a "$SH/src/$d" "$W/src/$d"
done
for f in doom-shareware.zip doom1.wad tdawn-demo.zip ralert-demo.zip zlib-1.3.1.tar.gz; do
    [ -e "$SH/src/$f" ] && [ ! -e "$W/src/$f" ] && cp -a "$SH/src/$f" "$W/src/$f"
done
# (tcc's hooks skip a file with their mark: the copy's source is upstream's, for these hooks to patch)
git -C "$W/src/tinycc" checkout -q -- . 2>/dev/null
# nothing compiled for the MMU machine is reused
# (pathtrace-data's .obj are the example meshes, not objects: the teapot's address is gone from the net)
find "$W/src" \( -name '*.o' -o -name '*.lo' -o -name '*.obj' \) ! -path '*/pathtrace-data/*' -delete
find "$W/src" -name '*.a' ! -path '*/musl/*' -delete
du -sh "$W/src"

step "start file, the fork header and the drivers"
S=$W/sysroot
GCCINC=$("$SH/toolchain/bin/riscv32-linux-gcc" -print-file-name=include)
# (-ffile-prefix-map: a __FILE__ says /nm/..., wherever the build folder is: client.c's assert put this folder's name
# in Nano-X's strings, and every window program's bytes went by it)
BASE="-mabi=ilp32 -fno-stack-protector -ffp-contract=off -ffile-prefix-map=$NM=/nm"
ARCH="-march=\${RV32_MARCH:-rv32imaf} $BASE"   # (RV32_MARCH: rv32imaf_zba_zbb for a Zb build)
# What toolchain.sh put into the libraries with the drivers before these is put in again by those below (a libc.a
# with its memcpy and libm of 20 October's drivers, globals not hidden and through the GOT, otherwise stays), into
# musl's own libc.a: put into one that has them, the members would lie in another order (and renamed ones stay),
# and the guest's linker, which goes by the index, would write other programs.
[ -f "$W/src/musl/lib/libc.a" ] && cp "$W/src/musl/lib/libc.a" "$S/lib/libc.a"
rm -f "$S/lib/memcpy-ours" "$S/lib/libm-ours" "$S/lib/vfprintf-ours" "$S/lib/rt-ours"
cp "$HERE/flatcrt.c" "$W/flatcrt.c"
"$SH/toolchain/bin/riscv32-linux-gcc" -march=rv32imaf $BASE -O2 -fPIC -nostdinc -isystem "$S/include" -isystem "$GCCINC" -c "$W/flatcrt.c" -o "$S/lib/flatcrt.o" || exit 1
cat > "$S/include/nommu_fork.h" <<'EOF'
/* No MMU: a fork is a vfork, whose child runs on its parent's memory until it execs or exits. */
#ifndef __ASSEMBLER__
int vfork(void) __attribute__((returns_twice));	/* (pid_t is int: no header, which autoconf's select test and gnulib meet) */
#define fork vfork
#endif
EOF
# Every global hidden, declarations too: a program is one static image, so a global declared `extern` is the
# program's own and is reached PC-relative (auipc and its use), not through the GOT (auipc, a load of its address,
# then its use). -fvisibility=hidden covers only what a file defines; the pragma covers what it declares. Not in
# a profile-guided C++ build (-fprofile-generate, -fprofile-use: Tiberian Dawn's and Red Alert's): GCC 13.3 stopped there
# on an internal error (a segmentation fault in TD's expand.cpp). C is (Quake's, Quake III's and Doom's).
cat > "$S/include/nommu_hidden.h" <<'EOF'
#ifndef __ASSEMBLER__
#pragma GCC visibility push(hidden)
#endif
EOF
FILTER='args=(); link=1; hidden="-include nommu_hidden.h"; hiddenxx=$hidden
for a in "$@"; do
    case "$a" in -c|-S|-E|-M|-MM) link=0 ;; -fprofile-use*|-fprofile-generate*) hiddenxx= ;; esac
    case "$a" in -fno-pie|-fno-PIE|-no-pie|-fno-pic|-fno-PIC|-Wl,-Ttext-segment=*) ;; *) args+=("$a") ;; esac
done'
LINK="-static -Wl,-pie,--no-dynamic-linker,-z,text -nostdlib -L$S/lib $S/lib/flatcrt.o $S/lib/crti.o"
LIBS="-Wl,--start-group -lc -lcompiler_rt -Wl,--end-group $S/lib/crtn.o"
cat > "$W/bin/rv32-cc" <<EOF
#!/usr/bin/env bash
$FILTER
inc="-nostdinc -isystem $S/include -isystem $GCCINC -include nommu_fork.h \$hidden"
[ \$link = 1 ] && exec riscv32-linux-gcc $ARCH -fPIE \$inc $LINK "\${args[@]}" $LIBS
exec riscv32-linux-gcc $ARCH -fPIE \$inc "\${args[@]}"
EOF
CXXINC="-nostdinc -nostdinc++ -isystem $REPO/linux/tdawn/cxx -isystem $SH/toolchain/riscv32-buildroot-linux-gnu/include/c++/13.3.0 -isystem $SH/toolchain/riscv32-buildroot-linux-gnu/include/c++/13.3.0/riscv32-buildroot-linux-gnu -isystem $S/include -isystem $GCCINC"
CXXF="-fno-exceptions -fno-rtti -fno-threadsafe-statics '-D__GLIBC_PREREQ(a,b)=0'"
cat > "$W/bin/rv32-c++" <<EOF
#!/usr/bin/env bash
$FILTER
[ \$link = 1 ] && exec riscv32-linux-g++ $ARCH $CXXF -fPIE $CXXINC \$hiddenxx $LINK "\${args[@]}" $LIBS
exec riscv32-linux-g++ $ARCH $CXXF -fPIE $CXXINC \$hiddenxx "\${args[@]}"
EOF
chmod +x "$W/bin/rv32-cc" "$W/bin/rv32-c++"
printf 'int main(void) { return fork() == 0 ? 0 : 0; }\n' > "$W/t.c"
PATH="$SH/toolchain/bin:$PATH" "$W/bin/rv32-cc" -O2 -no-pie "$W/t.c" -o "$W/t" && "$SH/toolchain/bin/riscv32-linux-readelf" -h "$W/t" | grep Type
step done
