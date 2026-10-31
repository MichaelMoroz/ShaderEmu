#!/usr/bin/env bash
# Builds Nano-X (Microwindows) with this directory's drivers for the Linux image, after
# linux/kernel/build.sh. Output: build/images/linux/root (docs/system/nanox.md).
#   wsl -- bash /mnt/c/.../linux/nanox/build.sh
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
. "$REPO/linux/userland/toolchain.sh"
MW=$WORK/src/microwindows
MW_COMMIT=a53c319
PROGRAMS="nano-X nxev demo-hello demo-blit demo-polygon nxbench nxbar nxwintest glxgears"

if [ ! -d "$MW" ]; then
    echo "== Microwindows source"
    git clone -q https://github.com/ghaerr/microwindows "$MW"
    git -C "$MW" checkout -q $MW_COMMIT
fi

# our change to the server (docs/system/nanox.md), on a clean copy of its sources
git -C "$MW" checkout -q -- src/nanox src/engine src/demos src/include
git -C "$MW" apply "$HERE/microwindows.patch"

# our drivers, and the rules that select them
cp "$HERE"/*_shaderemu.c "$MW/src/drivers/"
# (the worker cores' mailboxes, for the pointer's worker: scr_shaderemu.c)
cp "$REPO/programs/mc/mc.h" "$MW/src/drivers/"
# (a font file of the ROM, for the screen driver and for programs that measure text: docs/system/fonts.md)
cp "$HERE/sef.h" "$MW/src/drivers/"
# (where a program's window was last put is the registry's: wmclients.c)
cp "$HERE/../apps/registry.h" "$MW/src/nanox/"
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
rv32-cc -O2 -w -I"$MW/src/include" "$HERE/nxwintest.c" "$MW/src/lib/libnano-X.a" -o "$MW/src/bin/nxwintest"
# the stock glxgears on our OpenGL driver, as a Nano-X client (programs/linux, docs/machine/gpu.md)
GL=$REPO/programs
[ -f "$GL/build/fetch/glxgears.c" ] || python3 "$GL/linux/fetch.py"
# (its time arithmetic a frame out of doubles: programs/linux/gears_time.py)
python3 "$GL/linux/gears_time.py" "$GL/build/fetch/glxgears.c" "$WORK/src/glxgears_time.c"
rv32-cc -O2 -w -DGL_NANOX -I"$MW/src/include" -idirafter "$GL/linux/include" "$WORK/src/glxgears_time.c" \
    "$GL/linux/gl.c" "$MW/src/lib/libnano-X.a" -lm -o "$MW/src/bin/glxgears"
tr -d '\r' < "$HERE/nx" > "$OUT/nx"
# the Start menu lists what the nxapps.* files name (nxbar.c): a program adds itself with one
mkdir -p "$OUT/../share"
# (a name before a slash is a folder of the menu; folders come in the order they are first named)
printf '%s\n' "Games/Tetris=nxtetris" "Games/Mines=nxmine" "Games/Pinball=nxpinball" "Utilities/Terminal=nxterm" "Utilities/Calculator=nxcalc" \
    "Utilities/Clock=nxclock" "Other/Eyes=nxeyes" "Other/Gears (OpenGL)=glxgears" > "$OUT/../share/nxapps.10-nanox"
for p in $PROGRAMS; do
    [ -f "$MW/src/bin/$p" ] || { echo "not built: $p"; continue; }
    riscv32-linux-strip -o "$OUT/$p" "$MW/src/bin/$p"
done
ls -l "$OUT"
