#!/usr/bin/env bash
# Age of Empires (dcostap/aoe1-decomp on the machine, the 1997 trial's data). docs/games/aoe.md.
#   wsl -- bash linux/nommu/build.sh aoe/build.sh
# Neither the code nor the data is kept in this repository. The code is fetched here; the data only
# when an image is made (data.py, called by image.py for every image).
# AOE_TREE=keep leaves the tree as it is (then write its diff to aoe.patch). AOE_CHECK=1 only says
# what does not compile, one line a kind of error.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
UP=$WORK/src/aoe1
SRC=$WORK/src/aoe-tree
OBJ=$WORK/src/aoe-build
OUT=${AOE_OUT:-$REPO/build/aoe/out}   # (never the shared root: docs/games/aoe.md)
COMMIT=6d6d8b14023aa74e6ac30a6c88229d5b818fb805

if [ ! -d "$UP/.git" ]; then
    git clone -q --filter=blob:none --no-checkout https://github.com/dcostap/aoe1-decomp "$UP"
fi
git -C "$UP" sparse-checkout set --no-cone /src /include /third_party /LICENSE
if [ "${AOE_TREE:-}" != keep ]; then
    git -C "$UP" checkout -q -f "$COMMIT" && git -C "$UP" clean -q -fd
    [ ! -s "$HERE/aoe.patch" ] || git -C "$UP" apply --whitespace=nowarn "$HERE/aoe.patch"
fi
mkdir -p "$OBJ" "$SRC"
python3 "$HERE/tree.py" "$UP" "$SRC"
mkdir -p "$SRC/third_party"; rsync -a --delete "$UP/third_party/" "$SRC/third_party/"
# Asm_draw.asm, Inf32.asm and Drawarea_asm.asm are in port/aoe_asm.cpp; getdxver.cpp asks Windows what it has
OUTS="${AOE_OUTS:-getdxver.cpp Dsutil.c}"
COMMON="${AOE_OPT:--O2} -fno-pie -w -fsigned-char -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -fno-tree-vectorize -g -DAOE_SHADEREMU"
INC="-I$HERE/win -I$SRC/include -I$WORK/src/microwindows/src/include -I$REPO/linux/userland -idirafter $REPO/programs/linux/include"
FLAGS="$COMMON -fpermissive -std=gnu++98 -fms-extensions -fno-lifetime-dse -include $HERE/win/aoe_prefix.h $INC"
CFLAGS="$COMMON -include $HERE/win/aoe_prefix.h $INC"
list() {
    for f in "$SRC"/src/*.cpp "$SRC"/src/*.c "$HERE"/port/*.cpp "$HERE"/port/*.c; do
        [ -f "$f" ] || continue
        case " $OUTS " in *" $(basename "$f") "*) continue ;; esac
        echo "$f"
    done
}
if [ "${AOE_CHECK:-}" = 1 ]; then
    list | grep -v '\.c$' | xargs -P "$(nproc)" -I{} sh -c "rv32-c++ $FLAGS -fsyntax-only {} 2>&1 | grep -E 'error' | head -${AOE_CHECK_N:-400} || true" > "$OBJ/check.log" || true
    echo "errors: $(wc -l < "$OBJ/check.log")"
    sed -E 's/^[^ ]+: (fatal )?error: //' "$OBJ/check.log" | sed -E "s/‘|’/'/g" | sort | uniq -c | sort -rn | head -"${AOE_CHECK_TOP:-150}"
    exit 0
fi
# Objects: a file again when it, a header or the flags changed
echo "$FLAGS" > "$OBJ/flags.new"; cmp -s "$OBJ/flags.new" "$OBJ/flags" || { rm -f "$OBJ"/*.o; mv "$OBJ/flags.new" "$OBJ/flags"; }
newest=$(ls -t "$SRC"/include/*.h "$HERE"/win/*.h | sed -n 1p)   # (not head: under pipefail ls then dies of SIGPIPE, exit 141)
one() {
    f=$1; o=$OBJ/$(basename "${f%.*}").o
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$newest" -nt "$o" ]; then
        rm -f "$o"
        case "$f" in
            */window.c) rv32-cc -O2 -fno-pie -w -g -I$HERE/port -I$REPO/linux/apps -idirafter $REPO/programs/linux/include -I$WORK/src/microwindows/src/include -I$REPO/linux/userland -c "$f" -o "$o" 2>&1 | grep -E 'error' | head -20 || true ;;
            *.cpp) rv32-c++ $FLAGS -c "$f" -o "$o" 2>&1 | grep -E 'error' | head -20 || true ;;
            *) rv32-cc $CFLAGS -c "$f" -o "$o" 2>&1 | grep -E 'error' | head -20 || true ;;
        esac
    fi
    [ -f "$o" ] || echo "FAILED $f"
}
export -f one; export FLAGS CFLAGS OBJ newest HERE REPO WORK
list | xargs -P "$(nproc)" -I{} bash -c 'one {}' | tee "$OBJ/compile.log" | head -60
! grep -q . "$OBJ/compile.log" || { echo "does not compile"; exit 1; }
echo "objects: $(ls "$OBJ"/*.o | wc -l)"
# the GL library, compiled apart as in the other ports
rv32-cc -O2 -fno-pie -w -DNDEBUG -DSHADEREMU -idirafter $REPO/programs/linux/include -I$WORK/src/microwindows/src/include -I$REPO/linux/userland \
    -fno-common -fdata-sections -msmall-data-limit=0 -c "$REPO/programs/linux/gles.c" -o "$WORK/src/aoe-gles.o"
