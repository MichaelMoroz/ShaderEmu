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
# the sound card's test (docs/sound.md), and a second of 16-bit sound at 22,050 Hz to write to it
mkdir -p "$OUT/usr/bin" "$OUT/usr/share"
rv32-cc -O2 -Wall "$HERE/sndtest.c" -o "$WORK/src/sndtest"
riscv32-linux-strip -o "$OUT/usr/bin/sndtest" "$WORK/src/sndtest"
python3 -c "
import math, struct, sys
sys.stdout.buffer.write(b''.join(struct.pack('<h', int(9000 * math.sin(i * i / 9000.0) * (1 - i / 22050.0))) for i in range(22050)))
" > "$OUT/usr/share/snd-test.raw"
ls -l "$OUT/emuinit" "$OUT/usr/bin/sndtest"
