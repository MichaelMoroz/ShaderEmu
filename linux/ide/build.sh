#!/usr/bin/env bash
# Builds nxide, the place to write C, JavaScript and Python in (docs/apps/ide.md), into build/images/linux/root.
# After linux/nanox/build.sh; what it compiles and runs with in the guest is linux/tcc/build.sh's,
# linux/quickjs/build.sh's and linux/micropython/build.sh's.
#   wsl -- bash /mnt/c/.../linux/ide/build.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
OUT=$REPO/build/images/linux/root/usr
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
mkdir -p "$OUT/bin" "$OUT/share" "$WORK/src/ide"
rv32-cc -O2 -Wall -Wno-unused-function -I"$MW/src/include" -I"$HERE/../apps" -I"$HERE/../userland" "$HERE/nxide.c" \
    "$MW/src/lib/libnano-X.a" -o "$WORK/src/ide/nxide"
riscv32-linux-strip -o "$OUT/bin/nxide" "$WORK/src/ide/nxide"
riscv32-linux-nm -n "$WORK/src/ide/nxide" > "$WORK/src/nxide.nm"
# the examples the compiler says nothing of: the IDE does not show them to it again (clean.py).
# Needs linux/tcc/build.sh's compiler for the host; without it the IDE checks them as any text.
if [ -x "$WORK/build/tcc-cross/tcc" ] && [ -d "$WORK/build/tcc-sysroot/usr/include" ]; then
    python3 "$HERE/clean.py" "$WORK/build/tcc-cross/tcc" "$WORK/build/tcc-sysroot" "$HERE"/../tcc/examples/*.c > "$OUT/share/ide-clean"
else
    rm -f "$OUT/share/ide-clean"
fi
# the examples in the other languages beside tcc's (example-NAME.js, .py), and the Python debugger
for f in "$HERE"/examples/*.js "$HERE"/examples/*.py; do cp "$f" "$OUT/share/example-$(basename "$f")"; done
cp "$HERE/pydbg.py" "$OUT/share/ide-pydbg.py"
# the Start menu, and whose a C, JavaScript, Python or HLSL file is (docs/system/open.md: before the editor's line for text)
printf '%s\n' "Programming/IDE=nxide" > "$OUT/share/nxapps.28-ide"
printf '%s\n' "source   c,h,js,py,hlsl   -   nxide %s" > "$OUT/share/nxopen.15-ide"
ls -l "$OUT/bin/nxide"
