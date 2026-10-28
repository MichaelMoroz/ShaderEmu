#!/usr/bin/env bash
# Builds QuickJS (MIT, Fabrice Bellard's JavaScript engine): qjs, the interpreter and its REPL, and
# qjsc, which turns a script into bytecode or C, into build/images/linux/root (docs/system/scripts.md).
# They replace upstream's, which are dynamic ELF programs and do not run on the machine without an MMU.
#   wsl -- bash /mnt/c/.../linux/quickjs/build.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
Q=$WORK/src/quickjs
BUILD=$WORK/src/quickjs-build
OUT=${QUICKJS_OUT:-$REPO/build/images/linux/root}	# (QUICKJS_OUT: a private image's overlay)
COMMIT=3d5e064e9dd67c70f7962836505a7fa067bf0a4e	# the 2026-06-04 release
if [ ! -d "$Q/.git" ]; then
    echo "== QuickJS $COMMIT"
    git clone -q https://github.com/bellard/quickjs.git "$Q"
fi
# no os.Worker and no Atomics: both want threads and the A extension's atomics, which the machine lacks;
# and a bigint's limbs divided in 32-bit divisions (mp_div1.patch: 64-bit ones are library calls here)
# (the tree is put back only when it is not that commit with these changes: objects then stay)
if [ "$(git -C "$Q" rev-parse HEAD)" != $COMMIT ] || [ "$(git -C "$Q" diff --stat | tail -1)" != " 2 files changed, 45 insertions(+), 2 deletions(-)" ]; then
    git -C "$Q" checkout -q -f "$COMMIT" && git -C "$Q" clean -q -fd
    sed -i 's|^#define USE_WORKER$|/* #define USE_WORKER */|' "$Q/quickjs-libc.c"
    git -C "$Q" apply "$HERE/mp_div1.patch"
    sed -i 's|^#define CONFIG_ATOMICS$|/* #define CONFIG_ATOMICS */|' "$Q/quickjs.c"
fi
grep -q '^/\* #define USE_WORKER \*/' "$Q/quickjs-libc.c" && grep -q '^/\* #define CONFIG_ATOMICS \*/' "$Q/quickjs.c" && grep -q mp_divlu "$Q/quickjs.c" || { echo "QuickJS's switches moved"; exit 1; }
mkdir -p "$BUILD/host" "$BUILD/guest"
LIB="quickjs dtoa libregexp libunicode cutils quickjs-libc"
DEF="-D_GNU_SOURCE -DCONFIG_VERSION=\"$(cat "$Q/VERSION")\""

# qjsc for this computer, which compiles the REPL (repl.js) to C for the guest's qjs
new() { [ ! "$1" -nt "$2" ]; }	# an object older than its source, or none
for f in $LIB qjsc; do
    new "$BUILD/host/$f.o" "$Q/$f.c" && gcc -O2 -w $DEF -DCONFIG_CC=\"gcc\" -DCONFIG_PREFIX=\"/usr\" -c "$Q/$f.c" -o "$BUILD/host/$f.o" &
done
# the guest's at the same time: a script's stack is checked against 192 KB of the program's 256 (linux/nommu/flat.py)
GDEF="$DEF -DJS_DEFAULT_STACK_SIZE=196608 -DCONFIG_CC=\"cc\" -DCONFIG_PREFIX=\"/usr\""
for f in $LIB qjs qjsc; do
    new "$BUILD/guest/$f.o" "$Q/$f.c" && rv32-cc -O2 -w $GDEF -c "$Q/$f.c" -o "$BUILD/guest/$f.o" &
done
wait
gcc "$BUILD"/host/*.o -lm -lpthread -o "$BUILD/host-qjsc"
(cd "$Q" && "$BUILD/host-qjsc" -s -c -o "$BUILD/repl.c" -m repl.js)
rv32-cc -O2 -w $GDEF -I"$Q" -c "$BUILD/repl.c" -o "$BUILD/guest/repl.o"
L=$(for f in $LIB; do printf '%s ' "$BUILD/guest/$f.o"; done)
rv32-cc "$BUILD/guest/qjs.o" "$BUILD/guest/repl.o" $L -lm -o "$BUILD/qjs"
rv32-cc "$BUILD/guest/qjsc.o" $L -lm -o "$BUILD/qjsc"
mkdir -p "$OUT/usr/bin" "$OUT/usr/share/quickjs"
riscv32-linux-strip -o "$OUT/usr/bin/qjs" "$BUILD/qjs"
riscv32-linux-strip -o "$OUT/usr/bin/qjsc" "$BUILD/qjsc"
cp "$Q/LICENSE" "$OUT/usr/share/quickjs/LICENSE"
ls -l "$OUT/usr/bin/qjs" "$OUT/usr/bin/qjsc"