rv32-cc -O2 -fno-pie -w -idirafter $REPO/programs/linux/include -I$REPO/programs/mc -c "$REPO/programs/mc/mcw.c" -o "$WORK/src/aoe-mcw.o"
LIBS="$WORK/src/aoe-gles.o $WORK/src/aoe-mcw.o $WORK/src/microwindows/src/lib/libnano-X.a"
rv32-c++ -O2 -fno-pie -g "$OBJ"/*.o $LIBS -lm -o "$OBJ/aoe.elf" 2>"$OBJ/link.log" || true
grep -oE "undefined reference to .*" "$OBJ/link.log" | sort | uniq -c | sort -rn | head -"${AOE_CHECK_TOP:-300}" || true
grep -v "undefined reference\|in function" "$OBJ/link.log" | head -20 || true
[ -f "$OBJ/aoe.elf" ] && ! grep -q 'undefined reference' "$OBJ/link.log" || { echo "does not link"; exit 1; }
# the same objects for qemu-riscv32 (a flat program's start relies on the loader's relocations)
S="$WORK/sysroot/lib"
riscv32-linux-gcc -march=rv32imaf -mabi=ilp32 -O1 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -c "$HERE/host_mem.c" -o "$WORK/src/aoe-host-mem.o"
riscv32-linux-g++ -march=rv32imaf -mabi=ilp32 -static -no-pie -nostdlib -L"$S" -g "$S/crt1.o" "$S/crti.o" "$WORK/src/aoe-host-mem.o" "$OBJ"/*.o $LIBS \
    -lm -Wl,--start-group -lc -lcompiler_rt -Wl,--end-group "$S/crtn.o" -o "$OBJ/aoe-q.elf" 2>"$OBJ/linkq.log" || head -5 "$OBJ/linkq.log"
riscv32-linux-nm -n "$OBJ/aoe.elf" > "$WORK/src/aoe.nm"
echo "linked: $(riscv32-linux-size "$OBJ/aoe.elf" | tail -1)"

# what the port reads that the trial does not have as it is: its strings, its faces' pixels and its menus' pictures at the
# window's size, made in the machine (prep.c, port/aoe_slp.h)
rv32-cc -O2 -Wall -Wno-unused-function -I"$HERE" -I"$REPO/programs/mc" "$HERE/prep.c" "$REPO/programs/mc/mcw.c" "$REPO/programs/mc/mcwx.c" \
    -lm -o "$OBJ/aoeprep"
# Install: the program and its starter. The trial's installer comes when an image asks for it (data.py).
riscv32-linux-strip -o "$OBJ/aoe.bin" "$OBJ/aoe.elf"
mkdir -p "$OUT/usr/bin"
cp "$OBJ/aoe.bin" "$OUT/usr/bin/aoe.bin"
riscv32-linux-strip -o "$OUT/usr/bin/aoeprep" "$OBJ/aoeprep"
cat > "$OUT/usr/bin/aoe" <<'EOF'
#!/bin/sh
# Age of Empires (docs/games/aoe.md): Microsoft's trial lies decoded in the ROM (linux/aoe/data.py). Its licence is shown
# and accepted at each start (cabinst, docs/system/install.md), then the files are linked into /tmp/aoe, and its strings and
# its faces' pixels and its menus' pictures at its window's size are made from them (aoeprep); the folder is deleted when
# the game ends. An image with the installer itself (from before 4 November 2026) is unpacked as before
D=/tmp/aoe
if [ -d /usr/share/aoe/installed ]; then
    cabinst --title "Age of Empires (trial)" --licence eula.txt --key aoe.eula --installed /usr/share/aoe/installed $D || exit $?
else
    cabinst --title "Age of Empires (trial)" --licence eula.txt --key aoe.eula /usr/share/aoe/MSAoE.exe $D '*.drs' '*.ai' '*.per' \
    '*.rps' '*.cty' '*.col' '*.scn' '*.cpn' '*.ply' '*.mid' '*.wav' '*.inf' Tileedge.dat Empires.dat language.dll '*.ttf' || exit $?
fi
[ -f $D/aoe-fonts.bin ] && [ -f $D/aoe-menus.bin ] || aoeprep $D || exit 2
# (the music's instruments are not the trial's: DMXOPL's bank, in the ROM beside the installer)
[ -e $D/genmidi.op2 ] || ln -s /usr/share/aoe/genmidi.op2 $D/genmidi.op2 2>/dev/null
[ "$1" = --unpack ] && exit 0	# (only that: a test unpacks first, then plays)
P=/persistence; mkdir -p $P/aoe 2>/dev/null || P=/tmp; mkdir -p $P/aoe
# (not exec: the shell waits, and deletes what was unpacked however the game ends)
trap 'rm -rf $D' EXIT; trap 'exit 143' TERM INT HUP
AOE_DATA=${AOE_DATA:-$D} AOE_HOME=${AOE_HOME:-$P/aoe} aoe.bin "$@"
EOF
chmod +x "$OUT/usr/bin/aoe"
mkdir -p "$OUT/usr/share"
echo "Games/Age of Empires=aoe" > "$OUT/usr/share/nxapps.76-aoe"
echo "installed in $OUT"
