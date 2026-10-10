#!/usr/bin/env bash
# Builds the image's init (emuinit.c) into build/images/linux/root, from where
# tools/make_linux_image.py puts it in the image and makes it the kernel's init.
#   wsl -- bash /mnt/c/.../linux/userland/build.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/toolchain.sh"
OUT=$REPO/build/images/linux/root
mkdir -p "$OUT"
rv32-cc -Os -Wall ${EMUINIT_FLAGS:-} "$HERE/emuinit.c" -o "$WORK/src/emuinit"
riscv32-linux-strip -o "$OUT/emuinit" "$WORK/src/emuinit"
# the console's terminals (docs/console.md)
mkdir -p "$OUT/usr/bin"
rv32-cc -Os -Wall "$HERE/emumux.c" -o "$WORK/src/emumux"
riscv32-linux-strip -o "$OUT/usr/bin/emumux" "$WORK/src/emumux"
# busybox's network tools the ROM has no name for (docs/lan.md)
for applet in nc telnetd; do
    printf '#!/bin/sh\nexec busybox %s "$@"\n' "$applet" > "$OUT/usr/bin/$applet"
    chmod +x "$OUT/usr/bin/$applet"
done
# the sound card's test (docs/sound.md), and a second of 16-bit sound at 22,050 Hz to write to it
mkdir -p "$OUT/usr/bin" "$OUT/usr/share"
rv32-cc -O2 -Wall "$HERE/sndtest.c" -o "$WORK/src/sndtest"
riscv32-linux-strip -o "$OUT/usr/bin/sndtest" "$WORK/src/sndtest"
python3 -c "
import math, struct, sys
sys.stdout.buffer.write(b''.join(struct.pack('<h', int(9000 * math.sin(i * i / 9000.0) * (1 - i / 22050.0))) for i in range(22050)))
" > "$OUT/usr/share/snd-test.raw"
# the float instructions' test (docs/fpu.md): the only program here built to use them
rv32-cc -O2 -Wall -march=rv32imaf -mabi=ilp32 -ffp-contract=off -fno-math-errno "$HERE/fptest.c" -lm -o "$WORK/src/fptest"
riscv32-linux-strip -o "$OUT/usr/bin/fptest" "$WORK/src/fptest"
ls -l "$OUT/emuinit" "$OUT/usr/bin/sndtest" "$OUT/usr/bin/fptest"
