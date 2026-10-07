#!/usr/bin/env bash
# Builds Nano-X (Microwindows) with this directory's drivers for the Linux image, after
# linux/kernel/build.sh. Output: build/images/linux/root (docs/nanox.md).
#   wsl -- bash /mnt/c/.../linux/nanox/build.sh
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
. "$REPO/linux/userland/toolchain.sh"
MW=$WORK/src/microwindows
MW_COMMIT=a53c319
PROGRAMS="nano-X nxclock nxeyes nxterm nxcalc nxtetris nxmine nxev demo-hello demo-blit demo-polygon nxbench nxbar glxgears"

if [ ! -d "$MW" ]; then
    echo "== Microwindows source"
    git clone -q https://github.com/ghaerr/microwindows "$MW"
    git -C "$MW" checkout -q $MW_COMMIT
fi

# our change to the server (docs/nanox.md), on a clean copy of its sources
git -C "$MW" checkout -q -- src/nanox src/engine src/demos src/include
git -C "$MW" apply "$HERE/microwindows.patch"

# our drivers, and the rules that select them
cp "$HERE"/*_shaderemu.c "$MW/src/drivers/"
if ! grep -q SHADEREMU "$MW/src/drivers/Objects.rules"; then
    cat >> "$MW/src/drivers/Objects.rules" <<'EOF'

# ShaderEmu: display and GPU, keyboard, pointer
ifeq ($(SCREEN), SHADEREMU)
MW_CORE_OBJS += $(MW_DIR_OBJ)/drivers/scr_shaderemu.o
endif
ifeq ($(MOUSE), SHADEREMUMOUSE)
MW_CORE_OBJS += $(MW_DIR_OBJ)/drivers/mou_shaderemu.o
endif
ifeq ($(KEYBOARD), SHADEREMUKBD)
MW_CORE_OBJS += $(MW_DIR_OBJ)/drivers/kbd_shaderemu.o
endif
EOF
fi

echo "== build"
make -C "$MW/src" -k -j"$(nproc)" CONFIG="$HERE/config" CC=rv32-cc AR=riscv32-linux-ar HOSTCC=gcc 2>&1 |
    grep -E "error|Error|undefined|warning: .*shaderemu" | grep -v "(ignored)" || true

OUT=$REPO/build/images/linux/root/usr/bin
mkdir -p "$OUT"
# our own clients; nxbench is linked elsewhere so a profile can tell it from the server
rv32-cc -O2 -w -Wl,-Ttext-segment=0x1000000 -I"$MW/src/include" "$HERE/nxbench.c" "$MW/src/lib/libnano-X.a" -o "$MW/src/bin/nxbench"
rv32-cc -O2 -w -I"$MW/src/include" "$HERE/nxbar.c" "$MW/src/lib/libnano-X.a" -o "$MW/src/bin/nxbar"
# the stock glxgears on our OpenGL driver, as a Nano-X client (programs/linux, docs/gpu.md)
GL=$REPO/programs
[ -f "$GL/build/fetch/glxgears.c" ] || python3 "$GL/linux/fetch.py"
rv32-cc -O2 -w -DGL_NANOX -I"$MW/src/include" -idirafter "$GL/linux/include" "$GL/build/fetch/glxgears.c" \
    "$GL/linux/gl.c" "$MW/src/lib/libnano-X.a" -lm -o "$MW/src/bin/glxgears"
tr -d '\r' < "$HERE/nx" > "$OUT/nx"
# the Start menu lists what the nxapps.* files name (nxbar.c): a program adds itself with one
mkdir -p "$OUT/../share"
printf '%s\n' "Terminal=nxterm" "Calculator=nxcalc" "Clock=nxclock" "Eyes=nxeyes" "Tetris=nxtetris" "Mines=nxmine" \
    "Gears (OpenGL)=glxgears" > "$OUT/../share/nxapps.10-nanox"
for p in $PROGRAMS; do
    [ -f "$MW/src/bin/$p" ] || { echo "not built: $p"; continue; }
    riscv32-linux-strip -o "$OUT/$p" "$MW/src/bin/$p"
done
ls -l "$OUT"
